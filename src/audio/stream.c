#include "audio/stream.h"
#include "audio/decoder.h"
#include "net/http.h"
#include "net/url.h"
#include "util/log.h"
#include "util/mono.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "stream"

/* Comfortably larger than any MP3 frame (~1440 bytes at 320kbit/s), so a whole
 * frame is always available to the decoder once enough bytes have arrived. */
#define IN_BUF_SIZE      16384

/* If no byte arrives for this long the connection is treated as dead. Proxies
 * and mobile networks drop idle-looking connections without sending a FIN, so
 * waiting for an error would mean waiting forever. */
#define READ_TIMEOUT_MS  10000
#define CONNECT_TIMEOUT_MS 10000

/* Deliberately far shorter than the metadata connection's backoff.
 *
 * Silence is the most noticeable failure this app has, and the buffer only
 * covers ~3.5s, so every second of backoff past that is a second of nothing.
 * Chaos testing made the cost concrete: the shared 30s cap produced half a
 * minute of dead air after a run of drops. Metadata going stale for 30s is
 * barely visible; audio going quiet for 30s is the whole product failing.
 *
 * Retrying a live stream is cheap -- one GET, and the server is already
 * sending to other listeners -- so the tradeoff strongly favours trying
 * again sooner. */
/* The first retry is immediate. Most drops are transient, reconnecting costs
 * one GET, and on the device the connect itself already takes seconds -- so a
 * deliberate wait on top is silence bought for nothing. Later retries back off
 * normally, so a genuinely down server is still not hammered. */
#define BACKOFF_FIRST_MS 0
#define BACKOFF_MIN_MS   1000
#define BACKOFF_MAX_MS   8000

struct stream {
    char  url_str[512];
    url_t url;
    conn_tls_ctx_t *tls;
    ring_t *ring;
    decoder_t *dec;

    pthread_t thread;
    bool      thread_started;

    pthread_mutex_t lock;
    pthread_cond_t  format_cv;
    stream_stats_t  stats;

    volatile bool stop;
};

static void set_connected(stream_t *s, bool connected, unsigned backoff_ms)
{
    pthread_mutex_lock(&s->lock);
    s->stats.connected = connected;
    s->stats.backoff_ms = backoff_ms;
    pthread_mutex_unlock(&s->lock);
}

/* Pushes decoded samples into the ring, applying backpressure rather than
 * dropping. A full ring means we are ahead of realtime -- which is exactly
 * what the server's connect burst causes -- so waiting is correct and dropping
 * would be an audible glitch for no reason. */
static void push_samples(stream_t *s, const int16_t *pcm, int count)
{
    int written = 0;
    while (written < count && !s->stop) {
        size_t n = ring_write(s->ring, pcm + written, (size_t)(count - written));
        written += (int)n;
        if (n == 0)
            mono_sleep_ms(10);
    }
}

/* Sink for decoder_drain: publish the frame and, the first time round, the
 * stream format that main() is waiting on to open the audio device. */
static void on_decoded_frame(void *user, const int16_t *pcm, int samples)
{
    stream_t *s = user;

    push_samples(s, pcm, samples);

    pthread_mutex_lock(&s->lock);
    s->stats.samples_decoded += (uint64_t)samples;
    if (!s->stats.have_format &&
        decoder_format(s->dec, &s->stats.sample_rate, &s->stats.channels)) {
        s->stats.have_format = true;
        pthread_cond_broadcast(&s->format_cv);
    }
    s->stats.bitrate_kbps = decoder_bitrate_kbps(s->dec);
    pthread_mutex_unlock(&s->lock);
}

/* Runs one connection to exhaustion. Returns when it drops or stop is set. */
static void run_connection(stream_t *s)
{
    conn_t *conn = conn_open(s->tls, &s->url, CONNECT_TIMEOUT_MS);
    if (!conn) {
        pthread_mutex_lock(&s->lock);
        snprintf(s->stats.last_error, sizeof(s->stats.last_error), "%s",
                 conn_last_open_error());
        pthread_mutex_unlock(&s->lock);
        return;
    }

    pthread_mutex_lock(&s->lock);
    s->stats.last_error[0] = '\0';
    pthread_mutex_unlock(&s->lock);

    /* No "Icy-MetaData: 1": the WebSocket carries richer metadata than ICY
     * would, and requesting it would interleave metadata blocks into the audio
     * that we would then have to strip back out of every read. */
    const http_header_t extra[] = {
        { "Accept", "*/*" },
        { "Connection", "close" },
    };

    http_stream_t hs;
    int rc = http_get(&hs, conn, &s->url, extra,
                      sizeof(extra) / sizeof(extra[0]), CONNECT_TIMEOUT_MS);
    if (rc != 0) {
        LOGE(TAG, "request failed: %s", conn_last_error(conn));
        conn_close(conn);
        return;
    }
    if (hs.status != 200) {
        LOGE(TAG, "unexpected status %d", hs.status);
        conn_close(conn);
        return;
    }

    /* We rejoin a live stream mid-frame, so the decoder's bit reservoir refers
     * to audio we never saw. Resetting makes it resync cleanly; the cost is one
     * garbled frame at the join, which is inaudible. */
    decoder_reset(s->dec);
    set_connected(s, true, 0);
    LOGI(TAG, "streaming (%s)", hs.chunked ? "chunked" : "identity");

    uint8_t in[IN_BUF_SIZE];
    size_t  in_len = 0;

    while (!s->stop) {
        ssize_t n = http_read(&hs, in + in_len, sizeof(in) - in_len,
                              READ_TIMEOUT_MS);
        if (n == CONN_EOF) {
            LOGW(TAG, "stream ended");
            break;
        }
        if (n == CONN_TIMEOUT) {
            LOGW(TAG, "no data for %d ms; treating connection as dead",
                 READ_TIMEOUT_MS);
            break;
        }
        if (n < 0) {
            LOGE(TAG, "read error: %s", conn_last_error(conn));
            break;
        }

        in_len += (size_t)n;

        pthread_mutex_lock(&s->lock);
        s->stats.bytes_received += (uint64_t)n;
        s->stats.last_data_ms = mono_ms();
        pthread_mutex_unlock(&s->lock);

        /* at_eof is false: the stream never ends, so a partial frame at the
         * tail must be held back for the next read rather than decoded. */
        size_t consumed = decoder_drain(s->dec, in, in_len, false,
                                        on_decoded_frame, s);

        if (consumed > 0) {
            memmove(in, in + consumed, in_len - consumed);
            in_len -= consumed;
        }

        /* A full buffer that yielded nothing means this is not MP3 at all.
         * Dropping it silently would resync eventually; saying so does not. */
        if (in_len == sizeof(in)) {
            LOGE(TAG, "input buffer full with no decodable frame; resyncing");
            in_len = 0;
            decoder_reset(s->dec);
        }
    }

    conn_close(conn);
}

