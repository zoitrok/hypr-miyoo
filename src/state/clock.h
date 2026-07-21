#ifndef HYPR_CLOCK_H
#define HYPR_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Song progress without depending on the wall clock.
 *
 * The Miyoo Mini Plus has no battery-backed RTC; it learns the time over the
 * network once WiFi is up. That makes the wall clock correct *eventually*, but
 * badly behaved exactly when we care:
 *
 *   - At launch it is arbitrary. The app can easily start before WiFi
 *     associates and the time sync lands, so early frames would show nonsense.
 *   - When the sync does land it *steps* the clock, potentially by years, and
 *     potentially while a song is playing. Anything computing progress as
 *     `time(NULL) - start_time` jumps at that moment, which reads as a UI bug.
 *
 * The second is the reason this module exists rather than a startup delay: a
 * monotonic clock plus a server offset is unaffected by a step, so the display
 * stays smooth through the very event that would otherwise break it.
 *
 * Instead we estimate the offset between the server's clock and our monotonic
 * clock, from the round trip of the application-level ping/pong:
 *
 *     offset ~= server_time - (t_send + t_recv) / 2
 *
 * with half the round trip as the error bound. Samples are low-passed because
 * any single one can be skewed by a slow response; a sample whose round trip
 * is much worse than the best seen is discarded rather than averaged in.
 *
 * Note the protocol mixes units: pong timestamps are epoch milliseconds while
 * song start_time is epoch seconds. Everything here works in milliseconds and
 * converts at the edges. */

typedef struct {
    bool     have_offset;
    int64_t  offset_ms;      /* server_epoch_ms - mono_ms() */
    uint64_t best_rtt_ms;
    uint64_t last_rtt_ms;
    unsigned samples;

    uint64_t pending_send_ms; /* mono_ms() when the outstanding ping went out */
    bool     pending;
} hypr_clock_t;

void clock_init(hypr_clock_t *c);

/* Record that a ping was sent at now_mono_ms. */
void clock_ping_sent(hypr_clock_t *c, uint64_t now_mono_ms);

/* Feed a pong. server_ms is the server's epoch-milliseconds timestamp.
 * Returns true if the sample was accepted (false if unsolicited or discarded
 * as too noisy). */
bool clock_pong_received(hypr_clock_t *c, uint64_t now_mono_ms,
                         int64_t server_ms);

/* Server epoch milliseconds corresponding to now_mono_ms. Returns 0 when no
 * offset has been established yet. */
int64_t clock_server_now_ms(const hypr_clock_t *c, uint64_t now_mono_ms);

/* Seconds elapsed in a song that started at start_time_s (epoch SECONDS on the
 * server clock), clamped to [0, length_s].
 *
 * received_mono_ms is when we learned about the song; before any clock offset
 * exists we fall back to counting forward from that, which is correct to
 * within the song's first-seen position and never wildly wrong. */
double clock_song_elapsed(const hypr_clock_t *c, uint64_t now_mono_ms,
                          int64_t start_time_s, int length_s,
                          uint64_t received_mono_ms);

/* Age in seconds of something stamped at timestamp_s (epoch SECONDS,
 * fractional -- the units chat lines use). Negative if it is in the future,
 * which a slightly-off clock can produce for a message posted seconds ago.
 * Returns -1 when no offset is known yet or the timestamp is absent. */
double clock_age_seconds(const hypr_clock_t *c, uint64_t now_mono_ms,
                         double timestamp_s);

/* Formats an age as "now", "5m", "3h", "2d" into out. Compact on purpose:
 * this sits at the end of a chat line on a 640px screen. */
void clock_format_age(double age_seconds, char *out, size_t outlen);

#endif
