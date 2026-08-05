#include "net/timesync.h"
#include "util/log.h"
#include "tap.h"

/* The clock bootstrap is what makes TLS possible on this device at all, and a
 * date parser that returns the wrong year would set the clock wrong and fail
 * in a way indistinguishable from never having run. */

int main(void)
{
    log_init(LOG_ERROR);

    time_t t;

    /* The exact shape hypr.website returns. */
    CHECK(timesync_parse_http_date("Mon, 20 Jul 2026 07:09:24 GMT", &t),
          "parses a real Date header");
    CHECK_INT(t, 1784531364L);

    /* Every weekday and month name has to work, not just the one we saw. */
    CHECK(timesync_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT", &t) == false,
          "epoch zero is rejected as implausible");

    CHECK(timesync_parse_http_date("Fri, 31 Dec 2027 23:59:59 GMT", &t),
          "parses a year-end date");
    CHECK_INT(t, 1830297599L);

    CHECK(timesync_parse_http_date("Sun, 01 Mar 2026 00:00:00 GMT", &t),
          "parses March");
    CHECK_INT(t, 1772323200L);

    /* GMT is implied. A mktime-based parser would apply the host timezone and
     * be wrong by hours; timegm is what keeps this right. */
    CHECK(timesync_parse_http_date("Wed, 01 Jul 2026 12:00:00 GMT", &t),
          "parses midday");
    CHECK_INT(t, 1782907200L);

    /* Garbage must be rejected rather than yielding a plausible-looking time. */
    CHECK(!timesync_parse_http_date("not a date", &t), "garbage rejected");
    CHECK(!timesync_parse_http_date("", &t), "empty rejected");
    CHECK(!timesync_parse_http_date("Mon, 20 Jul", &t), "truncated rejected");

    /* Forward-only adoption is the entire security argument for taking the time
     * from an unauthenticated HTTP header, so it gets pinned down here rather
     * than left to a network test that cannot choose its inputs. */
    const time_t NOW = 1785801600L;   /* 2026-08-05 */

    CHECK(timesync_should_adopt(NOW + 86400L * 21, NOW),
          "a clock three weeks behind is corrected");
    CHECK(timesync_should_adopt(NOW + 3600, NOW),
          "an hour behind is corrected");

    /* Rewinding is what would let a spoofed Date revive an expired certificate,
     * so it must never happen -- not by a month, not by a second. */
    CHECK(!timesync_should_adopt(NOW - 86400L * 21, NOW),
          "a clock three weeks ahead is not rewound");
    CHECK(!timesync_should_adopt(NOW - 1, NOW),
          "a clock one second ahead is not rewound");

    /* Request latency and rounding must not make every launch rewrite the
     * clock, but the tolerance has to stay far below any certificate margin. */
    CHECK(!timesync_should_adopt(NOW + 30, NOW),
          "30s of skew is left alone");
    CHECK(!timesync_should_adopt(NOW, NOW),
          "an exactly-correct clock is left alone");
    CHECK(timesync_should_adopt(NOW + 61, NOW),
          "61s of skew is corrected");

    TAP_DONE();
}
