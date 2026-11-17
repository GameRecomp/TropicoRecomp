/* SPDX-License-Identifier: MIT */
/*
 * DirectDraw, for software rendering: display modes, surfaces whose pixels
 * live in guest memory, blits, flips, palettes and clippers. Presenting the
 * primary surface converts it to the platform's 32-bit frame.
 *
 * COM objects are small guest blocks: a vtable pointer and the index of the
 * host object. A vtable is a guest array of reserved import addresses named
 * after the methods ("IDirectDrawSurface7::Blt"), so a method without an
 * implementation stops with its name, like any import. One host object has
 * a guest interface per version it was asked for (IDirectDraw, ...2, 4, 7).
 */
#include <stdio.h>
#include <string.h>

#include "ddraw.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define DDRAW "DDRAW.dll"
#define COM "DDRAW_COM"

#define DD_OK 0u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_UNSUPPORTED 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define DDERR_NOTFOUND 0x887600ffu
#define DDERR_OUTOFMEMORY 0x8007000eu

#define DDSD_CAPS 0x1u
#define DDSD_HEIGHT 0x2u
#define DDSD_WIDTH 0x4u
#define DDSD_PITCH 0x8u
#define DDSD_BACKBUFFERCOUNT 0x20u
#define DDSD_LPSURFACE 0x800u
#define DDSD_PIXELFORMAT 0x1000u
#define DDSD_CKSRCBLT 0x10000u
#define DDSD_REFRESHRATE 0x40000u

#define DDSCAPS_BACKBUFFER 0x4u
#define DDSCAPS_COMPLEX 0x8u
#define DDSCAPS_FLIP 0x10u
#define DDSCAPS_FRONTBUFFER 0x20u
#define DDSCAPS_OFFSCREENPLAIN 0x40u
#define DDSCAPS_PRIMARYSURFACE 0x200u
#define DDSCAPS_SYSTEMMEMORY 0x800u
#define DDSCAPS_VIDEOMEMORY 0x4000u
#define DDSCAPS_VISIBLE 0x8000u
#define DDSCAPS_LOCALVIDMEM 0x10000000u

#define DDPF_PALETTEINDEXED8 0x20u
#define DDPF_RGB 0x40u

#define DDBLT_COLORFILL 0x400u
#define DDBLT_KEYSRC 0x8000u
#define DDBLT_KEYSRCOVERRIDE 0x10000u
#define DDBLTFAST_SRCCOLORKEY 0x1u
#define DDCKEY_SRCBLT 0x8u

/* ---- host objects ---- */

enum kind {
    O_FREE,
    O_DDRAW,
    O_SURFACE,
    O_PALETTE,
    O_CLIPPER,
    O_D3D = RC_DD_D3D,
    O_DEVICE = RC_DD_DEVICE,
    O_VB = RC_DD_VB,
};

typedef rc_dd_surface surface;

typedef struct palette {
    uint32_t rgb[256]; /* 0x00RRGGBB */
    uint32_t caps;
} palette;

#define VERSIONS 8
typedef struct object {
    enum kind kind;
    uint32_t refs;
    uint32_t iface[VERSIONS]; /* guest interface per version */
    surface s;
    palette p;
    uint32_t hwnd;
} object;

#define OBJECTS 4096
static object objects[OBJECTS];

static struct {
    uint32_t w, h, bpp;
    uint32_t primary;
    uint32_t hwnd;
} display = {640, 480, 16, 0, 0};

/* ---- vtables ---- */

static const char *const ddraw_methods[] = {
    "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper", "CreatePalette", "CreateSurface",
    "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces", "FlipToGDISurface", "GetCaps", "GetDisplayMode",
    "GetFourCCCodes", "GetGDISurface", "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus",
    "Initialize", "RestoreDisplayMode", "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank",
    "GetAvailableVidMem", "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel",
    "GetDeviceIdentifier", "StartModeTest", "EvaluateMode",
};
static const char *const surface_methods[] = {
    "QueryInterface", "AddRef", "Release", "AddAttachedSurface", "AddOverlayDirtyRect", "Blt", "BltBatch",
    "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces", "EnumOverlayZOrders", "Flip",
    "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper", "GetColorKey", "GetDC", "GetFlipStatus",
    "GetOverlayPosition", "GetPalette", "GetPixelFormat", "GetSurfaceDesc", "Initialize", "IsLost", "Lock",
    "ReleaseDC", "Restore", "SetClipper", "SetColorKey", "SetOverlayPosition", "SetPalette", "Unlock",
    "UpdateOverlay", "UpdateOverlayDisplay", "UpdateOverlayZOrder", "GetDDInterface", "PageLock",
    "PageUnlock", "SetSurfaceDesc", "SetPrivateData", "GetPrivateData", "FreePrivateData",
    "GetUniquenessValue", "ChangeUniquenessValue", "SetPriority", "GetPriority", "SetLOD", "GetLOD",
};
static const char *const palette_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries", "Initialize", "SetEntries",
};
static const char *const clipper_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd", "Initialize", "IsClipListChanged",
    "SetClipList", "SetHWnd",
};
/* Direct3D 7 (d3d.c implements them). */
static const char *const d3d_methods[] = {
    "QueryInterface", "AddRef", "Release", "EnumDevices", "CreateDevice", "CreateVertexBuffer",
    "EnumZBufferFormats", "EvictManagedTextures",
};
static const char *const device_methods[] = {
    "QueryInterface", "AddRef", "Release", "GetCaps", "EnumTextureFormats", "BeginScene", "EndScene",
    "GetDirect3D", "SetRenderTarget", "GetRenderTarget", "Clear", "SetTransform", "GetTransform",
    "SetViewport", "MultiplyTransform", "GetViewport", "SetMaterial", "GetMaterial", "SetLight", "GetLight",
    "SetRenderState", "GetRenderState", "BeginStateBlock", "EndStateBlock", "PreLoad", "DrawPrimitive",
    "DrawIndexedPrimitive", "SetClipStatus", "GetClipStatus", "DrawPrimitiveStrided",
    "DrawIndexedPrimitiveStrided", "DrawPrimitiveVB", "DrawIndexedPrimitiveVB", "ComputeSphereVisibility",
    "GetTexture", "SetTexture", "GetTextureStageState", "SetTextureStageState", "ValidateDevice",
    "ApplyStateBlock", "CaptureStateBlock", "DeleteStateBlock", "CreateStateBlock", "Load", "LightEnable",
    "GetLightEnable", "SetClipPlane", "GetClipPlane", "GetInfo",
};
static const char *const vb_methods[] = {
    "QueryInterface", "AddRef", "Release", "Lock", "Unlock", "ProcessVertices", "GetVertexBufferDesc",
    "Optimize", "ProcessVerticesStrided",
};

