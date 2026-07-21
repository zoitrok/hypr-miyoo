#include "state/clock.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#define TAG "clock"

/* Weight of each new sample in the low-pass filter. Low enough that one slow
 * response cannot yank the estimate, high enough to track a genuine drift or a
 * reconnect to a differently-skewed server within a few pings. */
#define SMOOTHING 0.25

/* A sample whose round trip is this much worse than the best we have seen is
 * discarded. Its midpoint assumption (that the request and response legs are
 * symmetric) is least likely to hold exactly when the network is congested. */
#define RTT_REJECT_FACTOR 4

void clock_init(hypr_clock_t *c)
{
    memset(c, 0, sizeof(*c));
    c->best_rtt_ms = UINT64_MAX;
}

void clock_ping_sent(hypr_clock_t *c, uint64_t now_mono_ms)
{
    c->pending_send_ms = now_mono_ms;
    c->pending = true;
}

bool clock_pong_received(hypr_clock_t *c, uint64_t now_mono_ms,
                         int64_t server_ms)
{
    if (!c->pending || server_ms <= 0)
        return false;

    c->pending = false;

    uint64_t rtt = now_mono_ms - c->pending_send_ms;
    c->last_rtt_ms = rtt;

    /* The server stamped the pong somewhere inside the round trip; without
     * more information the midpoint is the best guess, and half the RTT bounds
     * the error. */
    int64_t midpoint = (int64_t)(c->pending_send_ms + now_mono_ms) / 2;
    int64_t sample = server_ms - midpoint;

    if (!c->have_offset) {
        c->offset_ms = sample;
        c->have_offset = true;
        c->best_rtt_ms = rtt;
        c->samples = 1;
        LOGI(TAG, "server clock offset %lld ms (rtt %llu ms)",
             (long long)c->offset_ms, (unsigned long long)rtt);
        return true;
    }

    if (rtt > c->best_rtt_ms * RTT_REJECT_FACTOR) {
        LOGD(TAG, "discarding noisy sample (rtt %llu ms vs best %llu ms)",
             (unsigned long long)rtt, (unsigned long long)c->best_rtt_ms);
        return false;
    }

    if (rtt < c->best_rtt_ms)
        c->best_rtt_ms = rtt;

    int64_t before = c->offset_ms;
    c->offset_ms = (int64_t)((double)c->offset_ms * (1.0 - SMOOTHING) +
                             (double)sample * SMOOTHING);
    c->samples++;

    /* A large correction means the device's clock was stepped (NTP finally
     * landing, most likely). Worth a line in the log, since it explains any
     * one-off jump in the progress display. */
    if (before - c->offset_ms > 2000 || c->offset_ms - before > 2000)
        LOGI(TAG, "server clock offset moved %lld -> %lld ms",
             (long long)before, (long long)c->offset_ms);

    return true;
}

int64_t clock_server_now_ms(const hypr_clock_t *c, uint64_t now_mono_ms)
{
    if (!c->have_offset)
        return 0;
    return (int64_t)now_mono_ms + c->offset_ms;
}

double clock_song_elapsed(const hypr_clock_t *c, uint64_t now_mono_ms,
                          int64_t start_time_s, int length_s,
                          uint64_t received_mono_ms)
{
    double elapsed;

    if (c->have_offset && start_time_s > 0) {
        int64_t server_now_ms = clock_server_now_ms(c, now_mono_ms);
        elapsed = (double)(server_now_ms - start_time_s * 1000) / 1000.0;
    } else {
        /* No offset yet, or a song with no start time. Counting forward from
         * when we heard about it is off by however far in the song we joined,
         * but it advances at the right rate and is never absurd -- which is
         * what matters for a progress bar. */
        elapsed = (double)(now_mono_ms - received_mono_ms) / 1000.0;
    }

    if (elapsed < 0)
        elapsed = 0;
    if (length_s > 0 && elapsed > length_s)
        elapsed = length_s;

    return elapsed;
}

double clock_age_seconds(const hypr_clock_t *c, uint64_t now_mono_ms,
                         double timestamp_s)
{
    if (!c->have_offset || timestamp_s <= 0)
        return -1.0;
    return (double)clock_server_now_ms(c, now_mono_ms) / 1000.0 - timestamp_s;
}

void clock_format_age(double age_seconds, char *out, size_t outlen)
{
    if (age_seconds < 0) {
        /* Either the clock is not synced yet, or the message is a few seconds
         * "in the future" because our offset is slightly off. Both read better
         * as "now" than as a negative age. */
        snprintf(out, outlen, "now");
        return;
    }

    if (age_seconds < 60)
        snprintf(out, outlen, "now");
    else if (age_seconds < 3600)
        snprintf(out, outlen, "%dm", (int)(age_seconds / 60));
    else if (age_seconds < 86400)
        snprintf(out, outlen, "%dh", (int)(age_seconds / 3600));
    else
        snprintf(out, outlen, "%dd", (int)(age_seconds / 86400));
}
