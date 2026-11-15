/* SPDX-License-Identifier: MIT */
/*
 * Miles Sound System: the "quick" API (samples from memory) and streams
 * (music and speech from files), on the platform's mixer.
 *
 * Handles are small guest blocks, since Miles' handles are pointers. A
 * stream's data is read whole when it opens: through the file callbacks
 * the game installed (AIL_set_file_callbacks: its files may be inside its
 * archives), or else from the virtual file system. Stream callbacks run
 * from the message pump, where there is a guest CPU to call them with.
 */
#include <stdlib.h>
#include <string.h>

#include "kernel32/k32.h"
#include "mss32.h"
#include "rc/audio.h"
#include "rc/log.h"
#include "rc/process.h"
#include "rc/vm.h"
#include "user32/user32.h"

#define MSS "mss32.dll"

/* ---- handles ---- */

typedef struct voice {
    int used;
    uint32_t guest;   /* the handle the game holds */
    rc_sound *sound;
    rc_track *track;
    int started;
    uint32_t callback; /* streams: called when playback ends */
    int ended_reported;
    int is_stream;
} voice;

#define VOICES 256
static voice voices[VOICES];
static int audio_ok;
static uint32_t digital; /* the driver handle */
static uint32_t file_open, file_close, file_seek, file_read;

static voice *voice_of(uint32_t h)
{
    for (int i = 0; i < VOICES; i++) {
        if (voices[i].used && voices[i].guest == h) {
            return &voices[i];
        }
    }
    return NULL;
}

static voice *new_voice(rc_sound *s)
{
    for (int i = 0; i < VOICES; i++) {
        if (!voices[i].used) {
            voice *v = &voices[i];
            memset(v, 0, sizeof *v);
            v->used = 1;
            v->sound = s;
            v->track = rc_track_new();
            v->guest = rc_heap_alloc(16, true);
            return v;
        }
    }
    return NULL;
}

static void free_voice(voice *v)
{
    rc_track_free(v->track);
    rc_sound_free(v->sound);
    rc_heap_free(v->guest);
    v->used = 0;
}

static uint32_t text(const char *s)
{
    return rc_proc_str(s);
}

/* ---- the library ---- */

static uint32_t redist;

static void k_set_redist_directory(rc_cpu *c)
{
    uint32_t old = redist ? redist : text("");
    redist = RC_ARG(c, 0) ? rc_proc_str(rc_str(RC_ARG(c, 0))) : 0;
    RC_RETURN(c, old, 1);
}

static void k_startup(rc_cpu *c) { RC_RETURN(c, 1, 0); }
static void k_shutdown(rc_cpu *c) { RC_RETURN(c, 0, 0); }

static void k_last_error(rc_cpu *c)
{
    static uint32_t msg;
    if (!msg) {
        msg = text(audio_ok ? "" : "no sound device");
    }
    RC_RETURN(c, msg, 0);
}

static void k_set_file_callbacks(rc_cpu *c)
{
    file_open = RC_ARG(c, 0);
    file_close = RC_ARG(c, 1);
    file_seek = RC_ARG(c, 2);
    file_read = RC_ARG(c, 3);
    RC_RETURN(c, 0, 4);
}

static void k_quick_startup(rc_cpu *c)
{
    uint32_t use_digital = RC_ARG(c, 0), rate = RC_ARG(c, 2), channels = RC_ARG(c, 4);
    if (use_digital && !audio_ok) {
        audio_ok = rc_audio_open((int)(rate ? rate : 22050), channels == 1 ? 1 : 2);
        if (audio_ok) {
            digital = rc_heap_alloc(64, true);
        }
    }
    if (rc_trace("sound")) {
        rc_log("[sound] quick startup at %u Hz: %s", rate, audio_ok ? "ok" : "no device");
    }
    RC_RETURN(c, audio_ok, 5);
}

static void k_quick_shutdown(rc_cpu *c)
{
    for (int i = 0; i < VOICES; i++) {
        if (voices[i].used) {
            free_voice(&voices[i]);
        }
    }
    RC_RETURN(c, 0, 0);
}

static void k_quick_handles(rc_cpu *c)
{
    uint32_t v[3] = {digital, 0, 0};
    for (int i = 0; i < 3; i++) {
        if (RC_ARG(c, i)) {
            rc_st32(RC_ARG(c, i), v[i]);
        }
    }
    RC_RETURN(c, 0, 3);
}

/* ---- samples ---- */

static void k_quick_load_mem(rc_cpu *c)
{
    uint32_t mem = RC_ARG(c, 0), size = RC_ARG(c, 1);
    rc_sound *s = audio_ok && mem && size ? rc_sound_load(rc_ptr(mem), size) : NULL;
    voice *v = s ? new_voice(s) : NULL;
    if (!v) {
        rc_sound_free(s);
        RC_RETURN(c, 0, 2);
    }
    RC_RETURN(c, v->guest, 2);
}

static void k_quick_unload(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    if (v) {
        free_voice(v);
    }
    RC_RETURN(c, 0, 1);
}

static void k_quick_play(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    uint32_t loops = RC_ARG(c, 1);
    bool ok = v && rc_track_play(v->track, v->sound, (int)loops);
    if (v) {
        v->started = 1;
    }
    RC_RETURN(c, ok, 2);
}

static void k_quick_halt(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    if (v) {
        rc_track_stop(v->track);
    }
    RC_RETURN(c, 0, 1);
}

static void k_quick_status(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    /* QSTAT_LOADED 1, QSTAT_PLAYING 2, QSTAT_DONE 3 */
    uint32_t st = !v ? 3 : rc_track_playing(v->track) ? 2 : v->started ? 3 : 1;
    RC_RETURN(c, st, 1);
}