/* Methods per interface version. */
static uint32_t method_count(enum kind k, int v)
{
    switch (k) {
    case O_DDRAW: return v == 1 ? 23 : v == 2 ? 24 : v == 4 ? 28 : 30;
    case O_SURFACE: return v == 1 ? 36 : v == 2 ? 39 : v == 3 ? 40 : v == 4 ? 45 : 49;
    case O_PALETTE: return 7;
    case O_D3D: return 8;
    case O_DEVICE: return 49;
    case O_VB: return 9;
    default: return 9;
    }
}

static const char *iface_name(enum kind k)
{
    switch (k) {
    case O_DDRAW: return "IDirectDraw";
    case O_SURFACE: return "IDirectDrawSurface";
    case O_PALETTE: return "IDirectDrawPalette";
    case O_D3D: return "IDirect3D7";
    case O_DEVICE: return "IDirect3DDevice7";
    case O_VB: return "IDirect3DVertexBuffer7";
    default: return "IDirectDrawClipper";
    }
}

/* Method names that need a version of their own, because the signature
 * differs between versions. */
static const char *method_name(enum kind k, int v, uint32_t i, char *buf, size_t n)
{
    const char *m = k == O_DDRAW ? ddraw_methods[i]
                  : k == O_SURFACE ? surface_methods[i]
                  : k == O_PALETTE ? palette_methods[i]
                  : k == O_D3D ? d3d_methods[i]
                  : k == O_DEVICE ? device_methods[i]
                  : k == O_VB ? vb_methods[i]
                  : clipper_methods[i];
    int versioned = (k == O_DDRAW && v == 1 && !strcmp(m, "SetDisplayMode")) ||
                    (k == O_DDRAW && v >= 4 && (!strcmp(m, "EnumDisplayModes") || !strcmp(m, "GetDisplayMode"))) ||
                    (k == O_SURFACE && v >= 4 && (!strcmp(m, "GetCaps") || !strcmp(m, "GetSurfaceDesc") ||
                                                  !strcmp(m, "GetAttachedSurface")));
    snprintf(buf, n, "%s%s::%s", iface_name(k), versioned ? (v == 1 ? "1" : "4") : "", m);
    return buf;
}

static uint32_t vtables[16][VERSIONS];

static uint32_t vtable(enum kind k, int v)
{
    if (vtables[k][v]) {
        return vtables[k][v];
    }
    uint32_t n = method_count(k, v);
    uint32_t t = rc_heap_alloc(4 * n, false);
    for (uint32_t i = 0; i < n; i++) {
        char buf[96];
        method_name(k, v, i, buf, sizeof buf);
        /* The registry keeps the pointer: give it storage that lasts. */
        rc_st32(t + 4 * i, rc_import_address(COM, rc_str(rc_proc_str(buf))));
    }
    vtables[k][v] = t;
    return t;
}

static uint32_t new_object(enum kind k)
{
    for (uint32_t i = 1; i < OBJECTS; i++) {
        if (objects[i].kind == O_FREE) {
            memset(&objects[i], 0, sizeof objects[i]);
            objects[i].kind = k;
            objects[i].refs = 1;
            return i;
        }
    }
    rc_fatal("DirectDraw: out of objects");
}

/* The guest interface of object `o` for version `v`. */
static uint32_t iface(uint32_t o, int v)
{
    object *x = &objects[o];
    if (!x->iface[v]) {
        uint32_t g = rc_heap_alloc(12, false);
        rc_st32(g, vtable(x->kind, v));
        rc_st32(g + 4, o);
        rc_st32(g + 8, (uint32_t)v);
        x->iface[v] = g;
    }
    return x->iface[v];
}

static object *self(rc_cpu *c, enum kind k)
{
    uint32_t g = RC_ARG(c, 0);
    uint32_t o = g ? rc_ld32(g + 4) : 0;
    if (!o || o >= OBJECTS || objects[o].kind != k) {
        rc_fatal("DirectDraw: bad this pointer %#x for %s", g, iface_name(k));
    }
    return &objects[o];
}

static int self_version(rc_cpu *c) { return (int)rc_ld32(RC_ARG(c, 0) + 8); }

/* ---- for d3d.c ---- */

uint32_t rc_dd_new(int kind) { return new_object((enum kind)kind); }

uint32_t rc_dd_iface(uint32_t o, int v) { return iface(o, v); }

uint32_t rc_dd_self(rc_cpu *c, int kind) { return (uint32_t)(self(c, (enum kind)kind) - objects); }

uint32_t rc_dd_object_of(uint32_t guest, int kind)
{
    uint32_t o = guest ? rc_ld32(guest + 4) : 0;
    return o && o < OBJECTS && objects[o].kind == (enum kind)kind ? o : 0;
}

