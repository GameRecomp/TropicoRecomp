/* SPDX-License-Identifier: MIT */
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif
#include "rc/hostfs.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#include <io.h>
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <dirent.h>
#endif
#if defined(__EMSCRIPTEN__)
#include "rc/web.h"
#endif

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

bool rc_host_exists(const char *path, bool *is_dir, uint64_t *size)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return false;
    }
    if (is_dir) {
        *is_dir = S_ISDIR(st.st_mode);
    }
    if (size) {
        *size = (uint64_t)st.st_size;
    }
    return true;
}

bool rc_host_mkdir(const char *path)
{
#if defined(_WIN32)
    return _mkdir(path) == 0;
#else
    return mkdir(path, 0777) == 0;
#endif
}

bool rc_host_list(const char *dir, rc_host_dir_fn fn, void *ctx)
{
    char full[RC_PATH_MAX];
#if defined(_WIN32)
    struct _finddata_t fd;
    snprintf(full, sizeof full, "%s/*", dir);
    intptr_t h = _findfirst(full, &fd);
    if (h == -1) {
        return false;
    }
    do {
        if (strcmp(fd.name, ".") && strcmp(fd.name, "..")) {
            fn(ctx, fd.name, (fd.attrib & _A_SUBDIR) != 0, (uint64_t)fd.size);
        }
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
            continue;
        }
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        bool is_dir = false;
        uint64_t size = 0;
        rc_host_exists(full, &is_dir, &size);
        fn(ctx, e->d_name, is_dir, size);
    }
    closedir(d);
#endif
    return true;
}

typedef struct find_ctx {
    const char *want;
    char found[256];
} find_ctx;

static void match(void *p, const char *name, bool is_dir, uint64_t size)
{
    (void)is_dir;
    (void)size;
    find_ctx *f = p;
    if (!f->found[0] && ieq(name, f->want)) {
        snprintf(f->found, sizeof f->found, "%s", name);
    }
}

bool rc_host_resolve(const char *root, const char *rel, bool create_missing, char *out)
{
    snprintf(out, RC_PATH_MAX, "%s", root);
    const char *p = rel;
    while (*p) {
        while (*p == '/' || *p == '\\') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *e = p;
        while (*e && *e != '/' && *e != '\\') {
            e++;
        }
        char comp[256];
        size_t n = (size_t)(e - p) < sizeof comp - 1 ? (size_t)(e - p) : sizeof comp - 1;
        memcpy(comp, p, n);
        comp[n] = 0;
        bool last = true;
        for (const char *q = e; *q; q++) {
            if (*q != '/' && *q != '\\') {
                last = false;
                break;
            }
        }
        size_t len = strlen(out);
        char cand[RC_PATH_MAX];
        snprintf(cand, sizeof cand, "%s/%s", out, comp);
        if (!rc_host_exists(cand, NULL, NULL)) {
            find_ctx f = {comp, {0}};
            rc_host_list(out, match, &f);
            if (f.found[0]) {
                snprintf(cand, sizeof cand, "%s/%s", out, f.found);
            } else if (!create_missing && !last) {
                return false;
            } else if (create_missing && !last && !rc_host_mkdir(cand)) {
                return false;
            }
        }
        if (len + 1 + strlen(cand + len) >= RC_PATH_MAX) {
            return false;
        }
        snprintf(out, RC_PATH_MAX, "%s", cand);
        p = e;
    }
    return true;
}

uint8_t *rc_host_read(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = malloc((size_t)n + 1);
    if (buf && rc_host_fread(buf, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        *size = (uint32_t)n;
    }
    return buf;
}

size_t rc_host_fread(void *buf, size_t n, FILE *f)
{
    if (n == 0) {
        return 0;
    }
#if defined(__EMSCRIPTEN__)
    /* stdio reads ahead by up to its buffer's size beyond the request. */
    long pos = ftell(f);
    if (pos >= 0) {
        rc_web_prefetch(fileno(f), (uint32_t)pos, (uint32_t)(n + BUFSIZ));
    }
#endif
    return fread(buf, 1, n, f);
}
