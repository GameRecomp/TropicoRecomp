/* SPDX-License-Identifier: MIT */
#include "rc/log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rc_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void rc_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    abort();
}

int rc_trace(const char *what)
{
    static const char *list;
    static int read;
    if (!read) {
        list = getenv("RC_TRACE");
        read = 1;
    }
    if (!list) {
        return 0;
    }
    size_t n = strlen(what);
    for (const char *p = list; (p = strstr(p, what)) != NULL; p += n) {
        if ((p == list || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) {
            return 1;
        }
    }
    return 0;
}
