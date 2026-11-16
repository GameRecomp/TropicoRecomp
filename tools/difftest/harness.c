/* SPDX-License-Identifier: MIT */
/*
 * Runs test cases as lifted code: the counterpart of ref32.c. The generator
 * writes cases.c with one lifted program per test instance and the table
 * `rc_programs`; this harness reads the same input as the reference runner
 * and writes output in the same format.
 *
 * Built with RC_NATIVE_HARNESS, it runs the native back end's code instead:
 * the programs come from a bundle of blobs named on the command line
 * (u32 count, then per program u32 size and the blob). Built with
 * Emscripten and RC_NATIVE_HARNESS (run by Node), the bundle holds
 * WebAssembly modules from rc-emit-wasm, instantiated here against the
 * harness's memory and function table as the web shell does.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "case.h"
#include "rc/dispatch.h"
#include "rc/lifted.h"
#include "rc/native.h"

typedef struct rc_program {
    rc_fn entry;
    const rc_fn_entry *table;
    uint32_t count;
} rc_program;

#if defined(RC_NATIVE_HARNESS) && defined(__EMSCRIPTEN__)
#include <emscripten.h>

/* Instantiate the module `bytes`; returns its first table slot and sets
 * *tables to a copy of its rc.code section (*tables_len bytes). */
EM_JS(uint32_t, instantiate, (const uint8_t *bytes, uint32_t len, const uint32_t *helpers, uint8_t **tables,
                              uint32_t *tables_len), {
    const table = wasmExports['__indirect_function_table'];
    const m = new WebAssembly.Module(HEAPU8.slice(bytes, bytes + len));
    const sec = new Uint8Array(WebAssembly.Module.customSections(m, 'rc.code')[0]);
    const n = new DataView(sec.buffer).getUint32(24, true);
    const base = table.grow(n);
    const h = (i) => table.get(HEAPU32[(helpers >> 2) + i]);
    new WebAssembly.Instance(m, {
        rc: {dispatch: h(0), trap: h(1), cpuid: h(2), rdtsc: h(3), fpu: h(4), fstsw: h(5), point: h(6)},
        env: {memory: wasmMemory, table: table,
              table_base: new WebAssembly.Global({value: 'i32', mutable: false}, base)},
    });
    const p = _malloc(sec.length);
    HEAPU8.set(sec, p);
    HEAPU32[tables >> 2] = p;
    HEAPU32[tables_len >> 2] = sec.length;
    return base;
});
#endif

#if defined(RC_NATIVE_HARNESS)
static rc_program *rc_programs;
static uint32_t rc_program_count;

/* Map every blob of the bundle at `path`. */
static int load_bundle(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f || fread(&rc_program_count, 4, 1, f) != 1) {
        fprintf(stderr, "harness: cannot read %s\n", path);
        return 0;
    }
    rc_programs = calloc(rc_program_count, sizeof *rc_programs);
    for (uint32_t k = 0; k < rc_program_count; k++) {
        uint32_t n;
        if (fread(&n, 4, 1, f) != 1) {
            return 0;
        }
        uint8_t *b = malloc(n);
        if (!b || fread(b, 1, n, f) != n) {
            return 0;
        }
        rc_native_image img;
        char err[200];
#if defined(__EMSCRIPTEN__)
        uint8_t *tables;
        uint32_t tables_len;
        uint32_t base = instantiate(b, n, rc_wasm_helpers(), &tables, &tables_len);
        if (!rc_wasm_open(tables, tables_len, base, &img, err, sizeof err)) {
#else
        if (!rc_native_open(b, n, &img, err, sizeof err)) {
#endif
            fprintf(stderr, "harness: program %u: %s\n", k, err);
            return 0;
        }
        free(b);
        rc_programs[k].table = img.table;
        rc_programs[k].count = img.count;
        for (uint32_t i = 0; i < img.count; i++) {
            if (img.table[i].addr == RC_CASE_CODE) {
                rc_programs[k].entry = img.table[i].fn;
            }
        }
        if (!rc_programs[k].entry) {
            fprintf(stderr, "harness: program %u has no entry\n", k);
            return 0;
        }
    }
    fclose(f);
    return 1;
}
#else
extern const rc_program rc_programs[];
extern const uint32_t rc_program_count;
#endif

/* Unused here, but the runtime refers to them. */
const rc_fn_entry rc_lifted_table[1];
const uint32_t rc_lifted_count = 0;
const char *const rc_lifted_modules[] = {0};

static rc_case_in in;
static rc_case_out out;

int main(int argc, char **argv)
{
#if defined(RC_NATIVE_HARNESS)
    if (argc < 2 || !load_bundle(argv[1])) {
        return 2;
    }
#else
    (void)argc;
    (void)argv;
#endif
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
        rc_native_bind(&c);
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
