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
#include <stdio.h>
#include <string.h>

#include "ddraw.h"
#include "rc/hle.h"
#include "rc/log.h"
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
    if (ndevices == DEVICES || !rc_dd_surf(surf)) {
        rc_st32(out, 0);
        RC_RETURN(c, DDERR_INVALIDPARAMS, 4);
    }
    device *d = &devices[ndevices++];
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

/* ---- the rasteriser ---- */

/* A colour as four floats 0..1. */
typedef struct col {
    float r, g, b, a;
} col;

static col unpack_argb(uint32_t v)
{
    return (col){(float)((v >> 16) & 255) / 255.0f, (float)((v >> 8) & 255) / 255.0f, (float)(v & 255) / 255.0f,
                 (float)(v >> 24) / 255.0f};
}

/* A channel of a pixel by its mask, 0..1 (1 if the mask is empty). */
static float channel(uint32_t v, uint32_t mask)
{
    if (!mask) {
        return 1.0f;
    }
    uint32_t shift = 0;
    while (!((mask >> shift) & 1)) {
        shift++;
    }
    uint32_t m = mask >> shift;
    return (float)((v >> shift) & m) / (float)m;
}

static uint32_t pack(float f, uint32_t mask)
{
    if (!mask) {
        return 0;
    }
    uint32_t shift = 0;
    while (!((mask >> shift) & 1)) {
        shift++;
    }
    uint32_t m = mask >> shift;
    f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
    return ((uint32_t)(f * (float)m + 0.5f) & m) << shift;
}

static uint32_t texel_raw(const rc_dd_surface *t, uint32_t x, uint32_t y)
{
    uint32_t a = t->pixels + y * t->pitch;
    switch (t->bpp) {
    case 8: return rc_ld8(a + x);
    case 16: return rc_ld16(a + 2 * x);
    case 32: return rc_ld32(a + 4 * x);
    default: return rc_ld8(a + 3 * x) | (uint32_t)rc_ld16(a + 3 * x + 1) << 8;
    }
}

static col decode(const rc_dd_surface *t, uint32_t v)
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
    return (col){channel(v, rm), channel(v, gm), channel(v, bm), t->amask ? channel(v, t->amask) : 1.0f};
}

static void target_masks(const rc_dd_surface *t, uint32_t *rm, uint32_t *gm, uint32_t *bm)
{
    *rm = t->rmask;
    *gm = t->gmask;
    *bm = t->bmask;
    if (!*rm && !*gm && !*bm) {
        if (t->bpp == 16) {
            *rm = 0xf800;
            *gm = 0x07e0;
            *bm = 0x001f;
        } else {
            *rm = 0xff0000;
            *gm = 0xff00;
            *bm = 0xff;
        }
    }
}

static uint32_t encode(const rc_dd_surface *t, col c)
{
    uint32_t rm, gm, bm;
    target_masks(t, &rm, &gm, &bm);
    return pack(c.r, rm) | pack(c.g, gm) | pack(c.b, bm) | pack(c.a, t->amask);
}

static void put_raw(const rc_dd_surface *t, uint32_t x, uint32_t y, uint32_t v)
{
    uint32_t a = t->pixels + y * t->pitch;
    switch (t->bpp) {
    case 8: rc_st8(a + x, (uint8_t)v); break;
    case 16: rc_st16(a + 2 * x, (uint16_t)v); break;
    case 32: rc_st32(a + 4 * x, v); break;
    default:
        rc_st8(a + 3 * x, (uint8_t)v);
        rc_st16(a + 3 * x + 1, (uint16_t)(v >> 8));
        break;
    }
}

static void clear_target(rc_dd_surface *s, uint32_t count, uint32_t rects, uint32_t argb)
{
    uint32_t v = encode(s, unpack_argb(argb));
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
            for (int32_t x = r[0]; x < r[2]; x++) {
                put_raw(s, (uint32_t)x, (uint32_t)y, v);
            }
        }
    }
}

/* A vertex after transformation: screen position, depth, 1/w, colours and
 * up to two texture coordinate sets. */
