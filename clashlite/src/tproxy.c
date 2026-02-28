#include "clashlite.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/in.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <unistd.h>

/*
 * tproxy.c
 *
 * 透明代理（本机 TCP）核心逻辑：
 * - 监听本地端口（例如 127.0.0.1:12345）
 * - iptables 把本机进程的 TCP 连接 REDIRECT 到这个端口
 * - 程序对 accept() 到的连接：
 *   - 通过 getsockopt(SO_ORIGINAL_DST) 获取原始目标 IP:PORT
 *   - 如果目标 IP 落在 fake_range 中：
 *       - reverse map(fake_ip)->domain
 *       - 向 upstream DNS 解析 domain 得到 real_ip
 *       - 连接 real_ip:port（直连或 SOCKS5）
 *   - 否则：直连该 original ip:port（或 SOCKS5）
 * - 建立连接后，做双向转发（relay）
 *
 * 注意：
 * - 这里的“透明代理”更多是 iptables REDIRECT 的透明。
 * - 如果你要做网关/旁路由透明代理，推荐用 TPROXY + policy routing。
 */

#ifndef SO_ORIGINAL_DST
#define SO_ORIGINAL_DST 80
#endif

typedef struct {
    int client_fd;
    cl_config_t cfg;
    cl_fake_pool_t pool;
} worker_arg_t;

static int connect_tcp_ipv4_nbo(uint32_t ip_nbo, int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = ip_nbo;

    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(s);
        return -1;
    }
    return s;
}

/*
 * SOCKS5 CONNECT (no-auth)
 * - 连接 socks server 后，协商 method=0x00
 * - 发送 CONNECT 请求到目标 IPv4:port
 */
static int socks5_connect(const char *socks_host, int socks_port, uint32_t dst_ip_nbo, uint16_t dst_port) {
    // 连接 SOCKS5 服务器（MVP：只支持 socks_host 为 IPv4 字符串）
    struct in_addr sh;
    if (inet_pton(AF_INET, socks_host, &sh) != 1) return -1;

    int s = connect_tcp_ipv4_nbo(sh.s_addr, socks_port);
    if (s < 0) return -1;

    uint8_t buf[262];

    // greeting: VER=5, NMETHODS=1, METHODS[0]=0
    buf[0] = 0x05;
    buf[1] = 0x01;
    buf[2] = 0x00;
    if (send(s, buf, 3, 0) != 3) {
        close(s);
        return -1;
    }
    if (recv(s, buf, 2, MSG_WAITALL) != 2) {
        close(s);
        return -1;
    }
    if (buf[0] != 0x05 || buf[1] != 0x00) {
        close(s);
        return -1;
    }

    // CONNECT request
    // VER=5, CMD=1, RSV=0, ATYP=1(IPv4), DST.ADDR(4), DST.PORT(2)
    buf[0] = 0x05;
    buf[1] = 0x01;
    buf[2] = 0x00;
    buf[3] = 0x01;
    memcpy(buf + 4, &dst_ip_nbo, 4);
    buf[8] = (uint8_t)(dst_port >> 8);
    buf[9] = (uint8_t)(dst_port & 0xFF);

    if (send(s, buf, 10, 0) != 10) {
        close(s);
        return -1;
    }

    // response: VER, REP, RSV, ATYP, BND.ADDR, BND.PORT
    if (recv(s, buf, 4, MSG_WAITALL) != 4) {
        close(s);
        return -1;
    }
    if (buf[0] != 0x05 || buf[1] != 0x00) {
        close(s);
        return -1;
    }

    int atyp = buf[3];
    int to_read = 0;
    if (atyp == 0x01) to_read = 4 + 2;
    else if (atyp == 0x03) {
        // domain: 1 len + name + 2
        if (recv(s, buf, 1, MSG_WAITALL) != 1) { close(s); return -1; }
        to_read = buf[0] + 2;
    } else if (atyp == 0x04) to_read = 16 + 2;
    else { close(s); return -1; }

    if (to_read > 0) {
        if (recv(s, buf, to_read, MSG_WAITALL) != to_read) {
            close(s);
            return -1;
        }
    }

    return s; // now s is a connected tunnel
}

/*
 * 双向转发：
 * - 用 poll 监听两端可读
 * - 读一端写另一端
 * - 任何一端关闭则退出
 */
static void relay_bidirectional(int a, int b) {
    uint8_t buf[32 * 1024];

    while (1) {
        struct pollfd fds[2];
        fds[0].fd = a;
        fds[0].events = POLLIN;
        fds[1].fd = b;
        fds[1].events = POLLIN;

        int r = poll(fds, 2, -1);
        if (r <= 0) continue;

        for (int i = 0; i < 2; i++) {
            if (!(fds[i].revents & POLLIN)) continue;

            int src = (i == 0) ? a : b;
            int dst = (i == 0) ? b : a;

            ssize_t n = recv(src, buf, sizeof(buf), 0);
            if (n <= 0) {
                return;
            }

            ssize_t off = 0;
            while (off < n) {
                ssize_t m = send(dst, buf + off, (size_t)(n - off), 0);
                if (m <= 0) return;
                off += m;
            }
        }
    }
}

