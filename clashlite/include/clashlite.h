#pragma once

/*
 * clashlite.h
 *
 * 这个项目故意把功能拆得比较“直白”：
 * - FakeDNS：监听 UDP 53/1053，解析 A 查询；匹配规则则返回假 IP，并记录 domain<->fake_ip 映射。
 * - 透明代理：监听本地 TCP 端口，通过 SO_ORIGINAL_DST 取回原目标；
 *           如果目标在 fake_ip 池中，则反查 domain，再用上游 DNS 解析真实 IP，最后直连/走 SOCKS5。
 *
 * 目标：用尽量少的依赖，在 Linux 上把 Clash FakeDNS+透明代理 的关键链路跑通。
 */

#include <stdint.h>
#include <netinet/in.h>

#define CL_MAX_LINE 512
#define CL_MAX_RULES 256
#define CL_MAX_DOMAIN 255

typedef struct {
    char dns_listen[64];          // e.g. 127.0.0.1:1053
    char tproxy_listen[64];       // e.g. 127.0.0.1:12345
    char upstream_dns[64];        // e.g. 223.5.5.5:53
    char socks5_upstream[64];     // e.g. 127.0.0.1:7890 (optional)
    char fake_range_cidr[32];     // e.g. 198.18.0.0/15
    char fake_suffix_rules[1024]; // comma-separated suffix list; empty means all
    int fake_ttl;
    int verbose;
} cl_config_t;

typedef struct {
    uint32_t start;  // network byte order
    uint32_t end;    // network byte order (inclusive)
    uint32_t next;   // next alloc pointer (host order)
} cl_fake_pool_t;

// --- config ---
int cl_load_config(const char *path, cl_config_t *cfg);

// --- logging ---
void cl_log(int verbose, const char *fmt, ...);

// --- fake pool ---
int cl_fake_pool_init(cl_fake_pool_t *pool, const char *cidr);
uint32_t cl_fake_pool_alloc(cl_fake_pool_t *pool); // returns network-order IPv4
int cl_fake_pool_contains(const cl_fake_pool_t *pool, uint32_t ip_nbo);

// --- domain rules ---
int cl_rules_parse(const char *csv, char rules[][CL_MAX_DOMAIN], int max_rules);
int cl_rules_match_suffix(const char *domain, char rules[][CL_MAX_DOMAIN], int n_rules);

// --- mapping (domain<->fake_ip) ---
// 这里用简单链表/互斥锁实现，规模不大够用。
int cl_map_put(const char *domain, uint32_t fake_ip_nbo);
int cl_map_get_ip(const char *domain, uint32_t *out_fake_ip_nbo);
int cl_map_get_domain(uint32_t fake_ip_nbo, char *out_domain, int out_sz);

// --- DNS ---
int cl_dns_run(const cl_config_t *cfg, cl_fake_pool_t *pool, char rules[][CL_MAX_DOMAIN], int n_rules);

// --- TCP transparent proxy ---
int cl_tproxy_run(const cl_config_t *cfg, const cl_fake_pool_t *pool);

// --- helpers ---
int cl_parse_hostport(const char *s, char *host, int host_sz, int *port);
int cl_resolve_a_via_upstream(const char *upstream_host, int upstream_port, const char *domain, uint32_t *out_ip_nbo);
