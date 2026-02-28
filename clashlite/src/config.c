#include "clashlite.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 配置文件格式：简单 ini key=value
 * - 允许空行与 # 开头注释
 * - 不支持 section
 *
 * 这样做的好处：
 * - 不引入 JSON/YAML 解析依赖
 * - 符合 Linux "文本即接口" 的习惯
 */

static void trim(char *s) {
    // 去前后空白
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static void set_default(cl_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->dns_listen, sizeof(cfg->dns_listen), "%s", "127.0.0.1:1053");
    snprintf(cfg->tproxy_listen, sizeof(cfg->tproxy_listen), "%s", "127.0.0.1:12345");
    snprintf(cfg->upstream_dns, sizeof(cfg->upstream_dns), "%s", "223.5.5.5:53");
    cfg->socks5_upstream[0] = '\0';
    snprintf(cfg->fake_range_cidr, sizeof(cfg->fake_range_cidr), "%s", "198.18.0.0/15");
    cfg->fake_suffix_rules[0] = '\0';
    cfg->fake_ttl = 60;
    cfg->verbose = 1;
}

int cl_load_config(const char *path, cl_config_t *cfg) {
    set_default(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        return -1;
    }

    char line[CL_MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '\0') continue;
        if (line[0] == '#') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        trim(k);
        trim(v);

        if (strcmp(k, "dns_listen") == 0) {
            snprintf(cfg->dns_listen, sizeof(cfg->dns_listen), "%s", v);
        } else if (strcmp(k, "tproxy_listen") == 0) {
            snprintf(cfg->tproxy_listen, sizeof(cfg->tproxy_listen), "%s", v);
        } else if (strcmp(k, "upstream_dns") == 0) {
            snprintf(cfg->upstream_dns, sizeof(cfg->upstream_dns), "%s", v);
        } else if (strcmp(k, "socks5_upstream") == 0) {
            snprintf(cfg->socks5_upstream, sizeof(cfg->socks5_upstream), "%s", v);
        } else if (strcmp(k, "fake_range") == 0) {
            snprintf(cfg->fake_range_cidr, sizeof(cfg->fake_range_cidr), "%s", v);
        } else if (strcmp(k, "fake_suffix_rules") == 0) {
            snprintf(cfg->fake_suffix_rules, sizeof(cfg->fake_suffix_rules), "%s", v);
        } else if (strcmp(k, "fake_ttl") == 0) {
            cfg->fake_ttl = atoi(v);
        } else if (strcmp(k, "verbose") == 0) {
            cfg->verbose = atoi(v);
        }
    }

    fclose(f);
    return 0;
}
