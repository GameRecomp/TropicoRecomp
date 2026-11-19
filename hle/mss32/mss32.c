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

/* As many as are loaded: the game loads a sound for most clicks and
 * unloads few of them (Miles has no limit; a fixed table here ran out, and
 * the game's sounds went quiet). Each is allocated by itself, so that a
 * pointer to one stays good as the table grows. */
static voice **voices;
static int n_voices;
static int audio_ok;
static uint32_t digital; /* the driver handle */
static uint32_t file_open, file_close, file_seek, file_read;

static voice *voice_of(uint32_t h)
{
    for (int i = 0; i < n_voices; i++) {
        if (voices[i]->used && voices[i]->guest == h) {
            return voices[i];
        }
    }
    return NULL;
}

static voice *new_voice(rc_sound *s)
{
    int i = 0;
    while (i < n_voices && voices[i]->used) {
        i++;
    }
    if (i == n_voices) {
        voice **more = realloc(voices, (size_t)(n_voices + 1) * sizeof *more);
        voice *one = calloc(1, sizeof *one);
        if (!more || !one) {
            free(one);
            if (more) {
                voices = more;
            }
            return NULL;
        }
        voices = more;
        voices[n_voices++] = one;
    }
    voice *v = voices[i];
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->sound = s;
    v->track = rc_track_new();
    v->guest = rc_heap_alloc(16, true);
    return v;
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
    for (int i = 0; i < n_voices; i++) {
        if (voices[i]->used) {
            free_voice(voices[i]);
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
    if (rc_trace("sound")) {
        int n = 0;
        for (int i = 0; i < n_voices; i++) {
            n += voices[i]->used;
        }
        rc_log("[sound] quick load (%u bytes): %s, %d loaded", size, v ? "ok" : s ? "no handle left" : "not read", n);
    }
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
    for (int i = 0; i < n_voices; i++) {
        voice *v = voices[i];
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

/* ---- streamed samples (HSAMPLE): what Bink plays its sound through ----
 * The caller fills two buffers in turn (AIL_sample_buffer_ready says which
 * one may be loaded next); the data goes into a PCM stream. */

typedef struct sample {
    int used;
    uint32_t guest;
    rc_pcm *pcm;
    int format; /* bit 0: 16 bits, bit 1: stereo */
    int rate;
    float volume;
    int status; /* SMP_FREE 1, SMP_DONE 2, SMP_PLAYING 4, SMP_STOPPED 8 */
    int next_buffer;
    uint32_t last_len;
    uint32_t user[8];
    uint32_t eob; /* end-of-buffer callback, or 0 */
    uint32_t pending[2]; /* lengths of the buffers loaded and not played */
    int npending;
    uint32_t buf_len[2]; /* the last length loaded into buffer 0 and 1 */
    int newest;          /* the buffer loaded last, or -1 */
    /* On the virtual clock: bytes not yet played, and when that was. */
    uint32_t vqueued;
    uint64_t vtime;
    /* A whole sound given at once (AIL_set_named_sample_file,
     * AIL_set_sample_address): played on a track, not streamed. */
    rc_sound *sound;
    rc_track *track;
    int loops;           /* 0 forever */
    uint32_t eos;        /* end-of-sample callback, or 0 */
    /* On the virtual clock a whole sound ends by that clock: when it
     * started (or resumed) and how much of it was left. */
    uint64_t vstart;
    uint64_t vleft;
    float pan;
} sample;

#define SAMPLES 32
static sample samples[SAMPLES];

static sample *sample_of(uint32_t h)
{
    for (int i = 0; i < SAMPLES; i++) {
        if (samples[i].used && samples[i].guest == h) {
            return &samples[i];
        }
    }
    return NULL;
}

/* Whether a whole sound is still playing: by the device, or on the
 * virtual clock by its length. */
static bool sound_playing(sample *s)
{
    if (!rc_clock_virtual()) {
        return rc_track_playing(s->track);
    }
    if (s->status != 4) {
        return s->status == 8;
    }
    if (s->loops == 0) {
        return true;
    }
    return rc_ticks_peek_ms() - s->vstart < s->vleft;
}

/* A whole sound has played out: done, and its end-of-sample callback,
 * once. Whoever sees it first says so (the pump, or the game asking for
 * the status): Miles calls the callback as the sound ends, so a game that
 * sees it done has had the callback, and may count on that. */
static void sound_ended(rc_cpu *c, sample *s)
{
    s->status = 2;
    if (s->eos) {
        uint32_t a[1] = {s->guest};
        rc_call_guest(c, s->eos, 1, a);
    }
}

/* Leave whole-sound mode. */
static void drop_sound(sample *s)
{
    if (s->track) {
        rc_track_stop(s->track);
    }
    rc_sound_free(s->sound);
    s->sound = NULL;
}

static void k_allocate_sample_handle(rc_cpu *c)
{
    for (int i = 0; i < SAMPLES; i++) {
        if (!samples[i].used) {
            sample *s = &samples[i];
            memset(s, 0, sizeof *s);
            s->used = 1;
            s->guest = rc_heap_alloc(64, true);
            s->format = 1;
            s->rate = 22050;
            s->volume = 1.0f;
            s->status = 2;
            s->loops = 1;
            s->pan = 0.5f;
            RC_RETURN(c, s->guest, 1);
        }
    }
    RC_RETURN(c, 0, 1);
}

static void k_release_sample_handle(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s) {
        rc_pcm_free(s->pcm);
        drop_sound(s);
        rc_track_free(s->track);
        s->track = NULL;
        rc_heap_free(s->guest);
        s->used = 0;
    }
    RC_RETURN(c, 0, 1);
}

static void k_init_sample(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s) {
        rc_pcm_free(s->pcm);
        s->pcm = NULL;
        s->format = 1;
        s->rate = 22050;
        s->volume = 1.0f;
        s->status = 2;
        s->next_buffer = 0;
        s->last_len = 0;
        s->npending = 0;
        s->eob = 0;
        s->newest = -1;
        s->buf_len[0] = s->buf_len[1] = 0;
        s->vqueued = 0;
        drop_sound(s);
        s->loops = 1;
        s->eos = 0;
        s->pan = 0.5f;
    }
    RC_RETURN(c, 0, 1);
}

/* The PCM stream for the sample's current format, made when first needed. */
static rc_pcm *pcm_of(sample *s)
{
    if (!s->pcm && audio_ok) {
        s->pcm = rc_pcm_open(s->rate, (s->format & 2) ? 2 : 1, (s->format & 1) ? 16 : 8);
        rc_pcm_gain(s->pcm, s->volume);
        rc_pcm_pause(s->pcm, s->status != 4);
    }
    return s->pcm;
}

/* Bytes per second of the sample's format. */
static uint32_t byte_rate(const sample *s)
{
    return (uint32_t)s->rate * ((s->format & 1) ? 2u : 1u) * ((s->format & 2) ? 2u : 1u);
}

/* Bytes loaded and not yet played. On the virtual clock the sound is
 * counted as played as virtual time passes, and never reaches the device,
 * so that picture and sound keep to the same clock. */
static uint32_t queued_of(sample *s)
{
    if (!rc_clock_virtual()) {
        return audio_ok ? rc_pcm_queued(s->pcm) : 0;
    }
    uint64_t now = rc_ticks_peek_ms();
    if (s->status == 4 && now > s->vtime) {
        uint64_t played = (now - s->vtime) * byte_rate(s) / 1000u;
        s->vqueued = played >= s->vqueued ? 0 : s->vqueued - (uint32_t)played;
    }
    s->vtime = now;
    return s->vqueued;
}

static void put_data(sample *s, uint32_t data, uint32_t len)
{
    if (rc_clock_virtual()) {
        queued_of(s);
        s->vqueued += len;
        return;
    }
    rc_pcm_put(pcm_of(s), rc_ptr(data), len);
}

static void k_set_sample_type(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && (int)RC_ARG(c, 1) != s->format) {
        s->format = (int)RC_ARG(c, 1);
        rc_pcm_free(s->pcm);
        s->pcm = NULL;
    }
    RC_RETURN(c, 0, 3);
}

static void k_set_sample_playback_rate(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && (int)RC_ARG(c, 1) != s->rate && RC_ARG(c, 1)) {
        s->rate = (int)RC_ARG(c, 1);
        rc_pcm_free(s->pcm);
        s->pcm = NULL;
    }
    RC_RETURN(c, 0, 2);
}

