/* SPDX-License-Identifier: MIT */
#include "test.h"

#include "rc/dispatch.h"

int rc_test_failures;

/* What the generated code of a program provides: none here. */
const rc_fn_entry rc_lifted_table[1];
const uint32_t rc_lifted_count = 0;
const char *const rc_lifted_modules[] = {NULL};

int main(void)
{
    test_mem();
    test_cpu();
    test_x87();
    test_vm();
    test_thread();
    if (rc_test_failures) {
        fprintf(stderr, "%d check(s) failed\n", rc_test_failures);
        return 1;
    }
    puts("runtime tests passed");
    return 0;
}
