/* SPDX-License-Identifier: MIT */
/* Starting a recompiled program: the guest address space, the image, the
 * main thread, and the call of the entry point. */
#ifndef RC_PROCESS_H
#define RC_PROCESS_H

#include <stdint.h>

#include "rc/cpu.h"
#include "rc/log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rc_config {
    /* Short identifier: the prefix of environment switches. */
    const char *id;
    /* Host directory of the installed game (read only). */
    const char *game_dir;
    /* Host directory for files the game writes (saves, settings). */
    const char *user_dir;
    /* The executable inside game_dir. */
    const char *exe;
    /* Where the game believes it is installed, e.g. "C:\\Tropico". */
    const char *guest_dir;
    /* Extra command-line arguments for the guest, already joined. */
    const char *args;
    /* Called after the image is loaded, before the entry point: game
     * projects register their HLE modules and patches here. */
    void (*setup)(void);
} rc_config;

extern const rc_config *rc_cfg;

/* The guest command line, module path and similar process data live here. */
typedef struct rc_process {
    uint32_t image_base, image_size, entry;
    uint32_t stack_base, stack_limit;
    uint32_t tib, peb;
    uint32_t cmdline;     /* guest address of the command line */
    uint32_t module_path; /* guest address of the executable's full path */
} rc_process;

extern rc_process rc_proc;

/* Run the program; returns its exit code. */
int rc_run(const rc_config *cfg);

/* Leave the process (ExitProcess). */
RC_NORETURN void rc_exit(uint32_t code);

/* A small guest allocation that lives as long as the process. */
uint32_t rc_proc_alloc(uint32_t size);
uint32_t rc_proc_str(const char *s);

#ifdef __cplusplus
}
#endif

#endif