rc_dd_surface *rc_dd_surf(uint32_t o) { return o && o < OBJECTS && objects[o].kind == O_SURFACE ? &objects[o].s : NULL; }

uint32_t *rc_dd_refs(uint32_t o) { return &objects[o].refs; }

/* ---- pixel formats ---- */

static void format_for(uint32_t bpp, uint32_t *r, uint32_t *g, uint32_t *b)
{
    switch (bpp) {
    case 16: *r = 0xf800; *g = 0x07e0; *b = 0x001f; break;
    case 24:
    case 32: *r = 0xff0000; *g = 0x00ff00; *b = 0x0000ff; break;
    default: *r = *g = *b = 0; break;
    }
}

static void put_pixel_format(uint32_t p, const surface *s)
{
    memset(rc_ptr(p), 0, 32);
    rc_st32(p, 32);
    rc_st32(p + 4, s->bpp == 8 ? DDPF_PALETTEINDEXED8 | DDPF_RGB : DDPF_RGB | (s->amask ? 0x1u : 0u));
    rc_st32(p + 12, s->bpp);
    rc_st32(p + 16, s->rmask);
    rc_st32(p + 20, s->gmask);
    rc_st32(p + 24, s->bmask);
    rc_st32(p + 28, s->amask);
}

/* Fill a DDSURFACEDESC(2) for a surface; `lp` is the pixel address. */
static void put_desc(uint32_t d, const surface *s, uint32_t lp)
{
    uint32_t size = rc_ld32(d);
    memset(rc_ptr(d + 4), 0, (size > 4 && size <= 124 ? size : 108) - 4);
    rc_st32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT |
                       (lp ? DDSD_LPSURFACE : 0) | (s->back ? DDSD_BACKBUFFERCOUNT : 0));
    rc_st32(d + 8, s->h);
    rc_st32(d + 12, s->w);
    rc_st32(d + 16, s->pitch);
    rc_st32(d + 20, s->back ? 1 : 0);
    rc_st32(d + 36, lp);
    if (s->has_ckey) {
        rc_st32(d + 4, rc_ld32(d + 4) | DDSD_CKSRCBLT);
        rc_st32(d + 64, s->ckey);
        rc_st32(d + 68, s->ckey);
    }
    put_pixel_format(d + 72, s);
    rc_st32(d + 104, s->caps);
}

/* ---- presenting ---- */

static uint32_t frame[2048 * 2048];

static uint32_t to_rgb(const surface *s, const palette *p, uint32_t v)
{
    switch (s->bpp) {
    case 8:
        return p ? p->rgb[v & 0xff] : (v & 0xff) * 0x010101u;
    case 16: {
        uint32_t r, g, b;
        if (s->gmask == 0x07e0) {
            r = (v >> 11) & 31;
            g = (v >> 5) & 63;
            b = v & 31;
            return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
        r = (v >> 10) & 31;
        g = (v >> 5) & 31;
        b = v & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
    }
    default:
        return v & 0xffffff;
    }
}

static uint32_t pixel_at(const surface *s, uint32_t x, uint32_t y)
{
    uint32_t a = s->pixels + y * s->pitch;
    switch (s->bpp) {
    case 8: return rc_ld8(a + x);
    case 16: return rc_ld16(a + 2 * x);
    case 24: return rc_ld8(a + 3 * x) | (uint32_t)rc_ld16(a + 3 * x + 1) << 8;
    default: return rc_ld32(a + 4 * x);
    }
}

static void put_pixel(const surface *s, uint32_t x, uint32_t y, uint32_t v)
{
    uint32_t a = s->pixels + y * s->pitch;
    switch (s->bpp) {
    case 8: rc_st8(a + x, (uint8_t)v); break;
    case 16: rc_st16(a + 2 * x, (uint16_t)v); break;
    case 24:
        rc_st8(a + 3 * x, (uint8_t)v);
        rc_st16(a + 3 * x + 1, (uint16_t)(v >> 8));
        break;
    default: rc_st32(a + 4 * x, v); break;
    }
}

static unsigned frames;

static void present(void)
{
    if (!display.primary) {
        return;
    }
    const surface *s = &objects[display.primary].s;
    const palette *p = s->palette ? &objects[s->palette].p : NULL;
    if ((uint64_t)s->w * s->h > sizeof frame / 4) {
        return;
    }
    for (uint32_t y = 0; y < s->h; y++) {
        for (uint32_t x = 0; x < s->w; x++) {
            frame[y * s->w + x] = to_rgb(s, p, pixel_at(s, x, y));
        }
    }
    rc_plat_present(frame, (int)s->w, (int)s->h, (int)s->w);
    frames++;
    rc_present_hook(frames);
}

/* ---- surfaces ---- */

static uint32_t make_surface(uint32_t w, uint32_t h, uint32_t bpp, uint32_t caps)
{
    uint32_t o = new_object(O_SURFACE);
    surface *s = &objects[o].s;
    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->pitch = (w * (bpp / 8) + 3) & ~3u;
    s->caps = caps;
    format_for(bpp, &s->rmask, &s->gmask, &s->bmask);
    s->pixels = rc_heap_alloc(s->pitch * h, true);
    if (!s->pixels) {
        objects[o].kind = O_FREE;
        return 0;
    }
    return o;
}

static void k_CreateSurface(rc_cpu *c)
{
    self(c, O_DDRAW);
    int v = self_version(c);
    uint32_t d = RC_ARG(c, 1), out = RC_ARG(c, 2);
    uint32_t flags = rc_ld32(d + 4);
    uint32_t caps = (flags & DDSD_CAPS) ? rc_ld32(d + 104) : 0;
    uint32_t w = (flags & DDSD_WIDTH) ? rc_ld32(d + 12) : display.w;
    uint32_t h = (flags & DDSD_HEIGHT) ? rc_ld32(d + 8) : display.h;
    uint32_t bpp = display.bpp;
    if (flags & DDSD_PIXELFORMAT) {
        bpp = rc_ld32(d + 72 + 12);
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = display.w;
        h = display.h;
        bpp = display.bpp;
    }
    caps |= (caps & DDSCAPS_SYSTEMMEMORY) ? 0 : DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
    uint32_t o = make_surface(w, h, bpp, caps | ((caps & DDSCAPS_PRIMARYSURFACE) ? DDSCAPS_VISIBLE | DDSCAPS_FRONTBUFFER : 0));
    if (!o) {
        RC_RETURN(c, DDERR_OUTOFMEMORY, 4);
    }
    if ((flags & DDSD_PIXELFORMAT) && (bpp == 16 || bpp == 32) && (rc_ld32(d + 72 + 4) & 0x40)) {
        /* DDPF_RGB: its masks, and the alpha mask with DDPF_ALPHAPIXELS. */
        surface *s = &objects[o].s;
        s->rmask = rc_ld32(d + 72 + 16);
        s->gmask = rc_ld32(d + 72 + 20);
        s->bmask = rc_ld32(d + 72 + 24);
        s->amask = (rc_ld32(d + 72 + 4) & 0x1) ? rc_ld32(d + 72 + 28) : 0;
    }
    if (flags & DDSD_CKSRCBLT) {
        objects[o].s.ckey = rc_ld32(d + 64);
        objects[o].s.has_ckey = 1;
    }
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        display.primary = o;
        if ((flags & DDSD_BACKBUFFERCOUNT) && rc_ld32(d + 20) > 0) {
            uint32_t b = make_surface(w, h, bpp, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX |
                                                     DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM);
            objects[o].s.back = b;
        }
    }
    rc_st32(out, iface(o, v));
    RC_RETURN(c, DD_OK, 4);
}

