/* SPDX-License-Identifier: MIT */
/*
 * Direct3D 7 (IDirect3D7, IDirect3DDevice7, IDirect3DVertexBuffer7) on
 * DirectDraw's surfaces: a device draws into its render target, a surface
 * in guest memory, with a software rasteriser of the fixed-function
 * pipeline; textures are DirectDraw surfaces too.
 *
 * The device offers itself as the one hardware device ("Direct3D HAL").
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw.h"
#include "rc/hle.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"

#define COM "DDRAW_COM"
#define D3D_OK 0u
#define DDERR_INVALIDPARAMS 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define D3DENUMRET_OK 1u

#define M(i, n, f) {COM, i "::" n, f}

/* ---- device state ---- */

#define DEVICES 8
#define STATES 256
#define STAGES 8
#define TSS 32

typedef struct device {
    uint32_t object;   /* in DirectDraw's table */
    uint32_t target;   /* the render target's surface object */
    uint32_t zbuffer;  /* an attached z-buffer's surface object, or 0 */
    uint32_t rs[STATES];
    uint32_t tss[STAGES][TSS];
    uint32_t texture[STAGES]; /* surface objects */
    float world[16], view[16], proj[16];
    struct {
        uint32_t x, y, w, h;
        float minz, maxz;
    } vp;
    int in_scene;
} device;

static device devices[DEVICES];
static uint32_t ndevices;

static device *dev_of(rc_cpu *c)
{
    uint32_t o = rc_dd_self(c, RC_DD_DEVICE);
    for (uint32_t i = 0; i < ndevices; i++) {
        if (devices[i].object == o) {
            return &devices[i];
        }
    }
    rc_fatal("Direct3D: no device for object %u", o);
}

/* A device released for the last time gives its slot back. */
static void k_DevRelease(rc_cpu *c)
{
    rc_d3d_flush();
    uint32_t o = rc_ld32(RC_ARG(c, 0) + 4);
    uint32_t *r = rc_dd_refs(o);
    if (*r > 0 && --*r == 0) {
        for (uint32_t i = 0; i < ndevices; i++) {
            if (devices[i].object == o) {
                devices[i].object = 0;
            }
        }
    }
    RC_RETURN(c, *r, 1);
}

static void identity(float *m)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static float ldf(uint32_t a)
{
    uint32_t v = rc_ld32(a);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static void stf(uint32_t a, float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof v);
    rc_st32(a, v);
}

/* ---- IDirect3D7 ---- */

uint32_t rc_d3d_create(void)
{
    uint32_t o = rc_dd_new(RC_DD_D3D);
    return rc_dd_iface(o, 7);
}

static void k_D3DAddRef(rc_cpu *c)
{
    uint32_t o = rc_ld32(RC_ARG(c, 0) + 4);
    RC_RETURN(c, ++*rc_dd_refs(o), 1);
}

static void k_D3DRelease(rc_cpu *c)
{
    uint32_t o = rc_ld32(RC_ARG(c, 0) + 4);
    uint32_t *r = rc_dd_refs(o);
    if (*r > 0) {
        (*r)--;
    }
    RC_RETURN(c, *r, 1);
}

/* The hardware device's GUID (IID_IDirect3DHALDevice). */
static const uint8_t hal_guid[16] = {0xe0, 0x3d, 0xe6, 0x84, 0xaa, 0x46, 0xcf, 0x11,
                                     0x81, 0x6f, 0x00, 0x00, 0xc0, 0x20, 0x15, 0x6e};

/* D3DPRIMCAPS: what the rasteriser does. */
static void put_primcaps(uint32_t p)
{
    rc_st32(p, 56);
    rc_st32(p + 4, 0x2 | 0x10 | 0x20 | 0x40);      /* misc: mask Z, cull none, CW, CCW */
    rc_st32(p + 8, 0x1 | 0x10 | 0x80 | 0x10000);   /* raster: dither, Z test, fog vertex, Z bias */
    rc_st32(p + 12, 0xff);                         /* Z compare: all */
    rc_st32(p + 16, 0x1fff);                       /* source blend: all */
    rc_st32(p + 20, 0x1fff);                       /* destination blend: all */
    rc_st32(p + 24, 0xff);                         /* alpha compare: all */
    rc_st32(p + 28, 0x1 | 0x2 | 0x8 | 0x200 | 0x4000); /* shade: colour flat, Gouraud, ..., alpha */
    rc_st32(p + 32, 0x1 | 0x2 | 0x4 | 0x8 | 0x40); /* texture: perspective, pow2, alpha, transparency, square only */
    rc_st32(p + 36, 0x1 | 0x2 | 0x100 | 0x200 | 0x10000 | 0x20000); /* filter: nearest, linear, min/mag point/linear */
    rc_st32(p + 40, 0xff);                         /* texture blend */
    rc_st32(p + 44, 0x1 | 0x2 | 0x4 | 0x8);        /* address: wrap, mirror, clamp, border */
    rc_st32(p + 48, 0);
    rc_st32(p + 52, 0);
}

/* D3DDEVICEDESC7. */
static void put_devdesc(uint32_t d)
{
    memset(rc_ptr(d), 0, 236);
    rc_st32(d, 0x1 | 0x10 | 0x40 | 0x100 | 0x200 | 0x400 | 0x800 | 0x2000 | 0x80000);
    put_primcaps(d + 4);
    put_primcaps(d + 60);
    rc_st32(d + 116, 0x400 | 0x800 | 0x100); /* render depths: 16, 32, 24 bits */
    rc_st32(d + 120, 0x400);                 /* z-buffer: 16 bits */
    rc_st32(d + 124, 1);
    rc_st32(d + 128, 1);
    rc_st32(d + 132, 2048);
    rc_st32(d + 136, 2048);
    rc_st32(d + 140, 2048);
    rc_st32(d + 144, 2048);
    rc_st32(d + 148, 1);
    stf(d + 152, -2048.0f);
    stf(d + 156, -2048.0f);
    stf(d + 160, 2048.0f);
    stf(d + 164, 2048.0f);
    rc_st32(d + 176, 8);          /* FVF: 8 texture coordinate sets */
    rc_st32(d + 180, 0x3ffffff);  /* texture ops: all */
    rc_st16(d + 184, 8);
    rc_st16(d + 186, 8);
    rc_st32(d + 188, 8);
    stf(d + 192, 1e10f);
    memcpy(rc_ptr(d + 196), hal_guid, 16);
}

static void k_EnumDevices(rc_cpu *c)
{
    uint32_t cb = RC_ARG(c, 1), ctx = RC_ARG(c, 2);
    uint32_t desc = rc_heap_alloc(236, true);
    put_devdesc(desc);
    uint32_t args[4] = {rc_proc_str("Direct3D HAL"), rc_proc_str("Direct3D HAL"), desc, ctx};
    rc_call_guest(c, cb, 4, args);
    RC_RETURN(c, D3D_OK, 3);
}

static void k_EnumZBufferFormats(rc_cpu *c)
{
    uint32_t cb = RC_ARG(c, 2), ctx = RC_ARG(c, 3);
    uint32_t pf = rc_heap_alloc(32, true);
    rc_st32(pf, 32);
    rc_st32(pf + 4, 0x400); /* DDPF_ZBUFFER */
    rc_st32(pf + 12, 16);
    rc_st32(pf + 20, 0xffff);
    uint32_t args[2] = {pf, ctx};
    rc_call_guest(c, cb, 2, args);
    RC_RETURN(c, D3D_OK, 4);
}

static void k_EvictManagedTextures(rc_cpu *c) { RC_RETURN(c, D3D_OK, 1); }

static void k_CreateDevice(rc_cpu *c)
{
    uint32_t surf = rc_ld32(RC_ARG(c, 2) + 4), out = RC_ARG(c, 3);
    device *d = NULL;
    for (uint32_t i = 0; i < ndevices && !d; i++) {
        if (!devices[i].object) {
            d = &devices[i]; /* released */
        }
    }
    if (!d && ndevices < DEVICES) {
        d = &devices[ndevices++];
    }
    if (!d || !rc_dd_surf(surf)) {
        rc_st32(out, 0);
        RC_RETURN(c, DDERR_INVALIDPARAMS, 4);
    }
    memset(d, 0, sizeof *d);
    d->object = rc_dd_new(RC_DD_DEVICE);
    d->target = surf;
    identity(d->world);
    identity(d->view);
    identity(d->proj);
    rc_dd_surface *s = rc_dd_surf(surf);
    d->vp.w = s->w;
    d->vp.h = s->h;
    d->vp.maxz = 1.0f;
    /* The defaults of D3DRENDERSTATETYPE that matter. */
    d->rs[7] = 1;   /* ZENABLE */
    d->rs[8] = 3;   /* FILLMODE solid */
    d->rs[9] = 2;   /* SHADEMODE Gouraud */
    d->rs[14] = 1;  /* ZWRITEENABLE */
    d->rs[19] = 2;  /* SRCBLEND one */
    d->rs[20] = 1;  /* DESTBLEND zero */
    d->rs[22] = 3;  /* CULLMODE CCW */
    d->rs[23] = 4;  /* ZFUNC less-equal */
    d->rs[25] = 8;  /* ALPHAFUNC always */
    d->rs[137] = 1; /* LIGHTING */
    for (int i = 0; i < STAGES; i++) {
        d->tss[i][1] = i == 0 ? 4 : 1; /* COLOROP modulate / disable */
        d->tss[i][2] = 2;              /* COLORARG1 texture */
        d->tss[i][3] = 1;              /* COLORARG2 current */
        d->tss[i][4] = i == 0 ? 2 : 1; /* ALPHAOP select arg 1 / disable */
        d->tss[i][5] = 2;
        d->tss[i][6] = 1;
        d->tss[i][11] = (uint32_t)i;   /* TEXCOORDINDEX */
        d->tss[i][16] = 1;             /* MAGFILTER point */
        d->tss[i][17] = 1;             /* MINFILTER point */
    }
    if (rc_trace("d3d")) {
        rc_log("[d3d] device on a %ux%u surface", s->w, s->h);
    }
    rc_st32(out, rc_dd_iface(d->object, 7));
    RC_RETURN(c, D3D_OK, 4);
}