static void *worker_main(void *arg) {
    worker_arg_t *wa = (worker_arg_t *)arg;
    int cfd = wa->client_fd;

    // 获取原始目标
    struct sockaddr_in od;
    socklen_t odlen = sizeof(od);
    memset(&od, 0, sizeof(od));

    if (getsockopt(cfd, SOL_IP, SO_ORIGINAL_DST, &od, &odlen) != 0) {
        cl_log(wa->cfg.verbose, "SO_ORIGINAL_DST failed: %s", strerror(errno));
        close(cfd);
        free(wa);
        return NULL;
    }

    uint32_t dst_ip = od.sin_addr.s_addr; // network byte order
    uint16_t dst_port = ntohs(od.sin_port);

    // 如果是 fake ip，反查 domain 再解析真实 IP
    uint32_t real_ip = dst_ip;
    char domain[CL_MAX_DOMAIN] = {0};

    if (cl_fake_pool_contains(&wa->pool, dst_ip)) {
        if (cl_map_get_domain(dst_ip, domain, sizeof(domain)) == 0) {
            // 解析真实 A
            char up_host[64];
            int up_port;
            if (cl_parse_hostport(wa->cfg.upstream_dns, up_host, sizeof(up_host), &up_port) == 0) {
                if (cl_resolve_a_via_upstream(up_host, up_port, domain, &real_ip) != 0) {
                    cl_log(wa->cfg.verbose, "resolve failed: %s", domain);
                    close(cfd);
                    free(wa);
                    return NULL;
                }
            }
        } else {
            cl_log(wa->cfg.verbose, "fake ip but no domain mapping");
        }
    }

    // 建立到目标的连接（直连或 socks5）
    int rfd = -1;

    if (wa->cfg.socks5_upstream[0] != '\0') {
        char sh[64];
        int sp;
        if (cl_parse_hostport(wa->cfg.socks5_upstream, sh, sizeof(sh), &sp) != 0) {
            cl_log(wa->cfg.verbose, "bad socks5_upstream: %s", wa->cfg.socks5_upstream);
            close(cfd);
            free(wa);
            return NULL;
        }
        rfd = socks5_connect(sh, sp, real_ip, (uint16_t)dst_port);
    } else {
        rfd = connect_tcp_ipv4_nbo(real_ip, dst_port);
    }

    if (rfd < 0) {
        cl_log(wa->cfg.verbose, "connect remote failed");
        close(cfd);
        free(wa);
        return NULL;
    }

    if (wa->cfg.verbose) {
        char a1[INET_ADDRSTRLEN] = {0};
        char a2[INET_ADDRSTRLEN] = {0};
        struct in_addr ia1; ia1.s_addr = dst_ip;
        struct in_addr ia2; ia2.s_addr = real_ip;
        inet_ntop(AF_INET, &ia1, a1, sizeof(a1));
        inet_ntop(AF_INET, &ia2, a2, sizeof(a2));

        if (domain[0]) {
            cl_log(wa->cfg.verbose, "TCP %s:%u (fake) => %s:%u for domain=%s", a1, dst_port, a2, dst_port, domain);
        } else {
            cl_log(wa->cfg.verbose, "TCP %s:%u => %s:%u", a1, dst_port, a2, dst_port);
        }
    }

    relay_bidirectional(cfd, rfd);

    close(cfd);
    close(rfd);
    free(wa);
    return NULL;
}

int cl_tproxy_run(const cl_config_t *cfg, const cl_fake_pool_t *pool) {
    char host[64];
    int port;
    if (cl_parse_hostport(cfg->tproxy_listen, host, sizeof(host), &port) != 0) {
        cl_log(cfg->verbose, "tproxy_listen parse failed: %s", cfg->tproxy_listen);
        return -1;
    }

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        cl_log(cfg->verbose, "tproxy socket failed: %s", strerror(errno));
        return -1;
    }

    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    // 如果你用的是 TPROXY（网关模式），监听 socket 需要开启 IP_TRANSPARENT，
    // 否则内核可能拒绝把“目的地址不是本机”的连接交给我们。
    // 在 OUTPUT+REDIRECT（仅代理本机）场景里，它通常不是必须。
    if (setsockopt(s, SOL_IP, IP_TRANSPARENT, &one, sizeof(one)) != 0) {
        if (cfg->verbose) {
            cl_log(cfg->verbose, "setsockopt(IP_TRANSPARENT) failed (need CAP_NET_ADMIN/root): %s", strerror(errno));
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close(s);
        return -1;
    }

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        cl_log(cfg->verbose, "tproxy bind failed: %s", strerror(errno));
        close(s);
        return -1;
    }

    if (listen(s, 128) < 0) {
        cl_log(cfg->verbose, "listen failed: %s", strerror(errno));
        close(s);
        return -1;
    }

    cl_log(cfg->verbose, "Transparent proxy listening on %s", cfg->tproxy_listen);

    while (1) {
        int cfd = accept(s, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            cl_log(cfg->verbose, "accept error: %s", strerror(errno));
            continue;
        }

        worker_arg_t *wa = (worker_arg_t *)calloc(1, sizeof(worker_arg_t));
        wa->client_fd = cfd;
        wa->cfg = *cfg;
        wa->pool = *pool;

        pthread_t th;
        pthread_create(&th, NULL, worker_main, wa);
        pthread_detach(th);
    }

    close(s);
    return 0;
}