static void k_set_sample_volume(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s) {
        s->volume = (float)RC_ARG(c, 1) / 127.0f;
        rc_pcm_gain(s->pcm, s->volume);
        if (s->track) {
            rc_track_gain(s->track, s->volume);
        }
    }
    RC_RETURN(c, 0, 2);
}

static void k_set_sample_pan(rc_cpu *c) { RC_RETURN(c, 0, 2); }

static void k_start_sample(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && s->sound) {
        s->status = 4;
        s->vstart = rc_ticks_peek_ms();
        s->vleft = (uint64_t)rc_sound_ms(s->sound) * (uint64_t)(s->loops > 0 ? s->loops : 1);
        rc_track_gain(s->track, s->volume);
        if (!rc_clock_virtual()) {
            rc_track_play(s->track, s->sound, s->loops);
        }
    } else if (s) {
        queued_of(s);
        s->status = 4;
        rc_pcm_pause(pcm_of(s), false);
    }
    RC_RETURN(c, 0, 1);
}

static void k_stop_sample(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && s->sound) {
        if (s->status == 4 && rc_clock_virtual()) {
            uint64_t ran = rc_ticks_peek_ms() - s->vstart;
            s->vleft = ran >= s->vleft ? 0 : s->vleft - ran;
        }
        s->status = 8;
        rc_track_pause(s->track, true);
    } else if (s) {
        queued_of(s);
        s->status = 8;
        rc_pcm_pause(s->pcm, true);
    }
    RC_RETURN(c, 0, 1);
}