static void k_SurfaceQueryInterface(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    uint32_t iid = rc_ld32(RC_ARG(c, 1)), out = RC_ARG(c, 2);
    int v = iid == 0x6c14db81u ? 1 : iid == 0x57805885u ? 2 : iid == 0xda044e00u ? 3
          : iid == 0x0b2b8630u ? 4 : iid == 0x06675a80u ? 7 : 0;
    if (!v) {
        rc_st32(out, 0);
        RC_RETURN(c, E_NOINTERFACE, 3);
    }
    x->refs++;
    rc_st32(out, iface((uint32_t)(x - objects), v));
    RC_RETURN(c, DD_OK, 3);
}

static void k_AddRef(rc_cpu *c)
{
    uint32_t o = rc_ld32(RC_ARG(c, 0) + 4);
    RC_RETURN(c, ++objects[o].refs, 1);
}

static void k_Release(rc_cpu *c)
{
    uint32_t o = rc_ld32(RC_ARG(c, 0) + 4);
    object *x = &objects[o];
    if (x->refs > 0 && --x->refs == 0) {
        if (x->kind == O_SURFACE) {
            if (x->s.back) {
                rc_heap_free(objects[x->s.back].s.pixels);
                objects[x->s.back].kind = O_FREE;
            }
            rc_heap_free(x->s.pixels);
            if (display.primary == o) {
                display.primary = 0;
            }
        }
        /* The guest interfaces stay allocated: stale pointers fail loudly. */
        x->kind = O_FREE;
    }
    RC_RETURN(c, x->refs, 1);
}

static void k_GetSurfaceDesc(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    put_desc(RC_ARG(c, 1), &x->s, 0);
    RC_RETURN(c, DD_OK, 2);
}

static void k_GetPixelFormat(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    put_pixel_format(RC_ARG(c, 1), &x->s);
    RC_RETURN(c, DD_OK, 2);
}

static void k_SurfaceGetCaps(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    rc_st32(RC_ARG(c, 1), x->s.caps);
    RC_RETURN(c, DD_OK, 2);
}

static void k_SurfaceGetCaps4(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    uint32_t p = RC_ARG(c, 1);
    memset(rc_ptr(p), 0, 16);
    rc_st32(p, x->s.caps);
    RC_RETURN(c, DD_OK, 2);
}

static void k_GetAttachedSurface(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    uint32_t caps = rc_ld32(RC_ARG(c, 1)), out = RC_ARG(c, 2);
    if ((caps & DDSCAPS_BACKBUFFER) && x->s.back) {
        objects[x->s.back].refs++;
        rc_st32(out, iface(x->s.back, self_version(c)));
        RC_RETURN(c, DD_OK, 3);
    }
    rc_st32(out, 0);
    RC_RETURN(c, DDERR_NOTFOUND, 3);
}

static void k_Lock(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    uint32_t rect = RC_ARG(c, 1), d = RC_ARG(c, 2);
    surface *s = &x->s;
    uint32_t lp = s->pixels;
    if (rect) {
        lp += rc_ld32(rect + 4) * s->pitch + rc_ld32(rect) * (s->bpp / 8);
    }
    put_desc(d, s, lp);
    s->locked++;
    RC_RETURN(c, DD_OK, 5);
}

static void k_Unlock(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    if (x->s.locked) {
        x->s.locked--;
    }
    if ((uint32_t)(x - objects) == display.primary) {
        present();
    }
    RC_RETURN(c, DD_OK, 2);
}

static void k_IsLost(rc_cpu *c)
{
    self(c, O_SURFACE);
    RC_RETURN(c, DD_OK, 1);
}

static void k_Restore(rc_cpu *c)
{
    self(c, O_SURFACE);
    RC_RETURN(c, DD_OK, 1);
}