/* ---- IDirect3DDevice7: state ---- */

static void k_DevGetCaps(rc_cpu *c)
{
    put_devdesc(RC_ARG(c, 1));
    RC_RETURN(c, D3D_OK, 2);
}

/* Texture formats: RGB 565, ARGB 1555, ARGB 4444, ARGB 8888. */
static void k_EnumTextureFormats(rc_cpu *c)
{
    static const uint32_t fmts[4][5] = {
        {16, 0xf800, 0x07e0, 0x001f, 0},
        {16, 0x7c00, 0x03e0, 0x001f, 0x8000},
        {16, 0x0f00, 0x00f0, 0x000f, 0xf000},
        {32, 0xff0000, 0x00ff00, 0x0000ff, 0xff000000u},
    };
    dev_of(c);
    uint32_t cb = RC_ARG(c, 1), ctx = RC_ARG(c, 2);
    uint32_t pf = rc_heap_alloc(32, true);
    for (int i = 0; i < 4; i++) {
        memset(rc_ptr(pf), 0, 32);
        rc_st32(pf, 32);
        rc_st32(pf + 4, 0x40 | (fmts[i][4] ? 0x1 : 0)); /* DDPF_RGB, DDPF_ALPHAPIXELS */
        rc_st32(pf + 12, fmts[i][0]);
        rc_st32(pf + 16, fmts[i][1]);
        rc_st32(pf + 20, fmts[i][2]);
        rc_st32(pf + 24, fmts[i][3]);
        rc_st32(pf + 28, fmts[i][4]);
        uint32_t args[2] = {pf, ctx};
        if (rc_call_guest(c, cb, 2, args) == 0) {
            break;
        }
    }
    RC_RETURN(c, D3D_OK, 3);
}

static void k_BeginScene(rc_cpu *c)
{
    dev_of(c)->in_scene = 1;
    RC_RETURN(c, D3D_OK, 1);
}

static void k_EndScene(rc_cpu *c)
{
    dev_of(c)->in_scene = 0;
    RC_RETURN(c, D3D_OK, 1);
}

static void k_SetRenderTarget(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t s = rc_ld32(RC_ARG(c, 1) + 4);
    if (rc_dd_surf(s)) {
        d->target = s;
    }
    RC_RETURN(c, D3D_OK, 3);
}

static void k_GetRenderTarget(rc_cpu *c)
{
    device *d = dev_of(c);
    rc_st32(RC_ARG(c, 1), rc_dd_iface(d->target, 7));
    RC_RETURN(c, D3D_OK, 2);
}

static float *matrix(device *d, uint32_t which)
{
    switch (which) {
    case 1: return d->world; /* D3DTRANSFORMSTATE_WORLD */
    case 2: return d->view;
    case 3: return d->proj;
    default: return NULL;
    }
}

static void k_SetTransform(rc_cpu *c)
{
    device *d = dev_of(c);
    float *m = matrix(d, RC_ARG(c, 1));
    if (m) {
        for (int i = 0; i < 16; i++) {
            m[i] = ldf(RC_ARG(c, 2) + 4u * (uint32_t)i);
        }
    }
    RC_RETURN(c, D3D_OK, 3);
}

static void k_GetTransform(rc_cpu *c)
{
    device *d = dev_of(c);
    float *m = matrix(d, RC_ARG(c, 1));
    for (int i = 0; i < 16; i++) {
        stf(RC_ARG(c, 2) + 4u * (uint32_t)i, m ? m[i] : (i % 5 == 0 ? 1.0f : 0.0f));
    }
    RC_RETURN(c, D3D_OK, 3);
}

static void k_SetViewport(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t v = RC_ARG(c, 1);
    d->vp.x = rc_ld32(v);
    d->vp.y = rc_ld32(v + 4);
    d->vp.w = rc_ld32(v + 8);
    d->vp.h = rc_ld32(v + 12);
    d->vp.minz = ldf(v + 16);
    d->vp.maxz = ldf(v + 20);
    RC_RETURN(c, D3D_OK, 2);
}

static void k_GetViewport(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t v = RC_ARG(c, 1);
    rc_st32(v, d->vp.x);
    rc_st32(v + 4, d->vp.y);
    rc_st32(v + 8, d->vp.w);
    rc_st32(v + 12, d->vp.h);
    stf(v + 16, d->vp.minz);
    stf(v + 20, d->vp.maxz);
    RC_RETURN(c, D3D_OK, 2);
}

static void k_SetRenderState(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t s = RC_ARG(c, 1);
    if (s < STATES) {
        d->rs[s] = RC_ARG(c, 2);
    }
    RC_RETURN(c, D3D_OK, 3);
}

static void k_GetRenderState(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t s = RC_ARG(c, 1);
    rc_st32(RC_ARG(c, 2), s < STATES ? d->rs[s] : 0);
    RC_RETURN(c, D3D_OK, 3);
}

static void k_SetTexture(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t stage = RC_ARG(c, 1), g = RC_ARG(c, 2);
    if (stage < STAGES) {
        d->texture[stage] = g ? rc_ld32(g + 4) : 0;
    }
    RC_RETURN(c, D3D_OK, 3);
}

static void k_GetTexture(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t stage = RC_ARG(c, 1);
    uint32_t t = stage < STAGES ? d->texture[stage] : 0;
    rc_st32(RC_ARG(c, 2), t ? rc_dd_iface(t, 7) : 0);
    RC_RETURN(c, D3D_OK, 3);
}

static void k_SetTextureStageState(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t stage = RC_ARG(c, 1), s = RC_ARG(c, 2);
    if (stage < STAGES && s < TSS) {
        d->tss[stage][s] = RC_ARG(c, 3);
    }
    RC_RETURN(c, D3D_OK, 4);
}

static void k_GetTextureStageState(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t stage = RC_ARG(c, 1), s = RC_ARG(c, 2);
    rc_st32(RC_ARG(c, 3), stage < STAGES && s < TSS ? d->tss[stage][s] : 0);
    RC_RETURN(c, D3D_OK, 4);
}

static void k_SetMaterial(rc_cpu *c) { RC_RETURN(c, D3D_OK, 2); }
static void k_SetLight(rc_cpu *c) { RC_RETURN(c, D3D_OK, 3); }
static void k_LightEnable(rc_cpu *c) { RC_RETURN(c, D3D_OK, 3); }

static void k_ValidateDevice(rc_cpu *c)
{
    if (RC_ARG(c, 1)) {
        rc_st32(RC_ARG(c, 1), 1);
    }
    RC_RETURN(c, D3D_OK, 2);
}

static void k_PreLoad(rc_cpu *c) { RC_RETURN(c, D3D_OK, 2); }

/* ---- the rasteriser ----
 * Colours are four 8-bit channels. A surface's pixel format is worked out
 * once per draw (where each channel is and how wide), so that a pixel is
 * taken apart and put together with shifts and small tables.
 *
 * Pixel centres are at whole coordinates, as in Direct3D 7 (not at +0.5,
 * as from Direct3D 10 on): the game puts its 2D quads at half pixels
 * (a 256-texel tile from y 240.5 to 496.5) so that each covers whole rows
 * (241 to 496) and samples each texel at its middle. With centres at +0.5
 * a row lands on the edge between two tiles at v = 0, where rounding takes
 * texel row -1, the last one by wrapping: a line across the screen. */

typedef struct c8 {
    int r, g, b, a; /* 0..255 */
} c8;

/* A channel of a pixel format: where it is and how many bits it has
 * (0: absent, read as 255). */
typedef struct chan {
    uint8_t shift, bits;
} chan;

