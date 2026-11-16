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

static void (*fatal_hook)(const char *message);

void rc_set_fatal_hook(void (*hook)(const char *message)) { fatal_hook = hook; }

void rc_fatal(const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "fatal: %s\n", msg);
    fflush(stderr);
    /* Once only: the hook itself might fail fatally. */
    void (*hook)(const char *) = fatal_hook;
    fatal_hook = NULL;
    if (hook) {
        hook(msg);
    }
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
