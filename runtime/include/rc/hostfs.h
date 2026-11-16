/* SPDX-License-Identifier: MIT */
/* Host files, looked up the way Windows would: without regard to case. */
#ifndef RC_HOSTFS_H
#define RC_HOSTFS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RC_PATH_MAX 1024

/* Resolve `rel` (components separated by '/' or '\\') under the host
 * directory `root`, matching each component without regard to case.
 * Writes the host path to `out`. With `create_missing`, components that do
 * not exist are kept as given (and directories before the last are
 * created), so the path can be created. Returns false if a component
 * other than the last is missing (or cannot be created). */
bool rc_host_resolve(const char *root, const char *rel, bool create_missing, char *out);

bool rc_host_exists(const char *path, bool *is_dir, uint64_t *size);
bool rc_host_mkdir(const char *path);

/* Directory listing: call `fn` for every entry name except "." and "..". */
typedef void (*rc_host_dir_fn)(void *ctx, const char *name, bool is_dir, uint64_t size);
bool rc_host_list(const char *dir, rc_host_dir_fn fn, void *ctx);

/* Read a whole file; the caller frees the buffer. */
uint8_t *rc_host_read(const char *path, uint32_t *size);

/* fread(buf, 1, n, f). In a browser it first has the page fetch what the
 * read needs from its storage (rc/web.h); every read of game files goes
 * through it. */
size_t rc_host_fread(void *buf, size_t n, FILE *f);

#ifdef __cplusplus
}
#endif

#endif