typedef struct fmt {
    chan c[4]; /* r, g, b, a */
    uint32_t bpp;
} fmt;

/* expand[b][x]: an x of b bits widened to 8. */
static uint8_t expand[9][256];

static void init_expand(void)
{
    if (expand[8][255]) {
        return;
    }
    for (int b = 1; b <= 8; b++) {
        int m = (1 << b) - 1;
        for (int x = 0; x <= m; x++) {
            expand[b][x] = (uint8_t)((x * 255 + m / 2) / m);
        }
    }
}

static chan chan_of(uint32_t mask)
{
    chan c = {0, 0};
    if (!mask) {
        return c;
    }
    while (!((mask >> c.shift) & 1)) {
        c.shift++;
    }
    while (c.shift + c.bits < 32 && ((mask >> (c.shift + c.bits)) & 1)) {
        c.bits++;
    }
    if (c.bits > 8) { /* keep the top 8 */
        c.shift = (uint8_t)(c.shift + c.bits - 8);
        c.bits = 8;
    }
    return c;
}

static fmt fmt_of(const rc_dd_surface *t)
{
    uint32_t rm = t->rmask, gm = t->gmask, bm = t->bmask;
    if (!rm && !gm && !bm) {
        if (t->bpp == 16) {
            rm = 0xf800;
            gm = 0x07e0;
            bm = 0x001f;
        } else {
            rm = 0xff0000;
            gm = 0xff00;
            bm = 0xff;
        }
    }
    fmt f = {{chan_of(rm), chan_of(gm), chan_of(bm), chan_of(t->amask)}, t->bpp};
    return f;
}

static inline int get_chan(uint32_t v, chan c)
{
    return c.bits ? expand[c.bits][(v >> c.shift) & ((1u << c.bits) - 1)] : 255;
}

/* For a 16-bit format, every texel's channels at once (r, g, b, a from the
 * low byte up): made when a draw is set up (on one core), a few formats at
 * most. */
#define TEXEL_TABLES 8
static struct {
    fmt f;
    uint32_t *t;
} texel_tables[TEXEL_TABLES];

static const uint32_t *texel_table(const fmt *f)
{
    for (int i = 0; i < TEXEL_TABLES; i++) {
        if (texel_tables[i].t && !memcmp(&texel_tables[i].f, f, sizeof *f)) {
            return texel_tables[i].t;
        }
    }
    for (int i = 0; i < TEXEL_TABLES; i++) {
        if (!texel_tables[i].t) {
            uint32_t *t = malloc(65536 * sizeof *t);
            if (!t) {
                return NULL;
            }
            for (uint32_t v = 0; v < 65536; v++) {
                t[v] = (uint32_t)get_chan(v, f->c[0]) | (uint32_t)get_chan(v, f->c[1]) << 8 |
                       (uint32_t)get_chan(v, f->c[2]) << 16 | (uint32_t)get_chan(v, f->c[3]) << 24;
            }
            texel_tables[i].f = *f;
            texel_tables[i].t = t;
            return t;
        }
    }
    return NULL;
}

static inline c8 unpack(const fmt *f, uint32_t v)
{
    return (c8){get_chan(v, f->c[0]), get_chan(v, f->c[1]), get_chan(v, f->c[2]), get_chan(v, f->c[3])};
}

static inline uint32_t put_chan(int x, chan c)
{
    return c.bits ? ((uint32_t)x >> (8 - c.bits)) << c.shift : 0;
}

static inline uint32_t pack(const fmt *f, c8 c)
{
    return put_chan(c.r, f->c[0]) | put_chan(c.g, f->c[1]) | put_chan(c.b, f->c[2]) | put_chan(c.a, f->c[3]);
}

static c8 from_argb(uint32_t v) { return (c8){(int)((v >> 16) & 255), (int)((v >> 8) & 255), (int)(v & 255), (int)(v >> 24)}; }

static inline uint32_t raw_at(const uint8_t *row, uint32_t bpp, uint32_t x)
{
    switch (bpp) {
    case 8: return row[x];
    case 16: return (uint32_t)row[2 * x] | (uint32_t)row[2 * x + 1] << 8;
    case 32: return (uint32_t)row[4 * x] | (uint32_t)row[4 * x + 1] << 8 | (uint32_t)row[4 * x + 2] << 16 | (uint32_t)row[4 * x + 3] << 24;
    default: return (uint32_t)row[3 * x] | (uint32_t)row[3 * x + 1] << 8 | (uint32_t)row[3 * x + 2] << 16;
    }
}

static inline void raw_put(uint8_t *row, uint32_t bpp, uint32_t x, uint32_t v)
{
    switch (bpp) {
    case 8: row[x] = (uint8_t)v; break;
    case 16:
        row[2 * x] = (uint8_t)v;
        row[2 * x + 1] = (uint8_t)(v >> 8);
        break;
    case 32:
        row[4 * x] = (uint8_t)v;
        row[4 * x + 1] = (uint8_t)(v >> 8);
        row[4 * x + 2] = (uint8_t)(v >> 16);
        row[4 * x + 3] = (uint8_t)(v >> 24);
        break;
    default:
        row[3 * x] = (uint8_t)v;
        row[3 * x + 1] = (uint8_t)(v >> 8);
        row[3 * x + 2] = (uint8_t)(v >> 16);
        break;
    }
}

static void clear_target(rc_dd_surface *s, uint32_t count, uint32_t rects, uint32_t argb)
{
    init_expand();
    fmt f = fmt_of(s);
    uint32_t v = pack(&f, from_argb(argb));
    int32_t whole[4] = {0, 0, (int32_t)s->w, (int32_t)s->h};
    for (uint32_t i = 0; i < (count ? count : 1); i++) {
        int32_t r[4];
        for (int k = 0; k < 4; k++) {
            r[k] = count ? (int32_t)rc_ld32(rects + 16 * i + 4u * (uint32_t)k) : whole[k];
        }
        r[0] = r[0] < 0 ? 0 : r[0];
        r[1] = r[1] < 0 ? 0 : r[1];
        r[2] = r[2] > (int32_t)s->w ? (int32_t)s->w : r[2];
        r[3] = r[3] > (int32_t)s->h ? (int32_t)s->h : r[3];
        for (int32_t y = r[1]; y < r[3]; y++) {
            uint8_t *row = rc_ptr(s->pixels + (uint32_t)y * s->pitch);
            for (int32_t x = r[0]; x < r[2]; x++) {
                raw_put(row, s->bpp, (uint32_t)x, v);
            }
        }
    }
}

/* A vertex after transformation: screen position, depth, 1/w, colour and
 * up to two texture coordinate sets. */
typedef struct vtx {
    float x, y, z, rhw;
    float diffuse[4]; /* r, g, b, a, 0..255 */
    float u[2], v[2];
} vtx;

/* Where each part of a vertex is, by its FVF. */
typedef struct layout {
    uint32_t stride, pos, nfloat, diffuse, ntex, tex[8];
    int rhw, has_diffuse;
} layout;

static layout layout_of(uint32_t fvf)
{
    layout l = {0};
    uint32_t pos = fvf & 0x00e;
    l.rhw = pos == 0x004;
    l.nfloat = pos == 0x004 ? 4 : pos == 0x002 ? 3 : 3 + (pos - 0x004) / 2; /* XYZBn: n weights */
    uint32_t at = 4 * l.nfloat;
    if (fvf & 0x010) {
        at += 12; /* normal */
    }
    if (fvf & 0x020) {
        at += 4; /* point size / reserved */
    }
    if (fvf & 0x040) {
        l.has_diffuse = 1;
        l.diffuse = at;
        at += 4;
    }
    if (fvf & 0x080) {
        at += 4; /* specular */
    }
    l.ntex = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < l.ntex && i < 8; i++) {
        uint32_t size = (fvf >> (16 + 2 * i)) & 3; /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
        uint32_t n = size == 0 ? 2 : size == 1 ? 3 : size == 2 ? 4 : 1;
        l.tex[i] = at;
        at += 4 * n;
    }
    l.stride = at;
    return l;
}

static void mul4(const float *m, const float in[4], float out[4])
{
    for (int c = 0; c < 4; c++) {
        out[c] = in[0] * m[c] + in[1] * m[4 + c] + in[2] * m[8 + c] + in[3] * m[12 + c];
    }
}

static void matmul(const float *a, const float *b, float *o)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            o[4 * r + c] = a[4 * r] * b[c] + a[4 * r + 1] * b[4 + c] + a[4 * r + 2] * b[8 + c] + a[4 * r + 3] * b[12 + c];
        }
    }
}