static void k_resume_sample(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && s->sound) {
        s->status = 4;
        s->vstart = rc_ticks_peek_ms();
        rc_track_pause(s->track, false);
    } else if (s) {
        queued_of(s);
        s->status = 4;
        rc_pcm_pause(pcm_of(s), false);
    }
    RC_RETURN(c, 0, 1);
}

static void k_end_sample(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && s->sound) {
        rc_track_stop(s->track);
    }
    if (s) {
        s->status = 2;
        s->npending = 0;
        s->vqueued = 0;
        rc_pcm_clear(s->pcm);
    }
    RC_RETURN(c, 0, 1);
}

static void k_sample_status(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && s->sound && s->status == 4 && !sound_playing(s)) {
        sound_ended(c, s);
    }
    if (s && s->status == 4 && queued_of(s) == 0 && s->last_len) {
        /* Everything loaded has been played. */
        RC_RETURN(c, 2, 1);
    }
    RC_RETURN(c, s ? (uint32_t)s->status : 1, 1);
}

/* A buffer may be loaded while less than two buffers' worth is queued. */
static void k_sample_buffer_ready(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (!s) {
        RC_RETURN(c, 0xffffffffu, 1);
    }
    uint32_t queued = queued_of(s);
    if (s->last_len && queued >= 2 * s->last_len) {
        RC_RETURN(c, 0xffffffffu, 1);
    }
    RC_RETURN(c, (uint32_t)s->next_buffer, 1);
}

static void k_load_sample_buffer(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    uint32_t data = RC_ARG(c, 2), len = RC_ARG(c, 3);
    if (s) {
        if (len) {
            put_data(s, data, len);
            s->last_len = len;
            if (s->npending == 2) {
                s->pending[0] += s->pending[1];
                s->npending = 1;
            }
            s->pending[s->npending++] = len;
        }
        /* Loading into a sample that has finished starts it; a stopped
         * (paused) one stays stopped. */
        if (len && s->status == 2) {
            queued_of(s);
            s->status = 4;
            rc_pcm_pause(pcm_of(s), false);
        }
        s->buf_len[RC_ARG(c, 1) & 1] = len;
        s->newest = (int)(RC_ARG(c, 1) & 1);
        s->next_buffer = (int)(RC_ARG(c, 1) & 1) ^ 1;
    }
    RC_RETURN(c, 0, 4);
}

