/* SPDX-License-Identifier: MIT */
/* Sound on SDL3_mixer. */
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include "rc/audio.h"

struct rc_sound {
    MIX_Audio *audio;
};

struct rc_track {
    MIX_Track *track;
    bool paused;
};

static MIX_Mixer *mixer;

bool rc_audio_open(int rate, int channels)
{
    if (mixer) {
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO) || !MIX_Init()) {
        SDL_Log("audio: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, channels > 0 ? channels : 2, rate > 0 ? rate : 22050};
    mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec);
    if (!mixer) {
        SDL_Log("audio: %s", SDL_GetError());
        return false;
    }
    return true;
}

void rc_audio_close(void)
{
    if (mixer) {
        MIX_DestroyMixer(mixer);
        mixer = NULL;
    }
}

rc_sound *rc_sound_load(const void *data, size_t size)
{
    if (!mixer) {
        return NULL;
    }
    SDL_IOStream *io = SDL_IOFromConstMem(data, size);
    if (!io) {
        return NULL;
    }
    /* Predecoding copies the data, so the caller's memory may go away. */
    MIX_Audio *a = MIX_LoadAudio_IO(mixer, io, true, true);
    if (!a) {
        return NULL;
    }
    rc_sound *s = SDL_malloc(sizeof *s);
    if (!s) {
        MIX_DestroyAudio(a);
        return NULL;
    }
    s->audio = a;
    return s;
}

uint32_t rc_sound_ms(const rc_sound *s)
{
    if (!s) {
        return 0;
    }
    Sint64 frames = MIX_GetAudioDuration(s->audio);
    SDL_AudioSpec spec;
    if (frames <= 0 || !MIX_GetAudioFormat(s->audio, &spec) || spec.freq <= 0) {
        return 0;
    }
    return (uint32_t)(frames * 1000 / spec.freq);
}

void rc_sound_free(rc_sound *s)
{
    if (s) {
        MIX_DestroyAudio(s->audio);
        SDL_free(s);
    }
}

rc_track *rc_track_new(void)
{
    if (!mixer) {
        return NULL;
    }
    rc_track *t = SDL_calloc(1, sizeof *t);
    if (t && !(t->track = MIX_CreateTrack(mixer))) {
        SDL_free(t);
        t = NULL;
    }
    return t;
}

void rc_track_free(rc_track *t)
{
    if (t) {
        MIX_DestroyTrack(t->track);
        SDL_free(t);
    }
}

bool rc_track_play(rc_track *t, rc_sound *s, int loops)
{
    if (!t || !s || !MIX_SetTrackAudio(t->track, s->audio)) {
        return false;
    }
    /* Miles counts plays (0: forever); the mixer counts repeats (-1: forever). */
    MIX_SetTrackLoops(t->track, loops == 0 ? -1 : loops - 1);
    t->paused = false;
    return MIX_PlayTrack(t->track, 0);
}

void rc_track_stop(rc_track *t)
{
    if (t) {
        MIX_StopTrack(t->track, 0);
    }
}

void rc_track_pause(rc_track *t, bool paused)
{
    if (!t || t->paused == paused) {
        return;
    }
    t->paused = paused;
    if (paused) {
        MIX_PauseTrack(t->track);
    } else {
        MIX_ResumeTrack(t->track);
    }
}

bool rc_track_playing(rc_track *t) { return t && (t->paused || MIX_TrackPlaying(t->track)); }

void rc_track_gain(rc_track *t, float gain)
{
    if (t) {
        MIX_SetTrackGain(t->track, gain);
    }
}

void rc_track_speed(rc_track *t, float ratio)
{
    if (t && ratio > 0.0f) {
        MIX_SetTrackFrequencyRatio(t->track, ratio);
    }
}

struct rc_pcm {
    SDL_AudioStream *stream;
    MIX_Track *track;
    bool paused;
};

rc_pcm *rc_pcm_open(int rate, int channels, int bits)
{
    if (!mixer) {
        return NULL;
    }
    SDL_AudioSpec spec = {bits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16, channels, rate};
    rc_pcm *p = SDL_calloc(1, sizeof *p);
    if (!p) {
        return NULL;
    }
    p->stream = SDL_CreateAudioStream(&spec, &spec);
    p->track = MIX_CreateTrack(mixer);
    if (!p->stream || !p->track || !MIX_SetTrackAudioStream(p->track, p->stream) || !MIX_PlayTrack(p->track, 0)) {
        rc_pcm_free(p);
        return NULL;
    }
    return p;
}

void rc_pcm_free(rc_pcm *p)
{
    if (!p) {
        return;
    }
    if (p->track) {
        MIX_DestroyTrack(p->track);
    }
    if (p->stream) {
        SDL_DestroyAudioStream(p->stream);
    }
    SDL_free(p);
}

void rc_pcm_put(rc_pcm *p, const void *data, uint32_t size)
{
    if (p && size) {
        SDL_PutAudioStreamData(p->stream, data, (int)size);
        /* A track whose stream ran dry stops; start it again. */
        if (!p->paused && !MIX_TrackPlaying(p->track)) {
            MIX_PlayTrack(p->track, 0);
        }
    }
}

uint32_t rc_pcm_queued(rc_pcm *p)
{
    int n = p ? SDL_GetAudioStreamQueued(p->stream) : 0;
    return n > 0 ? (uint32_t)n : 0;
}

void rc_pcm_pause(rc_pcm *p, bool paused)
{
    if (!p || p->paused == paused) {
        return;
    }
    p->paused = paused;
    if (paused) {
        MIX_PauseTrack(p->track);
    } else if (MIX_TrackPlaying(p->track) || MIX_TrackPaused(p->track)) {
        MIX_ResumeTrack(p->track);
    } else {
        /* It ran dry (or never had data) and stopped. */
        MIX_PlayTrack(p->track, 0);
    }
}

void rc_pcm_gain(rc_pcm *p, float gain)
{
    if (p) {
        MIX_SetTrackGain(p->track, gain);
    }
}

void rc_pcm_clear(rc_pcm *p)
{
    if (p) {
        SDL_ClearAudioStream(p->stream);
    }
}
