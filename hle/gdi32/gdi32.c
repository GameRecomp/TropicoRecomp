/* SPDX-License-Identifier: MIT */
/*
 * gdi32: device queries, for now. The screen is described as a 1024x768
 * display at 32 bits per pixel; drawing and fonts come with the platform
 * layer.
 */
#include <string.h>

#include "gdi32.h"
#include "rc/hle.h"

#define G32 "GDI32.dll"

static uint32_t batch_limit = 20;

static void k_GdiSetBatchLimit(rc_cpu *c)
{
    uint32_t old = batch_limit;
    batch_limit = RC_ARG(c, 0) ? RC_ARG(c, 0) : 20;
    RC_RETURN(c, old, 1);
}

static void k_GetDeviceCaps(rc_cpu *c)
{
    uint32_t v;
    switch (RC_ARG(c, 1)) {
    case 2: v = 0x0003; break; /* DRIVERVERSION */
    case 4: v = 320; break;    /* HORZSIZE, mm */
    case 6: v = 240; break;    /* VERTSIZE, mm */
    /* A desktop larger than every resolution the game knows: Tropico offers
     * only the resolutions narrower than the desktop, and the host window
     * shows any of them at any size (rc/platform.h, rc_display). */
    case 8: v = 2048; break;   /* HORZRES */
    case 10: v = 1536; break;  /* VERTRES */
    case 12: v = 32; break;    /* BITSPIXEL */
    case 14: v = 1; break;     /* PLANES */
    case 24: v = 0xffffffffu; break; /* NUMCOLORS: more than 8 bits */
    case 38: v = 0x7e99; break;      /* RASTERCAPS, without RC_PALETTE */
    case 88: v = 96; break;    /* LOGPIXELSX */
    case 90: v = 96; break;    /* LOGPIXELSY */
    case 104: v = 0; break;    /* SIZEPALETTE */
    case 116: v = 60; break;   /* VREFRESH */
    default: v = 0; break;
    }
    RC_RETURN(c, v, 2);
}

static void k_GetSystemPaletteEntries(rc_cpu *c)
{
    /* No palette on a 32-bit display. */
    uint32_t n = RC_ARG(c, 2), p = RC_ARG(c, 3);
    if (p) {
        memset(rc_ptr(p), 0, 4 * n);
    }
    RC_RETURN(c, 0, 4);
}

const rc_import_def rc_gdi32_setup[] = {
    {G32, "GdiSetBatchLimit", k_GdiSetBatchLimit},
    {G32, "GetDeviceCaps", k_GetDeviceCaps},
    {G32, "GetSystemPaletteEntries", k_GetSystemPaletteEntries},
};
const uint32_t rc_gdi32_setup_count = sizeof rc_gdi32_setup / sizeof rc_gdi32_setup[0];