static void k_minimum_sample_buffer_size(rc_cpu *c)
{
    /* About 1/8 s of sound, rounded to 1 KiB. */
    uint32_t rate = RC_ARG(c, 1), format = RC_ARG(c, 2);
    uint32_t bytes = rate / 8 * ((format & 1) ? 2u : 1u) * ((format & 2) ? 2u : 1u);
    RC_RETURN(c, (bytes + 1023) & ~1023u, 3);
}

/* How far each buffer has played: the stream holds the newest buffer's
 * data at its end and the other's before it. */
static void k_sample_buffer_info(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    uint32_t pos[2] = {0, 0}, len[2] = {0, 0};
    if (s) {
        uint32_t queued = queued_of(s);
        for (int k = 0; k < 2; k++) {
            len[k] = s->buf_len[k];
            uint32_t left;
            if (k == s->newest) {
                left = queued < len[k] ? queued : len[k];
            } else {
                uint32_t newer = s->newest >= 0 ? s->buf_len[s->newest] : 0;
                left = queued > newer ? queued - newer : 0;
                left = left < len[k] ? left : len[k];
            }
            pos[k] = len[k] - left;
        }
    }
    if (RC_ARG(c, 1)) { rc_st32(RC_ARG(c, 1), pos[0]); }
    if (RC_ARG(c, 2)) { rc_st32(RC_ARG(c, 2), len[0]); }
    if (RC_ARG(c, 3)) { rc_st32(RC_ARG(c, 3), pos[1]); }
    if (RC_ARG(c, 4)) { rc_st32(RC_ARG(c, 4), len[1]); }
    RC_RETURN(c, 0, 5);
}

static void k_sample_playback_rate(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    RC_RETURN(c, s ? (uint32_t)s->rate : 0, 1);
}

static void k_sample_volume(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    RC_RETURN(c, s ? (uint32_t)(s->volume * 127.0f) : 0, 1);
}

static void k_mem_alloc_lock(rc_cpu *c) { RC_RETURN(c, rc_heap_alloc(RC_ARG(c, 0) ? RC_ARG(c, 0) : 1, false), 1); }

static void k_mem_free_lock(rc_cpu *c)
{
    if (RC_ARG(c, 0)) {
        rc_heap_free(RC_ARG(c, 0));
    }
    RC_RETURN(c, 0, 1);
}

static void k_set_sample_user_data(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s && RC_ARG(c, 1) < 8) {
        s->user[RC_ARG(c, 1)] = RC_ARG(c, 2);
    }
    RC_RETURN(c, 0, 3);
}

static void k_sample_user_data(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    RC_RETURN(c, s && RC_ARG(c, 1) < 8 ? s->user[RC_ARG(c, 1)] : 0, 2);
}

/* A whole sound for the sample, from a file's image in memory (WAV, MP3). */
static void k_set_named_sample_file(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    uint32_t image = RC_ARG(c, 2), size = RC_ARG(c, 3);
    if (!s || !image || !size) {
        RC_RETURN(c, 0, 5);
    }
    drop_sound(s);
    s->sound = audio_ok ? rc_sound_load(rc_ptr(image), size) : NULL;
    if (!s->track) {
        s->track = rc_track_new();
    }
    if (rc_trace("sound")) {
        const char *suffix = RC_ARG(c, 1) ? rc_str(RC_ARG(c, 1)) : "";
        rc_log("[sound] sample file (%s, %u bytes): %s", suffix, size, s->sound ? "ok" : "not read");
    }
    s->status = 2;
    RC_RETURN(c, s->sound || !audio_ok ? 1 : 0, 5);
}

static void le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v)
{
    le16(p, v);
    le16(p + 2, v >> 16);
}

/* Raw PCM in the sample's format (AIL_set_sample_type, its rate), made a
 * WAV so that the mixer reads it as a whole sound. */
