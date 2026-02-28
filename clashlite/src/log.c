#include "clashlite.h"

#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/*
 * 简单日志：带时间戳。
 * verbose=0 时只输出关键错误；verbose=1 输出更多过程。
 */
void cl_log(int verbose, const char *fmt, ...) {
    (void)verbose; // 当前实现：由调用方决定是否调用

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);

    char ts[32];
    strftime(ts, sizeof(ts), "%F %T", &tm);

    fprintf(stderr, "[%s] ", ts);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
}
