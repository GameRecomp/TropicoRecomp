/* SPDX-License-Identifier: MIT */
/*
 * The test-case format shared by the reference runner, the lifted-code
 * harness and the generator (recomp/crates/rc-difftest, which must match).
 *
 * stdin:  u32 count, then `count` rc_case_in
 * stdout: `count` rc_case_out
 */
#ifndef RC_CASE_H
#define RC_CASE_H

#define RC_CASE_CODE 0x00401000u
/* Mapped for code; each case brings this much of it. */
#define RC_CASE_CODE_MAX 0x1000u
#define RC_CASE_CODE_BYTES 512u
#define RC_CASE_WINDOW 0x10000000u
#define RC_CASE_WINDOW_SIZE 0x1000u

typedef struct rc_case_in {
    /* Which lifted program to run (the harness's index; the reference
     * runner ignores it). */
    unsigned int program;
    unsigned char code[RC_CASE_CODE_BYTES];
    /* eax ecx edx ebx esp ebp esi edi; esp points at the return slot. */
    unsigned int regs[8];
    unsigned int eflags;
    unsigned char window[RC_CASE_WINDOW_SIZE];
} rc_case_in;

typedef struct rc_case_out {
    unsigned int regs[8];
    unsigned int eflags;
    unsigned char window[RC_CASE_WINDOW_SIZE];
} rc_case_out;

#endif
