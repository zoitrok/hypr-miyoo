#ifndef HYPR_RING_H
#define HYPR_RING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A lock-free single-producer / single-consumer ring of interleaved S16 samples.
 *
 * Exactly one producer (the decode thread) and one consumer (the SDL audio
 * callback), which is what makes the lock-free form correct here. It is also
 * why a mutex would be the wrong choice: the callback runs on SDL's audio
 * thread under a hard deadline, and a mutex would let a decode thread stalled
 * in a TLS read hold it. That is precisely how a network hiccup turns into an
 * audible dropout. Here the callback can always make progress -- it reads
 * whatever is available and pads with silence.
 *
 * Counts are in *samples*, not frames: a stereo frame is 2 samples. Callers
 * work in samples throughout to avoid a channel-count multiply at every site. */

typedef struct {
    int16_t *buf;
    size_t   capacity; /* power of two, in samples */
    size_t   mask;

    /* Free-running counters, never wrapped to the buffer size. Unsigned
     * overflow is well-defined and the difference stays correct across it,
     * which sidesteps the usual full-vs-empty ambiguity of index-based rings. */
    volatile size_t head; /* total samples written (producer owns) */
    volatile size_t tail; /* total samples read    (consumer owns) */
} ring_t;

/* capacity_samples is rounded up to a power of two. Returns false on OOM. */
bool ring_init(ring_t *r, size_t capacity_samples);
void ring_free(ring_t *r);

/* Discards all buffered samples. Only safe when the consumer is stopped. */
void ring_reset(ring_t *r);

/* Producer side. Writes as much as fits and returns how many samples were
 * taken -- a short write means the buffer is full and the caller should slow
 * down rather than spin. */
size_t ring_write(ring_t *r, const int16_t *src, size_t n);

/* Consumer side. Reads as much as is available; a short read means the
 * producer has fallen behind (the caller should pad with silence). */
size_t ring_read(ring_t *r, int16_t *dst, size_t n);

/* Snapshots. Both are safe to call from either side, but are inherently
 * racy -- treat them as a level gauge, not a guarantee. */
size_t ring_available(const ring_t *r);
size_t ring_space(const ring_t *r);
size_t ring_capacity(const ring_t *r);

#endif
