#include "clashlite.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * dns.c
 *
 * 这是一个“够用的”DNS UDP 服务器实现：
 * - 仅处理 A/IN 查询
 * - 仅处理 1 个 question
 * - 支持两种模式：
 *   1) Fake：匹配规则时返回 fake A
 *   2) Forward：不匹配时转发给 upstream DNS，并把响应原封不动回给客户端
 *
 * 说明：
 * - 为了透明代理链路，本模块还负责：
 *   - domain -> fake ip 映射的生成/保存
 */

#pragma pack(push, 1)
typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} dns_hdr_t;
#pragma pack(pop)

static int parse_qname(const uint8_t *buf, int len, int off, char *out, int out_sz, int *out_next) {
    // QNAME 是 label 序列：len byte + label bytes，末尾 0
    int p = off;
    int w = 0;

    while (p < len) {
        uint8_t l = buf[p++];
        if (l == 0) {
            // end
            if (w > 0 && out[w - 1] == '.') w--; // remove trailing dot
            out[w] = '\0';
            *out_next = p;
            return 0;
        }
        if (l > 63) return -1;
        if (p + l > len) return -1;
        if (w + l + 1 >= out_sz) return -1;

        memcpy(out + w, buf + p, l);
        w += l;
        out[w++] = '.';
        p += l;
    }
    return -1;
}

static int build_fake_a_response(
    const cl_config_t *cfg,
    const uint8_t *req,
    int req_len,
    const char *qname,
    uint32_t fake_ip_nbo,
    uint8_t *out,
    int out_sz
) {
    if (req_len < (int)sizeof(dns_hdr_t)) return -1;
    if (out_sz < req_len + 16) return -1;

    // 复制原请求（header + question），然后在末尾追加 answer
    memcpy(out, req, req_len);

    dns_hdr_t *h = (dns_hdr_t *)out;

    // 设置 flags：QR=1 response；保留 RD；RA=1；RCODE=0
    uint16_t flags = ntohs(h->flags);
    flags |= 0x8000; // QR
    flags |= 0x0080; // RA
    flags &= 0xFFF0; // clear rcode
    h->flags = htons(flags);

    h->ancount = htons(1);
    h->nscount = htons(0);
    h->arcount = htons(0);

    int p = req_len;

    // Answer NAME：指针到 question 的 qname 起始位置 12 (0x0c)
    // 典型 DNS 报文：header 12 bytes 后就是 qname
    out[p++] = 0xC0;
    out[p++] = 0x0C;

    // TYPE A
    out[p++] = 0x00;
    out[p++] = 0x01;
    // CLASS IN
    out[p++] = 0x00;
    out[p++] = 0x01;

    // TTL
    uint32_t ttl = htonl((uint32_t)cfg->fake_ttl);
    memcpy(out + p, &ttl, 4);
    p += 4;

    // RDLENGTH=4
    out[p++] = 0x00;
    out[p++] = 0x04;

    // RDATA (IPv4)
    memcpy(out + p, &fake_ip_nbo, 4);
    p += 4;

    return p;
}

static int forward_to_upstream(
    const char *up_host,
    int up_port,
    const uint8_t *req,
    int req_len,
    uint8_t *resp,
    int resp_sz
) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in up;
    memset(&up, 0, sizeof(up));
    up.sin_family = AF_INET;
    up.sin_port = htons((uint16_t)up_port);
    if (inet_pton(AF_INET, up_host, &up.sin_addr) != 1) {
        close(s);
        return -1;
    }

    // 简单超时
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (sendto(s, req, req_len, 0, (struct sockaddr *)&up, sizeof(up)) < 0) {
        close(s);
        return -1;
    }

    int n = recvfrom(s, resp, resp_sz, 0, NULL, NULL);
    close(s);
    return n;
}

#include <pthread.h>

static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;

