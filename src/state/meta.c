#include "state/meta.h"
#include "net/ws.h"
#include "state/protocol.h"
#include "util/log.h"
#include "util/mono.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "meta"

#define CONNECT_TIMEOUT_MS 15000

/* Every 20s: often enough to keep the clock offset fresh and to notice a
 * frozen connection quickly, rare enough to be invisible on battery. */
#define PING_INTERVAL_MS 20000

/* The server pings us every 10s, so silence for this long means the connection
 * is dead even though the socket still looks open. Proxies drop idle-looking
 * connections without sending a FIN, so waiting for an error waits forever. */
#define STALE_TIMEOUT_MS 30000

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 30000

/* The protocol version this client was written against. A server ahead of us
 * may be speaking a shape we would misinterpret, so we say so rather than
 * quietly rendering nonsense. */
#define EXPECTED_BACKEND_VERSION 7

struct meta {
    char url[512];
    conn_tls_ctx_t *tls;
    app_state_t *state;

    pthread_t thread;
    bool      thread_started;
    volatile bool stop;
};

static void set_connected(meta_t *m, bool connected, unsigned backoff_ms)
{
    app_snapshot_t *st = state_lock(m->state);
    st->ws_connected = connected;
    st->backoff_ms = backoff_ms;
    state_unlock(m->state);
}

/* Runs one connection until it drops or stop is set. */
static void run_connection(meta_t *m)
{
    ws_t *ws = ws_connect(m->tls, m->url, CONNECT_TIMEOUT_MS);
    if (!ws) {
        app_snapshot_t *st = state_lock(m->state);
        snprintf(st->ws_error, sizeof(st->ws_error), "%s", conn_last_open_error());
        state_unlock(m->state);
        return;
    }

    {
        app_snapshot_t *st = state_lock(m->state);
        st->ws_error[0] = '\0';
        state_unlock(m->state);
    }

    set_connected(m, true, 0);

    /* Ping immediately rather than after the first interval. Until a pong
     * lands there is no clock offset, so song progress falls back to counting
     * from when we heard about the song -- right rate, wrong origin. Waiting
     * 20s for that means 20s of a visibly wrong progress bar on every launch. */
    uint64_t next_ping = mono_ms();

    while (!m->stop) {
        uint64_t now = mono_ms();

        if (now >= next_ping) {
            static const char PING[] = "{\"type\":\"ping\"}";
            if (ws_send_text(ws, PING, sizeof(PING) - 1) != 0) {
                LOGE(TAG, "ping failed: %s", ws_last_error(ws));
                break;
            }
            /* Timestamped before the send completes rather than after, so the
             * round trip we measure includes our own transmit path -- the
             * midpoint estimate assumes a symmetric round trip. */
            app_snapshot_t *st = state_lock(m->state);
            clock_ping_sent(&st->clock, now);
            state_unlock(m->state);

            next_ping = now + PING_INTERVAL_MS;
        }

        if (now - ws_last_activity_ms(ws) > STALE_TIMEOUT_MS) {
            LOGW(TAG, "no traffic for %d ms; connection is stale",
                 STALE_TIMEOUT_MS);
            break;
        }

        const char *payload = NULL;
        size_t len = 0;
        int rc = ws_recv(ws, &payload, &len, 1000);

        if (rc == WS_TIMEOUT)
            continue;
        if (rc == WS_CLOSED) {
            LOGI(TAG, "server closed the connection");
            break;
        }
        if (rc < 0) {
            LOGE(TAG, "receive failed: %s", ws_last_error(ws));
            break;
        }

        uint64_t recv_ms = mono_ms();
        proto_result_t res;

        /* The whole merge happens inside the lock, but it is only a parse of
         * an already-received buffer into fixed-size fields -- no I/O, no
         * drawing, and the parse tree is freed before we let go. */
        app_snapshot_t *st = state_lock(m->state);
        bool ok = protocol_apply(&st->playback, payload, len, recv_ms, &res);

        if (ok && res.type == PROTO_PONG)
            clock_pong_received(&st->clock, recv_ms, res.pong_timestamp_ms);

        bool snapshot = ok && res.type == PROTO_PLAYBACK_UPDATE;
        bool song_changed = ok && res.now_playing_changed;
        char title[SONG_TITLE_MAX], artist[SONG_ARTIST_MAX];
        snprintf(title, sizeof(title), "%s", st->playback.now_playing.title);
        snprintf(artist, sizeof(artist), "%s", st->playback.now_playing.artist);
        int version = st->playback.backend_version;
        state_unlock(m->state);

        if (!ok)
            continue;

        if (res.type == PROTO_BACKEND_VERSION) {
            LOGI(TAG, "backend version %d", version);
            if (version > EXPECTED_BACKEND_VERSION)
                LOGW(TAG, "backend is version %d but this client was built for "
                          "%d; some fields may be misread",
                     version, EXPECTED_BACKEND_VERSION);
        }

        if (snapshot)
            LOGI(TAG, "snapshot received (%zu bytes)", len);

        if (song_changed && title[0])
            LOGI(TAG, "now playing: %s - %s", artist, title);
    }

    ws_close(ws);
}

static void *meta_thread(void *arg)
{
    meta_t *m = arg;

    unsigned backoff = BACKOFF_MIN_MS;
    bool first = true;

    while (!m->stop) {
        if (!first) {
            app_snapshot_t *st = state_lock(m->state);
            st->ws_reconnects++;
            state_unlock(m->state);

            LOGI(TAG, "reconnecting in %u ms", backoff);
            set_connected(m, false, backoff);

            for (unsigned waited = 0; waited < backoff && !m->stop; waited += 100)
                mono_sleep_ms(100);
            if (m->stop)
                break;
        }
        first = false;

        uint64_t started = mono_ms();
        run_connection(m);
        set_connected(m, false, backoff);

        /* A connection that lasted treats the next failure as a fresh
         * incident, so an hourly blip does not creep the backoff up to 30s
         * and leave it there. */
        if (mono_ms() - started > 60000)
            backoff = BACKOFF_MIN_MS;
        else if (backoff < BACKOFF_MAX_MS)
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
    }

    LOGI(TAG, "metadata thread exiting");
    return NULL;
}

meta_t *meta_start(const char *url, conn_tls_ctx_t *tls, app_state_t *state)
{
    meta_t *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    snprintf(m->url, sizeof(m->url), "%s", url);
    m->tls = tls;
    m->state = state;

    if (pthread_create(&m->thread, NULL, meta_thread, m) != 0) {
        LOGE(TAG, "failed to start metadata thread");
        free(m);
        return NULL;
    }
    m->thread_started = true;
    return m;
}

void meta_stop(meta_t *m)
{
    if (!m)
        return;
    m->stop = true;
    if (m->thread_started)
        pthread_join(m->thread, NULL);
    free(m);
}
