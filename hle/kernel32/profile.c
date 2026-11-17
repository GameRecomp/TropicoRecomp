/* SPDX-License-Identifier: MIT */
/*
 * kernel32: initialisation files (GetPrivateProfile*, WritePrivateProfile*),
 * on the virtual file system.
 *
 * A file name without a directory is taken relative to the current
 * directory (the game's installation), where Windows would look in its own
 * directory: these games keep their .ini beside the executable. Reading
 * finds the user directory's copy first, then the game's; writing changes
 * the user directory's copy, made from the game's if there is none.
 *
 * As on Windows: section and key names compare without regard to case,
 * values lose the blanks around them and one pair of enclosing quotes, a
 * missing value gives the default with its trailing blanks removed, and a
 * NULL section (or key) lists the section names (or the section's keys),
 * each ending in a zero and the list in another.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "k32.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#if defined(__EMSCRIPTEN__)
#include "rc/web.h"
#define SAVED() rc_web_saved()
#else
#define SAVED() ((void)0)
#endif

/* The file's text (NUL-terminated, to be freed), or NULL. */
static char *read_ini(const char *guest, char *rel, size_t cap)
{
    char host[RC_PATH_MAX];
    if (!guest || !rc_k32_guest_rel(guest, rel, cap)) {
        return NULL;
    }
    bool is_dir = false;
    if (!rc_k32_find_existing(rel, host, &is_dir, NULL) || is_dir) {
        return NULL;
    }
    uint32_t n = 0;
    uint8_t *b = rc_host_read(host, &n);
    if (!b) {
        return NULL;
    }
    char *t = malloc((size_t)n + 1);
    if (t) {
        memcpy(t, b, n);
        t[n] = 0;
    }
    free(b);
    return t;
}

static int ieq(const char *a, size_t an, const char *b)
{
    size_t bn = strlen(b);
    if (an != bn) {
        return 0;
    }
    for (size_t i = 0; i < an; i++) {
        char x = a[i], y = b[i];
        x = (char)(x >= 'A' && x <= 'Z' ? x + 32 : x);
        y = (char)(y >= 'A' && y <= 'Z' ? y + 32 : y);
        if (x != y) {
            return 0;
        }
    }
    return 1;
}

static int blank(char ch) { return ch == ' ' || ch == '\t' || ch == '\r'; }

/* One line of the file, trimmed: [s, e). */
typedef struct line {
    const char *s, *e;     /* the trimmed line */
    const char *raw, *end; /* the whole line, and past its newline */
} line;

static int next_line(const char **p, line *l)
{
    if (!**p) {
        return 0;
    }
    l->raw = *p;
    const char *nl = strchr(*p, '\n');
    const char *e = nl ? nl : *p + strlen(*p);
    l->end = nl ? nl + 1 : e;
    *p = l->end;
    const char *s = l->raw;
    while (s < e && blank(*s)) {
        s++;
    }
    while (e > s && blank(e[-1])) {
        e--;
    }
    l->s = s;
    l->e = e;
    return 1;
}

/* A section header's name, if the line is one. */
static int section(const line *l, const char **n, size_t *len)
{
    if (l->s >= l->e || *l->s != '[') {
        return 0;
    }
    const char *close = memchr(l->s, ']', (size_t)(l->e - l->s));
    if (!close) {
        return 0;
    }
    const char *a = l->s + 1, *b = close;
    while (a < b && blank(*a)) {
        a++;
    }
    while (b > a && blank(b[-1])) {
        b--;
    }
    *n = a;
    *len = (size_t)(b - a);
    return 1;
}

/* A key's line: its name and value, trimmed. */
static int entry(const line *l, const char **k, size_t *kn, const char **v, size_t *vn)
{
    if (l->s >= l->e || *l->s == ';' || *l->s == '[') {
        return 0;
    }
    const char *eq = memchr(l->s, '=', (size_t)(l->e - l->s));
    const char *ke = eq ? eq : l->e;
    while (ke > l->s && blank(ke[-1])) {
        ke--;
    }
    *k = l->s;
    *kn = (size_t)(ke - l->s);
    if (!eq) {
        *v = l->e;
        *vn = 0;
        return 1;
    }
    const char *vs = eq + 1;
    while (vs < l->e && blank(*vs)) {
        vs++;
    }
    *v = vs;
    *vn = (size_t)(l->e - vs);
    if (*vn >= 2 && ((*v)[0] == '"' || (*v)[0] == '\'') && (*v)[*vn - 1] == (*v)[0]) {
        (*v)++;
        *vn -= 2;
    }
    return 1;
}

/* Copy into the guest's buffer of `size` bytes as GetPrivateProfileString
 * does: truncated, NUL-terminated; returns the characters copied. */
