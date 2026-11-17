/* SPDX-License-Identifier: MIT */
/*
 * VERSION.dll: a file's version resource, read from the file itself (on the
 * virtual file system), and lookups in it.
 *
 * GetFileVersionInfo gives the RT_VERSION resource as it is in the file: a
 * tree of VS_VERSIONINFO nodes { WORD length, WORD value length, WORD type
 * (1 text), WCHAR key[], padding, value, padding, children }. VerQueryValue
 * walks it by a path of keys: "\" is the root's VS_FIXEDFILEINFO,
 * "\VarFileInfo\Translation" a list of language and code page pairs, and
 * "\StringFileInfo\<lang><cp>\<name>" a text, given as the code page's
 * bytes in memory of its own (the resource holds UTF-16).
 */
#include <stdlib.h>
#include <string.h>

#include "k32.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/vm.h"

#define VER "VERSION.dll"
#define RT_VERSION 16u

/* The whole file, by guest path. */
static uint8_t *read_all(const char *guest, uint32_t *n)
{
    char rel[RC_PATH_MAX], host[RC_PATH_MAX];
    if (!guest || !rc_k32_guest_rel(guest, rel, sizeof rel) || !rc_k32_find_existing(rel, host, NULL, NULL)) {
        return NULL;
    }
    return rc_host_read(host, n);
}

static uint32_t u16(const uint8_t *b, uint32_t n, uint32_t at) { return at + 2 <= n ? (uint32_t)(b[at] | b[at + 1] << 8) : 0; }
static uint32_t u32(const uint8_t *b, uint32_t n, uint32_t at)
{
    return at + 4 <= n ? (uint32_t)b[at] | (uint32_t)b[at + 1] << 8 | (uint32_t)b[at + 2] << 16 | (uint32_t)b[at + 3] << 24 : 0;
}

/* The version resource of a PE file: its offset and size in `b`. */
static bool find_version(const uint8_t *b, uint32_t n, uint32_t *off, uint32_t *size)
{
    if (n < 0x40 || b[0] != 'M' || b[1] != 'Z') {
        return false;
    }
    uint32_t pe = u32(b, n, 0x3c);
    if (u32(b, n, pe) != 0x4550) {
        return false;
    }
    uint32_t nsec = u16(b, n, pe + 6), opt = u16(b, n, pe + 20);
    uint32_t rsrc_rva = u32(b, n, pe + 24 + 112); /* data directory 2 (PE32) */
    uint32_t sec = pe + 24 + opt;
    /* An RVA to a file offset. */
#define TO_OFF(rva, out)                                                                                \
    do {                                                                                                \
        (out) = 0;                                                                                      \
        for (uint32_t i_ = 0; i_ < nsec; i_++) {                                                        \
            uint32_t s_ = sec + 40 * i_;                                                                \
            uint32_t va_ = u32(b, n, s_ + 12), vs_ = u32(b, n, s_ + 8), raw_ = u32(b, n, s_ + 20);      \
            uint32_t rs_ = u32(b, n, s_ + 16);                                                          \
            if ((rva) >= va_ && (rva) < va_ + (vs_ > rs_ ? vs_ : rs_)) {                                \
                (out) = (rva) - va_ + raw_;                                                             \
            }                                                                                           \
        }                                                                                               \
    } while (0)
    uint32_t root;
    TO_OFF(rsrc_rva, root);
    if (!rsrc_rva || !root) {
        return false;
    }
    /* Three levels: type, name, language; the first entry of the last two. */
    uint32_t dir = root;
    uint32_t want = RT_VERSION;
    for (int level = 0; level < 3; level++) {
        uint32_t named = u16(b, n, dir + 12), ids = u16(b, n, dir + 14);
        uint32_t e = dir + 16, found = 0;
        bool any = false;
        for (uint32_t i = 0; i < named + ids; i++, e += 8) {
            uint32_t id = u32(b, n, e);
            if (level == 0 && ((id & 0x80000000u) || id != want)) {
                continue;
            }
            found = u32(b, n, e + 4);
            any = true;
            break;
        }
        if (!any) {
            return false;
        }
        if (found & 0x80000000u) {
            dir = root + (found & 0x7fffffffu);
        } else {
            uint32_t data = root + found;
            uint32_t rva = u32(b, n, data), sz = u32(b, n, data + 4), o;
            TO_OFF(rva, o);
            if (!o || o + sz > n) {
                return false;
            }
            *off = o;
            *size = sz;
            return true;
        }
    }
#undef TO_OFF
    return false;
}

