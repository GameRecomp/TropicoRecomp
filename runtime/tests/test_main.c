/* SPDX-License-Identifier: MIT */
#include "test.h"

int rc_test_failures;

int main(void)
{
    test_mem();
    test_cpu();
    if (rc_test_failures) {
        fprintf(stderr, "%d check(s) failed\n", rc_test_failures);
        return 1;
    }
    puts("runtime tests passed");
    return 0;
}