static uint32_t put(uint32_t buf, uint32_t size, const char *s, size_t n)
{
    if (!buf || size == 0) {
        return 0;
    }
    if (n > size - 1) {
        n = size - 1;
    }
    memcpy(rc_ptr(buf), s, n);
    rc_st8(buf + (uint32_t)n, 0);
    return (uint32_t)n;
}

/* A list of names, each NUL-terminated, the list ending in another NUL; as
 * much as fits. Returns the length without the final NUL (size - 2 when it
 * did not all fit). */
typedef struct list {
    uint32_t buf, size, at;
    int full;
} list;

static void list_add(list *l, const char *s, size_t n)
{
    if (l->full || l->size < 2) {
        l->full = 1;
        return;
    }
    if (l->at + n + 2 > l->size) {
        n = l->size - 2 - l->at;
        l->full = 1;
    }
    memcpy(rc_ptr(l->buf + l->at), s, n);
    l->at += (uint32_t)n;
    rc_st8(l->buf + l->at, 0);
    l->at++;
    rc_st8(l->buf + l->at, 0);
}

static uint32_t list_end(list *l)
{
    if (l->size < 2) {
        return 0;
    }
    if (l->at == 0) {
        rc_st8(l->buf, 0);
        rc_st8(l->buf + 1, 0);
    }
    return l->full ? l->size - 2 : l->at;
}

static uint32_t get_string(uint32_t app_p, uint32_t key_p, const char *def, uint32_t buf, uint32_t size,
                           uint32_t file_p)
{
    char rel[RC_PATH_MAX];
    char *t = read_ini(file_p ? rc_str(file_p) : NULL, rel, sizeof rel);
    const char *app = app_p ? rc_str(app_p) : NULL;
    const char *key = key_p ? rc_str(key_p) : NULL;
    if (!app || !key) {
        list l = {buf, size, 0, 0};
        const char *p = t ? t : "";
        line ln;
        int in = 0;
        while (next_line(&p, &ln)) {
            const char *n;
            size_t len;
            if (section(&ln, &n, &len)) {
                in = app && ieq(n, len, app);
                if (!app) {
                    list_add(&l, n, len);
                }
                continue;
            }
            const char *k, *v;
            size_t kn, vn;
            if (app && in && entry(&ln, &k, &kn, &v, &vn)) {
                list_add(&l, k, kn);
            }
        }
        free(t);
        return list_end(&l);
    }
    const char *p = t ? t : "";
    line ln;
    int in = 0;
    while (next_line(&p, &ln)) {
        const char *n;
        size_t len;
        if (section(&ln, &n, &len)) {
            in = ieq(n, len, app);
            continue;
        }
        const char *k, *v;
        size_t kn, vn;
        if (in && entry(&ln, &k, &kn, &v, &vn) && ieq(k, kn, key)) {
            uint32_t r = put(buf, size, v, vn);
            free(t);
            return r;
        }
    }
    free(t);
    const char *d = def ? def : "";
    size_t dn = strlen(d);
    while (dn > 0 && blank(d[dn - 1])) {
        dn--;
    }
    return put(buf, size, d, dn);
}

static void k_GetPrivateProfileStringA(rc_cpu *c)
{
    uint32_t def = RC_ARG(c, 2);
    uint32_t n = get_string(RC_ARG(c, 0), RC_ARG(c, 1), def ? rc_str(def) : NULL, RC_ARG(c, 3), RC_ARG(c, 4),
                            RC_ARG(c, 5));
    RC_RETURN(c, n, 6);
}

static void k_GetPrivateProfileIntA(rc_cpu *c)
{
    uint32_t app = RC_ARG(c, 0), key = RC_ARG(c, 1), def = RC_ARG(c, 2), file = RC_ARG(c, 3);
    char rel[RC_PATH_MAX];
    char *t = read_ini(file ? rc_str(file) : NULL, rel, sizeof rel);
    uint32_t r = def;
    if (t && app && key) {
        const char *p = t;
        line ln;
        int in = 0;
        while (next_line(&p, &ln)) {
            const char *n;
            size_t len;
            if (section(&ln, &n, &len)) {
                in = ieq(n, len, rc_str(app));
                continue;
            }
            const char *k, *v;
            size_t kn, vn;
            if (in && entry(&ln, &k, &kn, &v, &vn) && ieq(k, kn, rc_str(key))) {
                char num[32];
                size_t m = vn < sizeof num - 1 ? vn : sizeof num - 1;
                memcpy(num, v, m);
                num[m] = 0;
                /* Windows reads the leading decimal digits (a sign too); no
                 * digits give 0. */
                r = (uint32_t)strtol(num, NULL, 10);
                break;
            }
        }
    }
    free(t);
    RC_RETURN(c, r, 4);
}