static void k_Flip(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    surface *s = &x->s;
    if (s->back) {
        surface *b = &objects[s->back].s;
        uint32_t t = s->pixels;
        s->pixels = b->pixels;
        b->pixels = t;
    }
    if ((uint32_t)(x - objects) == display.primary) {
        present();
    }
    RC_RETURN(c, DD_OK, 3);
}

static void k_SetColorKey(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    uint32_t flags = RC_ARG(c, 1), key = RC_ARG(c, 2);
    if (flags & DDCKEY_SRCBLT) {
        if (key) {
            x->s.ckey = rc_ld32(key);
            x->s.has_ckey = 1;
        } else {
            x->s.has_ckey = 0;
        }
    }
    RC_RETURN(c, DD_OK, 3);
}

static void k_GetColorKey(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    if (!x->s.has_ckey) {
        RC_RETURN(c, 0x88760173u, 3); /* DDERR_NOCOLORKEY */
    }
    rc_st32(RC_ARG(c, 2), x->s.ckey);
    rc_st32(RC_ARG(c, 2) + 4, x->s.ckey);
    RC_RETURN(c, DD_OK, 3);
}

static void k_SetPalette(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    uint32_t g = RC_ARG(c, 1);
    x->s.palette = g ? rc_ld32(g + 4) : 0;
    RC_RETURN(c, DD_OK, 2);
}

static void k_SetClipper(rc_cpu *c)
{
    self(c, O_SURFACE);
    RC_RETURN(c, DD_OK, 2);
}

/* A rectangle from guest memory, or the whole surface; clipped to it. */
typedef struct rect {
    int32_t l, t, r, b;
} rect;

static rect get_rect(uint32_t p, const surface *s)
{
    rect r = {0, 0, (int32_t)s->w, (int32_t)s->h};
    if (p) {
        r.l = (int32_t)rc_ld32(p);
        r.t = (int32_t)rc_ld32(p + 4);
        r.r = (int32_t)rc_ld32(p + 8);
        r.b = (int32_t)rc_ld32(p + 12);
    }
    return r;
}

/* Copy src rectangle `sr` onto dst rectangle `dr`, scaling by nearest
 * neighbour if the sizes differ, skipping the colour key if asked. */
static void blit(surface *d, rect dr, const surface *s, rect sr, int keyed, uint32_t key)
{
    int32_t dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) {
        return;
    }
    int same = d->bpp == s->bpp;
    for (int32_t y = 0; y < dh; y++) {
        int32_t dy = dr.t + y;
        if (dy < 0 || dy >= (int32_t)d->h) {
            continue;
        }
        int32_t sy = sr.t + (int32_t)((int64_t)y * sh / dh);
        if (sy < 0 || sy >= (int32_t)s->h) {
            continue;
        }
        if (same && !keyed && dw == sw && dr.l >= 0 && dr.r <= (int32_t)d->w && sr.l >= 0 && sr.r <= (int32_t)s->w) {
            uint32_t bytes = s->bpp / 8;
            memmove(rc_ptr(d->pixels + (uint32_t)dy * d->pitch + (uint32_t)dr.l * bytes),
                    rc_ptr(s->pixels + (uint32_t)sy * s->pitch + (uint32_t)sr.l * bytes), (size_t)dw * bytes);
            continue;
        }
        for (int32_t x = 0; x < dw; x++) {
            int32_t dx = dr.l + x;
            int32_t sx = sr.l + (int32_t)((int64_t)x * sw / dw);
            if (dx < 0 || dx >= (int32_t)d->w || sx < 0 || sx >= (int32_t)s->w) {
                continue;
            }
            uint32_t v = pixel_at(s, (uint32_t)sx, (uint32_t)sy);
            if (keyed && v == key) {
                continue;
            }
            put_pixel(d, (uint32_t)dx, (uint32_t)dy, v);
        }
    }
}

static void fill(surface *d, rect r, uint32_t color)
{
    for (int32_t y = r.t < 0 ? 0 : r.t; y < r.b && y < (int32_t)d->h; y++) {
        for (int32_t x = r.l < 0 ? 0 : r.l; x < r.r && x < (int32_t)d->w; x++) {
            put_pixel(d, (uint32_t)x, (uint32_t)y, color);
        }
    }
}

static surface *surface_of(uint32_t g)
{
    uint32_t o = g ? rc_ld32(g + 4) : 0;
    return o && o < OBJECTS && objects[o].kind == O_SURFACE ? &objects[o].s : NULL;
}

static void k_Blt(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    uint32_t drp = RC_ARG(c, 1), src = RC_ARG(c, 2), srp = RC_ARG(c, 3), flags = RC_ARG(c, 4), fx = RC_ARG(c, 5);
    surface *d = &x->s;
    rect dr = get_rect(drp, d);
    if (flags & DDBLT_COLORFILL) {
        fill(d, dr, rc_ld32(fx + 80));
    } else {
        surface *s = surface_of(src);
        if (!s) {
            RC_RETURN(c, DDERR_INVALIDPARAMS, 6);
        }
        int keyed = (flags & (DDBLT_KEYSRC | DDBLT_KEYSRCOVERRIDE)) != 0;
        uint32_t key = (flags & DDBLT_KEYSRCOVERRIDE) ? rc_ld32(fx + 100) : s->ckey;
        blit(d, dr, s, get_rect(srp, s), keyed && (s->has_ckey || (flags & DDBLT_KEYSRCOVERRIDE)), key);
    }
    if ((uint32_t)(x - objects) == display.primary) {
        present();
    }
    RC_RETURN(c, DD_OK, 6);
}