static void *stream_thread(void *arg)
{
    stream_t *s = arg;

    s->dec = decoder_new();
    if (!s->dec) {
        LOGE(TAG, "failed to allocate decoder");
        return NULL;
    }

    unsigned backoff = BACKOFF_FIRST_MS;
    bool first = true;

    while (!s->stop) {
        if (!first) {
            pthread_mutex_lock(&s->lock);
            s->stats.reconnects++;
            pthread_mutex_unlock(&s->lock);

            LOGI(TAG, "reconnecting in %u ms", backoff);
            set_connected(s, false, backoff);

            /* Sleep in slices so stop is honoured promptly; a 30s backoff must
             * not make quitting the app take 30s. */
            for (unsigned waited = 0; waited < backoff && !s->stop; waited += 100)
                mono_sleep_ms(100);
            if (s->stop)
                break;
        }
        first = false;

        uint64_t started = mono_ms();
        run_connection(s);
        set_connected(s, false, backoff);

        /* A connection that survived a while was healthy; treat the next
         * failure as a fresh incident rather than escalating from wherever the
         * last one left off. Without this, a stream that drops once an hour
         * would creep to the cap and stay there.
         *
         * 15s rather than 30s: at 8s max backoff, insisting on 30s of health
         * before resetting means a flaky link never gets back to fast retries. */
        if (mono_ms() - started > 15000)
            backoff = BACKOFF_FIRST_MS;
        else if (backoff == 0)
            backoff = BACKOFF_MIN_MS;
        else if (backoff < BACKOFF_MAX_MS)
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
    }

    decoder_free(s->dec);
    s->dec = NULL;
    LOGI(TAG, "stream thread exiting");
    return NULL;
}

stream_t *stream_start(const char *url, conn_tls_ctx_t *tls, ring_t *ring)
{
    stream_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    if (!url_parse(url, &s->url)) {
        LOGE(TAG, "cannot parse stream URL '%s'", url);
        free(s);
        return NULL;
    }
    snprintf(s->url_str, sizeof(s->url_str), "%s", url);

    if (s->url.tls && !tls) {
        LOGE(TAG, "stream URL is TLS but no TLS context was supplied");
        free(s);
        return NULL;
    }

    s->tls = tls;
    s->ring = ring;

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->format_cv, NULL);

    if (pthread_create(&s->thread, NULL, stream_thread, s) != 0) {
        LOGE(TAG, "failed to start stream thread");
        pthread_cond_destroy(&s->format_cv);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    s->thread_started = true;

    return s;
}

bool stream_wait_format(stream_t *s, int timeout_ms, int *sample_rate,
                        int *channels)
{
    uint64_t deadline = mono_ms() + (uint64_t)timeout_ms;
    bool got = false;

    pthread_mutex_lock(&s->lock);
    while (!s->stats.have_format && !s->stop) {
        uint64_t now = mono_ms();
        if (now >= deadline)
            break;

        /* pthread_cond_timedwait needs a realtime deadline, which is the one
         * place we are forced to touch the wall clock. A clock step here can
         * only cause an early or late wakeup of a loop that re-checks its own
         * monotonic deadline, so it cannot cause a wrong result. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t remaining = deadline - now;
        ts.tv_sec += (time_t)(remaining / 1000);
        ts.tv_nsec += (long)((remaining % 1000) * 1000000L);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s->format_cv, &s->lock, &ts);
    }

    if (s->stats.have_format) {
        got = true;
        if (sample_rate)
            *sample_rate = s->stats.sample_rate;
        if (channels)
            *channels = s->stats.channels;
    }
    pthread_mutex_unlock(&s->lock);

    return got;
}

void stream_get_stats(stream_t *s, stream_stats_t *out)
{
    pthread_mutex_lock(&s->lock);
    *out = s->stats;
    pthread_mutex_unlock(&s->lock);
}

void stream_stop(stream_t *s)
{
    if (!s)
        return;

    s->stop = true;

    /* Wake anyone blocked in stream_wait_format so shutdown is not gated on
     * a stream that never produced a frame. */
    pthread_mutex_lock(&s->lock);
    pthread_cond_broadcast(&s->format_cv);
    pthread_mutex_unlock(&s->lock);

    if (s->thread_started)
        pthread_join(s->thread, NULL);

    pthread_cond_destroy(&s->format_cv);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
