/* SPDX-License-Identifier: MIT */
/*
 * Art for the chosen resolution (patch_resolution.c): the game draws its
 * interface from art made for each of its five sizes, and at a size it has
 * none for, slot 4 would use the 1600x1200 set. The art generator of the
 * Tropico resolution patch (third_party/tropico-resolution-patch) rescales
 * that set, read from the user's own archives, to the chosen size; the
 * result goes to data/ in the user directory, where the game finds a loose
 * file before the one in its archives (the virtual file system looks there
 * first, hle/kernel32/file.c).
 *
 * A set is made once per resolution and set of archives: the marker
 * data/ARTSET-MODE.txt names both, and a set that does not match is made
 * again. Without a chosen resolution the files of a set are removed
 * (data/ARTSET-MANIFEST.txt lists them), so the stock art applies again.
 *
 * Unlike the original, which reads every archive whole, entries are read
 * one at a time: a browser cannot hold the archives in memory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "patches.h"

#include "../../third_party/tropico-resolution-patch/artgen.h"
#include "rc/hostfs.h"
#include "rc/log.h"
#include "rc/process.h"

#define MARKER "ARTSET-MODE.txt"
#define MANIFEST "ARTSET-MANIFEST.txt"

/* Reading entries of the archives, one at a time. */
typedef struct reader {
    const ag_index *ix;
    FILE *f[AG_MAX_ARCHIVES];
    const ag_assets *as;
    /* The bytes of assets read so far (for the font masters), by asset. */
    unsigned char **cache;
    size_t *cache_len;
    size_t cached;
} reader;

static unsigned char *read_entry(reader *r, const ag_entry *e, size_t max, size_t *len)
{
    int a = e->archive;
    if (a < 0 || a >= AG_MAX_ARCHIVES || !r->ix->present[a]) {
        return NULL;
    }
    if (!r->f[a] && !(r->f[a] = fopen(r->ix->paths[a], "rb"))) {
        return NULL;
    }
    size_t n = max < e->size ? max : e->size;
    unsigned char *b = malloc(n ? n : 1);
    if (!b || fseek(r->f[a], (long)e->offset, SEEK_SET) != 0 || rc_host_fread(b, n, r->f[a]) != n) {
        free(b);
        return NULL;
    }
    *len = n;
    return b;
}

static unsigned char *harvest_read(size_t ent, size_t max, size_t *len, void *ctx)
{
    reader *r = ctx;
    return read_entry(r, &r->ix->ent[ent], max, len);
}

/* An asset's bytes, kept while the set is made unless they grow large. */
static const unsigned char *asset(size_t j, size_t *len, void *ctx)
{
    reader *r = ctx;
    if (!r->cache[j]) {
        if (r->cached > (64u << 20)) {
            for (size_t k = 0; k < r->as->n; k++) {
                free(r->cache[k]);
                r->cache[k] = NULL;
            }
            r->cached = 0;
        }
        r->cache[j] = read_entry(r, r->as->src[j], r->as->src[j]->size, &r->cache_len[j]);
        if (!r->cache[j]) {
            return NULL;
        }
        r->cached += r->cache_len[j];
    }
    *len = r->cache_len[j];
    return r->cache[j];
}

static bool read_text(const char *path, char *out, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t got = fread(out, 1, n - 1, f);
    fclose(f);
    out[got] = 0;
    while (got && (out[got - 1] == '\n' || out[got - 1] == '\r')) {
        out[--got] = 0;
    }
    return true;
}

/* Remove the files of a set made earlier, and its marker. */
static void remove_set(const char *out_dir)
{
    char path[RC_PATH_MAX + 64], name[256];
    snprintf(path, sizeof path, "%s/" MANIFEST, out_dir);
    FILE *m = fopen(path, "rb");
    if (!m) {
        return;
    }
    while (fgets(name, sizeof name, m)) {
        name[strcspn(name, "\r\n")] = 0;
        if (name[0] && !strchr(name, '/') && !strchr(name, '\\')) {
            char p[RC_PATH_MAX + 300];
            snprintf(p, sizeof p, "%s/%s", out_dir, name);
            remove(p);
        }
    }
    fclose(m);
    remove(path);
    snprintf(path, sizeof path, "%s/" MARKER, out_dir);
    remove(path);
    rc_log("art: the set for a chosen resolution is removed");
}