static void k_BltFast(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    int32_t dx = (int32_t)RC_ARG(c, 1), dy = (int32_t)RC_ARG(c, 2);
    uint32_t src = RC_ARG(c, 3), srp = RC_ARG(c, 4), trans = RC_ARG(c, 5);
    surface *s = surface_of(src);
    if (!s) {
        RC_RETURN(c, DDERR_INVALIDPARAMS, 6);
    }
    rect sr = get_rect(srp, s);
    rect dr = {dx, dy, dx + (sr.r - sr.l), dy + (sr.b - sr.t)};
    blit(&x->s, dr, s, sr, (trans & DDBLTFAST_SRCCOLORKEY) && s->has_ckey, s->ckey);
    if ((uint32_t)(x - objects) == display.primary) {
        present();
    }
    RC_RETURN(c, DD_OK, 6);
}

static void k_GetDC(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    const surface *s = &x->s;
    const palette *p = s->palette ? &objects[s->palette].p : NULL;
    rc_st32(RC_ARG(c, 1), rc_gdi_surface_dc(s->pixels, s->w, s->h, s->pitch, s->bpp, s->rmask, s->gmask,
                                             s->bmask, p ? p->rgb : NULL));
    RC_RETURN(c, DD_OK, 2);
}

static void k_ReleaseDC(rc_cpu *c)
{
    object *x = self(c, O_SURFACE);
    rc_gdi_release_surface_dc(RC_ARG(c, 1));
    if ((uint32_t)(x - objects) == display.primary) {
        present();
    }
    RC_RETURN(c, DD_OK, 2);
}

/* ---- palettes and clippers ---- */

static void k_CreatePalette(rc_cpu *c)
{
    self(c, O_DDRAW);
    uint32_t flags = RC_ARG(c, 1), entries = RC_ARG(c, 2), out = RC_ARG(c, 3);
    uint32_t o = new_object(O_PALETTE);
    palette *p = &objects[o].p;
    p->caps = flags;
    for (uint32_t i = 0; i < 256 && entries; i++) {
        uint32_t e = rc_ld32(entries + 4 * i); /* PALETTEENTRY: r, g, b, flags */
        p->rgb[i] = ((e & 0xff) << 16) | (e & 0xff00) | ((e >> 16) & 0xff);
    }
    rc_st32(out, iface(o, 1));
    RC_RETURN(c, DD_OK, 4);
}

static void k_SetEntries(rc_cpu *c)
{
    object *x = self(c, O_PALETTE);
    uint32_t start = RC_ARG(c, 2), count = RC_ARG(c, 3), entries = RC_ARG(c, 4);
    for (uint32_t i = 0; i < count && start + i < 256; i++) {
        uint32_t e = rc_ld32(entries + 4 * i);
        x->p.rgb[start + i] = ((e & 0xff) << 16) | (e & 0xff00) | ((e >> 16) & 0xff);
    }
    if (display.primary && objects[display.primary].s.palette == (uint32_t)(x - objects)) {
        present();
    }
    RC_RETURN(c, DD_OK, 5);
}

static void k_GetEntries(rc_cpu *c)
{
    object *x = self(c, O_PALETTE);
    uint32_t start = RC_ARG(c, 2), count = RC_ARG(c, 3), entries = RC_ARG(c, 4);
    for (uint32_t i = 0; i < count && start + i < 256; i++) {
        uint32_t v = x->p.rgb[start + i];
        rc_st32(entries + 4 * i, ((v >> 16) & 0xff) | (v & 0xff00) | ((v & 0xff) << 16));
    }
    RC_RETURN(c, DD_OK, 5);
}

static void k_CreateClipper(rc_cpu *c)
{
    self(c, O_DDRAW);
    uint32_t o = new_object(O_CLIPPER);
    rc_st32(RC_ARG(c, 2), iface(o, 1));
    RC_RETURN(c, DD_OK, 4);
}

/* A clip list (RGNDATA): only its bounds are kept; blits are clipped to the
 * surface anyway. */
static void k_ClipperSetClipList(rc_cpu *c)
{
    object *x = self(c, O_CLIPPER);
    uint32_t rgn = RC_ARG(c, 1);
    x->hwnd = 0;
    if (rc_trace("ddraw") && rgn) {
        rc_log("[ddraw] clip list of %u rectangles, bounds %d,%d-%d,%d", rc_ld32(rgn + 8),
               (int32_t)rc_ld32(rgn + 16), (int32_t)rc_ld32(rgn + 20), (int32_t)rc_ld32(rgn + 24),
               (int32_t)rc_ld32(rgn + 28));
    }
    RC_RETURN(c, DD_OK, 3);
}

static void k_ClipperSetHWnd(rc_cpu *c)
{
    object *x = self(c, O_CLIPPER);
    x->hwnd = RC_ARG(c, 2);
    RC_RETURN(c, DD_OK, 3);
}

/* ---- the DirectDraw object ---- */

static void k_DDQueryInterface(rc_cpu *c)
{
    object *x = self(c, O_DDRAW);
    uint32_t iid = rc_ld32(RC_ARG(c, 1)), out = RC_ARG(c, 2);
    int v = iid == 0x6c14db80u ? 1 : iid == 0xb3a6f3e0u ? 2 : iid == 0x9c59509au ? 4 : iid == 0x15e65ec0u ? 7 : 0;
    if (iid == 0xf5049e77u) { /* IID_IDirect3D7 */
        rc_st32(out, rc_d3d_create());
        RC_RETURN(c, DD_OK, 3);
    }
    if (!v) {
        if (rc_trace("ddraw")) {
            rc_log("[ddraw] QueryInterface for %08x: not offered", iid);
        }
        rc_st32(out, 0);
        RC_RETURN(c, E_NOINTERFACE, 3);
    }
    x->refs++;
    rc_st32(out, iface((uint32_t)(x - objects), v));
    RC_RETURN(c, DD_OK, 3);
}

static void k_SetCooperativeLevel(rc_cpu *c)
{
    self(c, O_DDRAW);
    display.hwnd = RC_ARG(c, 1);
    RC_RETURN(c, DD_OK, 3);
}

