#include "clashlite.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/in.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/*
 * udp_tproxy.c
 *
 * 目标：为“网关/路由模式”提供 UDP 透明代理。
 *
 * 为什么 UDP 难：
 * - UDP 没有连接概念，必须为每个 "(client, original_dst)" 组合维护会话。
 * - 透明代理要做到“对 client 来说，响应看起来像来自 original_dst”。
 *
 * 解决方案（MVP）：
 * 1) iptables 使用 TPROXY 把 UDP 流量送到本地端口（cfg.udp_listen）。
 * 2) 本程序：
 *    - 在监听 socket 上开启 IP_RECVORIGDSTADDR，从 recvmsg() 控制消息里拿到原目标地址。
 *    - 以 (client_ip:port, orig_dst_ip:port) 作为 key 建会话。
 *    - 每个会话：
 *        - 建立到 SOCKS5 的 TCP 控制连接，执行 UDP ASSOCIATE 得到 relay 地址。
 *        - 创建一个 UDP socket 发/收 relay。
 *        - 创建一个 "to_client" UDP socket：IP_TRANSPARENT + bind(orig_dst) + connect(client)
 *          用它把从 relay 收到的数据发回 client（源地址/端口伪装为 orig_dst）。
 *
 * SOCKS5 UDP 协议要点：
 * - 先 TCP 握手协商 method
 * - 发送 UDP ASSOCIATE（CMD=3），服务器返回 relay 的 IP:PORT
 * - UDP 数据包格式：
 *   +----+----+----+----+----+----+----+----+----+----+
 *   | RSV| RSV|FRAG|ATYP| DST.ADDR | DST.PORT |  DATA  |
 *   +----+----+----+----+----+----+----+----+----+----+
 *   RSV=0x0000, FRAG=0x00（不支持分片）
 */

#ifndef IP_RECVORIGDSTADDR
#define IP_RECVORIGDSTADDR IP_ORIGDSTADDR
#endif

typedef struct udp_sess {
    struct sockaddr_in client;
    struct sockaddr_in orig_dst;

    int socks_tcp;
    int relay_udp;
    struct sockaddr_in relay_addr;

    int to_client;

    time_t last_active;
    pthread_t th;

    struct udp_sess *next;
} udp_sess_t;

static udp_sess_t *g_sessions = NULL;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static int addr_equal(const struct sockaddr_in *a, const struct sockaddr_in *b) {
    return a->sin_addr.s_addr == b->sin_addr.s_addr && a->sin_port == b->sin_port;
}

static void addr_to_str(const struct sockaddr_in *a, char *out, int out_sz) {
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
    snprintf(out, out_sz, "%s:%u", ip, (unsigned)ntohs(a->sin_port));
}

static int tcp_connect_hostport(const char *host, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        close(s);
        return -1;
    }

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }

    return s;
}

static int socks5_handshake_noauth(int s) {
    uint8_t buf[8];
    buf[0] = 0x05;
    buf[1] = 0x01;
    buf[2] = 0x00;
    if (send(s, buf, 3, 0) != 3) return -1;
    if (recv(s, buf, 2, MSG_WAITALL) != 2) return -1;
    if (buf[0] != 0x05 || buf[1] != 0x00) return -1;
    return 0;
}

static int socks5_udp_associate(int s, struct sockaddr_in *out_relay) {
    uint8_t buf[64];

    // CMD=3 UDP ASSOCIATE, ATYP=IPv4, ADDR=0.0.0.0, PORT=0
    buf[0] = 0x05;
    buf[1] = 0x03;
    buf[2] = 0x00;
    buf[3] = 0x01;
    memset(buf + 4, 0, 4);
    buf[8] = 0;
    buf[9] = 0;

    if (send(s, buf, 10, 0) != 10) return -1;

    // reply: VER, REP, RSV, ATYP, BND.ADDR, BND.PORT
    if (recv(s, buf, 4, MSG_WAITALL) != 4) return -1;
    if (buf[0] != 0x05 || buf[1] != 0x00) return -1;

    int atyp = buf[3];
    if (atyp != 0x01) {
        // MVP：仅支持 IPv4 relay
        return -1;
    }

    if (recv(s, buf, 6, MSG_WAITALL) != 6) return -1;

    memset(out_relay, 0, sizeof(*out_relay));
    out_relay->sin_family = AF_INET;
    memcpy(&out_relay->sin_addr.s_addr, buf, 4);
    out_relay->sin_port = (uint16_t)((buf[4] << 8) | buf[5]);

    return 0;
}

static int make_udp_socket_to_relay(const struct sockaddr_in *relay) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    // connect() 不是必须，但这样 send()/recv() 更简单
    if (connect(s, (const struct sockaddr *)relay, sizeof(*relay)) != 0) {
        close(s);
        return -1;
    }

    // 设置读取超时，便于会话回收
    struct timeval tv;
    tv.tv_sec = 60;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return s;
}

