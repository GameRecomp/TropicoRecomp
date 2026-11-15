/* SPDX-License-Identifier: MIT */
/*
 * The reference CPU for differential tests: runs each test case on the real
 * processor in 32-bit mode and reports the state it ends in.
 *
 * A freestanding 32-bit Linux program, no libc: built with
 *   clang -m32 -O1 -ffreestanding -fno-pic -no-pie -nostdlib -static
 * Test code is copied to 0x00401000 and memory to the window at
 * 0x10000000, the same addresses the lifted code uses. The case's ESP
 * points at the return address, which leads back to `rc_capture`.
 *
 * Input on stdin, output on stdout, both little-endian, as described in
 * case.h. A development tool only: nothing here ships.
 */
#include "case.h"

typedef unsigned int u32;
typedef unsigned char u8;

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static int sys6(int n, int a, int b, int c, int d, int e, int f)
{
    int r;
    /* ebp is the sixth argument; save it around the call. */
    int args[2] = {f, 0};
    __asm__ volatile("push %%ebp\n\tmov (%7), %%ebp\n\tint $0x80\n\tpop %%ebp"
                     : "=a"(r)
                     : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "r"(args)
                     : "memory");
    return r;
}

static void exit_(int code)
{
    sys3(1, code, 0, 0);
    for (;;) {
    }
}

static int read_all(void *p, u32 n)
{
    u8 *b = p;
    while (n) {
        int r = sys3(3, 0, (int)b, (int)n);
        if (r <= 0) {
            return 0;
        }
        b += r;
        n -= (u32)r;
    }
    return 1;
}

static void write_all(const void *p, u32 n)
{
    const u8 *b = p;
    while (n) {
        int r = sys3(4, 1, (int)b, (int)n);
        if (r <= 0) {
            exit_(3);
        }
        b += r;
        n -= (u32)r;
    }
}

static void *map_fixed(u32 addr, u32 len)
{
    /* mmap2(addr, len, rwx, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) */
    return (void *)sys6(192, (int)addr, (int)len, 7, 0x32, -1, 0);
}

static void copy(void *d, const void *s, u32 n)
{
    u8 *dd = d;
    const u8 *ss = s;
    while (n--) {
        *dd++ = *ss++;
    }
}

/* In: eax ecx edx ebx esp ebp esi edi eflags. Out: the same. */
u32 g_in[9];
u32 g_out[9];
u32 g_saved_esp;
u32 g_code = RC_CASE_CODE;

void run_case(void);
void rc_capture(void);
__asm__(".text\n"
        ".globl run_case\n"
        "run_case:\n"
        "  pushal\n"
        "  movl %esp, g_saved_esp\n"
        "  pushl g_in+32\n"
        "  popfl\n"
        "  movl g_in+0, %eax\n"
        "  movl g_in+4, %ecx\n"
        "  movl g_in+8, %edx\n"
        "  movl g_in+12, %ebx\n"
        "  movl g_in+20, %ebp\n"
        "  movl g_in+24, %esi\n"
        "  movl g_in+28, %edi\n"
        "  movl g_in+16, %esp\n"
        "  jmp *g_code\n"
        ".globl rc_capture\n"
        "rc_capture:\n"
        "  movl %eax, g_out+0\n"
        "  movl %ecx, g_out+4\n"
        "  movl %edx, g_out+8\n"
        "  movl %ebx, g_out+12\n"
        "  movl %esp, g_out+16\n"
        "  movl %ebp, g_out+20\n"
        "  movl %esi, g_out+24\n"
        "  movl %edi, g_out+28\n"
        "  pushfl\n"
        "  popl g_out+32\n"
        "  cld\n"
        "  movl g_saved_esp, %esp\n"
        "  popal\n"
        "  ret\n");

static rc_case_in in;
static rc_case_out out;

void _start(void)
{
    u8 *code = map_fixed(RC_CASE_CODE, RC_CASE_CODE_MAX);
    u8 *win = map_fixed(RC_CASE_WINDOW, RC_CASE_WINDOW_SIZE);
    if ((u32)code != RC_CASE_CODE || (u32)win != RC_CASE_WINDOW) {
        exit_(2);
    }
    u32 count;
    if (!read_all(&count, 4)) {
        exit_(1);
    }
    for (u32 k = 0; k < count; k++) {
        if (!read_all(&in, sizeof in)) {
            exit_(1);
        }
        copy(code, in.code, RC_CASE_CODE_BYTES);
        copy(win, in.window, RC_CASE_WINDOW_SIZE);
        /* The return address leads back here. */
        u32 sp = in.regs[4] - RC_CASE_WINDOW;
        u32 back = (u32)&rc_capture;
        copy(win + sp, &back, 4);
        copy(g_in, in.regs, 32);
        g_in[8] = in.eflags;
        run_case();
        copy(out.regs, g_out, 32);
        out.eflags = g_out[8];
        copy(out.window, win, RC_CASE_WINDOW_SIZE);
        write_all(&out, sizeof out);
    }
    exit_(0);
}
