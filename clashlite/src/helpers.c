#include "clashlite.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * 解析 host:port
 * - host 可为 IPv4 字符串（本项目 MVP 不支持 IPv6 host:port 语法）
 */
int cl_parse_hostport(const char *s, char *host, int host_sz, int *port) {
    const char *p = strrchr(s, ':');
    if (!p) return -1;

    int hlen = (int)(p - s);
    if (hlen <= 0 || hlen >= host_sz) return -1;

    memcpy(host, s, hlen);
    host[hlen] = '\0';

    int prt = atoi(p + 1);
    if (prt <= 0 || prt > 65535) return -1;
    *port = prt;
    return 0;
}
