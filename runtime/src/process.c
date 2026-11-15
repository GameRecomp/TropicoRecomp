/* SPDX-License-Identifier: MIT */
/*
 * Loading the executable into the guest address space and running it.
 *
 * The image goes to its preferred base (Tropico's executables are never
 * moved), sections are copied, the BSS stays zero, and every import slot
 * receives the reserved address of its import. The main thread gets a 1 MiB
 * stack and a thread information block that fs addresses, with an empty
 * exception handler chain.
 */
#include "rc/process.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/mem.h"
#include "rc/vm.h"
#include "rc/x87.h"

const rc_config *rc_cfg;
rc_process rc_proc;

#define PROC_DATA 0x00010000u
#define PROC_DATA_SIZE 0x00010000u
#define STACK_LO 0x00110000u
#define STACK_SIZE 0x00100000u
#define TIB_ADDR 0x7ffde000u
#define PEB_ADDR 0x7ffdf000u
/* What the entry point returns to; reaching it ends the process. */
#define EXIT_RETURN 0xeffffff8u

static uint32_t proc_next = PROC_DATA;

uint32_t rc_proc_alloc(uint32_t size)
{
    size = (size + 15) & ~15u;
    if (proc_next + size > PROC_DATA + PROC_DATA_SIZE) {
        rc_fatal("process data area full");
    }
    uint32_t a = proc_next;
    proc_next += size;
    return a;
}

uint32_t rc_proc_str(const char *s)
{
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t a = rc_proc_alloc(n);
    memcpy(rc_ptr(a), s, n);
    return a;
}

void rc_exit(uint32_t code)
{
    fflush(stdout);
    fflush(stderr);
    exit((int)code);
}

static uint32_t rd16(const uint8_t *b, uint32_t at) { return (uint32_t)b[at] | ((uint32_t)b[at + 1] << 8); }

static uint32_t rd32(const uint8_t *b, uint32_t at)
{
    return rd16(b, at) | (rd16(b, at + 2) << 16);
}

/* Load the PE image in `file` into guest memory. */
static void load_image(const uint8_t *file, uint32_t size)
{
    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z') {
        rc_fatal("%s: not an executable", rc_cfg->exe);
    }
    uint32_t pe = rd32(file, 0x3c);
    if (pe + 0xf8 > size || memcmp(file + pe, "PE\0\0", 4) != 0) {
        rc_fatal("%s: not a PE file", rc_cfg->exe);
    }
    uint32_t nsec = rd16(file, pe + 6);
    uint32_t optsize = rd16(file, pe + 20);
    uint32_t opt = pe + 24;
    uint32_t base = rd32(file, opt + 28);
    uint32_t image_size = rd32(file, opt + 56);
    uint32_t headers = rd32(file, opt + 60);
    uint32_t imports = rd32(file, opt + 96 + 8);

    if (rc_vm_alloc(base, image_size, true) != base) {
        rc_fatal("cannot place the image at %#010x", base);
    }
    memcpy(rc_ptr(base), file, headers < size ? headers : size);
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t h = opt + optsize + 40 * i;
        uint32_t vsize = rd32(file, h + 8), rva = rd32(file, h + 12);
        uint32_t raw = rd32(file, h + 16), off = rd32(file, h + 20);
        uint32_t n = raw < vsize || vsize == 0 ? raw : vsize;
        if (off + n > size || rva + n > image_size) {
            rc_fatal("%s: section %u out of range", rc_cfg->exe, i);
        }
        memcpy(rc_ptr(base + rva), file + off, n);
    }

    /* Bind the imports: each slot gets its reserved address. */
    for (uint32_t d = base + imports; imports; d += 20) {
        uint32_t oft = rc_ld32(d), name = rc_ld32(d + 12), ft = rc_ld32(d + 16);
        if (!oft && !name && !ft) {
            break;
        }
        const char *dll = rc_str(base + name);
        uint32_t look = base + (oft ? oft : ft);
        for (uint32_t j = 0;; j++) {
            uint32_t v = rc_ld32(look + 4 * j);
            if (v == 0) {
                break;
            }
            const char *fn;
            if (v & 0x80000000u) {
                char tmp[16];
                snprintf(tmp, sizeof tmp, "#%u", v & 0xffff);
                fn = rc_str(rc_proc_str(tmp));
            } else {
                fn = rc_str(base + v + 2);
            }
            rc_st32(base + ft + 4 * j, rc_import_address(dll, fn));
        }
    }
    rc_proc.image_base = base;
    rc_proc.image_size = image_size;
    rc_proc.entry = base + rd32(file, opt + 16);
}

