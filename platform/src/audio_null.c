/* SPDX-License-Identifier: MIT */
/* No sound device: nothing opens, nothing plays. */
#include "rc/audio.h"

bool rc_audio_open(int rate, int channels)
{
    (void)rate;
    (void)channels;
    return false;
}

void rc_audio_close(void) {}

rc_sound *rc_sound_load(const void *data, size_t size)
{
    (void)data;
    (void)size;
    return 0;
}

void rc_sound_free(rc_sound *s) { (void)s; }

uint32_t rc_sound_ms(const rc_sound *s)
{
    (void)s;
    return 0;
}
rc_track *rc_track_new(void) { return 0; }
void rc_track_free(rc_track *t) { (void)t; }

bool rc_track_play(rc_track *t, rc_sound *s, int loops)
{
    (void)t;
    (void)s;
    (void)loops;
    return false;
}

void rc_track_stop(rc_track *t) { (void)t; }

void rc_track_pause(rc_track *t, bool paused)
{
    (void)t;
    (void)paused;
}

bool rc_track_playing(rc_track *t)
{
    (void)t;
    return false;
}

void rc_track_gain(rc_track *t, float gain)
{
    (void)t;
    (void)gain;
}

void rc_track_speed(rc_track *t, float ratio)
{
    (void)t;
    (void)ratio;
}

rc_pcm *rc_pcm_open(int rate, int channels, int bits)
{
    (void)rate;
    (void)channels;
    (void)bits;
    return 0;
}

void rc_pcm_free(rc_pcm *p) { (void)p; }

void rc_pcm_put(rc_pcm *p, const void *data, uint32_t size)
{
    (void)p;
    (void)data;
    (void)size;
}

uint32_t rc_pcm_queued(rc_pcm *p)
{
    (void)p;
    return 0;
}

void rc_pcm_pause(rc_pcm *p, bool paused)
{
    (void)p;
    (void)paused;
}

void rc_pcm_gain(rc_pcm *p, float gain)
{
    (void)p;
    (void)gain;
}

void rc_pcm_clear(rc_pcm *p) { (void)p; }
