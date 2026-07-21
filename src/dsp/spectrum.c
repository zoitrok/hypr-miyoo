#include "dsp/spectrum.h"
#include "dsp/fft.h"

#include <math.h>
#include <string.h>

/* Attack fast, release slow. A meter that falls as fast as it rises reads as
 * noise; the asymmetry is most of what makes an analyser look good. */
#define ATTACK   0.55f
#define RELEASE  3.2f    /* units per second */

#define PEAK_FALL 0.85f  /* units per second */

/* Ignore everything below this: a 640px-wide display cannot show sub-bass
 * resolution, and DC/rumble would peg the first bar permanently. */
#define MIN_HZ    45.0
#define MAX_HZ 16000.0

void spectrum_init(spectrum_t *s, int bars)
{
    memset(s, 0, sizeof(*s));
    if (bars < 1)
        bars = 1;
    if (bars > SPECTRUM_MAX_BARS)
        bars = SPECTRUM_MAX_BARS;
    s->bars = bars;
}

static void decay_only(spectrum_t *s, double dt)
{
    for (int i = 0; i < s->bars; i++) {
        s->value[i] -= (float)(RELEASE * dt);
        if (s->value[i] < 0)
            s->value[i] = 0;
        s->peak[i] -= (float)(PEAK_FALL * dt);
        if (s->peak[i] < s->value[i])
            s->peak[i] = s->value[i];
    }
}

void spectrum_update(spectrum_t *s, const float *mono, int count,
                     int sample_rate, double dt_seconds)
{
    if (dt_seconds < 0)
        dt_seconds = 0;
    if (dt_seconds > 0.25)
        dt_seconds = 0.25;

    if (!mono || count < SPECTRUM_FFT_SIZE || sample_rate <= 0) {
        decay_only(s, dt_seconds);
        return;
    }

    static float re[SPECTRUM_FFT_SIZE];
    static float im[SPECTRUM_FFT_SIZE];

    /* Hann window. Without it the rectangular window's spectral leakage
     * smears every tone across neighbouring bars and the display turns into
     * an undifferentiated wall. */
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i /
                                      (SPECTRUM_FFT_SIZE - 1)));
        re[i] = mono[i] * w;
        im[i] = 0.0f;
    }

    fft_forward(re, im, SPECTRUM_FFT_SIZE);

    const int half = SPECTRUM_FFT_SIZE / 2;
    const double bin_hz = (double)sample_rate / SPECTRUM_FFT_SIZE;

    for (int b = 0; b < s->bars; b++) {
        /* Logarithmic band edges: pitch is logarithmic, so linear bands would
         * give almost every bar to the treble and cram all the musically
         * interesting content into the first two. */
        double f0 = MIN_HZ * pow(MAX_HZ / MIN_HZ, (double)b / s->bars);
        double f1 = MIN_HZ * pow(MAX_HZ / MIN_HZ, (double)(b + 1) / s->bars);

        int k0 = (int)(f0 / bin_hz);
        int k1 = (int)(f1 / bin_hz);
        if (k0 < 1) k0 = 1;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > half) k1 = half;
        if (k0 >= half) k0 = half - 1;

        /* Peak within the band, not mean: a single strong partial should show
         * as a bar, and averaging would bury it under its quiet neighbours. */
        float peak = 0.0f;
        for (int k = k0; k < k1; k++) {
            float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
            if (mag > peak)
                peak = mag;
        }

        /* To dB, then map a useful window onto 0..1. Linear magnitude would
         * leave the display flat for all but the loudest passages. */
        float db = 20.0f * log10f(peak / (SPECTRUM_FFT_SIZE * 0.25f) + 1e-9f);
        float v = (db + 62.0f) / 62.0f;
        if (v < 0) v = 0;
        if (v > 1) v = 1;

        /* Tilt the top end up: music has far less energy up there, and without
         * this the right half of the display barely moves. */
        v *= 1.0f + 0.5f * ((float)b / s->bars);
        if (v > 1) v = 1;

        if (v > s->value[b])
            s->value[b] += (v - s->value[b]) * ATTACK;
        else {
            s->value[b] -= (float)(RELEASE * dt_seconds);
            if (s->value[b] < v)
                s->value[b] = v;
        }

        if (s->value[b] > s->peak[b])
            s->peak[b] = s->value[b];
        else {
            s->peak[b] -= (float)(PEAK_FALL * dt_seconds);
            if (s->peak[b] < s->value[b])
                s->peak[b] = s->value[b];
        }
    }
}