static int make_set(const char *game_data, const char *exe_path, const char *out_dir, int w, int h)
{
    double font_scale = (double)h / 1080.0;
    ag_index ix;
    if (ag_index_load(&ix, game_data) < 0) {
        rc_log("art: no archives in %s", game_data);
        return -1;
    }
    reader r;
    memset(&r, 0, sizeof r);
    r.ix = &ix;
    uint32_t exe_len = 0;
    uint8_t *exe = rc_host_read(exe_path, &exe_len);
    ag_names names;
    if (!exe || ag_harvest_cb(&ix, exe, exe_len, harvest_read, &r, &names) < 0) {
        rc_log("art: cannot read the names of the art");
        free(exe);
        ag_index_free(&ix);
        return -1;
    }
    free(exe);
    ag_assets as;
    ag_resolve(&ix, &names, 1, &as);
    r.as = &as;
    r.cache = calloc(as.n ? as.n : 1, sizeof *r.cache);
    r.cache_len = calloc(as.n ? as.n : 1, sizeof *r.cache_len);

    char path[RC_PATH_MAX + 300];
    snprintf(path, sizeof path, "%s/" MANIFEST, out_dir);
    FILE *man = fopen(path, "wb");
    size_t ok = 0, failed = 0;
    for (size_t i = 0; r.cache && i < as.n; i++) {
        size_t len = 0, mlen = 0;
        const unsigned char *d = asset(i, &len, &r);
        if (!d) {
            failed++;
            continue;
        }
        int mj = ag_pick_master_cb(&as, i, asset, &r, font_scale);
        const unsigned char *mp = mj >= 0 ? asset((size_t)mj, &mlen, &r) : NULL;
        d = asset(i, &len, &r); /* (the masters may have emptied the cache) */
        if (!d) {
            failed++;
            continue;
        }
        int fw = as.from_i06[i] ? 640 : 1600, fh = as.from_i06[i] ? 480 : 1200;
        size_t olen = 0;
        ag_master_report rep;
        unsigned char *o =
            ag_rescale_container(d, len, w, h, fw, fh, font_scale, 0, mp, mp ? mlen : 0, &rep, &olen);
        if (!o) {
            continue; /* not a sprite container */
        }
        snprintf(path, sizeof path, "%s/%s", out_dir, as.name[i]);
        FILE *f = fopen(path, "wb");
        if (f && fwrite(o, 1, olen, f) == olen) {
            ok++;
            if (man) {
                fprintf(man, "%s\n", as.name[i]);
            }
        } else {
            failed++;
        }
        if (f) {
            fclose(f);
        }
        free(o);
    }
    if (man) {
        fclose(man);
    }
    for (size_t k = 0; r.cache && k < as.n; k++) {
        free(r.cache[k]);
    }
    free(r.cache);
    free(r.cache_len);
    for (int a = 0; a < AG_MAX_ARCHIVES; a++) {
        if (r.f[a]) {
            fclose(r.f[a]);
        }
    }
    ag_assets_free(&as);
    ag_names_free(&names);

    /* The marker last: an interrupted set is made again next time. */
    if (failed || !ok) {
        rc_log("art: %zu written, %zu failed; made again next time", ok, failed);
        ag_index_free(&ix);
        return -1;
    }
    char marker[4096];
    ag_marker_text(ix.dir, w, h, marker, sizeof marker);
    snprintf(path, sizeof path, "%s/" MARKER, out_dir);
    FILE *mk = fopen(path, "wb");
    if (mk) {
        fprintf(mk, "%s\n", marker);
        fclose(mk);
    }
    ag_index_free(&ix);
    return (int)ok;
}

void tropico1_patch_art(int w, int h)
{
    char game_data[RC_PATH_MAX], exe_path[RC_PATH_MAX], out_dir[RC_PATH_MAX];
    if (!rc_host_resolve(rc_cfg->game_dir, "data", false, game_data) ||
        !rc_host_resolve(rc_cfg->game_dir, rc_cfg->exe, false, exe_path) ||
        !rc_host_resolve(rc_cfg->user_dir, "data", true, out_dir)) {
        return;
    }
    if (!w || !h) {
        remove_set(out_dir);
        return;
    }
    rc_host_mkdir(out_dir);
    char have[4096], want[4096], path[RC_PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/" MARKER, out_dir);
    ag_marker_text(game_data, w, h, want, sizeof want);
    if (read_text(path, have, sizeof have) && !strcmp(have, want)) {
        return;
    }
    remove_set(out_dir);
    rc_log("art: making the art for %dx%d from the game's archives", w, h);
    uint64_t t0 = rc_ticks_ms();
    int n = make_set(game_data, exe_path, out_dir, w, h);
    if (n > 0) {
        rc_log("art: %d pieces of art in %llu ms", n, (unsigned long long)(rc_ticks_ms() - t0));
    }
}