static void k_GetFileVersionInfoSizeA(rc_cpu *c)
{
    uint32_t handle = RC_ARG(c, 1);
    uint32_t n = 0, off = 0, size = 0;
    uint8_t *b = read_all(rc_str(RC_ARG(c, 0)), &n);
    bool ok = b && find_version(b, n, &off, &size);
    free(b);
    if (handle) {
        rc_st32(handle, 0);
    }
    if (rc_trace("files")) {
        rc_log("[version] %s: %s", rc_str(RC_ARG(c, 0)), ok ? "has a version" : "none");
    }
    if (!ok) {
        rc_last_error = 1813; /* ERROR_RESOURCE_TYPE_NOT_FOUND */
        RC_RETURN(c, 0, 2);
    }
    RC_RETURN(c, size, 2);
}

static void k_GetFileVersionInfoA(rc_cpu *c)
{
    uint32_t len = RC_ARG(c, 2), out = RC_ARG(c, 3);
    uint32_t n = 0, off = 0, size = 0;
    uint8_t *b = read_all(rc_str(RC_ARG(c, 0)), &n);
    bool ok = b && find_version(b, n, &off, &size);
    if (ok) {
        memcpy(rc_ptr(out), b + off, size < len ? size : len);
    }
    free(b);
    RC_RETURN(c, ok ? 1 : 0, 4);
}

/* A node of the tree at guest address `p`: its key (as ASCII), value and
 * children. */
typedef struct node {
    uint32_t len, vlen, type;
    char key[64];
    uint32_t value, children, end;
} node;

static uint32_t align4(uint32_t a) { return (a + 3) & ~3u; }

static void read_node(uint32_t p, node *nd)
{
    nd->len = rc_ld16(p);
    nd->vlen = rc_ld16(p + 2);
    nd->type = rc_ld16(p + 4);
    uint32_t k = p + 6;
    size_t i = 0;
    for (uint16_t ch; (ch = rc_ld16(k)) != 0; k += 2) {
        if (i + 1 < sizeof nd->key) {
            nd->key[i++] = ch < 0x80 ? (char)ch : '?';
        }
    }
    nd->key[i] = 0;
    nd->value = align4(k + 2);
    uint32_t vbytes = nd->type == 1 ? nd->vlen * 2 : nd->vlen;
    nd->children = align4(nd->value + vbytes);
    nd->end = p + nd->len;
}

static int ieq(const char *a, const char *b, size_t bn)
{
    size_t i = 0;
    for (; i < bn && a[i]; i++) {
        char x = a[i], y = b[i];
        x = (char)(x >= 'A' && x <= 'Z' ? x + 32 : x);
        y = (char)(y >= 'A' && y <= 'Z' ? y + 32 : y);
        if (x != y) {
            return 0;
        }
    }
    return i == bn && a[i] == 0;
}

static void k_VerQueryValueA(rc_cpu *c)
{
    uint32_t block = RC_ARG(c, 0), out = RC_ARG(c, 2), outlen = RC_ARG(c, 3);
    const char *path = rc_str(RC_ARG(c, 1));
    node nd;
    read_node(block, &nd);
    const char *p = path;
    while (*p == '\\') {
        p++;
    }
    while (*p) {
        const char *e = strchr(p, '\\');
        size_t kn = e ? (size_t)(e - p) : strlen(p);
        node child;
        bool found = false;
        for (uint32_t at = nd.children; at + 6 <= nd.end;) {
            read_node(at, &child);
            if (child.len == 0) {
                break;
            }
            if (ieq(child.key, p, kn)) {
                found = true;
                break;
            }
            at = align4(at + child.len);
        }
        if (!found) {
            RC_RETURN(c, 0, 4);
        }
        nd = child;
        p += kn;
        while (*p == '\\') {
            p++;
        }
    }
    if (nd.vlen == 0) {
        RC_RETURN(c, 0, 4);
    }
    uint32_t value = nd.value, len = nd.vlen;
    if (nd.type == 1) {
        /* A text: in the ANSI code page, kept in memory of its own. */
        uint32_t a = rc_heap_alloc(len + 1, true);
        if (!a) {
            RC_RETURN(c, 0, 4);
        }
        for (uint32_t i = 0; i < len; i++) {
            uint16_t ch = rc_ld16(value + 2 * i);
            rc_st8(a + i, ch < 0x100 ? (uint8_t)ch : '?');
        }
        value = a;
    }
    if (out) {
        rc_st32(out, value);
    }
    if (outlen) {
        rc_st32(outlen, len);
    }
    RC_RETURN(c, 1, 4);
}

const rc_import_def rc_version_defs[] = {
    {VER, "GetFileVersionInfoSizeA", k_GetFileVersionInfoSizeA},
    {VER, "GetFileVersionInfoA", k_GetFileVersionInfoA},
    {VER, "VerQueryValueA", k_VerQueryValueA},
};
const uint32_t rc_version_count = sizeof rc_version_defs / sizeof rc_version_defs[0];
