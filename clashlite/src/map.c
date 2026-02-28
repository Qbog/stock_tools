#include "clashlite.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * domain <-> fake_ip 的映射表
 *
 * 说明：
 * - Clash 的 FakeDNS 通常需要：
 *   - domain -> fake ip（给 DNS 回答）
 *   - fake ip -> domain（透明代理接管连接时反查）
 *
 * - 为了减少依赖，这里用最简单的链表实现：
 *   - 规模：几百～几千条时完全够用
 *   - 复杂度：O(n)
 *
 * 如果你后续规模更大，可以替换为 hash（uthash / 自己写哈希表）。
 */

typedef struct node {
    char domain[CL_MAX_DOMAIN];
    uint32_t fake_ip; // network byte order
    struct node *next;
} node_t;

static node_t *g_head = NULL;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static void normalize_domain(const char *in, char *out, int out_sz) {
    // 规则：全小写、去掉末尾 '.'
    int n = 0;
    for (; *in && n < out_sz - 1; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        out[n++] = c;
    }
    out[n] = '\0';
    // remove trailing dot
    n = (int)strlen(out);
    if (n > 0 && out[n - 1] == '.') out[n - 1] = '\0';
}

int cl_map_put(const char *domain, uint32_t fake_ip_nbo) {
    char d[CL_MAX_DOMAIN];
    normalize_domain(domain, d, sizeof(d));

    pthread_mutex_lock(&g_mu);

    // if exists, update
    for (node_t *p = g_head; p; p = p->next) {
        if (strcmp(p->domain, d) == 0) {
            p->fake_ip = fake_ip_nbo;
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
    }

    node_t *n = (node_t *)calloc(1, sizeof(node_t));
    if (!n) {
        pthread_mutex_unlock(&g_mu);
        return -1;
    }

    snprintf(n->domain, sizeof(n->domain), "%s", d);
    n->fake_ip = fake_ip_nbo;
    n->next = g_head;
    g_head = n;

    pthread_mutex_unlock(&g_mu);
    return 0;
}

int cl_map_get_ip(const char *domain, uint32_t *out_fake_ip_nbo) {
    char d[CL_MAX_DOMAIN];
    normalize_domain(domain, d, sizeof(d));

    pthread_mutex_lock(&g_mu);
    for (node_t *p = g_head; p; p = p->next) {
        if (strcmp(p->domain, d) == 0) {
            *out_fake_ip_nbo = p->fake_ip;
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return -1;
}

int cl_map_get_domain(uint32_t fake_ip_nbo, char *out_domain, int out_sz) {
    pthread_mutex_lock(&g_mu);
    for (node_t *p = g_head; p; p = p->next) {
        if (p->fake_ip == fake_ip_nbo) {
            snprintf(out_domain, out_sz, "%s", p->domain);
            pthread_mutex_unlock(&g_mu);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return -1;
}
