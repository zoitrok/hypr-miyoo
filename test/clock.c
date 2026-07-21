#include "state/clock.h"
#include "util/log.h"
#include "tap.h"

#include <math.h>

/* The device learns the time over the network rather than from an RTC. That
 * makes the wall clock right eventually but badly behaved at launch (arbitrary
 * until the sync lands) and, worse, it *steps* when the sync does land --
 * possibly mid-song. These tests pin down that a wall-clock step cannot
 * perturb song progress, which is the whole reason this module exists. */

/* A whole second, so that deriving start_time (epoch SECONDS) from it is
 * exact and the expected elapsed values are obvious. Real start_time really is
 * whole seconds while server time carries milliseconds, so production elapsed
 * legitimately has a fractional part -- exercised separately below. */
#define SERVER_EPOCH_MS 1784469127000LL

int main(void)
{
    log_init(LOG_ERROR);

    /* --- offset estimation ------------------------------------------------ */
    {
        hypr_clock_t c;
        clock_init(&c);

        CHECK_INT(c.have_offset, 0);
        CHECK_INT(clock_server_now_ms(&c, 1000), 0);

        /* An unsolicited pong must be ignored: without a matching send time
         * there is no round trip to take the midpoint of. */
        CHECK(!clock_pong_received(&c, 5000, SERVER_EPOCH_MS), "unsolicited pong ignored");
        CHECK_INT(c.have_offset, 0);

        /* Ping at mono 1000, pong at mono 1100, server says T.
         * Midpoint is 1050, so offset = T - 1050. */
        clock_ping_sent(&c, 1000);
        CHECK(clock_pong_received(&c, 1100, SERVER_EPOCH_MS), "pong accepted");
        CHECK_INT(c.have_offset, 1);
        CHECK_INT(c.offset_ms, SERVER_EPOCH_MS - 1050);
        CHECK_INT(c.last_rtt_ms, 100);

        /* Server time at any monotonic instant. */
        CHECK_INT(clock_server_now_ms(&c, 2050), SERVER_EPOCH_MS + 1000);
    }

    /* A wildly delayed response is discarded rather than averaged in: its
     * midpoint assumption is least valid exactly when the network is worst. */
    {
        hypr_clock_t c;
        clock_init(&c);

        clock_ping_sent(&c, 1000);
        clock_pong_received(&c, 1020, SERVER_EPOCH_MS); /* rtt 20, best */
        int64_t good = c.offset_ms;

        clock_ping_sent(&c, 2000);
        CHECK(!clock_pong_received(&c, 4000, SERVER_EPOCH_MS + 1000),
              "2000ms rtt rejected against a 20ms best");
        CHECK_INT(c.offset_ms, good);
    }

    /* Successive good samples are smoothed, so one mildly noisy reading
     * nudges rather than yanks the estimate. */
    {
        hypr_clock_t c;
        clock_init(&c);

        clock_ping_sent(&c, 1000);
        clock_pong_received(&c, 1020, SERVER_EPOCH_MS);
        int64_t first = c.offset_ms;

        clock_ping_sent(&c, 2000);
        clock_pong_received(&c, 2020, SERVER_EPOCH_MS + 1000 + 400);

        CHECK(c.offset_ms > first, "offset moved toward the new sample");
        CHECK(c.offset_ms < first + 400, "but not all the way to it");
        CHECK_INT(c.samples, 2);
    }

    /* --- song progress ---------------------------------------------------- */
    {
        hypr_clock_t c;
        clock_init(&c);

        const int64_t start_s = SERVER_EPOCH_MS / 1000 - 30; /* 30s ago */
        const int length = 200;

        /* Before any sync, progress counts forward from when the song was
         * received: the wrong origin but the right rate, which is far better
         * than a number derived from an unsynced wall clock. */
        double e = clock_song_elapsed(&c, 5000, start_s, length, 3000);
        CHECK(fabs(e - 2.0) < 0.01, "unsynced fallback counts from receipt (%.2f)", e);

        /* Once synced, it reports the true position in the song. */
        clock_ping_sent(&c, 10000);
        clock_pong_received(&c, 10000, SERVER_EPOCH_MS);
        e = clock_song_elapsed(&c, 10000, start_s, length, 3000);
        CHECK(fabs(e - 30.0) < 0.01, "synced elapsed is the true position (%.2f)", e);

        /* And it advances in real time. */
        e = clock_song_elapsed(&c, 15000, start_s, length, 3000);
        CHECK(fabs(e - 35.0) < 0.01, "elapsed advances with the clock (%.2f)", e);

        /* Clamped at both ends: a progress bar must never read negative or
         * run past the end while waiting for the next now_playing. */
        e = clock_song_elapsed(&c, 10000, SERVER_EPOCH_MS / 1000 + 60, length, 3000);
        CHECK_INT((int)e, 0);

        e = clock_song_elapsed(&c, 10000 + 999999, start_s, length, 3000);
        CHECK_INT((int)e, length);

        /* A song with no start_time falls back rather than reporting garbage. */
        e = clock_song_elapsed(&c, 5000, 0, length, 3000);
        CHECK(fabs(e - 2.0) < 0.01, "no start_time falls back to receipt (%.2f)", e);

        /* start_time is whole seconds but server time carries milliseconds, so
         * elapsed has a real fractional part. Keeping it is what lets the
         * progress bar move smoothly rather than stepping once a second. */
        hypr_clock_t frac;
        clock_init(&frac);
        clock_ping_sent(&frac, 10000);
        clock_pong_received(&frac, 10000, SERVER_EPOCH_MS + 250);
        e = clock_song_elapsed(&frac, 10000, start_s, length, 3000);
        CHECK(fabs(e - 30.25) < 0.01,
              "sub-second precision is preserved (%.3f)", e);
    }

    /* --- the case this module exists for ----------------------------------
     * The network time sync lands mid-song and steps the wall clock. Nothing
     * here reads the wall clock, so progress must be completely unaffected --
     * the monotonic clock and the server offset both survive the step. */
    {
        hypr_clock_t c;
        clock_init(&c);

        const int64_t start_s = SERVER_EPOCH_MS / 1000 - 30;

        clock_ping_sent(&c, 10000);
        clock_pong_received(&c, 10000, SERVER_EPOCH_MS);

        double before = clock_song_elapsed(&c, 12000, start_s, 200, 3000);

        /* Simulate the step: the wall clock jumps years, but mono_ms() and our
         * offset are untouched, so the next ping/pong agrees with the last. */
        clock_ping_sent(&c, 13000);
        clock_pong_received(&c, 13000, SERVER_EPOCH_MS + 3000);

        double after = clock_song_elapsed(&c, 13000, start_s, 200, 3000);

        CHECK(fabs((after - before) - 1.0) < 0.05,
              "progress advanced 1s across a wall-clock step, not jumped "
              "(%.2f -> %.2f)", before, after);
    }

    /* --- relative ages for chat lines -------------------------------------- */
    {
        hypr_clock_t c;
        clock_init(&c);

        char out[16];

        /* With no offset yet there is no meaningful age, and "now" reads
         * better than a negative number or a blank. */
        CHECK(clock_age_seconds(&c, 1000, 1784469000.0) < 0, "no offset -> no age");
        clock_format_age(-1, out, sizeof(out));
        CHECK_STR(out, "now");

        clock_ping_sent(&c, 10000);
        clock_pong_received(&c, 10000, SERVER_EPOCH_MS);

        double base = (double)SERVER_EPOCH_MS / 1000.0;
        CHECK(fabs(clock_age_seconds(&c, 10000, base - 300) - 300.0) < 0.01,
              "age of a 5-minute-old line");

        /* A message stamped slightly in the future is what a marginally off
         * offset produces for something posted seconds ago; it must not
         * render as a negative age. */
        CHECK(clock_age_seconds(&c, 10000, base + 2) < 0, "future timestamp is negative");
        clock_format_age(clock_age_seconds(&c, 10000, base + 2), out, sizeof(out));
        CHECK_STR(out, "now");

        clock_format_age(0, out, sizeof(out));      CHECK_STR(out, "now");
        clock_format_age(59, out, sizeof(out));     CHECK_STR(out, "now");
        clock_format_age(60, out, sizeof(out));     CHECK_STR(out, "1m");
        clock_format_age(3599, out, sizeof(out));   CHECK_STR(out, "59m");
        clock_format_age(3600, out, sizeof(out));   CHECK_STR(out, "1h");
        clock_format_age(86399, out, sizeof(out));  CHECK_STR(out, "23h");
        clock_format_age(86400, out, sizeof(out));  CHECK_STR(out, "1d");
        clock_format_age(86400 * 9, out, sizeof(out)); CHECK_STR(out, "9d");

        /* A timestamp of 0 means the field was absent. */
        CHECK(clock_age_seconds(&c, 10000, 0.0) < 0, "absent timestamp -> no age");
    }

    TAP_DONE();
}
