#ifndef HYPR_AUDIO_OUT_H
#define HYPR_AUDIO_OUT_H

#include <stdbool.h>
#include <stdint.h>

#include "audio/ring.h"

/* SDL audio output. Opened exactly once, from the format the decoder actually
 * reported, and never reopened -- the Miyoo's MI_AO backend emits a pop on open
 * and re-initialising it mid-session is a documented source of trouble. A
 * dropped stream is handled by letting the ring drain to silence, not by
 * closing the device. */

/* sample_rate and channels come from stream_wait_format(). Returns false if the
 * device could not be opened. */
bool audio_out_open(int sample_rate, int channels, ring_t *ring);

void audio_out_play(void);   /* unpause */
void audio_out_pause(void);
void audio_out_close(void);

/* Number of callbacks that could not be fully satisfied from the ring and were
 * padded with silence. The single most useful health metric on the device. */
uint64_t audio_out_underruns(void);

/* Total samples handed to SDL, including silence padding. Divided by
 * rate*channels and elapsed time this gives the true consumption rate, which
 * is the only way to tell an over-consuming audio device apart from an
 * under-delivering network -- they look identical from the buffer level alone. */
uint64_t audio_out_samples_written(void);

/* Most recent samples handed to the device, mixed to mono, for the visualiser.
 * Copies up to max floats into out and returns how many were written.
 *
 * The tap is on the consumer side deliberately. Tapping the decoder instead
 * would show what was decoded rather than what is audible, and the ring buffer
 * runs ~2 seconds deep -- the display would lead the sound by that much. Safe
 * to call from the render thread; it never blocks the audio callback. */
int audio_out_tap(float *out, int max);

/* What SDL actually gave us, which is not necessarily what we asked for. */
int audio_out_rate(void);
int audio_out_channels(void);
int audio_out_buffer_samples(void);

#endif