/* Read and transform vertex `i`; false if it is behind the eye. */
static bool load_vertex(const device *d, const layout *l, const float *wvp, uint32_t base, uint32_t i, vtx *v)
{
    uint32_t a = base + i * l->stride;
    if (l->rhw) {
        v->x = ldf(a);
        v->y = ldf(a + 4);
        v->z = ldf(a + 8);
        v->rhw = ldf(a + 12);
    } else {
        float p[4] = {ldf(a), ldf(a + 4), ldf(a + 8), 1.0f}, cl[4];
        mul4(wvp, p, cl);
        if (cl[3] <= 1e-6f) {
            return false;
        }
        float iw = 1.0f / cl[3];
        v->x = (float)d->vp.x + (1.0f + cl[0] * iw) * 0.5f * (float)d->vp.w;
        v->y = (float)d->vp.y + (1.0f - cl[1] * iw) * 0.5f * (float)d->vp.h;
        v->z = d->vp.minz + cl[2] * iw * (d->vp.maxz - d->vp.minz);
        v->rhw = iw;
    }
    uint32_t dc = l->has_diffuse ? rc_ld32(a + l->diffuse) : 0xffffffffu;
    v->diffuse[0] = (float)((dc >> 16) & 255);
    v->diffuse[1] = (float)((dc >> 8) & 255);
    v->diffuse[2] = (float)(dc & 255);
    v->diffuse[3] = (float)(dc >> 24);
    for (int t = 0; t < 2; t++) {
        uint32_t set = d->tss[t][11] & 7;
        if (set < l->ntex) {
            v->u[t] = ldf(a + l->tex[set]);
            v->v[t] = ldf(a + l->tex[set] + 4);
        } else {
            v->u[t] = v->v[t] = 0.0f;
        }
    }
    return true;
}

/* What a draw needs per pixel, worked out once. */
typedef struct stage {
    int on;              /* the stage takes part */
    const uint8_t *pix;  /* the texture's pixels, or NULL */
    uint32_t w, h, pitch, bpp;
    fmt f;
    int keyed;
    uint32_t key;
    int linear;
    const uint32_t *texels; /* a 16-bit texture's channels (texel_table), or NULL */
    uint32_t au, av; /* address modes */
    uint32_t cop, aop, c1, c2, a1, a2;
} stage;

typedef struct rctx {
    const device *d;
    uint32_t cull;               /* the device's state when the draw was made */
    int32_t vx, vy, vw, vh;      /* (its viewport) */
    rc_dd_surface *t;
    fmt tf;
    uint8_t *tpix;
    rc_dd_surface *z;
    stage st[2];
    int flat, alpha_test, aref, afunc, blend, sb, db, ztest, zwrite, zfunc, keyed_discard;
    c8 tfactor;
} rctx;

static void setup(rctx *r, const device *d, rc_dd_surface *t, rc_dd_surface *z)
{
    init_expand();
    memset(r, 0, sizeof *r);
    r->d = d;
    r->cull = d->rs[22];
    r->vx = (int32_t)d->vp.x;
    r->vy = (int32_t)d->vp.y;
    r->vw = (int32_t)d->vp.w;
    r->vh = (int32_t)d->vp.h;
    r->t = t;
    r->tf = fmt_of(t);
    r->tpix = rc_ptr(t->pixels);
    r->z = z && d->rs[7] ? z : NULL;
    r->ztest = r->z != NULL;
    r->zwrite = d->rs[14] != 0;
    r->zfunc = (int)d->rs[23];
    r->flat = d->rs[9] == 1;
    r->alpha_test = d->rs[15] != 0;
    r->aref = (int)(d->rs[24] & 255);
    r->afunc = (int)d->rs[25];
    r->blend = d->rs[27] != 0;
    r->sb = (int)d->rs[19];
    r->db = (int)d->rs[20];
    if (r->sb == 12 || r->sb == 13) { /* BOTHSRCALPHA, BOTHINVSRCALPHA */
        r->db = r->sb == 12 ? 6 : 5;
        r->sb = r->sb == 12 ? 5 : 6;
    }
    r->tfactor = from_argb(d->rs[60]);
    for (int s = 0; s < 2; s++) {
        stage *g = &r->st[s];
        g->cop = d->tss[s][1];
        g->aop = d->tss[s][4];
        if (g->cop == 1 || (s == 1 && !r->st[0].on)) {
            break; /* DISABLE: this and later stages */
        }
        g->on = 1;
        g->c1 = d->tss[s][2];
        g->c2 = d->tss[s][3];
        g->a1 = d->tss[s][5];
        g->a2 = d->tss[s][6];
        const rc_dd_surface *x = rc_dd_surf(d->texture[s]);
        if (x && x->w && x->h && x->pixels) {
            g->pix = rc_ptr(x->pixels);
            g->w = x->w;
            g->h = x->h;
            g->pitch = x->pitch;
            g->bpp = x->bpp;
            g->f = fmt_of(x);
            g->keyed = d->rs[41] && x->has_ckey; /* COLORKEYENABLE */
            g->key = x->ckey;
            g->linear = d->tss[s][16] == 2 || d->tss[s][17] == 2;
            g->texels = g->bpp == 16 ? texel_table(&g->f) : NULL;
            g->au = d->tss[s][13] ? d->tss[s][13] : d->tss[s][12];
            g->av = d->tss[s][14] ? d->tss[s][14] : d->tss[s][12];
        }
    }
    r->keyed_discard = d->rs[41] && r->st[0].pix && !r->blend;
}

/* floorf as an integer, without a library call (the same for the values
 * a texture coordinate times a size takes). */
static inline int ifloor(float x)
{
    int i = (int)x;
    return i - (x < (float)i);
}

/* A texel coordinate by the address mode. */
static inline int addr(int i, int n, uint32_t mode)
{
    if (mode <= 1 && (n & (n - 1)) == 0) {
        return i & (n - 1); /* wrap, a power of two */
    }
    switch (mode) {
    case 3: /* clamp */
    case 4: /* border */
        return i < 0 ? 0 : i >= n ? n - 1 : i;
    case 2: { /* mirror */
        int p = ((i % (2 * n)) + 2 * n) % (2 * n);
        return p < n ? p : 2 * n - 1 - p;
    }
    default:
        return ((i % n) + n) % n;
    }
}

static inline c8 texel(const stage *g, int x, int y)
{
    uint32_t raw = raw_at(g->pix + (uint32_t)y * g->pitch, g->bpp, (uint32_t)x);
    if (g->keyed && raw == g->key) {
        return (c8){0, 0, 0, 0};
    }
    return unpack(&g->f, raw);
}

static c8 sample(const stage *g, float u, float v)
{
    if (!g->linear) {
        int x = ifloor(u * (float)g->w), y = ifloor(v * (float)g->h);
        return texel(g, addr(x, (int)g->w, g->au), addr(y, (int)g->h, g->av));
    }
    /* Bilinear, colours weighted by alpha so that keyed texels do not
     * bleed their colour. */
    float fu = u * (float)g->w - 0.5f, fv = v * (float)g->h - 0.5f;
    int x0 = ifloor(fu), y0 = ifloor(fv);
    int ax = (int)((fu - (float)x0) * 256.0f), ay = (int)((fv - (float)y0) * 256.0f);
    int r = 0, gg = 0, b = 0, a = 0;
    for (int k = 0; k < 4; k++) {
        c8 c = texel(g, addr(x0 + (k & 1), (int)g->w, g->au), addr(y0 + (k >> 1), (int)g->h, g->av));
        int wgt = ((k & 1) ? ax : 256 - ax) * ((k >> 1) ? ay : 256 - ay); /* /65536 */
        int wa = wgt / 256 * c.a;                                         /* /(256*255) */
        r += c.r * wa;
        gg += c.g * wa;
        b += c.b * wa;
        a += wa;
    }
    if (a <= 0) {
        return (c8){0, 0, 0, 0};
    }
    return (c8){r / a, gg / a, b / a, a / 256 > 255 ? 255 : a / 256};
}

static inline int mul8(int a, int b) { return (a * b + 127) / 255; }

static inline int clamp8(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }

/* A texture stage argument. */
static inline c8 arg(const rctx *r, uint32_t a, c8 diffuse, c8 current, c8 tex)
{
    c8 c;
    switch (a & 0xf) {
    case 0: c = diffuse; break;
    case 2: c = tex; break;
    case 3: c = r->tfactor; break;
    default: c = current; break;
    }
    if (a & 0x10) { /* complement */
        c = (c8){255 - c.r, 255 - c.g, 255 - c.b, 255 - c.a};
    }
    if (a & 0x20) { /* alpha replicate */
        c.r = c.g = c.b = c.a;
    }
    return c;
}