static void k_quick_set_volume(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    if (v) {
        rc_track_gain(v->track, (float)RC_ARG(c, 1) / 127.0f);
    }
    RC_RETURN(c, 0, 3);
}

static void k_quick_set_speed(rc_cpu *c)
{
    /* The speed is a sample rate; Miles samples of the time are 22 kHz. */
    voice *v = voice_of(RC_ARG(c, 0));
    if (v && RC_ARG(c, 1)) {
        rc_track_speed(v->track, (float)RC_ARG(c, 1) / 22050.0f);
    }
    RC_RETURN(c, 0, 2);
}

/* ---- streams ---- */

/* Read a whole file through the game's callbacks. */
static uint8_t *read_with_callbacks(rc_cpu *c, const char *name, uint32_t *size)
{
    uint32_t hbuf = rc_heap_alloc(4, true), nbuf = rc_proc_alloc((uint32_t)strlen(name) + 1);
    rc_put_str(nbuf, (uint32_t)strlen(name) + 1, name);
    uint32_t a[4] = {nbuf, hbuf};
    uint32_t ok = rc_call_guest(c, file_open, 2, a);
    uint32_t h = rc_ld32(hbuf);
    rc_heap_free(hbuf);
    if (!ok) {
        return NULL;
    }
    uint32_t chunk = 0x10000, buf = rc_heap_alloc(chunk, false);
    uint8_t *data = NULL;
    uint32_t n = 0;
    for (;;) {
        uint32_t r[3] = {h, buf, chunk};
        uint32_t got = rc_call_guest(c, file_read, 3, r);
        if (got == 0 || got > chunk) {
            break;
        }
        uint8_t *grown = realloc(data, n + got);
        if (!grown) {
            break;
        }
        data = grown;
        memcpy(data + n, rc_ptr(buf), got);
        n += got;
        if (got < chunk) {
            break;
        }
    }
    rc_heap_free(buf);
    uint32_t cl[1] = {h};
    rc_call_guest(c, file_close, 1, cl);
    *size = n;
    return data;
}

static void k_open_stream(rc_cpu *c)
{
    const char *name = rc_str(RC_ARG(c, 1));
    if (!audio_ok || !name) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t size = 0;
    uint8_t *data = NULL;
    if (file_open && file_read) {
        data = read_with_callbacks(c, name, &size);
    } else {
        /* Ask for the size first, then the whole file. */
        uint8_t probe[1];
        if (rc_vfs_read_head(name, 0, probe, 1) == 1) {
            uint32_t cap = 64u << 20;
            data = malloc(cap);
            long r = data ? rc_vfs_read_head(name, 0, data, cap) : -1;
            size = r > 0 ? (uint32_t)r : 0;
        }
    }
    rc_sound *s = size ? rc_sound_load(data, size) : NULL;
    free(data);
    voice *v = s ? new_voice(s) : NULL;
    if (rc_trace("sound")) {
        rc_log("[sound] stream %s: %s", name, v ? "open" : "cannot be read");
    }
    if (!v) {
        rc_sound_free(s);
        RC_RETURN(c, 0, 3);
    }
    v->is_stream = 1;
    RC_RETURN(c, v->guest, 3);
}

static void k_close_stream(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    if (v) {
        free_voice(v);
    }
    RC_RETURN(c, 0, 1);
}

static void k_start_stream(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    if (v) {
        rc_track_play(v->track, v->sound, 1);
        v->started = 1;
        v->ended_reported = 0;
    }
    RC_RETURN(c, 0, 1);
}

static void k_pause_stream(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    if (v) {
        rc_track_pause(v->track, RC_ARG(c, 1) != 0);
    }
    RC_RETURN(c, 0, 2);
}

static void k_set_stream_volume(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    if (v) {
        rc_track_gain(v->track, (float)RC_ARG(c, 1) / 127.0f);
    }
    RC_RETURN(c, 0, 2);
}

static void k_register_stream_callback(rc_cpu *c)
{
    voice *v = voice_of(RC_ARG(c, 0));
    uint32_t old = v ? v->callback : 0;
    if (v) {
        v->callback = RC_ARG(c, 1);
    }
    RC_RETURN(c, old, 2);
}

/* Streams that have ended get their callback, once, from the pump. */
static void pump(rc_cpu *c)
{
    for (int i = 0; i < VOICES; i++) {
        voice *v = &voices[i];
        if (v->used && v->is_stream && v->started && v->callback && !v->ended_reported &&
            !rc_track_playing(v->track)) {
            v->ended_reported = 1;
            uint32_t a[1] = {v->guest};
            rc_call_guest(c, v->callback, 1, a);
        }
    }
}

static void k_service_stream(rc_cpu *c) { RC_RETURN(c, 0, 2); }
static void k_auto_service_stream(rc_cpu *c) { RC_RETURN(c, 0, 2); }

void rc_mss32_init(void) { rc_add_pump_hook(pump); }

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
    {MSS, "_AIL_close_stream@4", k_close_stream},
    {MSS, "_AIL_start_stream@4", k_start_stream},
    {MSS, "_AIL_pause_stream@8", k_pause_stream},
    {MSS, "_AIL_set_stream_volume@8", k_set_stream_volume},
    {MSS, "_AIL_register_stream_callback@8", k_register_stream_callback},
    {MSS, "_AIL_service_stream@8", k_service_stream},
    {MSS, "_AIL_auto_service_stream@8", k_auto_service_stream},
};
const uint32_t rc_mss32_count = sizeof rc_mss32_defs / sizeof rc_mss32_defs[0];
