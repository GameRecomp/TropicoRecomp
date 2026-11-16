/* SPDX-License-Identifier: MIT */
/* The runtime in a browser: see rc/web.h. */
#if defined(__EMSCRIPTEN__)

#include "rc/web.h"

#include <emscripten.h>

void rc_web_poll(void)
{
    static double last;
    double now = emscripten_get_now();
    if (now - last >= 16.0) {
        rc_web_yield();
        last = emscripten_get_now();
    }
}

#else
/* ISO C wants something in a translation unit. */
typedef int rc_web_unused;
#endif
