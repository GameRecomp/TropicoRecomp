/* SPDX-License-Identifier: MIT */
/*
 * WinMM: the millisecond clock, and a wave output that reports no device,
 * so that code wanting one (Bink without Miles) does without sound.
 */
#include "winmm.h"

#include "rc/process.h"
#include "rc/thread.h"

#define WMM "WINMM.dll"
#define MMSYSERR_NODRIVER 6u

static void k_timeGetTime(rc_cpu *c)
{
    rc_thread_yield();
    rc_thread_poll();
    RC_RETURN(c, (uint32_t)rc_ticks_ms(), 0);
}

static void k_waveOutOpen(rc_cpu *c) { RC_RETURN(c, MMSYSERR_NODRIVER, 6); }

/* Calls on a device that cannot have been opened. */
static void k_waveOut1(rc_cpu *c) { RC_RETURN(c, MMSYSERR_NODRIVER, 1); }
static void k_waveOut2(rc_cpu *c) { RC_RETURN(c, MMSYSERR_NODRIVER, 2); }
static void k_waveOut3(rc_cpu *c) { RC_RETURN(c, MMSYSERR_NODRIVER, 3); }

const rc_import_def rc_winmm_defs[] = {
    {WMM, "timeGetTime", k_timeGetTime},
    {WMM, "waveOutOpen", k_waveOutOpen},
    {WMM, "waveOutClose", k_waveOut1},
    {WMM, "waveOutReset", k_waveOut1},
    {WMM, "waveOutPause", k_waveOut1},
    {WMM, "waveOutRestart", k_waveOut1},
    {WMM, "waveOutSetVolume", k_waveOut2},
    {WMM, "waveOutWrite", k_waveOut3},
    {WMM, "waveOutPrepareHeader", k_waveOut3},
    {WMM, "waveOutUnprepareHeader", k_waveOut3},
};
const uint32_t rc_winmm_count = sizeof rc_winmm_defs / sizeof rc_winmm_defs[0];