static void k_set_sample_address(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    uint32_t start = RC_ARG(c, 1), len = RC_ARG(c, 2);
    if (!s) {
        RC_RETURN(c, 0, 3);
    }
    drop_sound(s);
    if (!s->track) {
        s->track = rc_track_new();
    }
    if (audio_ok && start && len) {
        uint8_t *w = malloc((size_t)len + 44);
        if (w) {
            uint32_t ch = (s->format & 2) ? 2 : 1, bits = (s->format & 1) ? 16 : 8;
            memcpy(w, "RIFF", 4);
            le32(w + 4, 36 + len);
            memcpy(w + 8, "WAVEfmt ", 8);
            le32(w + 16, 16);
            le16(w + 20, 1);
            le16(w + 22, ch);
            le32(w + 24, (uint32_t)s->rate);
            le32(w + 28, (uint32_t)s->rate * ch * bits / 8);
            le16(w + 32, ch * bits / 8);
            le16(w + 34, bits);
            memcpy(w + 36, "data", 4);
            le32(w + 40, len);
            memcpy(w + 44, rc_ptr(start), len);
            s->sound = rc_sound_load(w, (size_t)len + 44);
            free(w);
        }
    }
    s->status = 2;
    RC_RETURN(c, 0, 3);
}

static void k_set_sample_loop_count(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s) {
        s->loops = (int)RC_ARG(c, 1);
    }
    RC_RETURN(c, 0, 2);
}

static void k_register_EOS_callback(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    uint32_t old = s ? s->eos : 0;
    if (s) {
        s->eos = RC_ARG(c, 1);
    }
    RC_RETURN(c, old, 2);
}