int cl_dns_run(const cl_config_t *cfg, cl_fake_pool_t *pool, char rules[][CL_MAX_DOMAIN], int n_rules) {
    char host[64];
    int port;
    if (cl_parse_hostport(cfg->dns_listen, host, sizeof(host), &port) != 0) {
        cl_log(cfg->verbose, "dns_listen parse failed: %s", cfg->dns_listen);
        return -1;
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        cl_log(cfg->verbose, "dns socket failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        cl_log(cfg->verbose, "dns listen host invalid: %s", host);
        close(s);
        return -1;
    }

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        cl_log(cfg->verbose, "dns bind failed (%s): %s", cfg->dns_listen, strerror(errno));
        close(s);
        return -1;
    }

    char up_host[64];
    int up_port;
    if (cl_parse_hostport(cfg->upstream_dns, up_host, sizeof(up_host), &up_port) != 0) {
        cl_log(cfg->verbose, "upstream_dns parse failed: %s", cfg->upstream_dns);
        close(s);
        return -1;
    }

    cl_log(cfg->verbose, "FakeDNS listening on %s (upstream %s)", cfg->dns_listen, cfg->upstream_dns);

    uint8_t buf[1500];
    uint8_t resp[1500];

    while (1) {
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&cli, &clen);
        if (n < 0) {
            if (errno == EINTR) continue;
            cl_log(cfg->verbose, "dns recvfrom error: %s", strerror(errno));
            continue;
        }
        if (n < (int)sizeof(dns_hdr_t)) continue;

        dns_hdr_t *h = (dns_hdr_t *)buf;
        int qd = ntohs(h->qdcount);
        if (qd != 1) {
            // MVP: 只支持 1 个 question，其他直接转发
            int m = forward_to_upstream(up_host, up_port, buf, n, resp, sizeof(resp));
            if (m > 0) sendto(s, resp, m, 0, (struct sockaddr *)&cli, clen);
            continue;
        }

        // 解析 question
        int off = (int)sizeof(dns_hdr_t);
        char qname[CL_MAX_DOMAIN];
        int next;
        if (parse_qname(buf, n, off, qname, sizeof(qname), &next) != 0) {
            continue;
        }
        if (next + 4 > n) continue;
        uint16_t qtype = (uint16_t)((buf[next] << 8) | buf[next + 1]);
        uint16_t qclass = (uint16_t)((buf[next + 2] << 8) | buf[next + 3]);

        // 只处理 A IN
        int is_a = (qtype == 1 && qclass == 1);
        if (!is_a) {
            int m = forward_to_upstream(up_host, up_port, buf, n, resp, sizeof(resp));
            if (m > 0) sendto(s, resp, m, 0, (struct sockaddr *)&cli, clen);
            continue;
        }

        // normalize domain to lower
        for (char *p = qname; *p; p++) {
            if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
        }

        int do_fake = cl_rules_match_suffix(qname, rules, n_rules);
        if (!do_fake) {
            int m = forward_to_upstream(up_host, up_port, buf, n, resp, sizeof(resp));
            if (m > 0) sendto(s, resp, m, 0, (struct sockaddr *)&cli, clen);
            continue;
        }

        // get or alloc fake ip
        uint32_t fake_ip;
        if (cl_map_get_ip(qname, &fake_ip) != 0) {
            // allocate from shared pool (need lock)
            pthread_mutex_lock(&g_pool_mu);
            fake_ip = cl_fake_pool_alloc(pool);
            pthread_mutex_unlock(&g_pool_mu);

            cl_map_put(qname, fake_ip);
        }

        int m = build_fake_a_response(cfg, buf, n, qname, fake_ip, resp, sizeof(resp));
        if (m > 0) {
            sendto(s, resp, m, 0, (struct sockaddr *)&cli, clen);
        }

        if (cfg->verbose) {
            struct in_addr a;
            a.s_addr = fake_ip;
            cl_log(cfg->verbose, "FakeDNS A %s -> %s", qname, inet_ntoa(a));
        }
    }

    close(s);
    return 0;
}

/*
 * 供透明代理使用：通过上游 DNS 解析 domain 的真实 A 记录。
 * - 这里复用 forward 逻辑：构造一个最小 DNS query，发送给 upstream，解析第一个 A。
 */
static int build_query_a(uint16_t id, const char *domain, uint8_t *out, int out_sz) {
    if (out_sz < 64) return -1;

    dns_hdr_t h;
    memset(&h, 0, sizeof(h));
    h.id = htons(id);
    h.flags = htons(0x0100); // RD=1
    h.qdcount = htons(1);

    int p = 0;
    memcpy(out + p, &h, sizeof(h));
    p += (int)sizeof(h);

    // QNAME
    const char *s = domain;
    while (*s) {
        const char *dot = strchr(s, '.');
        int len = dot ? (int)(dot - s) : (int)strlen(s);
        if (len <= 0 || len > 63) return -1;
        if (p + 1 + len >= out_sz) return -1;
        out[p++] = (uint8_t)len;
        memcpy(out + p, s, len);
        p += len;
        if (!dot) break;
        s = dot + 1;
    }
    out[p++] = 0x00;

    // QTYPE A, QCLASS IN
    out[p++] = 0x00; out[p++] = 0x01;
    out[p++] = 0x00; out[p++] = 0x01;

    return p;
}

int cl_resolve_a_via_upstream(const char *upstream_host, int upstream_port, const char *domain, uint32_t *out_ip_nbo) {
    uint8_t q[512];
    uint8_t r[1500];

    uint16_t id = (uint16_t)(rand() & 0xFFFF);
    int qlen = build_query_a(id, domain, q, sizeof(q));
    if (qlen < 0) return -1;

    int n = forward_to_upstream(upstream_host, upstream_port, q, qlen, r, sizeof(r));
    if (n < (int)sizeof(dns_hdr_t)) return -1;

    dns_hdr_t *h = (dns_hdr_t *)r;
    if (ntohs(h->id) != id) {
        // 不是同一个 id 也可能出现（UDP），简单起见直接失败
        return -1;
    }

    int an = ntohs(h->ancount);
    int qd = ntohs(h->qdcount);
    if (qd < 1 || an < 1) return -1;

    // skip question
    int off = (int)sizeof(dns_hdr_t);
    char tmp[CL_MAX_DOMAIN];
    int next;
    if (parse_qname(r, n, off, tmp, sizeof(tmp), &next) != 0) return -1;
    off = next + 4;
    if (off >= n) return -1;

    // parse first answer (very simplified)
    // NAME can be pointer (0xC0xx)
    if (off + 2 > n) return -1;
    if ((r[off] & 0xC0) == 0xC0) {
        off += 2;
    } else {
        // rarely, full name
        if (parse_qname(r, n, off, tmp, sizeof(tmp), &next) != 0) return -1;
        off = next;
    }

    if (off + 10 > n) return -1;
    uint16_t type = (uint16_t)((r[off] << 8) | r[off + 1]);
    uint16_t klass = (uint16_t)((r[off + 2] << 8) | r[off + 3]);
    // TTL 4 bytes skip
    uint16_t rdlen = (uint16_t)((r[off + 8] << 8) | r[off + 9]);
    off += 10;

    if (type != 1 || klass != 1 || rdlen != 4) return -1;
    if (off + 4 > n) return -1;

    memcpy(out_ip_nbo, r + off, 4);
    return 0;
}
