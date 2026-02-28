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
