#ifndef HYPR_SPECTRUM_H
#define HYPR_SPECTRUM_H

#include <stdbool.h>

/* Turns raw PCM into bar heights for the analyser.
 *
 * Runs on the render thread, never in the audio callback: the callback has a
 * hard deadline and must do nothing but copy. It reads the tap that
 * audio_out.c fills with the samples it just handed to the device -- tapping
 * the decoder instead would put the display ~2 seconds ahead of the sound,
 * since that is how deep the ring buffer runs. */

#define SPECTRUM_FFT_SIZE 512
#define SPECTRUM_MAX_BARS 48

typedef struct {
    int   bars;
    float value[SPECTRUM_MAX_BARS];  /* 0..1, smoothed */
    float peak[SPECTRUM_MAX_BARS];   /* 0..1, peak-hold with decay */
} spectrum_t;

void spectrum_init(spectrum_t *s, int bars);

/* Consumes up to SPECTRUM_FFT_SIZE mono samples and updates the bars.
 * dt_seconds paces the decay so it looks the same at any frame rate.
 * Passing fewer samples than the window (or none) decays toward silence. */
void spectrum_update(spectrum_t *s, const float *mono, int count,
                     int sample_rate, double dt_seconds);

#endif