typedef struct vtx {
    float x, y, z, rhw;
    col diffuse;
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
    v->diffuse = l->has_diffuse ? unpack_argb(rc_ld32(a + l->diffuse)) : (col){1, 1, 1, 1};
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

static float wrap_coord(float f, uint32_t mode)
{
    switch (mode) {
    case 3: /* clamp */
    case 4: /* border */
        return f < 0.0f ? 0.0f : f > 0.99999f ? 0.99999f : f;
    case 2: { /* mirror */
        float m = f - 2.0f * floorf(f * 0.5f);
        return m > 1.0f ? 2.0f - m : m;
    }
    default:
        return f - floorf(f);
    }
}

static col sample(const device *d, int stage, float u, float v)
{
    const rc_dd_surface *t = rc_dd_surf(d->texture[stage]);
    if (!t || !t->w || !t->h) {
        return (col){1, 1, 1, 1};
    }
    uint32_t au = d->tss[stage][13] ? d->tss[stage][13] : d->tss[stage][12];
    uint32_t av = d->tss[stage][14] ? d->tss[stage][14] : d->tss[stage][12];
    u = wrap_coord(u, au) * (float)t->w;
    v = wrap_coord(v, av) * (float)t->h;
    bool linear = d->tss[stage][16] == 2 || d->tss[stage][17] == 2;
    uint32_t keyed = d->rs[41] && t->has_ckey; /* COLORKEYENABLE */
    if (!linear) {
        uint32_t x = (uint32_t)u, y = (uint32_t)v;
        x = x >= t->w ? t->w - 1 : x;
        y = y >= t->h ? t->h - 1 : y;
        uint32_t raw = texel_raw(t, x, y);
        col c = decode(t, raw);
        if (keyed && raw == t->ckey) {
            c.a = 0.0f;
        }
        return c;
    }
    /* Bilinear, with the colour key making a texel transparent. */
    float fu = u - 0.5f, fv = v - 0.5f;
    int x0 = (int)floorf(fu), y0 = (int)floorf(fv);
    float ax = fu - (float)x0, ay = fv - (float)y0;
    col acc = {0, 0, 0, 0};
    for (int k = 0; k < 4; k++) {
        int x = x0 + (k & 1), y = y0 + (k >> 1);
        if (au == 3 || au == 4) {
            x = x < 0 ? 0 : x >= (int)t->w ? (int)t->w - 1 : x;
        } else {
            x = ((x % (int)t->w) + (int)t->w) % (int)t->w;
        }
        if (av == 3 || av == 4) {
            y = y < 0 ? 0 : y >= (int)t->h ? (int)t->h - 1 : y;
        } else {
            y = ((y % (int)t->h) + (int)t->h) % (int)t->h;
        }
        uint32_t raw = texel_raw(t, (uint32_t)x, (uint32_t)y);
        col c = decode(t, raw);
        if (keyed && raw == t->ckey) {
            c = (col){0, 0, 0, 0};
        }
        float wgt = ((k & 1) ? ax : 1.0f - ax) * ((k >> 1) ? ay : 1.0f - ay);
        acc.r += c.r * c.a * wgt;
        acc.g += c.g * c.a * wgt;
        acc.b += c.b * c.a * wgt;
        acc.a += c.a * wgt;
    }
    if (acc.a > 0.0f) {
        acc.r /= acc.a;
        acc.g /= acc.a;
        acc.b /= acc.a;
    }
    return acc;
}

/* A texture stage argument. */
static col arg(const device *d, uint32_t a, col diffuse, col current, col tex)
{
    col c;
    switch (a & 0xf) {
    case 0: c = diffuse; break;
    case 2: c = tex; break;
    case 3: c = unpack_argb(d->rs[60]); break; /* TEXTUREFACTOR */
    default: c = current; break;
    }
    if (a & 0x10) { /* complement */
        c = (col){1 - c.r, 1 - c.g, 1 - c.b, 1 - c.a};
    }
    if (a & 0x20) { /* alpha replicate */
        c.r = c.g = c.b = c.a;
    }
    return c;
}

static float op1(uint32_t op, float a, float b, float alpha_tex, float alpha_diff, float alpha_cur)
{
    switch (op) {
    case 2: return a;                       /* SELECTARG1 */
    case 3: return b;                       /* SELECTARG2 */
    case 4: return a * b;                   /* MODULATE */
    case 5: return a * b * 2.0f;            /* MODULATE2X */
    case 6: return a * b * 4.0f;            /* MODULATE4X */
    case 7: return a + b;                   /* ADD */
    case 8: return a + b - 0.5f;            /* ADDSIGNED */
    case 9: return (a + b - 0.5f) * 2.0f;   /* ADDSIGNED2X */
    case 10: return a - b;                  /* SUBTRACT */
    case 11: return a + b - a * b;          /* ADDSMOOTH */
    case 12: return a * alpha_diff + b * (1 - alpha_diff); /* BLENDDIFFUSEALPHA */
    case 13: return a * alpha_tex + b * (1 - alpha_tex);   /* BLENDTEXTUREALPHA */
    case 16: return a * alpha_cur + b * (1 - alpha_cur);   /* BLENDCURRENTALPHA */
    default: return a * b;
    }
}

/* The colour of a fragment through the texture stages. */
static col shade(const device *d, col diffuse, const float *u, const float *v)
{
    col current = diffuse;
    for (int s = 0; s < 2; s++) {
        uint32_t cop = d->tss[s][1], aop = d->tss[s][4];
        if (cop == 1) { /* DISABLE: this and later stages */
            break;
        }
        col tex = d->texture[s] ? sample(d, s, u[s], v[s]) : (col){1, 1, 1, 1};
        col a1 = arg(d, d->tss[s][2], diffuse, current, tex), a2 = arg(d, d->tss[s][3], diffuse, current, tex);
        col out;
        out.r = op1(cop, a1.r, a2.r, tex.a, diffuse.a, current.a);
        out.g = op1(cop, a1.g, a2.g, tex.a, diffuse.a, current.a);
        out.b = op1(cop, a1.b, a2.b, tex.a, diffuse.a, current.a);
        if (aop != 1) {
            col b1 = arg(d, d->tss[s][5], diffuse, current, tex), b2 = arg(d, d->tss[s][6], diffuse, current, tex);
            out.a = op1(aop, b1.a, b2.a, tex.a, diffuse.a, current.a);
        } else {
            out.a = current.a;
        }
        current = out;
    }
    current.r = current.r < 0 ? 0 : current.r > 1 ? 1 : current.r;
    current.g = current.g < 0 ? 0 : current.g > 1 ? 1 : current.g;
    current.b = current.b < 0 ? 0 : current.b > 1 ? 1 : current.b;
    current.a = current.a < 0 ? 0 : current.a > 1 ? 1 : current.a;
    return current;
}

static bool compare(uint32_t fn, float a, float ref)
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

static col factor(uint32_t f, col src, col dst)
{
    switch (f) {
    case 1: return (col){0, 0, 0, 0};
    case 3: return src;
    case 4: return (col){1 - src.r, 1 - src.g, 1 - src.b, 1 - src.a};
    case 5: return (col){src.a, src.a, src.a, src.a};
    case 6: return (col){1 - src.a, 1 - src.a, 1 - src.a, 1 - src.a};
    case 7: return (col){dst.a, dst.a, dst.a, dst.a};
    case 8: return (col){1 - dst.a, 1 - dst.a, 1 - dst.a, 1 - dst.a};
    case 9: return dst;
    case 10: return (col){1 - dst.r, 1 - dst.g, 1 - dst.b, 1 - dst.a};
    case 11: {
        float f2 = src.a < 1 - dst.a ? src.a : 1 - dst.a;
        return (col){f2, f2, f2, 1};
    }
    default: return (col){1, 1, 1, 1};
    }
}

static void pixel(const device *d, rc_dd_surface *t, rc_dd_surface *z, int x, int y, float depth, col c)
{
    if (z && d->rs[7]) { /* ZENABLE */
        uint32_t za = z->pixels + (uint32_t)y * z->pitch + 2u * (uint32_t)x;
        float old = (float)rc_ld16(za) / 65535.0f;
        if (!compare(d->rs[23], depth, old)) {
            return;
        }
        if (d->rs[14]) {
            float dz = depth < 0 ? 0 : depth > 1 ? 1 : depth;
            rc_st16(za, (uint16_t)(dz * 65535.0f));
        }
    }
    if (d->rs[27]) { /* ALPHABLENDENABLE */
        uint32_t sb = d->rs[19], db = d->rs[20];
        col dst = decode(t, texel_raw(t, (uint32_t)x, (uint32_t)y));
        if (!t->amask) {
            dst.a = 1.0f;
        }
        if (sb == 12 || sb == 13) { /* BOTHSRCALPHA, BOTHINVSRCALPHA */
            db = sb == 12 ? 6 : 5;
            sb = sb == 12 ? 5 : 6;
        }
        col fs = factor(sb, c, dst), fd = factor(db, c, dst);
        c = (col){c.r * fs.r + dst.r * fd.r, c.g * fs.g + dst.g * fd.g, c.b * fs.b + dst.b * fd.b,
                  c.a * fs.a + dst.a * fd.a};
    }
    put_raw(t, (uint32_t)x, (uint32_t)y, encode(t, c));
}

static float edge(const vtx *a, const vtx *b, float x, float y) { return (b->x - a->x) * (y - a->y) - (b->y - a->y) * (x - a->x); }

static void triangle(const device *d, rc_dd_surface *t, rc_dd_surface *z, const vtx *v0, const vtx *v1, const vtx *v2)
{
    float area = edge(v0, v1, v2->x, v2->y);
    if (area == 0.0f) {
        return;
    }
    uint32_t cull = d->rs[22];
    if ((cull == 2 && area > 0) || (cull == 3 && area < 0)) {
        return; /* D3DCULL_CW, D3DCULL_CCW (y down: clockwise has positive area) */
    }
    if (area < 0) {
        const vtx *tmp = v1;
        v1 = v2;
        v2 = tmp;
        area = -area;
    }
    int32_t cx0 = (int32_t)d->vp.x, cy0 = (int32_t)d->vp.y;
    int32_t cx1 = cx0 + (int32_t)d->vp.w, cy1 = cy0 + (int32_t)d->vp.h;
    cx1 = cx1 > (int32_t)t->w ? (int32_t)t->w : cx1;
    cy1 = cy1 > (int32_t)t->h ? (int32_t)t->h : cy1;
    cx0 = cx0 < 0 ? 0 : cx0;
    cy0 = cy0 < 0 ? 0 : cy0;
    float minx = fminf(v0->x, fminf(v1->x, v2->x)), maxx = fmaxf(v0->x, fmaxf(v1->x, v2->x));
    float miny = fminf(v0->y, fminf(v1->y, v2->y)), maxy = fmaxf(v0->y, fmaxf(v1->y, v2->y));
    int32_t x0 = (int32_t)ceilf(minx - 0.5f), x1 = (int32_t)floorf(maxx - 0.5f);
    int32_t y0 = (int32_t)ceilf(miny - 0.5f), y1 = (int32_t)floorf(maxy - 0.5f);
    x0 = x0 < cx0 ? cx0 : x0;
    y0 = y0 < cy0 ? cy0 : y0;
    x1 = x1 >= cx1 ? cx1 - 1 : x1;
    y1 = y1 >= cy1 ? cy1 - 1 : y1;
    if (x0 > x1 || y0 > y1) {
        return;
    }
    bool flat = d->rs[9] == 1; /* SHADEMODE flat */
    bool alpha_test = d->rs[15] != 0;
    float aref = (float)(d->rs[24] & 255) / 255.0f;
    float inv = 1.0f / area;
    for (int32_t y = y0; y <= y1; y++) {
        float py = (float)y + 0.5f;
        for (int32_t x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f;
            float w0 = edge(v1, v2, px, py), w1 = edge(v2, v0, px, py), w2 = edge(v0, v1, px, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) {
                continue;
            }
            /* Top-left rule, roughly: an edge exactly through the centre
             * belongs to one side only. */
            if ((w0 == 0 && !(v2->y < v1->y || (v2->y == v1->y && v2->x < v1->x))) ||
                (w1 == 0 && !(v0->y < v2->y || (v0->y == v2->y && v0->x < v2->x))) ||
                (w2 == 0 && !(v1->y < v0->y || (v1->y == v0->y && v1->x < v0->x)))) {
                continue;
            }
            float b0 = w0 * inv, b1 = w1 * inv, b2 = w2 * inv;
            float depth = b0 * v0->z + b1 * v1->z + b2 * v2->z;
            /* Perspective-correct texture coordinates. */
            float q0 = b0 * v0->rhw, q1 = b1 * v1->rhw, q2 = b2 * v2->rhw;
            float q = q0 + q1 + q2;
            float iq = q != 0.0f ? 1.0f / q : 0.0f;
            float u[2], v[2];
            for (int s = 0; s < 2; s++) {
                u[s] = (q0 * v0->u[s] + q1 * v1->u[s] + q2 * v2->u[s]) * iq;
                v[s] = (q0 * v0->v[s] + q1 * v1->v[s] + q2 * v2->v[s]) * iq;
            }
            col dc;
            if (flat) {
                dc = v0->diffuse;
            } else {
                dc.r = b0 * v0->diffuse.r + b1 * v1->diffuse.r + b2 * v2->diffuse.r;
                dc.g = b0 * v0->diffuse.g + b1 * v1->diffuse.g + b2 * v2->diffuse.g;
                dc.b = b0 * v0->diffuse.b + b1 * v1->diffuse.b + b2 * v2->diffuse.b;
                dc.a = b0 * v0->diffuse.a + b1 * v1->diffuse.a + b2 * v2->diffuse.a;
            }
            col c = shade(d, dc, u, v);
            if (alpha_test && !compare(d->rs[25], c.a, aref)) {
                continue;
            }
            if (d->rs[41] && d->texture[0] && c.a <= 0.0f && !d->rs[27]) {
                continue; /* colour-keyed texel */
            }
            pixel(d, t, z, x, y, depth, c);
        }
    }
}

static void draw(device *d, uint32_t type, uint32_t fvf, uint32_t verts, uint32_t n, uint32_t idx, uint32_t nidx)
{
    rc_dd_surface *t = rc_dd_surf(d->target);
    if (!t || !t->pixels) {
        return;
    }
    rc_dd_surface *z = rc_dd_surf(t->zbuffer);
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
    for (uint32_t k = 0; k + 2 < count || (type <= 3 && k < count); k++) {
        uint32_t a, b, cc;
        switch (type) {
        case 4: /* triangle list */
            if (k % 3 || k + 2 >= count) {
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
            if (k == 0) {
                continue;
            }
            if (k + 1 >= count) {
                continue;
            }
            a = 0;
            b = k;
            cc = k + 1;
            break;
        default:
            continue; /* points and lines are not drawn */
        }
        uint32_t ids[3] = {INDEX(a), INDEX(b), INDEX(cc)};
        for (int j = 0; j < 3; j++) {
            ok[j] = ids[j] < n || !idx ? load_vertex(d, &l, wvp, verts, ids[j], &tri[j]) : false;
        }
        if (ok[0] && ok[1] && ok[2]) {
            triangle(d, t, z, &tri[0], &tri[1], &tri[2]);
        }
    }
#undef INDEX
}

/* ---- drawing ---- */

static void k_Clear(rc_cpu *c)
{
    device *d = dev_of(c);
    uint32_t count = RC_ARG(c, 1), rects = RC_ARG(c, 2), flags = RC_ARG(c, 3), color = RC_ARG(c, 4);
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
    M("IDirect3DDevice7", "Release", k_D3DRelease),
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