static inline int op1(uint32_t op, int a, int b, int at, int ad, int ac)
{
    switch (op) {
    case 2: return a;                                      /* SELECTARG1 */
    case 3: return b;                                      /* SELECTARG2 */
    case 4: return mul8(a, b);                             /* MODULATE */
    case 5: return clamp8(2 * mul8(a, b));                 /* MODULATE2X */
    case 6: return clamp8(4 * mul8(a, b));                 /* MODULATE4X */
    case 7: return clamp8(a + b);                          /* ADD */
    case 8: return clamp8(a + b - 128);                    /* ADDSIGNED */
    case 9: return clamp8(2 * (a + b - 128));              /* ADDSIGNED2X */
    case 10: return clamp8(a - b);                         /* SUBTRACT */
    case 11: return clamp8(a + b - mul8(a, b));            /* ADDSMOOTH */
    case 12: return mul8(a, ad) + mul8(b, 255 - ad);       /* BLENDDIFFUSEALPHA */
    case 13: return mul8(a, at) + mul8(b, 255 - at);       /* BLENDTEXTUREALPHA */
    case 16: return mul8(a, ac) + mul8(b, 255 - ac);       /* BLENDCURRENTALPHA */
    default: return mul8(a, b);
    }
}

/* The colour of a fragment through the texture stages. */
static inline c8 shade(const rctx *r, c8 diffuse, const float *u, const float *v)
{
    c8 current = diffuse;
    for (int s = 0; s < 2 && r->st[s].on; s++) {
        const stage *g = &r->st[s];
        c8 tex = g->pix ? sample(g, u[s], v[s]) : (c8){255, 255, 255, 255};
        c8 x1 = arg(r, g->c1, diffuse, current, tex), x2 = arg(r, g->c2, diffuse, current, tex);
        c8 out;
        out.r = op1(g->cop, x1.r, x2.r, tex.a, diffuse.a, current.a);
        out.g = op1(g->cop, x1.g, x2.g, tex.a, diffuse.a, current.a);
        out.b = op1(g->cop, x1.b, x2.b, tex.a, diffuse.a, current.a);
        if (g->aop != 1) {
            c8 y1 = arg(r, g->a1, diffuse, current, tex), y2 = arg(r, g->a2, diffuse, current, tex);
            out.a = op1(g->aop, y1.a, y2.a, tex.a, diffuse.a, current.a);
        } else {
            out.a = current.a;
        }
        current = out;
    }
    return current;
}

static inline bool compare(int fn, int a, int ref)
{
    switch (fn) {
    case 1: return false;
    case 2: return a < ref;
    case 3: return a == ref;
    case 4: return a <= ref;
    case 5: return a > ref;
    case 6: return a != ref;
    case 7: return a >= ref;
    default: return true;
    }
}

static inline c8 factor(int f, c8 src, c8 dst)
{
    switch (f) {
    case 1: return (c8){0, 0, 0, 0};
    case 3: return src;
    case 4: return (c8){255 - src.r, 255 - src.g, 255 - src.b, 255 - src.a};
    case 5: return (c8){src.a, src.a, src.a, src.a};
    case 6: return (c8){255 - src.a, 255 - src.a, 255 - src.a, 255 - src.a};
    case 7: return (c8){dst.a, dst.a, dst.a, dst.a};
    case 8: return (c8){255 - dst.a, 255 - dst.a, 255 - dst.a, 255 - dst.a};
    case 9: return dst;
    case 10: return (c8){255 - dst.r, 255 - dst.g, 255 - dst.b, 255 - dst.a};
    case 11: {
        int f2 = src.a < 255 - dst.a ? src.a : 255 - dst.a;
        return (c8){f2, f2, f2, 255};
    }
    default: return (c8){255, 255, 255, 255};
    }
}

static inline void pixel(const rctx *r, uint8_t *row, int x, int y, float depth, c8 c)
{
    if (r->ztest) {
        uint8_t *zp = rc_ptr(r->z->pixels + (uint32_t)y * r->z->pitch + 2u * (uint32_t)x);
        float dz = depth < 0 ? 0 : depth > 1 ? 1 : depth;
        int nz = (int)(dz * 65535.0f), old = zp[0] | zp[1] << 8;
        if (!compare(r->zfunc, nz, old)) {
            return;
        }
        if (r->zwrite) {
            zp[0] = (uint8_t)nz;
            zp[1] = (uint8_t)(nz >> 8);
        }
    }
    if (r->blend) {
        c8 dst = unpack(&r->tf, raw_at(row, r->tf.bpp, (uint32_t)x));
        c8 fs = factor(r->sb, c, dst), fd = factor(r->db, c, dst);
        c = (c8){clamp8(mul8(c.r, fs.r) + mul8(dst.r, fd.r)), clamp8(mul8(c.g, fs.g) + mul8(dst.g, fd.g)),
                 clamp8(mul8(c.b, fs.b) + mul8(dst.b, fd.b)), clamp8(mul8(c.a, fs.a) + mul8(dst.a, fd.a))};
    }
    raw_put(row, r->tf.bpp, (uint32_t)x, pack(&r->tf, c));
}

static float edge(const vtx *a, const vtx *b, float x, float y) { return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x); }

/* Whether a pixel exactly on edge a->b belongs to the triangle (top-left
 * rule, for the winding used below). */
static bool owns(const vtx *a, const vtx *b) { return b->y < a->y || (b->y == a->y && b->x < a->x); }

/* The pixels of a row the three edge functions can all be non-negative
 * at, one wider each side (the pixel test stays): from w at x0 and their
 * steps per pixel. False if none. */
static bool row_span(float w0, float w1, float w2, float d0, float d1, float d2, int32_t x0, int32_t x1, int32_t *lo,
                     int32_t *hi)
{
    float l = (float)x0, h = (float)x1;
    const float w[3] = {w0, w1, w2}, d[3] = {d0, d1, d2};
    for (int i = 0; i < 3; i++) {
        if (d[i] > 0) {
            float at = (float)x0 - w[i] / d[i];
            l = at > l ? at : l;
        } else if (d[i] < 0) {
            float at = (float)x0 - w[i] / d[i];
            h = at < h ? at : h;
        } else if (w[i] < 0) {
            return false;
        }
    }
    int32_t a = (int32_t)floorf(l) - 1, b = (int32_t)ceilf(h) + 1;
    *lo = a < x0 ? x0 : a;
    *hi = b > x1 ? x1 : b;
    return *lo <= *hi;
}

/* A triangle, its rows from by0 up to by1 only (a band: bands drawn on
 * several cores at once come out as the whole drawn on one). */
