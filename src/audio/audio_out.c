#include "audio/audio_out.h"
#include "util/log.h"

#include <SDL.h>
#include <string.h>

#include "dsp/spectrum.h"

#define TAG "audio"

/* ~23ms at 44.1kHz. Small enough that the pause/play transitions are crisp,
 * large enough that the callback rate stays modest on a 1.2GHz A7. SDL rounds
 * to a power of two anyway. */
#ifndef AUDIO_BUFFER_FRAMES
#define AUDIO_BUFFER_FRAMES 1024
#endif

static ring_t  *g_ring;
static bool     g_open;
static uint64_t g_underruns;
static uint64_t g_samples_written;
static SDL_AudioSpec g_spec;

/* Visualiser tap, published by the callback and read by the renderer.
 *
 * A seqlock rather than a mutex: the callback must never block, and the reader
 * can simply retry. The sequence is odd while a write is in progress, so a
 * reader that sees an odd count, or a different count either side of its copy,
 * knows it read a torn buffer and tries again. Dropping a frame of the
 * visualiser is free; stalling the audio thread is not. */
static float    g_tap[SPECTRUM_FFT_SIZE];
static unsigned g_tap_seq;
static int      g_tap_len;

static void tap_publish(const int16_t *pcm, size_t samples, int channels)
{
    if (channels < 1)
        return;

    size_t frames = samples / (size_t)channels;
    if (frames == 0)
        return;

    /* Keep only the most recent window; a callback larger than the FFT size
     * would otherwise publish its beginning rather than its end. */
    size_t take = frames < SPECTRUM_FFT_SIZE ? frames : SPECTRUM_FFT_SIZE;
    const int16_t *src = pcm + (frames - take) * (size_t)channels;

    __atomic_add_fetch(&g_tap_seq, 1, __ATOMIC_ACQ_REL);   /* now odd */

    if (take < SPECTRUM_FFT_SIZE) {
        /* Slide the existing window along so short callbacks still accumulate
         * a full FFT frame rather than analysing a fragment padded with zeros. */
        memmove(g_tap, g_tap + take, (SPECTRUM_FFT_SIZE - take) * sizeof(float));
    }

    float *dst = g_tap + (SPECTRUM_FFT_SIZE - take);
    for (size_t i = 0; i < take; i++) {
        int acc = 0;
        for (int c = 0; c < channels; c++)
            acc += src[i * (size_t)channels + (size_t)c];
        dst[i] = (float)acc / (float)channels / 32768.0f;
    }

    g_tap_len = SPECTRUM_FFT_SIZE;

    __atomic_add_fetch(&g_tap_seq, 1, __ATOMIC_ACQ_REL);   /* even again */
}

int audio_out_tap(float *out, int max)
{
    for (int attempt = 0; attempt < 4; attempt++) {
        unsigned before = __atomic_load_n(&g_tap_seq, __ATOMIC_ACQUIRE);
        if (before & 1u)
            continue;                       /* write in progress */

        int n = g_tap_len;
        if (n > max)
            n = max;
        if (n > 0)
            memcpy(out, g_tap, (size_t)n * sizeof(float));

        unsigned after = __atomic_load_n(&g_tap_seq, __ATOMIC_ACQUIRE);
        if (before == after)
            return n;                       /* clean read */
    }
    return 0;   /* contended; the visualiser simply decays this frame */
}

/* Runs on SDL's audio thread under a hard deadline. It does exactly one thing:
 * copy from the ring, pad any shortfall with silence. No locks, no allocation,
 * no logging -- anything that can block here is a dropout. */
static void audio_callback(void *userdata, Uint8 *out, int len)
{
    (void)userdata;

    int16_t *dst = (int16_t *)out;
    size_t want = (size_t)len / sizeof(int16_t);

    size_t got = ring_read(g_ring, dst, want);

    __atomic_add_fetch(&g_samples_written, want, __ATOMIC_RELAXED);

    if (got > 0)
        tap_publish(dst, got, g_spec.channels);

    if (got < want) {
        memset(dst + got, 0, (want - got) * sizeof(int16_t));
        __atomic_add_fetch(&g_underruns, 1, __ATOMIC_RELAXED);
    }
}

bool audio_out_open(int sample_rate, int channels, ring_t *ring)
{
    if (g_open) {
        LOGW(TAG, "audio device already open");
        return true;
    }

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOGE(TAG, "SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
        return false;
    }

    g_ring = ring;
    g_underruns = 0;
    g_samples_written = 0;

    SDL_AudioSpec desired;
    memset(&desired, 0, sizeof(desired));
    desired.freq = sample_rate;
    desired.format = AUDIO_S16SYS;
    desired.channels = (Uint8)channels;
    desired.samples = AUDIO_BUFFER_FRAMES;
    desired.callback = audio_callback;
    desired.userdata = NULL;

    memset(&g_spec, 0, sizeof(g_spec));
    if (SDL_OpenAudio(&desired, &g_spec) != 0) {
        LOGE(TAG, "SDL_OpenAudio failed: %s", SDL_GetError());
        return false;
    }

    g_open = true;

    LOGI(TAG, "opened %d Hz, %d channel(s), %d sample buffer",
         g_spec.freq, g_spec.channels, g_spec.samples);

    /* SDL 1.2 drivers routinely hand back something other than what was asked
     * for, and the device's MI_AO backend may only accept certain rates. This
     * is not fatal, but it would make playback run at the wrong speed, so it
     * must be loud rather than silent. */
    if (g_spec.freq != sample_rate)
        LOGW(TAG, "sample rate mismatch: asked %d Hz, got %d Hz "
                  "(playback will be pitched until resampling is wired up)",
             sample_rate, g_spec.freq);
    if (g_spec.channels != channels)
        LOGW(TAG, "channel count mismatch: asked %d, got %d",
             channels, g_spec.channels);
    if (g_spec.format != AUDIO_S16SYS)
        LOGW(TAG, "sample format mismatch: asked S16, got 0x%04x", g_spec.format);

    return true;
}

void audio_out_play(void)
{
    if (g_open)
        SDL_PauseAudio(0);
}

void audio_out_pause(void)
{
    if (g_open)
        SDL_PauseAudio(1);
}

void audio_out_close(void)
{
    if (!g_open)
        return;
    SDL_PauseAudio(1);
    SDL_CloseAudio();
    g_open = false;
    g_ring = NULL;
}

uint64_t audio_out_underruns(void)
{
    return __atomic_load_n(&g_underruns, __ATOMIC_RELAXED);
}

uint64_t audio_out_samples_written(void)
{
    return __atomic_load_n(&g_samples_written, __ATOMIC_RELAXED);
}

int audio_out_rate(void)           { return g_spec.freq; }
int audio_out_channels(void)       { return g_spec.channels; }
int audio_out_buffer_samples(void) { return g_spec.samples; }
