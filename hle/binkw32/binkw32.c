/* SPDX-License-Identifier: MIT */
/*
 * Bink, without pictures for now: BinkOpen reads the movie's header (size,
 * frame count, frame rate) and returns a movie whose frames decode to
 * nothing and need no waiting, so a game plays it through at once and
 * carries on. Our own Bink 1 decoder replaces this (milestone M8).
 *
 * The BINK structure starts with Width, Height, Frames, FrameNum,
 * LastFrameNum, FrameRate, FrameRateDiv: what callers read.
 */
#include <string.h>

#include "binkw32.h"
#include "kernel32/k32.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"

#define BINK "binkw32.dll"
#define BINKFILEHANDLE 0x00800000u
#define BINK_SIZE 0x400u

static uint32_t rd32(const uint8_t *b) { return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

static void k_BinkOpen(rc_cpu *c)
{
    uint32_t name = RC_ARG(c, 0), flags = RC_ARG(c, 1);
    uint8_t h[44];
    long n = (flags & BINKFILEHANDLE) ? rc_vfs_read_head(NULL, name, h, sizeof h)
                                      : rc_vfs_read_head(rc_str(name), 0, h, sizeof h);
    if (n < 44 || memcmp(h, "BIK", 3) != 0) {
        if (rc_trace("bink")) {
            rc_log("[bink] open %s: not a Bink file", (flags & BINKFILEHANDLE) ? "(handle)" : rc_str(name));
        }
        RC_RETURN(c, 0, 2);
    }
    uint32_t b = rc_heap_alloc(BINK_SIZE, true);
    rc_st32(b + 0, rd32(h + 20));      /* Width */
    rc_st32(b + 4, rd32(h + 24));      /* Height */
    rc_st32(b + 8, rd32(h + 8));       /* Frames */
    rc_st32(b + 12, 1);                /* FrameNum */
    rc_st32(b + 20, rd32(h + 28));     /* FrameRate */
    rc_st32(b + 24, rd32(h + 32));     /* FrameRateDiv */
    rc_st32(b + 32, flags);            /* OpenFlags */
    if (rc_trace("bink")) {
        rc_log("[bink] open %s: %ux%u, %u frames (shown as nothing)",
               (flags & BINKFILEHANDLE) ? "(handle)" : rc_str(name), rd32(h + 20), rd32(h + 24), rd32(h + 8));
    }
    RC_RETURN(c, b, 2);
}

static void k_BinkClose(rc_cpu *c)
{
    rc_heap_free(RC_ARG(c, 0));
    RC_RETURN(c, 0, 1);
}

static void k_BinkNextFrame(rc_cpu *c)
{
    uint32_t b = RC_ARG(c, 0);
    rc_st32(b + 16, rc_ld32(b + 12)); /* LastFrameNum */
    rc_st32(b + 12, rc_ld32(b + 12) + 1);
    RC_RETURN(c, 0, 1);
}

static void k_BinkGoto(rc_cpu *c)
{
    uint32_t b = RC_ARG(c, 0), frame = RC_ARG(c, 1);
    rc_st32(b + 16, rc_ld32(b + 12));
    rc_st32(b + 12, frame ? frame : 1);
    RC_RETURN(c, 0, 3);
}

static void k_BinkGetError(rc_cpu *c)
{
    static uint32_t msg;
    if (!msg) {
        msg = rc_proc_str("the file is not a Bink movie");
    }
    RC_RETURN(c, msg, 0);
}

static void k_BinkGetSummary(rc_cpu *c)
{
    uint32_t b = RC_ARG(c, 0), s = RC_ARG(c, 1);
    memset(rc_ptr(s), 0, 0x80);
    rc_st32(s, rc_ld32(b));
    rc_st32(s + 4, rc_ld32(b + 4));
    rc_st32(s + 16, rc_ld32(b + 8)); /* TotalFrames */
    RC_RETURN(c, 0, 2);
}

static void k_BinkGetKeyFrame(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 1), 3); }
static void k_BinkSetSoundSystem(rc_cpu *c) { RC_RETURN(c, 1, 2); }
static void k_BinkOpenMiles(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_BinkDoFrame(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_BinkWait(rc_cpu *c) { RC_RETURN(c, 0, 1); }
/* The frame is black: clear the destination rectangle. The low bits of
 * the flags give the surface format (8-bit, 24, 32, then 16-bit kinds). */
static void k_BinkCopyToBuffer(rc_cpu *c)
{
    uint32_t b = RC_ARG(c, 0), dest = RC_ARG(c, 1), pitch = RC_ARG(c, 2), dh = RC_ARG(c, 3);
    uint32_t dx = RC_ARG(c, 4), dy = RC_ARG(c, 5), type = RC_ARG(c, 6) & 0xf;
    uint32_t bytes = type == 0 ? 1 : type <= 2 ? 3 : type <= 6 ? 4 : 2;
    uint32_t w = rc_ld32(b), h = rc_ld32(b + 4);
    for (uint32_t y = 0; y < h && dy + y < dh; y++) {
        memset(rc_ptr(dest + (dy + y) * pitch + dx * bytes), 0, (size_t)w * bytes);
    }
    RC_RETURN(c, 0, 7);
}

const rc_import_def rc_binkw32_defs[] = {
    {BINK, "_BinkOpen@8", k_BinkOpen},
    {BINK, "_BinkClose@4", k_BinkClose},
    {BINK, "_BinkGetError@0", k_BinkGetError},
    {BINK, "_BinkSetSoundSystem@8", k_BinkSetSoundSystem},
    {BINK, "_BinkOpenMiles@4", k_BinkOpenMiles},
    {BINK, "_BinkDoFrame@4", k_BinkDoFrame},
    {BINK, "_BinkNextFrame@4", k_BinkNextFrame},
    {BINK, "_BinkWait@4", k_BinkWait},
    {BINK, "_BinkGetSummary@8", k_BinkGetSummary},
    {BINK, "_BinkGetKeyFrame@12", k_BinkGetKeyFrame},
    {BINK, "_BinkGoto@12", k_BinkGoto},
    {BINK, "_BinkCopyToBuffer@28", k_BinkCopyToBuffer},
};
const uint32_t rc_binkw32_count = sizeof rc_binkw32_defs / sizeof rc_binkw32_defs[0];