static void triangle(const rctx *r, const vtx *v0, const vtx *v1, const vtx *v2, int32_t by0, int32_t by1)
{
    float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.0f) {
        return;
    }
    uint32_t cull = r->cull;
    if ((cull == 2 && area > 0) || (cull == 3 && area < 0)) {
        return; /* D3DCULL_CW, D3DCULL_CCW (y down: clockwise has positive area) */
    }
    if (area < 0) {
        const vtx *tmp = v1;
        v1 = v2;
        v2 = tmp;
        area = -area;
    }
    rc_dd_surface *t = r->t;
    int32_t cx0 = r->vx, cy0 = r->vy;
    int32_t cx1 = cx0 + r->vw, cy1 = cy0 + r->vh;
    cx1 = cx1 > (int32_t)t->w ? (int32_t)t->w : cx1;
    cy1 = cy1 > (int32_t)t->h ? (int32_t)t->h : cy1;
    cx0 = cx0 < 0 ? 0 : cx0;
    cy0 = cy0 < 0 ? 0 : cy0;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int32_t x0 = (int32_t)ceilf(minx), x1 = (int32_t)floorf(maxx);
    int32_t y0 = (int32_t)ceilf(miny), y1 = (int32_t)floorf(maxy);
    x0 = x0 < cx0 ? cx0 : x0;
    y0 = y0 < cy0 ? cy0 : y0;
    x1 = x1 >= cx1 ? cx1 - 1 : x1;
    y1 = y1 >= cy1 ? cy1 - 1 : y1;
    y0 = y0 < by0 ? by0 : y0;
    y1 = y1 >= by1 ? by1 - 1 : y1;
    if (x0 > x1 || y0 > y1) {
        return;
    }
    bool own0 = owns(v1, v2), own1 = owns(v2, v0), own2 = owns(v0, v1);
    float inv = 1.0f / area;
    /* Edge functions step by these per pixel to the right. */
    float d0 = -(v2->y - v1->y), d1 = -(v0->y - v2->y), d2 = -(v1->y - v0->y);
    bool same_colour = true;
    for (int k = 0; k < 4; k++) {
        same_colour &= v0->diffuse[k] == v1->diffuse[k] && v0->diffuse[k] == v2->diffuse[k];
    }
    c8 const_colour = {(int)v0->diffuse[0], (int)v0->diffuse[1], (int)v0->diffuse[2], (int)v0->diffuse[3]};
    bool textured = r->st[0].on && (r->st[0].pix || (r->st[1].on && r->st[1].pix));
    /* With one 1/w (drawn flat, as 2D is) texture coordinates are affine:
     * they step by constants. */
    bool affine = v0->rhw == v1->rhw && v0->rhw == v2->rhw;
    float du[2], dv[2];
    for (int s = 0; s < 2; s++) {
        du[s] = (d0 * v0->u[s] + d1 * v1->u[s] + d2 * v2->u[s]) * inv;
        dv[s] = (d0 * v0->v[s] + d1 * v1->v[s] + d2 * v2->v[s]) * inv;
    }
    /* The texel itself, unchanged: one texture in the target's format,
     * sampled by points, times white (or selected alone), nothing else. */
    const stage *g0 = &r->st[0];
    bool copy = affine && g0->pix && !g0->linear && !r->st[1].on && !r->blend && !r->alpha_test && !r->ztest &&
                g0->bpp == r->tf.bpp && g0->bpp == 16 && memcmp(&g0->f, &r->tf, sizeof g0->f) == 0 &&
                ((g0->cop == 2 && (g0->c1 & 0x3f) == 2) ||
                 (g0->cop == 4 && same_colour && const_colour.r == 255 && const_colour.g == 255 &&
                  const_colour.b == 255 && (((g0->c1 & 0x3f) == 2 && (g0->c2 & 0x3f) <= 1) ||
                                            ((g0->c2 & 0x3f) == 2 && (g0->c1 & 0x3f) <= 1)))) &&
                (g0->aop == 1 || g0->aop == 2 || g0->aop == 4 || g0->aop == 3);
    if (copy) {
        int tw = (int)g0->w, th = (int)g0->h;
        for (int32_t y = y0; y <= y1; y++) {
            float py = (float)y, px = (float)x0;
            float w0 = edge(v1, v2, px, py), w1 = edge(v2, v0, px, py), w2 = edge(v0, v1, px, py);
            float u = (w0 * v0->u[0] + w1 * v1->u[0] + w2 * v2->u[0]) * inv;
            float v = (w0 * v0->v[0] + w1 * v1->v[0] + w2 * v2->v[0]) * inv;
            uint16_t *row = (uint16_t *)(void *)(r->tpix + (uint32_t)y * t->pitch);
            int32_t lo, hi;
            if (!row_span(w0, w1, w2, d0, d1, d2, x0, x1, &lo, &hi)) {
                continue;
            }
            float k = (float)(lo - x0);
            w0 += d0 * k;
            w1 += d1 * k;
            w2 += d2 * k;
            u += du[0] * k;
            v += dv[0] * k;
            for (int32_t x = lo; x <= hi; x++, w0 += d0, w1 += d1, w2 += d2, u += du[0], v += dv[0]) {
                if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !own0) || (w1 == 0 && !own1) || (w2 == 0 && !own2)) {
                    continue;
                }
                int tx = addr(ifloor(u * (float)tw), tw, g0->au), ty = addr(ifloor(v * (float)th), th, g0->av);
                const uint8_t *tp = g0->pix + (uint32_t)ty * g0->pitch + 2u * (uint32_t)tx;
                uint16_t raw = (uint16_t)(tp[0] | tp[1] << 8);
                if (g0->keyed && raw == g0->key) {
                    continue;
                }
                row[x] = raw;
            }
        }
        return;
    }
    /* One 16-bit texture, sampled by points, times the vertex colour (or
     * alone), its alpha from the texture and the colour as the stage says,
     * blended by that alpha (or not), into RGB565: what the game draws
     * almost everything with. The colour steps across a row in 16.16. */
    bool rgb565 = r->tf.bpp == 16 && r->tf.c[0].shift == 11 && r->tf.c[0].bits == 5 && r->tf.c[1].bits == 6 &&
                  r->tf.c[2].bits == 5 && !r->tf.c[3].bits;
    uint32_t ca1 = g0->c1 & 0x3f, ca2 = g0->c2 & 0x3f, aa1 = g0->a1 & 0x3f, aa2 = g0->a2 & 0x3f;
    /* Colour: 0 texture alone, 1 texture times colour. Alpha: 0 texture,
     * 1 colour, 2 both, 3 opaque. */
    int cmode = -1, amode = -1;
    if (g0->cop == 2 && ca1 == 2) {
        cmode = 0;
    } else if (g0->cop == 4 && ((ca1 == 2 && ca2 <= 1) || (ca2 == 2 && ca1 <= 1))) {
        cmode = 1;
    }
    if ((g0->aop == 2 && aa1 == 2) || (g0->aop == 3 && aa2 == 2)) {
        amode = 0;
    } else if ((g0->aop == 2 && aa1 <= 1) || (g0->aop == 3 && aa2 <= 1)) {
        amode = 1;
    } else if (g0->aop == 4 && ((aa1 == 2 && aa2 <= 1) || (aa2 == 2 && aa1 <= 1))) {
        amode = 2;
    } else if (g0->aop == 1) {
        amode = 1; /* DISABLE: the colour's */
    }
    bool fast = affine && rgb565 && g0->pix && g0->texels && !g0->linear && !r->st[1].on && !r->alpha_test &&
                !r->ztest && g0->bpp == 16 && cmode >= 0 && amode >= 0 && (!r->blend || (r->sb == 5 && r->db == 6));
    if (fast) {
        int tw = (int)g0->w, th = (int)g0->h;
        const uint32_t *texels = g0->texels;
        /* Colour steps per pixel to the right, 16.16. */
        int32_t dcol[4];
        for (int k = 0; k < 4; k++) {
            float dk = (d0 * v0->diffuse[k] + d1 * v1->diffuse[k] + d2 * v2->diffuse[k]) * inv;
            dcol[k] = (int32_t)(dk * 65536.0f);
        }
        for (int32_t y = y0; y <= y1; y++) {
            float py = (float)y, px = (float)x0;
            float w0 = edge(v1, v2, px, py), w1 = edge(v2, v0, px, py), w2 = edge(v0, v1, px, py);
            float u = (w0 * v0->u[0] + w1 * v1->u[0] + w2 * v2->u[0]) * inv;
            float v = (w0 * v0->v[0] + w1 * v1->v[0] + w2 * v2->v[0]) * inv;
            int32_t col[4];
            for (int k = 0; k < 4; k++) {
                float ck = r->flat ? v0->diffuse[k]
                                   : (w0 * v0->diffuse[k] + w1 * v1->diffuse[k] + w2 * v2->diffuse[k]) * inv;
                col[k] = (int32_t)(ck * 65536.0f) + 32768;
            }
            if (r->flat) {
                memset(dcol, 0, sizeof dcol);
            }
            uint16_t *row = (uint16_t *)(void *)(r->tpix + (uint32_t)y * t->pitch);
            int32_t lo, hi;
            if (!row_span(w0, w1, w2, d0, d1, d2, x0, x1, &lo, &hi)) {
                continue;
            }
            float k = (float)(lo - x0);
            w0 += d0 * k;
            w1 += d1 * k;
            w2 += d2 * k;
            u += du[0] * k;
            v += dv[0] * k;
            for (int c = 0; c < 4; c++) {
                col[c] += dcol[c] * (lo - x0);
            }
            for (int32_t x = lo; x <= hi; x++, w0 += d0, w1 += d1, w2 += d2, u += du[0], v += dv[0],
                         col[0] += dcol[0], col[1] += dcol[1], col[2] += dcol[2], col[3] += dcol[3]) {
                if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !own0) || (w1 == 0 && !own1) || (w2 == 0 && !own2)) {
                    continue;
                }
                int tx = addr(ifloor(u * (float)tw), tw, g0->au), ty = addr(ifloor(v * (float)th), th, g0->av);
                const uint8_t *tp = g0->pix + (uint32_t)ty * g0->pitch + 2u * (uint32_t)tx;
                uint32_t raw = (uint32_t)(tp[0] | tp[1] << 8);
                if (g0->keyed && raw == g0->key) {
                    if (!r->blend) {
                        continue; /* colour-keyed texel */
                    }
                    continue; /* blended: alpha 0, no change */
                }
                int dr = clamp8(col[0] >> 16), dg = clamp8(col[1] >> 16), db = clamp8(col[2] >> 16),
                    da = clamp8(col[3] >> 16);
                uint32_t tx8 = texels[raw];
                int sr = (int)(tx8 & 255), sg = (int)((tx8 >> 8) & 255), sb = (int)((tx8 >> 16) & 255);
                int ta = (int)(tx8 >> 24);
                if (cmode == 1) {
                    sr = mul8(sr, dr);
                    sg = mul8(sg, dg);
                    sb = mul8(sb, db);
                }
                int sa = amode == 0 ? ta : amode == 1 ? da : amode == 2 ? mul8(ta, da) : 255;
                if (r->blend) {
                    if (sa == 0) {
                        continue;
                    }
                    if (sa < 255) {
                        uint32_t dst = row[x];
                        int ia = 255 - sa;
                        sr = (sr * sa + expand[5][dst >> 11] * ia + 127) / 255;
                        sg = (sg * sa + expand[6][(dst >> 5) & 63] * ia + 127) / 255;
                        sb = (sb * sa + expand[5][dst & 31] * ia + 127) / 255;
                    }
                }
                row[x] = (uint16_t)((sr >> 3) << 11 | (sg >> 2) << 5 | (sb >> 3));
            }
        }
        return;
    }
    if (rc_trace("d3dslow")) {
        rc_log("[d3dslow] %d blend %d %d/%d at %d st0 %u:%u,%u/%u:%u,%u bpp %u a%u lin %d st1 %d aff %d same %d col %02x%02x%02x%02x", (int)(area / 2), r->blend, r->sb, r->db, r->alpha_test, g0->cop, g0->c1, g0->c2, g0->aop, g0->a1, g0->a2, g0->bpp, g0->f.c[3].bits, g0->linear, r->st[1].on, affine, same_colour, const_colour.a, const_colour.r, const_colour.g, const_colour.b);
    }
    for (int32_t y = y0; y <= y1; y++) {
        float py = (float)y, px = (float)x0;
        float w0 = edge(v1, v2, px, py), w1 = edge(v2, v0, px, py), w2 = edge(v0, v1, px, py);
        uint8_t *row = r->tpix + (uint32_t)y * t->pitch;
        for (int32_t x = x0; x <= x1; x++, w0 += d0, w1 += d1, w2 += d2) {
            if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !own0) || (w1 == 0 && !own1) || (w2 == 0 && !own2)) {
                continue;
            }
            float b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
            float depth = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            c8 dc;
            if (r->flat || same_colour) {
                dc = const_colour;
            } else {
                dc.r = (int)(b0 * v0->diffuse[0] + b1 * v1->diffuse[0] + b2 * v2->diffuse[0] + 0.5f);
                dc.g = (int)(b0 * v0->diffuse[1] + b1 * v1->diffuse[1] + b2 * v2->diffuse[1] + 0.5f);
                dc.b = (int)(b0 * v0->diffuse[2] + b1 * v1->diffuse[2] + b2 * v2->diffuse[2] + 0.5f);
                dc.a = (int)(b0 * v0->diffuse[3] + b1 * v1->diffuse[3] + b2 * v2->diffuse[3] + 0.5f);
            }
            float u[2] = {0, 0}, v[2] = {0, 0};
            if (textured && affine) {
                for (int s = 0; s < 2; s++) {
                    u[s] = b0 * v0->u[s] + b1 * v1->u[s] + b2 * v2->u[s];
                    v[s] = b0 * v0->v[s] + b1 * v1->v[s] + b2 * v2->v[s];
                }
            } else if (textured) {
                /* Perspective-correct texture coordinates. */
                float q0 = b0 * v0->rhw, q1 = b1 * v1->rhw, q2 = b2 * v2->rhw;
                float q = q0 + q1 + q2;
                float iq = q != 0.0f ? 1.0f / q : 0.0f;
                for (int s = 0; s < 2; s++) {
                    u[s] = (q0 * v0->u[s] + q1 * v1->u[s] + q2 * v2->u[s]) * iq;
                    v[s] = (q0 * v0->v[s] + q1 * v1->v[s] + q2 * v2->v[s]) * iq;
                }
            }
            c8 c = shade(r, dc, u, v);
            if (r->alpha_test && !compare(r->afunc, c.a, r->aref)) {
                continue;
            }
            if (r->keyed_discard && c.a <= 0) {
                continue; /* colour-keyed texel */
            }
            pixel(r, row, x, y, depth, c);
        }
    }
}