void rc_ddraw_mode(uint32_t *w, uint32_t *h)
{
    *w = display.w;
    *h = display.h;
}

static uint32_t extra_w, extra_h;

void rc_ddraw_extra_mode(uint32_t w, uint32_t h)
{
    extra_w = w;
    extra_h = h;
}

static void set_mode(uint32_t w, uint32_t h, uint32_t bpp)
{
    display.w = w;
    display.h = h;
    display.bpp = bpp;
    rc_window_resize(display.hwnd ? display.hwnd : rc_main_window(), (int32_t)w, (int32_t)h);
    if (rc_trace("ddraw")) {
        rc_log("[ddraw] display mode %ux%ux%u", w, h, bpp);
    }
}

static void k_SetDisplayMode1(rc_cpu *c)
{
    self(c, O_DDRAW);
    set_mode(RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3));
    RC_RETURN(c, DD_OK, 4);
}

static void k_SetDisplayMode(rc_cpu *c)
{
    self(c, O_DDRAW);
    set_mode(RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3));
    RC_RETURN(c, DD_OK, 6);
}

static void k_RestoreDisplayMode(rc_cpu *c)
{
    self(c, O_DDRAW);
    RC_RETURN(c, DD_OK, 1);
}

static void mode_desc(uint32_t d, uint32_t size, uint32_t w, uint32_t h, uint32_t bpp)
{
    memset(rc_ptr(d), 0, size);
    rc_st32(d, size);
    rc_st32(d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_REFRESHRATE);
    rc_st32(d + 8, h);
    rc_st32(d + 12, w);
    rc_st32(d + 16, w * (bpp / 8));
    rc_st32(d + 24, 60);
    surface s = {.bpp = bpp};
    format_for(bpp, &s.rmask, &s.gmask, &s.bmask);
    put_pixel_format(d + 72, &s);
}

static void enum_modes(rc_cpu *c, uint32_t size)
{
    uint32_t ctx = RC_ARG(c, 3), fn = RC_ARG(c, 4);
    static const uint32_t sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 1024}, {1600, 1200}};
    static const uint32_t depths[] = {8, 16, 32};
    uint32_t d = rc_heap_alloc(size, true);
    unsigned n = sizeof sizes / sizeof sizes[0];
    for (unsigned i = 0; i <= n; i++) {
        uint32_t w = i < n ? sizes[i][0] : extra_w, h = i < n ? sizes[i][1] : extra_h;
        if (!w || !h) {
            continue;
        }
        for (unsigned j = 0; j < 3; j++) {
            mode_desc(d, size, w, h, depths[j]);
            uint32_t args[2] = {d, ctx};
            if (!rc_call_guest(c, fn, 2, args)) {
                rc_heap_free(d);
                return;
            }
        }
    }
    rc_heap_free(d);
}

static void k_EnumDisplayModes(rc_cpu *c)
{
    self(c, O_DDRAW);
    enum_modes(c, 108);
    RC_RETURN(c, DD_OK, 5);
}

static void k_EnumDisplayModes4(rc_cpu *c)
{
    self(c, O_DDRAW);
    enum_modes(c, 124);
    RC_RETURN(c, DD_OK, 5);
}

static void k_GetDisplayMode(rc_cpu *c)
{
    self(c, O_DDRAW);
    mode_desc(RC_ARG(c, 1), 108, display.w, display.h, display.bpp);
    RC_RETURN(c, DD_OK, 2);
}

static void k_GetDisplayMode4(rc_cpu *c)
{
    self(c, O_DDRAW);
    mode_desc(RC_ARG(c, 1), 124, display.w, display.h, display.bpp);
    RC_RETURN(c, DD_OK, 2);
}

static void k_DDGetCaps(rc_cpu *c)
{
    self(c, O_DDRAW);
    uint32_t caps[2] = {RC_ARG(c, 1), RC_ARG(c, 2)};
    for (int i = 0; i < 2; i++) {
        uint32_t p = caps[i];
        if (!p) {
            continue;
        }
        uint32_t size = rc_ld32(p);
        if (size < 8 || size > 1024) {
            size = 316;
        }
        memset(rc_ptr(p + 4), 0, size - 4);
        if (i == 0) {
            rc_st32(p + 4, 0x00000040u | 0x00000400u); /* DDCAPS_BLT | DDCAPS_COLORKEY */
            rc_st32(p + 60, 64u << 20);                /* dwVidMemTotal */
            rc_st32(p + 64, 48u << 20);                /* dwVidMemFree */
        }
    }
    RC_RETURN(c, DD_OK, 3);
}

static void k_GetAvailableVidMem(rc_cpu *c)
{
    self(c, O_DDRAW);
    if (RC_ARG(c, 2)) {
        rc_st32(RC_ARG(c, 2), 64u << 20);
    }
    if (RC_ARG(c, 3)) {
        rc_st32(RC_ARG(c, 3), 48u << 20);
    }
    RC_RETURN(c, DD_OK, 4);
}

static void k_WaitForVerticalBlank(rc_cpu *c)
{
    self(c, O_DDRAW);
    RC_RETURN(c, DD_OK, 3);
}

static void k_DDRelease(rc_cpu *c)
{
    object *x = self(c, O_DDRAW);
    if (x->refs > 0) {
        x->refs--;
    }
    RC_RETURN(c, x->refs, 1);
}

/* ---- exports ---- */

static uint32_t the_ddraw;

static void k_DirectDrawCreate(rc_cpu *c)
{
    uint32_t out = RC_ARG(c, 1);
    if (!the_ddraw) {
        the_ddraw = new_object(O_DDRAW);
    }
    objects[the_ddraw].refs++;
    rc_st32(out, iface(the_ddraw, 1));
    RC_RETURN(c, DD_OK, 3);
}

