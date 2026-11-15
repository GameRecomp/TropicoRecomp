/* SPDX-License-Identifier: MIT */
/*
 * Runs test cases as lifted code: the counterpart of ref32.c. The generator
 * writes cases.c with one lifted program per test instance and the table
 * `rc_programs`; this harness reads the same input as the reference runner
 * and writes output in the same format.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "case.h"
#include "rc/dispatch.h"
#include "rc/lifted.h"

typedef struct rc_program {
    rc_fn entry;
    const rc_fn_entry *table;
    uint32_t count;
} rc_program;

extern const rc_program rc_programs[];
extern const uint32_t rc_program_count;

/* Unused here, but the runtime refers to them. */
const rc_fn_entry rc_lifted_table[1];
const uint32_t rc_lifted_count = 0;
const char *const rc_lifted_modules[] = {0};

static rc_case_in in;
static rc_case_out out;

int main(void)
{
    if (!rc_mem_reserve(UINT64_C(1) << 32) || !rc_mem_commit(RC_CASE_CODE, RC_CASE_CODE_MAX) ||
        !rc_mem_commit(RC_CASE_WINDOW, RC_CASE_WINDOW_SIZE)) {
        fprintf(stderr, "harness: cannot set up guest memory\n");
        return 2;
    }
    uint32_t count;
    if (fread(&count, 4, 1, stdin) != 1) {
        return 1;
    }
    for (uint32_t k = 0; k < count; k++) {
        if (fread(&in, sizeof in, 1, stdin) != 1 || in.program >= rc_program_count) {
            return 1;
        }
        const rc_program *p = &rc_programs[in.program];
        memcpy(rc_ptr(RC_CASE_CODE), in.code, RC_CASE_CODE_BYTES);
        memcpy(rc_ptr(RC_CASE_WINDOW), in.window, RC_CASE_WINDOW_SIZE);
        rc_cpu c;
        memset(&c, 0, sizeof c);
        memcpy(c.r, in.regs, 32);
        rc_set_eflags(&c, in.eflags);
        rc_set_lifted(p->table, p->count);
        p->entry(&c);
        memcpy(out.regs, c.r, 32);
        out.eflags = rc_eflags(&c) | 0x200; /* IF, as user mode reads it */
        memcpy(out.window, rc_ptr(RC_CASE_WINDOW), RC_CASE_WINDOW_SIZE);
        if (fwrite(&out, sizeof out, 1, stdout) != 1) {
            return 3;
        }
    }
    return 0;
}
