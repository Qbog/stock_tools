#include "clashlite.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * main.c
 *
 * 运行方式：
 *   ./clashlite -c config.ini
 *
 * 进程会启动两个线程：
 * - FakeDNS (UDP)
 * - Transparent proxy (TCP)
 *
 * 注意：
 * - 仅启动程序不足以实现“透明代理”，还需要配套 iptables 规则和系统 DNS 指向。
 */

typedef struct {
    cl_config_t cfg;
    cl_fake_pool_t *pool;
    char (*rules)[CL_MAX_DOMAIN];
    int n_rules;
} dns_thread_arg_t;

static void *dns_thread(void *arg) {
    dns_thread_arg_t *a = (dns_thread_arg_t *)arg;
    cl_dns_run(&a->cfg, a->pool, a->rules, a->n_rules);
    return NULL;
}

typedef struct {
    cl_config_t cfg;
} udp_thread_arg_t;

static void *udp_thread(void *arg) {
    udp_thread_arg_t *a = (udp_thread_arg_t *)arg;
    cl_udp_tproxy_run(&a->cfg);
    return NULL;
}

static void usage(const char *argv0) {
    fprintf(stderr, "Usage: %s -c <config.ini>\n", argv0);
}

int main(int argc, char **argv) {
    const char *conf = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            conf = argv[++i];
        }
    }

    if (!conf) {
        usage(argv[0]);
        return 2;
    }

    cl_config_t cfg;
    if (cl_load_config(conf, &cfg) != 0) {
        fprintf(stderr, "Failed to load config: %s\n", conf);
        return 1;
    }

    cl_fake_pool_t pool;
    if (cl_fake_pool_init(&pool, cfg.fake_range_cidr) != 0) {
        fprintf(stderr, "Bad fake_range: %s\n", cfg.fake_range_cidr);
        return 1;
    }

    static char rules[CL_MAX_RULES][CL_MAX_DOMAIN];
    int n_rules = 0;

    // 1) 从 csv 解析
    n_rules += cl_rules_parse(cfg.fake_suffix_rules, rules + n_rules, CL_MAX_RULES - n_rules);

    // 2) 从文件加载（可选）
    if (cfg.fake_rules_file[0] != '\0') {
        int m = cl_rules_load_file(cfg.fake_rules_file, rules + n_rules, CL_MAX_RULES - n_rules);
        if (m < 0) {
            fprintf(stderr, "Failed to load fake_rules_file: %s\n", cfg.fake_rules_file);
            return 1;
        }
        n_rules += m;
    }

    if (cfg.verbose) {
        cl_log(cfg.verbose, "Config loaded: dns=%s tproxy_tcp=%s tproxy_udp=%s upstream_dns=%s socks5=%s fake_range=%s rules=%d",
            cfg.dns_listen,
            cfg.tproxy_listen,
            cfg.udp_listen,
            cfg.upstream_dns,
            cfg.socks5_upstream[0] ? cfg.socks5_upstream : "(direct)",
            cfg.fake_range_cidr,
            n_rules
        );
    }

    // 启动 FakeDNS 线程
    pthread_t th_dns;
    dns_thread_arg_t da;
    memset(&da, 0, sizeof(da));
    da.cfg = cfg;
    da.pool = &pool;
    da.rules = rules;
    da.n_rules = n_rules;
    pthread_create(&th_dns, NULL, dns_thread, &da);

    // 启动 UDP 透明代理线程（需要 socks5_upstream，因为 UDP 走 SOCKS5 UDP ASSOCIATE）
    pthread_t th_udp;
    udp_thread_arg_t ua;
    memset(&ua, 0, sizeof(ua));
    ua.cfg = cfg;

    if (cfg.socks5_upstream[0] != '\0') {
        pthread_create(&th_udp, NULL, udp_thread, &ua);
    } else {
        cl_log(cfg.verbose, "UDP tproxy disabled: socks5_upstream not set");
    }

    // 主线程跑 TCP 透明代理（阻塞）
    cl_tproxy_run(&cfg, &pool);

    return 0;
}