int rc_run(const rc_config *cfg)
{
    rc_cfg = cfg;
    if (!rc_mem_reserve(UINT64_C(1) << 32)) {
        rc_fatal("cannot reserve the guest address space");
    }
    if (rc_vm_alloc(PROC_DATA, PROC_DATA_SIZE, true) != PROC_DATA) {
        rc_fatal("cannot set up the process data area");
    }

    char path[RC_PATH_MAX];
    if (!rc_host_resolve(cfg->game_dir, cfg->exe, false, path) || !rc_host_exists(path, NULL, NULL)) {
        rc_fatal("%s not found in \"%s\": pass the installed game's directory as the first argument",
                 cfg->exe, cfg->game_dir);
    }
    uint32_t size = 0;
    uint8_t *file = rc_host_read(path, &size);
    if (!file) {
        rc_fatal("cannot read %s", path);
    }
    if (cfg->setup) {
        cfg->setup();
    }
    load_image(file, size);
    free(file);

    /* Process data the API hands out. */
    char buf[1024];
    snprintf(buf, sizeof buf, "%s\\%s", cfg->guest_dir, cfg->exe);
    rc_proc.module_path = rc_proc_str(buf);
    snprintf(buf, sizeof buf, "\"%s\\%s\"%s%s", cfg->guest_dir, cfg->exe,
             cfg->args && *cfg->args ? " " : "", cfg->args ? cfg->args : "");
    rc_proc.cmdline = rc_proc_str(buf);

    /* The main thread: stack, TIB and PEB. */
    if (rc_vm_alloc(STACK_LO, STACK_SIZE, true) != STACK_LO ||
        rc_vm_alloc(TIB_ADDR & ~(RC_VM_GRANULE - 1), RC_VM_GRANULE, true) == 0) {
        rc_fatal("cannot set up the main thread");
    }
    rc_proc.stack_limit = STACK_LO;
    rc_proc.stack_base = STACK_LO + STACK_SIZE;
    rc_proc.tib = TIB_ADDR;
    rc_proc.peb = PEB_ADDR;
    rc_st32(TIB_ADDR + 0x00, 0xffffffffu); /* no exception handler */
    rc_st32(TIB_ADDR + 0x04, rc_proc.stack_base);
    rc_st32(TIB_ADDR + 0x08, rc_proc.stack_limit);
    rc_st32(TIB_ADDR + 0x18, TIB_ADDR); /* self */
    rc_st32(TIB_ADDR + 0x20, 1);        /* process id */
    rc_st32(TIB_ADDR + 0x24, 1);        /* thread id */
    rc_st32(TIB_ADDR + 0x30, PEB_ADDR);
    rc_st32(PEB_ADDR + 0x08, rc_proc.image_base);

    if (rc_trace("imports")) {
        rc_set_import_trace(true);
    }

    rc_set_lifted(rc_lifted_table, rc_lifted_count);
    static rc_cpu cpu;
    rc_cpu *c = &cpu;
    memset(c, 0, sizeof *c);
    rc_finit(c);
    c->fpu_control = 0x027f; /* as Windows starts threads: 53-bit precision */
    c->fs_base = TIB_ADDR;
    c->r[RC_ESP] = rc_proc.stack_base - 16;
    rc_push32(c, EXIT_RETURN);
    rc_dispatch(c, rc_proc.entry);
    /* The entry point returned: the process ends with its result. */
    return (int)c->r[RC_EAX];
}