/* Drawing is deferred: each draw's triangles (their vertices transformed)
 * are queued with the state it was made in, until anything needs the
 * pixels (a surface is locked, copied, shown or released, the target
 * cleared: rc_d3d_flush). Then the whole queue is drawn in bands of rows
 * on all cores when there are enough pixels: each band draws every
 * triangle in its order, so every pixel comes out as drawn on one core,
 * one draw after the other. */
typedef struct queued {
    rctx r;
    uint32_t first, n; /* its triangles in the queue's */
} queued;
static queued *draws;
static uint32_t n_draws, cap_draws;
static vtx *tris;
static uint32_t n_tris, cap_tris;
static float queued_pixels;

/* Beyond these the queue is drawn before more is added. */
#define QUEUE_DRAWS 8192
#define QUEUE_TRIS 262144
/* Below this many pixels (the triangles' boxes) the queue is drawn on one
 * core. */
#define PARALLEL_PIXELS 20000

typedef struct {
    int32_t rows; /* each band's */
} bands;

static void draw_band(int32_t from, int32_t to)
{
    for (uint32_t i = 0; i < n_draws; i++) {
        const queued *q = &draws[i];
        for (uint32_t k = q->first; k < q->first + q->n; k++) {
            const vtx *t = &tris[3 * k];
            triangle(&q->r, &t[0], &t[1], &t[2], from, to);
        }
    }
}

static void band(void *ctx, int i)
{
    const bands *b = ctx;
    draw_band(i * b->rows, (i + 1) * b->rows);
}

void rc_d3d_flush(void)
{
    if (!n_draws) {
        return;
    }
    int cores = rc_plat_workers();
    int32_t rows = 0;
    for (uint32_t i = 0; i < n_draws; i++) {
        int32_t h = (int32_t)draws[i].r.t->h;
        rows = h > rows ? h : rows;
    }
    if (cores > 1 && queued_pixels >= PARALLEL_PIXELS && rows > 0 && !rc_trace("d3dslow")) {
        /* Several bands per core, so that a core done early takes another. */
        int32_t nb = cores * 4;
        bands b = {(rows + nb - 1) / nb};
        if (b.rows < 8) {
            b.rows = 8;
        }
        rc_plat_parallel((rows + b.rows - 1) / b.rows, band, &b);
    } else {
        draw_band(0, INT32_MAX);
    }
    n_draws = 0;
    n_tris = 0;
    queued_pixels = 0;
}

/* Room for one more draw and triangle in the queue; false if there is no
 * memory. */
static bool queue_room(void)
{
    if (n_draws == cap_draws) {
        uint32_t cap = cap_draws ? cap_draws * 2 : 64;
        queued *more = realloc(draws, (size_t)cap * sizeof *draws);
        if (!more) {
            return false;
        }
        draws = more;
        cap_draws = cap;
    }
    if (n_tris == cap_tris) {
        uint32_t cap = cap_tris ? cap_tris * 2 : 1024;
        vtx *more = realloc(tris, (size_t)cap * 3 * sizeof *tris);
        if (!more) {
            return false;
        }
        tris = more;
        cap_tris = cap;
    }
    return true;
}

static void draw(device *d, uint32_t type, uint32_t fvf, uint32_t verts, uint32_t n, uint32_t idx, uint32_t nidx)
{
    rc_dd_surface *t = rc_dd_surf(d->target);
    if (!t || !t->pixels) {
        return;
    }
    if (n_draws >= QUEUE_DRAWS || n_tris >= QUEUE_TRIS) {
        rc_d3d_flush();
    }
    if (!queue_room()) {
        rc_d3d_flush();
        if (!queue_room()) {
            return;
        }
    }
    queued *q = &draws[n_draws];
    setup(&q->r, d, t, rc_dd_surf(t->zbuffer));
    q->first = n_tris;
    q->n = 0;
    layout l = layout_of(fvf);
    float wv[16], wvp[16];
    matmul(d->world, d->view, wv);
    matmul(wv, d->proj, wvp);
    uint32_t count = idx ? nidx : n;
    if (rc_trace("d3d")) {
        rc_log("[d3d] draw type %u fvf %#x, %u vertices, %u indices", type, fvf, n, nidx);
    }
#define INDEX(k) (idx ? (uint32_t)rc_ld16(idx + 2u * (k)) : (k))
    vtx tri[3];
    bool ok[3];
    for (uint32_t k = 0; k + 2 < count; k++) {
        uint32_t a, b, cc;
        switch (type) {
        case 4: /* triangle list */
            if (k % 3) {
                continue;
            }
            a = k;
            b = k + 1;
            cc = k + 2;
            break;
        case 5: /* strip: every other one turned round */
            a = k;
            b = (k & 1) ? k + 2 : k + 1;
            cc = (k & 1) ? k + 1 : k + 2;
            break;
        case 6: /* fan */
            a = 0;
            b = k + 1;
            cc = k + 2;
            break;
        default:
            continue; /* points and lines are not drawn */
        }
        uint32_t ids[3] = {INDEX(a), INDEX(b), INDEX(cc)};
        for (int j = 0; j < 3; j++) {
            ok[j] = ids[j] < n || !idx ? load_vertex(d, &l, wvp, verts, ids[j], &tri[j]) : false;
        }
        if (!(ok[0] && ok[1] && ok[2])) {
            continue;
        }
        if (n_tris == cap_tris && !queue_room()) {
            break;
        }
        memcpy(&tris[3 * n_tris], tri, sizeof tri);
        n_tris++;
        q->n++;
        float w = fmaxf(tri[0].x, fmaxf(tri[1].x, tri[2].x)) - fminf(tri[0].x, fminf(tri[1].x, tri[2].x));
        float h = fmaxf(tri[0].y, fmaxf(tri[1].y, tri[2].y)) - fminf(tri[0].y, fminf(tri[1].y, tri[2].y));
        queued_pixels += w * h;
    }
#undef INDEX
    if (q->n) {
        n_draws++;
    }
}