static int make_udp_socket_to_client(const struct sockaddr_in *orig_dst, const struct sockaddr_in *client) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;

    // 允许绑定/发送 "非本机地址"：透明代理必备
    int one = 1;
    if (setsockopt(s, SOL_IP, IP_TRANSPARENT, &one, sizeof(one)) != 0) {
        // 没有权限/内核不支持会失败
        // 如果失败，透明回包的源地址将不正确。
        close(s);
        return -1;
    }

    if (bind(s, (const struct sockaddr *)orig_dst, sizeof(*orig_dst)) != 0) {
        close(s);
        return -1;
    }

    if (connect(s, (const struct sockaddr *)client, sizeof(*client)) != 0) {
        close(s);
        return -1;
    }

    return s;
}

static int build_socks5_udp_req_ipv4(const struct sockaddr_in *dst, const uint8_t *data, int data_len, uint8_t *out, int out_sz) {
    // header 10 bytes + payload
    if (out_sz < 10 + data_len) return -1;

    out[0] = 0x00; // RSV
    out[1] = 0x00; // RSV
    out[2] = 0x00; // FRAG
    out[3] = 0x01; // ATYP IPv4

    memcpy(out + 4, &dst->sin_addr.s_addr, 4);

    uint16_t p = dst->sin_port; // network order
    out[8] = (uint8_t)(p >> 8);
    out[9] = (uint8_t)(p & 0xFF);

    memcpy(out + 10, data, (size_t)data_len);
    return 10 + data_len;
}

static int parse_socks5_udp_resp_ipv4(const uint8_t *pkt, int pkt_len, struct sockaddr_in *src, const uint8_t **out_data, int *out_len) {
    if (pkt_len < 10) return -1;
    if (pkt[0] != 0x00 || pkt[1] != 0x00) return -1;
    if (pkt[2] != 0x00) return -1; // no frag

    if (pkt[3] != 0x01) return -1; // IPv4 only

    memset(src, 0, sizeof(*src));
    src->sin_family = AF_INET;
    memcpy(&src->sin_addr.s_addr, pkt + 4, 4);
    src->sin_port = (uint16_t)((pkt[8] << 8) | pkt[9]);

    *out_data = pkt + 10;
    *out_len = pkt_len - 10;
    return 0;
}

static void *udp_relay_thread(void *arg) {
    udp_sess_t *sess = (udp_sess_t *)arg;

    uint8_t buf[64 * 1024];
    while (1) {
        int n = recv(sess->relay_udp, buf, sizeof(buf), 0);
        if (n < 0) {
            // timeout -> try cleanup
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                time_t now = time(NULL);
                if (now - sess->last_active > 60) {
                    break;
                }
                continue;
            }
            break;
        }

        sess->last_active = time(NULL);

        struct sockaddr_in src;
        const uint8_t *payload;
        int payload_len;
        if (parse_socks5_udp_resp_ipv4(buf, n, &src, &payload, &payload_len) != 0) {
            continue;
        }

        // 把 payload 发回 client（注意：此处 src 是远端地址/端口；
        // 但我们的 to_client socket 已经 bind 到 orig_dst，所以源地址伪装为 orig_dst。
        // 对大多数场景而言：orig_dst 就是 client 期待的“远端”。
        // 如果 remote != orig_dst（例如 NAT/负载均衡变化），透明性会下降。
        (void)src;

        if (send(sess->to_client, payload, (size_t)payload_len, 0) < 0) {
            break;
        }
    }

    // remove from list
    pthread_mutex_lock(&g_mu);
    udp_sess_t **pp = &g_sessions;
    while (*pp) {
        if (*pp == sess) {
            *pp = sess->next;
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_mu);

    close(sess->to_client);
    close(sess->relay_udp);
    close(sess->socks_tcp);
    free(sess);
    return NULL;
}

