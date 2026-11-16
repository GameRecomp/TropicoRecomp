/* SPDX-License-Identifier: MIT */
/*
 * Included before third_party/tropico-resolution-patch/artgen.c (and only
 * there): what it needs of POSIX on every platform we build for, and its
 * reads through rc_host_fread, so that in a browser the archive's blocks
 * are fetched before they are read (rc/hostfs.h).
 */
#ifndef TROPICO1_ARTGEN_SHIM_H
#define TROPICO1_ARTGEN_SHIM_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/hostfs.h"

static inline size_t artgen_fread(void *p, size_t size, size_t n, FILE *f)
{
    return size ? rc_host_fread(p, size * n, f) / size : 0;
}
#define fread artgen_fread

#if defined(_MSC_VER)
#define __attribute__(x)
#define strdup _strdup
/* A directory listing, as much of <dirent.h> as artgen.c uses. */
#include <io.h>
struct dirent {
    char d_name[260];
};
typedef struct {
    intptr_t h;
    struct _finddata_t fd;
    int first;
    struct dirent de;
} DIR;
static inline DIR *opendir(const char *dir)
{
    char pat[1100];
    snprintf(pat, sizeof pat, "%s/*", dir);
    DIR *d = (DIR *)calloc(1, sizeof *d);
    if (!d) {
        return NULL;
    }
    d->h = _findfirst(pat, &d->fd);
    if (d->h == -1) {
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}
static inline struct dirent *readdir(DIR *d)
{
    if (!d->first && _findnext(d->h, &d->fd) != 0) {
        return NULL;
    }
    d->first = 0;
    snprintf(d->de.d_name, sizeof d->de.d_name, "%s", d->fd.name);
    return &d->de;
}
static inline int closedir(DIR *d)
{
    _findclose(d->h);
    free(d);
    return 0;
}
#define TROPICO1_HAVE_DIRENT 1
#endif

#endif
