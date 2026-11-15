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