static float f32_of(uint32_t v)
{
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

static uint32_t u32_of(float f)
{
    uint32_t v;
    memcpy(&v, &f, sizeof v);
    return v;
}

/* Volume 0.0 to 1.0, pan 0.0 (left) to 1.0 (right); pan is kept only. */
static void k_set_sample_volume_pan(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    if (s) {
        float v = f32_of(RC_ARG(c, 1));
        s->volume = v < 0.0f ? 0.0f : v;
        s->pan = f32_of(RC_ARG(c, 2));
        rc_pcm_gain(s->pcm, s->volume);
        if (s->track) {
            rc_track_gain(s->track, s->volume);
        }
    }
    RC_RETURN(c, 0, 3);
}

static void k_sample_volume_pan(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    uint32_t vol = RC_ARG(c, 1), pan = RC_ARG(c, 2);
    if (vol) {
        rc_st32(vol, u32_of(s ? s->volume : 1.0f));
    }
    if (pan) {
        rc_st32(pan, u32_of(s ? s->pan : 0.5f));
    }
    RC_RETURN(c, 0, 3);
}

/* The newer way to open the output: (rate, bits, channels, flags). */
static void k_open_digital_driver(rc_cpu *c)
{
    uint32_t rate = RC_ARG(c, 0), channels = RC_ARG(c, 2);
    if (!audio_ok) {
        audio_ok = rc_audio_open((int)(rate ? rate : 22050), channels == 1 ? 1 : 2);
        if (audio_ok && !digital) {
            digital = rc_heap_alloc(64, true);
        }
    }
    if (rc_trace("sound")) {
        rc_log("[sound] digital driver at %u Hz: %s", rate, audio_ok ? "ok" : "no device");
    }
    RC_RETURN(c, audio_ok ? digital : 0, 4);
}

/* Giving the device up while in the background, and taking it back. */
static void k_digital_handle_release(rc_cpu *c) { RC_RETURN(c, 1, 1); }
static void k_digital_handle_reacquire(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static void k_register_EOB_callback(rc_cpu *c)
{
    sample *s = sample_of(RC_ARG(c, 0));
    uint32_t old = s ? s->eob : 0;
    if (s) {
        s->eob = RC_ARG(c, 1);
    }
    RC_RETURN(c, old, 2);
}

/* Preferences Bink reads: everything at its default (0). */
static void k_get_preference(rc_cpu *c) { RC_RETURN(c, 0, 1); }

/* The output's latency in milliseconds. */
static void k_digital_latency(rc_cpu *c) { RC_RETURN(c, audio_ok ? 50 : 0, 1); }

static void k_serve(rc_cpu *c) { RC_RETURN(c, 0, 0); }

/* Report buffers that have played out to their end-of-buffer callbacks. */
static void sample_pump(rc_cpu *c)
{
    for (int i = 0; i < SAMPLES; i++) {
        sample *s = &samples[i];
        if (!s->used || s->status != 4) {
            continue;
        }
        if (s->sound) {
            /* A whole sound that has played out. */
            if (!sound_playing(s)) {
                sound_ended(c, s);
            }
            continue;
        }
        uint32_t queued = queued_of(s);
        while (s->npending > 0 && queued <= (s->npending == 2 ? s->pending[1] : 0)) {
            s->pending[0] = s->pending[1];
            s->npending--;
            if (s->eob) {
                uint32_t a[1] = {s->guest};
                rc_call_guest(c, s->eob, 1, a);
            }
        }
    }
}

void rc_mss32_init(void)
{
    rc_add_pump_hook(pump);
    rc_add_pump_hook(sample_pump);
}

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
    {MSS, "_AIL_allocate_sample_handle@4", k_allocate_sample_handle},
    {MSS, "_AIL_release_sample_handle@4", k_release_sample_handle},
    {MSS, "_AIL_init_sample@4", k_init_sample},
    {MSS, "_AIL_set_sample_type@12", k_set_sample_type},
    {MSS, "_AIL_set_sample_playback_rate@8", k_set_sample_playback_rate},
    {MSS, "_AIL_set_sample_volume@8", k_set_sample_volume},
    {MSS, "_AIL_set_sample_pan@8", k_set_sample_pan},
    {MSS, "_AIL_start_sample@4", k_start_sample},
    {MSS, "_AIL_stop_sample@4", k_stop_sample},
    {MSS, "_AIL_resume_sample@4", k_resume_sample},
    {MSS, "_AIL_end_sample@4", k_end_sample},
    {MSS, "_AIL_sample_status@4", k_sample_status},
    {MSS, "_AIL_sample_buffer_ready@4", k_sample_buffer_ready},
    {MSS, "_AIL_load_sample_buffer@16", k_load_sample_buffer},
    {MSS, "_AIL_minimum_sample_buffer_size@12", k_minimum_sample_buffer_size},
    {MSS, "_AIL_sample_buffer_info@20", k_sample_buffer_info},
    {MSS, "_AIL_sample_playback_rate@4", k_sample_playback_rate},
    {MSS, "_AIL_sample_volume@4", k_sample_volume},
    {MSS, "_AIL_mem_alloc_lock@4", k_mem_alloc_lock},
    {MSS, "_AIL_mem_free_lock@4", k_mem_free_lock},
    {MSS, "_AIL_set_sample_user_data@12", k_set_sample_user_data},
    {MSS, "_AIL_sample_user_data@8", k_sample_user_data},
    {MSS, "_AIL_register_EOB_callback@8", k_register_EOB_callback},
    {MSS, "_AIL_set_named_sample_file@20", k_set_named_sample_file},
    {MSS, "_AIL_set_sample_address@12", k_set_sample_address},
    {MSS, "_AIL_set_sample_loop_count@8", k_set_sample_loop_count},
    {MSS, "_AIL_register_EOS_callback@8", k_register_EOS_callback},
    {MSS, "_AIL_set_sample_volume_pan@12", k_set_sample_volume_pan},
    {MSS, "_AIL_sample_volume_pan@12", k_sample_volume_pan},
    {MSS, "_AIL_open_digital_driver@16", k_open_digital_driver},
    {MSS, "_AIL_digital_handle_release@4", k_digital_handle_release},
    {MSS, "_AIL_digital_handle_reacquire@4", k_digital_handle_reacquire},
    {MSS, "_AIL_get_preference@4", k_get_preference},
    {MSS, "_AIL_digital_latency@4", k_digital_latency},
    {MSS, "_AIL_serve@0", k_serve},
};
const uint32_t rc_mss32_count = sizeof rc_mss32_defs / sizeof rc_mss32_defs[0];
