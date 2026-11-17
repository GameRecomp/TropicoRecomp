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
#include "rc/native.h"
#include "rc/thread.h"

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
    /* (The last page is the patch variables', rc/dispatch.h.) */
    if (proc_next + size > RC_PATCH_VARS) {
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

/* ---- modules: the executable and the DLLs lifted with it ---- */

#define MODULES 8
static rc_module modules[MODULES];
static uint32_t nmodules;

static int same_name(const char *a, const char *b)
{
    /* Compare file names without case and without a ".dll" ending. */
    const char *sa = strrchr(a, '\\'), *sb = strrchr(b, '\\');
    a = sa ? sa + 1 : a;
    b = sb ? sb + 1 : b;
    size_t la = strlen(a), lb = strlen(b);
    if (la > 4 && !strncmp(a + la - 4, ".dll", 4)) { la -= 4; }
    if (la > 4 && !strncmp(a + la - 4, ".DLL", 4)) { la -= 4; }
    if (lb > 4 && !strncmp(b + lb - 4, ".dll", 4)) { lb -= 4; }
    if (lb > 4 && !strncmp(b + lb - 4, ".DLL", 4)) { lb -= 4; }
    if (la != lb) {
        return 0;
    }
    for (size_t i = 0; i < la; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') { x += 32; }
        if (y >= 'A' && y <= 'Z') { y += 32; }
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

const rc_module *rc_module_find(const char *name)
{
    for (uint32_t i = 0; i < nmodules; i++) {
        if (same_name(modules[i].name, name)) {
            return &modules[i];
        }
    }
    return NULL;
}

const rc_module *rc_module_at(uint32_t base)
{
    for (uint32_t i = 0; i < nmodules; i++) {
        if (modules[i].base == base) {
            return &modules[i];
        }
    }
    return NULL;
}

uint32_t rc_module_export(const rc_module *m, const char *name)
{
    uint32_t base = m->base;
    uint32_t pe = base + rc_ld32(base + 0x3c);
    uint32_t dir = rc_ld32(pe + 24 + 96);
    if (!dir) {
        return 0;
    }
    uint32_t e = base + dir;
    uint32_t ord_base = rc_ld32(e + 16), nfuncs = rc_ld32(e + 20), nnames = rc_ld32(e + 24);
    uint32_t funcs = base + rc_ld32(e + 28), names = base + rc_ld32(e + 32), ords = base + rc_ld32(e + 36);
    if (name[0] == '#') {
        uint32_t i = (uint32_t)strtoul(name + 1, NULL, 10) - ord_base;
        return i < nfuncs ? base + rc_ld32(funcs + 4 * i) : 0;
    }
    for (uint32_t i = 0; i < nnames; i++) {
        if (!strcmp(rc_str(base + rc_ld32(names + 4 * i)), name)) {
            uint32_t idx = rc_ld16(ords + 2 * i);
            return idx < nfuncs ? base + rc_ld32(funcs + 4 * idx) : 0;
        }
    }
    return 0;
}

/* Load the PE image in `file` into guest memory as module `name`. */
static rc_module *load_image(const char *name, const uint8_t *file, uint32_t size)
{
    if (nmodules == MODULES) {
        rc_fatal("too many modules");
    }
    if (size < 0x40 || file[0] != 'M' || file[1] != 'Z') {
        rc_fatal("%s: not an executable", name);
    }
    uint32_t pe = rd32(file, 0x3c);
    if (pe + 0xf8 > size || memcmp(file + pe, "PE\0\0", 4) != 0) {
        rc_fatal("%s: not a PE file", name);
    }
    uint32_t nsec = rd16(file, pe + 6);
    uint32_t optsize = rd16(file, pe + 20);
    uint32_t opt = pe + 24;
    uint32_t base = rd32(file, opt + 28);
    uint32_t image_size = rd32(file, opt + 56);
    uint32_t headers = rd32(file, opt + 60);
    uint32_t imports = rd32(file, opt + 96 + 8);

    if (rc_vm_alloc(base, image_size, true) != base) {
        rc_fatal("cannot place %s at %#010x", name, base);
    }
    memcpy(rc_ptr(base), file, headers < size ? headers : size);
    for (uint32_t i = 0; i < nsec; i++) {
        uint32_t h = opt + optsize + 40 * i;
        uint32_t vsize = rd32(file, h + 8), rva = rd32(file, h + 12);
        uint32_t raw = rd32(file, h + 16), off = rd32(file, h + 20);
        uint32_t n = raw < vsize || vsize == 0 ? raw : vsize;
        if (off + n > size || rva + n > image_size) {
            rc_fatal("%s: section %u out of range", name, i);
        }
        memcpy(rc_ptr(base + rva), file + off, n);
    }

    /* Bind the imports: a lifted module's export, or a reserved address. */
    for (uint32_t d = base + imports; imports; d += 20) {
        uint32_t oft = rc_ld32(d), dname = rc_ld32(d + 12), ft = rc_ld32(d + 16);
        if (!oft && !dname && !ft) {
            break;
        }
        const char *dll = rc_str(base + dname);
        const rc_module *lifted = rc_module_find(dll);
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
            uint32_t addr = lifted ? rc_module_export(lifted, fn) : 0;
            if (lifted && !addr) {
                rc_fatal("%s imports %s!%s, which %s does not export", name, dll, fn, lifted->name);
            }
            rc_st32(base + ft + 4 * j, addr ? addr : rc_import_address(dll, fn));
        }
    }
    rc_module *m = &modules[nmodules++];
    snprintf(m->name, sizeof m->name, "%s", name);
    m->base = base;
    m->size = image_size;
    m->entry = rd32(file, opt + 16) ? base + rd32(file, opt + 16) : 0;
    m->is_dll = (rd16(file, pe + 22) & 0x2000) != 0;
    return m;
}

/* RC_TRACE=exports: calls into the exports of the lifted DLLs, with their
 * first stack arguments. */
static struct {
    uint32_t addr;
    const char *name;
} traced[1024];
static uint32_t ntraced;

static void export_hook(rc_cpu *c, uint32_t target)
{
    for (uint32_t i = 0; i < ntraced; i++) {
        if (traced[i].addr == target) {
            uint32_t sp = c->r[RC_ESP];
            rc_log("[export] %s(%#x, %#x, %#x, %#x) from %#010x", traced[i].name, rc_ld32(sp + 4),
                   rc_ld32(sp + 8), rc_ld32(sp + 12), rc_ld32(sp + 16), rc_ld32(sp));
            return;
        }
    }
}

static void trace_exports(void)
{
    for (uint32_t k = 0; k < nmodules; k++) {
        const rc_module *m = &modules[k];
        if (!m->is_dll) {
            continue;
        }
        uint32_t pe = m->base + rc_ld32(m->base + 0x3c);
        uint32_t dir = rc_ld32(pe + 24 + 96);
        if (!dir) {
            continue;
        }
        uint32_t e = m->base + dir;
        uint32_t nnames = rc_ld32(e + 24), funcs = m->base + rc_ld32(e + 28);
        uint32_t names = m->base + rc_ld32(e + 32), ords = m->base + rc_ld32(e + 36);
        for (uint32_t i = 0; i < nnames && ntraced < 1024; i++) {
            traced[ntraced].name = rc_str(m->base + rc_ld32(names + 4 * i));
            traced[ntraced].addr = m->base + rc_ld32(funcs + 4 * rc_ld16(ords + 2 * i));
            ntraced++;
        }
    }
    rc_set_call_hook(export_hook);
}

static rc_module *load_file(const char *name)
{
    char path[RC_PATH_MAX];
    if (!rc_host_resolve(rc_cfg->game_dir, name, false, path) || !rc_host_exists(path, NULL, NULL)) {
        rc_fatal("%s not found in \"%s\": pass the installed game's directory as the first argument",
                 name, rc_cfg->game_dir);
    }
    uint32_t size = 0;
    uint8_t *file = rc_host_read(path, &size);
    if (!file) {
        rc_fatal("cannot read %s", path);
    }
    rc_module *m = load_image(name, file, size);
    free(file);
    return m;
}

int rc_run(const rc_config *cfg)
{
    rc_cfg = cfg;
    if (!rc_mem_reserve(UINT64_C(1) << 32)) {
        rc_fatal("cannot reserve the guest address space");
    }
    rc_fault_install();
    if (rc_vm_alloc(PROC_DATA, PROC_DATA_SIZE, true) != PROC_DATA) {
        rc_fatal("cannot set up the process data area");
    }

    if (cfg->setup) {
        cfg->setup();
    }
    /* The DLLs lifted with the program first, so that the executable's
     * imports from them bind to their code. */
    const char *const *mods = rc_native_modules();
    for (const char *const *d = mods ? mods : rc_lifted_modules; *d; d++) {
        load_file(*d);
    }
    rc_module *exe = load_file(cfg->exe);
    rc_proc.image_base = exe->base;
    rc_proc.image_size = exe->size;
    rc_proc.entry = exe->entry;

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

    if (cfg->loaded) {
        cfg->loaded();
    }

    if (rc_trace("imports")) {
        rc_set_import_trace(true);
    }
    if (rc_trace("exports")) {
        trace_exports();
    }

    /* The lifted code: a loaded native blob, or the C compiled in. */
    if (!rc_native_loaded()) {
        rc_set_lifted(rc_lifted_table, rc_lifted_count);
    }
    static rc_cpu cpu;
    rc_cpu *c = &cpu;
    memset(c, 0, sizeof *c);
    rc_native_bind(c);
    rc_finit(c);
    c->fpu_control = 0x027f; /* as Windows starts threads: 53-bit precision */
    c->fs_base = TIB_ADDR;
    c->r[RC_ESP] = rc_proc.stack_base - 16;
    rc_thread_init_main(c, TIB_ADDR);
    /* DllMain(instance, DLL_PROCESS_ATTACH, 0) for each lifted DLL. */
    for (uint32_t i = 0; i < nmodules; i++) {
        if (modules[i].is_dll && modules[i].entry) {
            uint32_t args[3] = {modules[i].base, 1, 0};
            if (!rc_call_guest(c, modules[i].entry, 3, args)) {
                rc_fatal("%s did not initialise", modules[i].name);
            }
        }
    }
    rc_push32(c, EXIT_RETURN);
    rc_dispatch(c, rc_proc.entry);
    /* The entry point returned: the process ends with its result. */
    return (int)c->r[RC_EAX];
}
