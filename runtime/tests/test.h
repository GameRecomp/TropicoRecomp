/* SPDX-License-Identifier: MIT */
/* A minimal test harness: no dependencies, works under Emscripten. */
#ifndef RC_TEST_H
#define RC_TEST_H

#include <stdio.h>

extern int rc_test_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);               \
            rc_test_failures++;                                                                    \
        }                                                                                          \
    } while (0)

#define CHECK_EQ(a, b)                                                                             \
    do {                                                                                           \
        unsigned long long va_ = (unsigned long long)(a), vb_ = (unsigned long long)(b);           \
        if (va_ != vb_) {                                                                          \
            fprintf(stderr, "%s:%d: %s == %s: %#llx != %#llx\n", __FILE__, __LINE__, #a, #b, va_, \
                    vb_);                                                                          \
            rc_test_failures++;                                                                    \
        }                                                                                          \
    } while (0)

void test_mem(void);
void test_cpu(void);

#endif
