#ifndef HYPR_DECODER_H
#define HYPR_DECODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Thin wrapper over minimp3. Keeps the vendored header out of every other
 * translation unit and gives callers a byte-oriented interface: push whatever
 * arrived, get whole frames back. */

/* minimp3's MINIMP3_MAX_SAMPLES_PER_FRAME (1152 * 2). */
#define DECODER_MAX_SAMPLES_PER_FRAME 2304

/* minimp3's MINIMP3_MAX_FRAME_BYTES.
 *
 * This bound is load-bearing, not decorative. minimp3 requires a *complete*
 * frame in the buffer: handed less, it does not report "need more data" -- it
 * consumes the bytes as junk and returns no samples. Calling it with a partial
 * frame therefore silently destroys audio. decoder_drain() enforces this; do
 * not hand-roll the loop. */
#define DECODER_MAX_FRAME_BYTES 2884

typedef struct decoder decoder_t;

decoder_t *decoder_new(void);
void decoder_free(decoder_t *d);

/* Forgets stream position but keeps the discovered format. Call after a
 * reconnect: we rejoin mid-frame and the bit reservoir no longer refers to
 * anything we have. */
void decoder_reset(decoder_t *d);

/* Receives one decoded frame. samples is the interleaved sample count
 * (frames * channels), never zero. */
typedef void (*decoder_sink_fn)(void *user, const int16_t *pcm, int samples);

/* Decodes every complete frame in in[0..in_len), calling sink for each, and
 * returns how many bytes were consumed. The caller keeps the unconsumed tail
 * and prepends it to the next read.
 *
 * Holds back the final DECODER_MAX_FRAME_BYTES unless at_eof, because a frame
 * that straddles the end of the buffer would otherwise be eaten as junk.
 * at_eof=true means no more bytes will ever arrive, so the remainder is decoded
 * for whatever is left in it. */
size_t decoder_drain(decoder_t *d, const uint8_t *in, size_t in_len,
                     bool at_eof, decoder_sink_fn sink, void *user);

/* Format of the most recently decoded frame. Returns false until the first
 * frame has been decoded -- which is why the audio device is opened only after
 * the stream has produced audio, rather than guessed at startup. */
bool decoder_format(const decoder_t *d, int *sample_rate, int *channels);

/* Bitrate of the last decoded frame, in kbit/s. 0 if unknown. Diagnostic only. */
int decoder_bitrate_kbps(const decoder_t *d);

#endif