static void k_DirectDrawCreateEx(rc_cpu *c)
{
    uint32_t out = RC_ARG(c, 1);
    if (!the_ddraw) {
        the_ddraw = new_object(O_DDRAW);
    }
    objects[the_ddraw].refs++;
    rc_st32(out, iface(the_ddraw, 7));
    RC_RETURN(c, DD_OK, 4);
}

static void k_DirectDrawEnumerateExA(rc_cpu *c)
{
    uint32_t fn = RC_ARG(c, 0), ctx = RC_ARG(c, 1);
    uint32_t desc = rc_proc_str("Primary Display Driver"), name = rc_proc_str("display");
    uint32_t args[5] = {0, desc, name, ctx, 0};
    rc_call_guest(c, fn, 5, args);
    RC_RETURN(c, DD_OK, 3);
}

static void k_DirectDrawEnumerateA(rc_cpu *c)
{
    uint32_t fn = RC_ARG(c, 0), ctx = RC_ARG(c, 1);
    uint32_t desc = rc_proc_str("Primary Display Driver"), name = rc_proc_str("display");
    uint32_t args[4] = {0, desc, name, ctx};
    rc_call_guest(c, fn, 4, args);
    RC_RETURN(c, DD_OK, 2);
}

#define M(i, n, f) {COM, i "::" n, f}

const rc_import_def rc_ddraw_defs[] = {
    {DDRAW, "DirectDrawCreate", k_DirectDrawCreate},
    {DDRAW, "DirectDrawCreateEx", k_DirectDrawCreateEx},
    {DDRAW, "DirectDrawEnumerateExA", k_DirectDrawEnumerateExA},
    {DDRAW, "DirectDrawEnumerateA", k_DirectDrawEnumerateA},

    M("IDirectDraw", "QueryInterface", k_DDQueryInterface),
    M("IDirectDraw", "AddRef", k_AddRef),
    M("IDirectDraw", "Release", k_DDRelease),
    M("IDirectDraw", "CreateClipper", k_CreateClipper),
    M("IDirectDraw", "CreatePalette", k_CreatePalette),
    M("IDirectDraw", "CreateSurface", k_CreateSurface),
    M("IDirectDraw", "EnumDisplayModes", k_EnumDisplayModes),
    M("IDirectDraw4", "EnumDisplayModes", k_EnumDisplayModes4),
    M("IDirectDraw", "GetCaps", k_DDGetCaps),
    M("IDirectDraw", "GetDisplayMode", k_GetDisplayMode),
    M("IDirectDraw4", "GetDisplayMode", k_GetDisplayMode4),
    M("IDirectDraw", "RestoreDisplayMode", k_RestoreDisplayMode),
    M("IDirectDraw", "SetCooperativeLevel", k_SetCooperativeLevel),
    M("IDirectDraw1", "SetDisplayMode", k_SetDisplayMode1),
    M("IDirectDraw", "SetDisplayMode", k_SetDisplayMode),
    M("IDirectDraw", "WaitForVerticalBlank", k_WaitForVerticalBlank),
    M("IDirectDraw", "GetAvailableVidMem", k_GetAvailableVidMem),

    M("IDirectDrawSurface", "QueryInterface", k_SurfaceQueryInterface),
    M("IDirectDrawSurface", "AddRef", k_AddRef),
    M("IDirectDrawSurface", "Release", k_Release),
    M("IDirectDrawSurface", "Blt", k_Blt),
    M("IDirectDrawSurface", "BltFast", k_BltFast),
    M("IDirectDrawSurface", "Flip", k_Flip),
    M("IDirectDrawSurface", "GetAttachedSurface", k_GetAttachedSurface),
    M("IDirectDrawSurface4", "GetAttachedSurface", k_GetAttachedSurface),
    M("IDirectDrawSurface", "GetCaps", k_SurfaceGetCaps),
    M("IDirectDrawSurface4", "GetCaps", k_SurfaceGetCaps4),
    M("IDirectDrawSurface", "GetColorKey", k_GetColorKey),
    M("IDirectDrawSurface", "GetDC", k_GetDC),
    M("IDirectDrawSurface", "GetPixelFormat", k_GetPixelFormat),
    M("IDirectDrawSurface", "GetSurfaceDesc", k_GetSurfaceDesc),
    M("IDirectDrawSurface4", "GetSurfaceDesc", k_GetSurfaceDesc),
    M("IDirectDrawSurface", "IsLost", k_IsLost),
    M("IDirectDrawSurface", "Lock", k_Lock),
    M("IDirectDrawSurface", "ReleaseDC", k_ReleaseDC),
    M("IDirectDrawSurface", "Restore", k_Restore),
    M("IDirectDrawSurface", "SetClipper", k_SetClipper),
    M("IDirectDrawSurface", "SetColorKey", k_SetColorKey),
    M("IDirectDrawSurface", "SetPalette", k_SetPalette),
    M("IDirectDrawSurface", "Unlock", k_Unlock),

    M("IDirectDrawPalette", "AddRef", k_AddRef),
    M("IDirectDrawPalette", "Release", k_Release),
    M("IDirectDrawPalette", "GetEntries", k_GetEntries),
    M("IDirectDrawPalette", "SetEntries", k_SetEntries),

    M("IDirectDrawClipper", "AddRef", k_AddRef),
    M("IDirectDrawClipper", "Release", k_Release),
    M("IDirectDrawClipper", "SetHWnd", k_ClipperSetHWnd),
    M("IDirectDrawClipper", "SetClipList", k_ClipperSetClipList),
};
const uint32_t rc_ddraw_count = sizeof rc_ddraw_defs / sizeof rc_ddraw_defs[0];
