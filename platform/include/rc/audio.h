/* SPDX-License-Identifier: MIT */
/*
 * Sound for the Miles reimplementation: sounds decoded from memory (WAV,
 * MP3, whatever the mixer reads), played on tracks with volume, speed,
 * loops and pause. SDL3_mixer implements it; the null platform plays
 * nothing but keeps the state a caller can ask for, with every track
 * ending at once.
 */
#ifndef RC_AUDIO_H
#define RC_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rc_sound rc_sound;
typedef struct rc_track rc_track;

/* Open the output. False if there is no sound device. */
bool rc_audio_open(int rate, int channels);
void rc_audio_close(void);

/* Decode a sound from memory (copied). NULL if it cannot be read. */
rc_sound *rc_sound_load(const void *data, size_t size);
void rc_sound_free(rc_sound *s);

rc_track *rc_track_new(void);
void rc_track_free(rc_track *t);

/* Play a sound; loops: 0 forever, 1 once, n times. */
bool rc_track_play(rc_track *t, rc_sound *s, int loops);
void rc_track_stop(rc_track *t);
void rc_track_pause(rc_track *t, bool paused);
bool rc_track_playing(rc_track *t);
/* 0.0 to 1.0 (and above). */
void rc_track_gain(rc_track *t, float gain);
/* Playback speed relative to the sound's own rate. */
void rc_track_speed(rc_track *t, float ratio);

#ifdef __cplusplus
}
#endif

#endif
