/* SPDX-License-Identifier: MIT */
/*
 * Miles Sound System, silent for now: the library starts, but no sound
 * device opens (AIL_quick_startup fails), so the game runs without sound,
 * as on a machine without a sound card. The real implementation on
 * SDL3_mixer replaces this (milestone M8).
 */
#include "mss32.h"
#include "rc/process.h"

#define MSS "mss32.dll"

static uint32_t empty_string(void)
{
    static uint32_t s;
    if (!s) {
        s = rc_proc_str("");
    }
    return s;
}

static uint32_t redist;

static void k_set_redist_directory(rc_cpu *c)
{
    uint32_t old = redist ? redist : empty_string();
    redist = RC_ARG(c, 0) ? rc_proc_str(rc_str(RC_ARG(c, 0))) : 0;
    RC_RETURN(c, old, 1);
}

static void k_startup(rc_cpu *c) { RC_RETURN(c, 1, 0); }
static void k_shutdown(rc_cpu *c) { RC_RETURN(c, 0, 0); }
static void k_last_error(rc_cpu *c) { RC_RETURN(c, rc_proc_str("no sound device"), 0); }

static void k_quick_startup(rc_cpu *c) { RC_RETURN(c, 0, 5); }
static void k_quick_shutdown(rc_cpu *c) { RC_RETURN(c, 0, 0); }

static void k_quick_handles(rc_cpu *c)
{
    for (int i = 0; i < 3; i++) {
        if (RC_ARG(c, i)) {
            rc_st32(RC_ARG(c, i), 0);
        }
    }
    RC_RETURN(c, 0, 3);
}

static void k_quick_load_mem(rc_cpu *c) { RC_RETURN(c, 0, 2); }
static void k_quick_unload(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_quick_play(rc_cpu *c) { RC_RETURN(c, 0, 2); }
static void k_quick_halt(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_quick_status(rc_cpu *c) { RC_RETURN(c, 3, 1); } /* QSTAT_DONE */
static void k_quick_set_volume(rc_cpu *c) { RC_RETURN(c, 0, 3); }
static void k_quick_set_speed(rc_cpu *c) { RC_RETURN(c, 0, 2); }
static void k_set_file_callbacks(rc_cpu *c) { RC_RETURN(c, 0, 4); }

static void k_open_stream(rc_cpu *c) { RC_RETURN(c, 0, 3); }
static void k_stream_1(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_stream_2(rc_cpu *c) { RC_RETURN(c, 0, 2); }

const rc_import_def rc_mss32_defs[] = {
    {MSS, "_AIL_set_redist_directory@4", k_set_redist_directory},
    {MSS, "_AIL_startup@0", k_startup},
    {MSS, "_AIL_shutdown@0", k_shutdown},
    {MSS, "_AIL_last_error@0", k_last_error},
    {MSS, "_AIL_quick_startup@20", k_quick_startup},
    {MSS, "_AIL_quick_shutdown@0", k_quick_shutdown},
    {MSS, "_AIL_quick_handles@12", k_quick_handles},
    {MSS, "_AIL_quick_load_mem@8", k_quick_load_mem},
    {MSS, "_AIL_quick_unload@4", k_quick_unload},
    {MSS, "_AIL_quick_play@8", k_quick_play},
    {MSS, "_AIL_quick_halt@4", k_quick_halt},
    {MSS, "_AIL_quick_status@4", k_quick_status},
    {MSS, "_AIL_quick_set_volume@12", k_quick_set_volume},
    {MSS, "_AIL_quick_set_speed@8", k_quick_set_speed},
    {MSS, "_AIL_set_file_callbacks@16", k_set_file_callbacks},
    {MSS, "_AIL_open_stream@12", k_open_stream},
    {MSS, "_AIL_close_stream@4", k_stream_1},
    {MSS, "_AIL_start_stream@4", k_stream_1},
    {MSS, "_AIL_pause_stream@8", k_stream_2},
    {MSS, "_AIL_set_stream_volume@8", k_stream_2},
    {MSS, "_AIL_register_stream_callback@8", k_stream_2},
    {MSS, "_AIL_service_stream@8", k_stream_2},
    {MSS, "_AIL_auto_service_stream@8", k_stream_2},
};
const uint32_t rc_mss32_count = sizeof rc_mss32_defs / sizeof rc_mss32_defs[0];