/* ---- drawing ---- */

static void k_Clear(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t count = RC_ARG(c, 1), rects = RC_ARG(c, 2), flags = RC_ARG(c, 3), color = RC_ARG(c, 4);
    rc_d3d_flush();
    rc_dd_surface *s = rc_dd_surf(d->target);
    if (s && (flags & 1)) { /* D3DCLEAR_TARGET */
        clear_target(s, count, rects, color);
    }
    RC_RETURN(c, D3D_OK, 7);
}

static void k_DrawPrimitive(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t type = RC_ARG(c, 1), fvf = RC_ARG(c, 2), verts = RC_ARG(c, 3), n = RC_ARG(c, 4);
    draw(d, type, fvf, verts, n, 0, 0);
    RC_RETURN(c, D3D_OK, 6);
}

static void k_DrawIndexedPrimitive(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t type = RC_ARG(c, 1), fvf = RC_ARG(c, 2), verts = RC_ARG(c, 3), n = RC_ARG(c, 4);
    uint32_t idx = RC_ARG(c, 5), nidx = RC_ARG(c, 6);
    draw(d, type, fvf, verts, n, idx, nidx);
    RC_RETURN(c, D3D_OK, 8);
}

/* ---- vertex buffers: vertices in guest memory, of one FVF ---- */

#define VBS 1024
static struct vb {
    uint32_t object, fvf, n, mem, caps;
} vbs[VBS];

static struct vb *vb_of(uint32_t o)
{
    for (int i = 0; i < VBS; i++) {
        if (vbs[i].object == o && o) {
            return &vbs[i];
        }
    }
    return NULL;
}

static void k_CreateVertexBuffer(rc_cpu *c)
{
    uint32_t desc = RC_ARG(c, 1), out = RC_ARG(c, 2);
    uint32_t caps = rc_ld32(desc + 4), fvf = rc_ld32(desc + 8), n = rc_ld32(desc + 12);
    layout l = layout_of(fvf);
    struct vb *v = NULL;
    for (int i = 0; i < VBS && !v; i++) {
        if (!vbs[i].object) {
            v = &vbs[i];
        }
    }
    uint32_t mem = v && n ? rc_heap_alloc(n * l.stride, true) : 0;
    if (!mem) {
        rc_st32(out, 0);
        RC_RETURN(c, DDERR_INVALIDPARAMS, 4);
    }
    v->object = rc_dd_new(RC_DD_VB);
    v->fvf = fvf;
    v->n = n;
    v->mem = mem;
    v->caps = caps;
    rc_st32(out, rc_dd_iface(v->object, 7));
    RC_RETURN(c, D3D_OK, 4);
}

static void k_VBRelease(rc_cpu *c)
{
    uint32_t o = rc_ld32(RC_ARG(c, 0) + 4);
    uint32_t *r = rc_dd_refs(o);
    if (*r > 0 && --*r == 0) {
        struct vb *v = vb_of(o);
        if (v) {
            rc_heap_free(v->mem);
            memset(v, 0, sizeof *v);
        }
    }
    RC_RETURN(c, *r, 1);
}

static void k_VBLock(rc_cpu *c)
{
    struct vb *v = vb_of(rc_dd_self(c, RC_DD_VB));
    uint32_t ptr = RC_ARG(c, 2), size = RC_ARG(c, 3);
    if (!v) {
        RC_RETURN(c, DDERR_INVALIDPARAMS, 4);
    }
    if (ptr) {
        rc_st32(ptr, v->mem);
    }
    if (size) {
        rc_st32(size, v->n * layout_of(v->fvf).stride);
    }
    RC_RETURN(c, D3D_OK, 4);
}

static void k_VBUnlock(rc_cpu *c) { RC_RETURN(c, D3D_OK, 1); }

static void k_VBGetDesc(rc_cpu *c)
{
    struct vb *v = vb_of(rc_dd_self(c, RC_DD_VB));
    uint32_t d = RC_ARG(c, 1);
    if (v && d) {
        rc_st32(d + 4, v->caps);
        rc_st32(d + 8, v->fvf);
        rc_st32(d + 12, v->n);
    }
    RC_RETURN(c, D3D_OK, 2);
}

static void k_VBOptimize(rc_cpu *c) { RC_RETURN(c, D3D_OK, 3); }

static void k_DrawPrimitiveVB(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t type = RC_ARG(c, 1), start = RC_ARG(c, 3), n = RC_ARG(c, 4);
    struct vb *v = vb_of(rc_ld32(RC_ARG(c, 2) + 4));
    if (v) {
        uint32_t stride = layout_of(v->fvf).stride;
        draw(d, type, v->fvf, v->mem + start * stride, n, 0, 0);
    }
    RC_RETURN(c, D3D_OK, 6);
}

static void k_DrawIndexedPrimitiveVB(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t type = RC_ARG(c, 1), start = RC_ARG(c, 3), n = RC_ARG(c, 4), idx = RC_ARG(c, 5), nidx = RC_ARG(c, 6);
    struct vb *v = vb_of(rc_ld32(RC_ARG(c, 2) + 4));
    if (v) {
        uint32_t stride = layout_of(v->fvf).stride;
        draw(d, type, v->fvf, v->mem + start * stride, n, idx, nidx);
    }
    RC_RETURN(c, D3D_OK, 8);
}

const rc_import_def rc_d3d_defs[] = {
    M("IDirect3D7", "CreateVertexBuffer", k_CreateVertexBuffer),
    M("IDirect3DVertexBuffer7", "AddRef", k_D3DAddRef),
    M("IDirect3DVertexBuffer7", "Release", k_VBRelease),
    M("IDirect3DVertexBuffer7", "Lock", k_VBLock),
    M("IDirect3DVertexBuffer7", "Unlock", k_VBUnlock),
    M("IDirect3DVertexBuffer7", "GetVertexBufferDesc", k_VBGetDesc),
    M("IDirect3DVertexBuffer7", "Optimize", k_VBOptimize),
    M("IDirect3DDevice7", "DrawPrimitiveVB", k_DrawPrimitiveVB),
    M("IDirect3DDevice7", "DrawIndexedPrimitiveVB", k_DrawIndexedPrimitiveVB),
    M("IDirect3D7", "AddRef", k_D3DAddRef),
    M("IDirect3D7", "Release", k_D3DRelease),
    M("IDirect3D7", "EnumDevices", k_EnumDevices),
    M("IDirect3D7", "CreateDevice", k_CreateDevice),
    M("IDirect3D7", "EnumZBufferFormats", k_EnumZBufferFormats),
    M("IDirect3D7", "EvictManagedTextures", k_EvictManagedTextures),

    M("IDirect3DDevice7", "AddRef", k_D3DAddRef),
    M("IDirect3DDevice7", "Release", k_DevRelease),
    M("IDirect3DDevice7", "GetCaps", k_DevGetCaps),
    M("IDirect3DDevice7", "EnumTextureFormats", k_EnumTextureFormats),
    M("IDirect3DDevice7", "BeginScene", k_BeginScene),
    M("IDirect3DDevice7", "EndScene", k_EndScene),
    M("IDirect3DDevice7", "SetRenderTarget", k_SetRenderTarget),
    M("IDirect3DDevice7", "GetRenderTarget", k_GetRenderTarget),
    M("IDirect3DDevice7", "Clear", k_Clear),
    M("IDirect3DDevice7", "SetTransform", k_SetTransform),
    M("IDirect3DDevice7", "GetTransform", k_GetTransform),
    M("IDirect3DDevice7", "SetViewport", k_SetViewport),
    M("IDirect3DDevice7", "GetViewport", k_GetViewport),
    M("IDirect3DDevice7", "SetMaterial", k_SetMaterial),
    M("IDirect3DDevice7", "SetLight", k_SetLight),
    M("IDirect3DDevice7", "LightEnable", k_LightEnable),
    M("IDirect3DDevice7", "SetRenderState", k_SetRenderState),
    M("IDirect3DDevice7", "GetRenderState", k_GetRenderState),
    M("IDirect3DDevice7", "PreLoad", k_PreLoad),
    M("IDirect3DDevice7", "DrawPrimitive", k_DrawPrimitive),
    M("IDirect3DDevice7", "DrawIndexedPrimitive", k_DrawIndexedPrimitive),
    M("IDirect3DDevice7", "GetTexture", k_GetTexture),
    M("IDirect3DDevice7", "SetTexture", k_SetTexture),
    M("IDirect3DDevice7", "GetTextureStageState", k_GetTextureStageState),
    M("IDirect3DDevice7", "SetTextureStageState", k_SetTextureStageState),
    M("IDirect3DDevice7", "ValidateDevice", k_ValidateDevice),
};
const uint32_t rc_d3d_count = sizeof rc_d3d_defs / sizeof rc_d3d_defs[0];
