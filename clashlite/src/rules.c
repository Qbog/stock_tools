#include "clashlite.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*
 * FakeDNS 规则：以“域名后缀”匹配为最小可用。
 * - 配置为逗号分隔：.google.com,.youtube.com
 * - 如果规则为空 -> 默认对所有 A 查询 Fake
 */

static void lower_str(char *s) {
    for (; *s; s++) {
        *s = (char)tolower((unsigned char)*s);
    }
}

int cl_rules_parse(const char *csv, char rules[][CL_MAX_DOMAIN], int max_rules) {
    if (!csv || csv[0] == '\0') return 0;

    int n = 0;
    const char *p = csv;
    while (*p && n < max_rules) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;

        const char *q = p;
        while (*q && *q != ',') q++;

        int len = (int)(q - p);
        if (len > 0 && len < CL_MAX_DOMAIN) {
            memcpy(rules[n], p, len);
            rules[n][len] = '\0';
            lower_str(rules[n]);
            n++;
        }

        p = q;
    }
    return n;
}

/*
 * 判断 domain 是否匹配任意 suffix。
 * - domain 应为小写无尾点形式（调用方可先 normalize）
 */
int cl_rules_match_suffix(const char *domain, char rules[][CL_MAX_DOMAIN], int n_rules) {
    if (n_rules == 0) return 1; // 空规则：全匹配

    int dlen = (int)strlen(domain);
    for (int i = 0; i < n_rules; i++) {
        const char *suf = rules[i];
        int slen = (int)strlen(suf);
        if (slen == 0) continue;
        if (slen > dlen) continue;

        if (strcmp(domain + (dlen - slen), suf) == 0) {
            return 1;
        }
    }
    return 0;
}

/*
 * 从文件加载规则（每行一个域名/后缀）。
 * - 支持注释：# 开头
 * - 支持空行
 * - 读取后统一转小写
 * - 文件内容建议像这样：
 *     # fake only these
 *     .google.com
 *     .youtube.com
 *     openai.com
 */
int cl_rules_load_file(const char *path, char rules[][CL_MAX_DOMAIN], int max_rules) {
    if (!path || path[0] == '\0') return 0;

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int n = 0;
    char line[CL_MAX_LINE];
    while (fgets(line, sizeof(line), f) && n < max_rules) {
        // trim
        int len = (int)strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            len--;
        }

        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#') continue;

        // remove trailing spaces
        char *e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1])) e--;
        *e = '\0';

        if (*p == '\0') continue;

        snprintf(rules[n], CL_MAX_DOMAIN, "%s", p);
        lower_str(rules[n]);
        n++;
    }

    fclose(f);
    return n;
}