static void k_WritePrivateProfileStringA(rc_cpu *c)
{
    uint32_t app_p = RC_ARG(c, 0), key_p = RC_ARG(c, 1), val_p = RC_ARG(c, 2), file_p = RC_ARG(c, 3);
    if (!app_p || !file_p) {
        RC_RETURN(c, app_p ? 0 : 1, 4); /* NULL section: flush the cache */
    }
    const char *app = rc_str(app_p);
    const char *key = key_p ? rc_str(key_p) : NULL;
    const char *val = val_p ? rc_str(val_p) : NULL;
    char rel[RC_PATH_MAX];
    if (!rc_k32_guest_rel(rc_str(file_p), rel, sizeof rel)) {
        RC_RETURN(c, 0, 4);
    }
    char *t = read_ini(rc_str(file_p), rel, sizeof rel);
    const char *src = t ? t : "";
    size_t cap = strlen(src) + strlen(app) + (key ? strlen(key) : 0) + (val ? strlen(val) : 0) + 16;
    char *out = malloc(cap);
    if (!out) {
        free(t);
        RC_RETURN(c, 0, 4);
    }
    size_t o = 0;
#define EMIT(ptr, len)                                                                                 \
    do {                                                                                               \
        memcpy(out + o, (ptr), (len));                                                                 \
        o += (len);                                                                                    \
    } while (0)
    const char *p = src;
    line ln;
    int in = 0, found_section = 0, done = 0;
    const char *section_end = NULL; /* where to add a new key: after the section's last line */
    size_t insert_at = 0;
    while (next_line(&p, &ln)) {
        const char *n;
        size_t len;
        if (section(&ln, &n, &len)) {
            if (in && !done && key && val) {
                insert_at = o; /* the section ends here */
                section_end = ln.raw;
            }
            in = ieq(n, len, app);
            if (in) {
                found_section = 1;
                if (!key) {
                    continue; /* the whole section goes */
                }
            }
            EMIT(ln.raw, (size_t)(ln.end - ln.raw));
            continue;
        }
        if (in && !key) {
            continue;
        }
        const char *k, *v;
        size_t kn, vn;
        if (in && key && !done && entry(&ln, &k, &kn, &v, &vn) && ieq(k, kn, key)) {
            done = 1;
            if (val) {
                o += (size_t)snprintf(out + o, cap - o, "%s=%s\r\n", key, val);
            }
            continue;
        }
        EMIT(ln.raw, (size_t)(ln.end - ln.raw));
    }
    if (key && val && !done) {
        char add[1024];
        int an = 0;
        if (!found_section) {
            if (o > 0 && out[o - 1] != '\n') {
                an += snprintf(add + an, sizeof add - (size_t)an, "\r\n");
            }
            an += snprintf(add + an, sizeof add - (size_t)an, "[%s]\r\n", app);
            insert_at = o;
        } else if (!section_end) {
            insert_at = o; /* the section is the last one */
            if (o > 0 && out[o - 1] != '\n') {
                an += snprintf(add + an, sizeof add - (size_t)an, "\r\n");
            }
        }
        an += snprintf(add + an, sizeof add - (size_t)an, "%s=%s\r\n", key, val);
        size_t alen = an > 0 && (size_t)an < sizeof add ? (size_t)an : 0;
        char *grown = realloc(out, o + alen + 1);
        if (!grown) {
            free(out);
            free(t);
            RC_RETURN(c, 0, 4);
        }
        out = grown;
        memmove(out + insert_at + alen, out + insert_at, o - insert_at);
        memcpy(out + insert_at, add, alen);
        o += alen;
    }
#undef EMIT
    free(t);
    char target[RC_PATH_MAX];
    int ok = rc_host_resolve(rc_cfg->user_dir, rel, true, target);
    FILE *f = ok ? fopen(target, "wb") : NULL;
    if (f) {
        ok = fwrite(out, 1, o, f) == o;
        ok &= fclose(f) == 0;
        SAVED();
    } else {
        ok = 0;
    }
    free(out);
    if (rc_trace("files")) {
        rc_log("[file] profile [%s] %s=%s in \"%s\": %s", app, key ? key : "(section)", val ? val : "(none)",
               rel, ok ? "written" : "failed");
    }
    RC_RETURN(c, ok ? 1 : 0, 4);
}

const rc_import_def rc_k32_profile[] = {
    {K32, "GetPrivateProfileStringA", k_GetPrivateProfileStringA},
    {K32, "GetPrivateProfileIntA", k_GetPrivateProfileIntA},
    {K32, "WritePrivateProfileStringA", k_WritePrivateProfileStringA},
};
const uint32_t rc_k32_profile_count = sizeof rc_k32_profile / sizeof rc_k32_profile[0];
