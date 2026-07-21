#ifndef HYPR_STREAM_H
#define HYPR_STREAM_H

#include <stdbool.h>
#include <stdint.h>

#include "audio/ring.h"
#include "net/conn.h"

/* Owns a background thread that keeps the ring buffer fed from the network:
 * connect -> HTTP GET -> de-frame -> MP3 decode -> ring, reconnecting forever
 * on failure.
 *
 * It deliberately never touches the audio device and never blocks the renderer.
 * A dead network shows up as the ring draining and the UI reporting
 * "reconnecting", not as a stall anywhere else in the app. */

typedef struct stream stream_t;

typedef struct {
    bool     connected;
    bool     have_format;
    int      sample_rate;
    int      channels;
    int      bitrate_kbps;
    uint64_t bytes_received;
    uint64_t samples_decoded;
    unsigned reconnects;
    uint64_t last_data_ms;   /* mono_ms() of the last byte received */
    unsigned backoff_ms;     /* current reconnect delay, 0 when connected */

    /* Why the last attempt failed, for the UI to show. Empty when healthy. */
    char last_error[160];
} stream_stats_t;

/* Starts the thread immediately; it connects asynchronously.
 * url must outlive the stream. Returns NULL on failure to start. */
stream_t *stream_start(const char *url, conn_tls_ctx_t *tls, ring_t *ring);

/* Blocks until the first frame has been decoded and the sample rate and
 * channel count are known, or the timeout expires. Returns false on timeout.
 *
 * The audio device is opened from these values rather than assumed, so that
 * the common case needs no resampling at all -- and so that a stream whose
 * rate we guessed wrong plays at the right speed instead of the wrong one. */
bool stream_wait_format(stream_t *s, int timeout_ms, int *sample_rate,
                        int *channels);

void stream_get_stats(stream_t *s, stream_stats_t *out);

/* Signals the thread to stop and joins it. Safe to call once. */
void stream_stop(stream_t *s);

#endif
