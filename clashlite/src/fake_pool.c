#include "clashlite.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

/*
 * Fake IP 地址池：从一个 CIDR 中顺序分配。
 * - 默认用 198.18.0.0/15（RFC 2544 基准测试保留段）
 * - 这里不做复杂的“回收”逻辑：
 *   - 典型 FakeDNS 使用场景里，映射是长期存在的；
 *   - 地址池足够大（/15 有 131072 个 IP），对个人/小规模使用够用。
 */

static int parse_cidr(const char *cidr, uint32_t *net_host, int *prefix) {
    char ip[64];
    const char *slash = strchr(cidr, '/');
    if (!slash) return -1;

    int iplen = (int)(slash - cidr);
    if (iplen <= 0 || iplen >= (int)sizeof(ip)) return -1;

    memcpy(ip, cidr, iplen);
    ip[iplen] = '\0';

    int pfx = atoi(slash + 1);
    if (pfx < 0 || pfx > 32) return -1;

    struct in_addr a;
    if (inet_pton(AF_INET, ip, &a) != 1) return -1;

    *net_host = ntohl(a.s_addr);
    *prefix = pfx;
    return 0;
}

int cl_fake_pool_init(cl_fake_pool_t *pool, const char *cidr) {
    uint32_t net;
    int pfx;
    if (parse_cidr(cidr, &net, &pfx) != 0) return -1;

    uint32_t mask = (pfx == 0) ? 0 : (0xFFFFFFFFu << (32 - pfx));
    uint32_t start = net & mask;
    uint32_t end = start | (~mask);

    pool->start = htonl(start);
    pool->end = htonl(end);
    pool->next = start + 1; // 避免网络地址

    return 0;
}

uint32_t cl_fake_pool_alloc(cl_fake_pool_t *pool) {
    // 简单循环分配；达到 end 后从 start+1 重新开始
    uint32_t start_h = ntohl(pool->start);
    uint32_t end_h = ntohl(pool->end);

    if (pool->next >= end_h) {
        pool->next = start_h + 1;
    }

    uint32_t ip = pool->next;
    pool->next++;
    return htonl(ip);
}

int cl_fake_pool_contains(const cl_fake_pool_t *pool, uint32_t ip_nbo) {
    uint32_t ip = ntohl(ip_nbo);
    uint32_t s = ntohl(pool->start);
    uint32_t e = ntohl(pool->end);
    return (ip >= s && ip <= e);
}