static udp_sess_t *get_or_create_session(const cl_config_t *cfg, const struct sockaddr_in *client, const struct sockaddr_in *orig_dst) {
    pthread_mutex_lock(&g_mu);
    for (udp_sess_t *p = g_sessions; p; p = p->next) {
        if (addr_equal(&p->client, client) && addr_equal(&p->orig_dst, orig_dst)) {
            p->last_active = time(NULL);
            pthread_mutex_unlock(&g_mu);
            return p;
        }
    }
    pthread_mutex_unlock(&g_mu);

    // create new session (slow path)
    char sh[64];
    int sp;
    if (cl_parse_hostport(cfg->socks5_upstream, sh, sizeof(sh), &sp) != 0) {
        return NULL;
    }

    int tcp = tcp_connect_hostport(sh, sp);
    if (tcp < 0) return NULL;

    if (socks5_handshake_noauth(tcp) != 0) {
        close(tcp);
        return NULL;
    }

    struct sockaddr_in relay;
    if (socks5_udp_associate(tcp, &relay) != 0) {
        close(tcp);
        return NULL;
    }

    int rudp = make_udp_socket_to_relay(&relay);
    if (rudp < 0) {
        close(tcp);
        return NULL;
    }

    int to_cli = make_udp_socket_to_client(orig_dst, client);
    if (to_cli < 0) {
        close(rudp);
        close(tcp);
        return NULL;
    }

    udp_sess_t *sess = (udp_sess_t *)calloc(1, sizeof(*sess));
    sess->client = *client;
    sess->orig_dst = *orig_dst;
    sess->socks_tcp = tcp;
    sess->relay_udp = rudp;
    sess->relay_addr = relay;
    sess->to_client = to_cli;
    sess->last_active = time(NULL);

    pthread_mutex_lock(&g_mu);
    sess->next = g_sessions;
    g_sessions = sess;
    pthread_mutex_unlock(&g_mu);

    pthread_create(&sess->th, NULL, udp_relay_thread, sess);
    pthread_detach(sess->th);

    if (cfg->verbose) {
        char c[64], d[64], r[64];
        addr_to_str(client, c, sizeof(c));
        addr_to_str(orig_dst, d, sizeof(d));
        addr_to_str(&relay, r, sizeof(r));
        cl_log(cfg->verbose, "UDP session created client=%s dst=%s relay=%s", c, d, r);
    }

    return sess;
}

static int recvmsg_orig_dst(int s, uint8_t *buf, int buf_sz, struct sockaddr_in *out_client, struct sockaddr_in *out_orig_dst) {
    struct msghdr msg;
    struct iovec iov;
    char cbuf[256];

    memset(&msg, 0, sizeof(msg));
    memset(out_client, 0, sizeof(*out_client));
    memset(out_orig_dst, 0, sizeof(*out_orig_dst));

    iov.iov_base = buf;
    iov.iov_len = (size_t)buf_sz;

    msg.msg_name = out_client;
    msg.msg_namelen = sizeof(*out_client);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    int n = recvmsg(s, &msg, 0);
    if (n < 0) return -1;

    // parse control messages for IP_ORIGDSTADDR
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_IP && c->cmsg_type == IP_ORIGDSTADDR) {
            struct sockaddr_in *a = (struct sockaddr_in *)CMSG_DATA(c);
            *out_orig_dst = *a;
            return n;
        }
    }

    // 如果没拿到原目标，返回错误
    errno = ENOENT;
    return -1;
}

int cl_udp_tproxy_run(const cl_config_t *cfg) {
    if (!cfg->socks5_upstream[0]) {
        cl_log(cfg->verbose, "UDP tproxy requires socks5_upstream");
        return -1;
    }

    char host[64];
    int port;
    if (cl_parse_hostport(cfg->udp_listen, host, sizeof(host), &port) != 0) {
        cl_log(cfg->verbose, "udp_listen parse failed: %s", cfg->udp_listen);
        return -1;
    }

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        cl_log(cfg->verbose, "udp socket failed: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // 透明代理 + 取原目标地址
    if (setsockopt(s, SOL_IP, IP_TRANSPARENT, &one, sizeof(one)) != 0) {
        cl_log(cfg->verbose, "setsockopt IP_TRANSPARENT failed (need root): %s", strerror(errno));
        close(s);
        return -1;
    }

    if (setsockopt(s, SOL_IP, IP_RECVORIGDSTADDR, &one, sizeof(one)) != 0) {
        cl_log(cfg->verbose, "setsockopt IP_RECVORIGDSTADDR failed: %s", strerror(errno));
        close(s);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(s);
        return -1;
    }

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        cl_log(cfg->verbose, "udp bind failed: %s", strerror(errno));
        close(s);
        return -1;
    }

    cl_log(cfg->verbose, "UDP transparent proxy listening on %s (SOCKS5 %s)", cfg->udp_listen, cfg->socks5_upstream);

    uint8_t pkt[64 * 1024];
    uint8_t out[64 * 1024 + 32];

    while (1) {
        struct sockaddr_in client, orig_dst;
        int n = recvmsg_orig_dst(s, pkt, sizeof(pkt), &client, &orig_dst);
        if (n < 0) {
            if (errno == EINTR) continue;
            continue;
        }

        udp_sess_t *sess = get_or_create_session(cfg, &client, &orig_dst);
        if (!sess) continue;

        int m = build_socks5_udp_req_ipv4(&orig_dst, pkt, n, out, sizeof(out));
        if (m < 0) continue;

        if (send(sess->relay_udp, out, (size_t)m, 0) < 0) {
            continue;
        }

        sess->last_active = time(NULL);
    }

    close(s);
    return 0;
}
