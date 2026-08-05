#include "net/timesync.h"
#include "net/conn.h"
#include "net/http.h"
#include "net/url.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#define TAG "time"

/* 2023-01-01. Anything earlier is a clock that was never set rather than a
 * clock that is merely wrong -- no real device boots into 2022 by accident. */
#define PLAUSIBLE_AFTER 1672531200L

/* How far the local clock may trail the server before we reset it. Small enough
 * that we never sit on a clock wrong enough to matter for certificate dates,
 * large enough that request latency and rounding do not make every launch
 * rewrite the clock for no reason. */
#define SKEW_TOLERANCE_SEC 60

/* Set when the clock cannot be believed for certificate dates: either it was
 * never set, or we learned it was wrong and could not correct it. Written once
 * by the timesync thread and read by whichever threads are handshaking, which
 * is why it is only ever set, never cleared -- there is no state machine here
 * to race against. */
static volatile bool g_dates_untrusted;

bool timesync_clock_is_plausible(void)
{
    return (long)time(NULL) >= PLAUSIBLE_AFTER;
}

bool timesync_dates_are_trustworthy(void)
{
    return timesync_clock_is_plausible() && !g_dates_untrusted;
}

bool timesync_should_adopt(time_t server_now, time_t local_now)
{
    return server_now > local_now + SKEW_TOLERANCE_SEC;
}

bool timesync_parse_http_date(const char *value, time_t *out)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));

    if (!strptime(value, "%a, %d %b %Y %H:%M:%S", &tm))
        return false;

    /* HTTP dates are always GMT, so timegm rather than mktime -- mktime would
     * apply the local timezone and silently shift the result. */
    time_t t = timegm(&tm);
    if (t < PLAUSIBLE_AFTER)
        return false;

    *out = t;
    return true;
}

/* Asking only when the clock reads 1970 was not enough, and the failure it let
 * through is worth spelling out because it looks like a certificate problem and
 * is not.
 *
 * The device has no RTC, so its clock comes from whatever the firmware restored
 * at boot -- typically the timestamp saved at the last shutdown. A handheld that
 * spent three weeks in a drawer therefore boots into a date three weeks stale:
 * comfortably past the plausibility floor, so the old code returned immediately
 * and never corrected it. That was harmless only for as long as the server's
 * certificate had been issued longer ago than any such clock was stale. The
 * moment the certificate is renewed, it is "not yet valid" to every lagging
 * device, the handshake fails on the validity dates, and the reconnect backoff
 * climbs against something that cannot succeed -- the exact permanent failure
 * this file exists to prevent, just arriving through the other date bound.
 *
 * So: always ask, and adopt anything meaningfully ahead of us. */
bool timesync_bootstrap(const char *host)
{
    bool was_unset = !timesync_clock_is_plausible();

    if (was_unset)
        LOGW(TAG, "system clock is unset; asking http://%s for the time", host);
    else
        LOGI(TAG, "checking the system clock against http://%s", host);

    char url_str[320];
    snprintf(url_str, sizeof(url_str), "http://%s/", host);

    /* Every path that ends without a usable time leaves an unset clock unset,
     * and a clock that never got set cannot be used to judge validity dates. */
    url_t url;
    if (!url_parse(url_str, &url)) {
        g_dates_untrusted |= was_unset;
        return false;
    }

    /* Plain HTTP on purpose: this exists precisely because TLS cannot work
     * yet, so it must not depend on it. */
    conn_t *conn = conn_open(NULL, &url, 8000);
    if (!conn) {
        LOGE(TAG, "could not reach %s over plain HTTP: %s", host,
             conn_last_open_error());
        g_dates_untrusted |= was_unset;
        return false;
    }

    const http_header_t extra[] = { { "Connection", "close" } };

    http_stream_t hs;
    int rc = http_get(&hs, conn, &url, extra, 1, 8000);
    if (rc != 0) {
        LOGE(TAG, "no response from %s: %s", host, conn_last_error(conn));
        conn_close(conn);
        g_dates_untrusted |= was_unset;
        return false;
    }

    /* Any status will do -- a redirect or even a 404 still carries Date. */
    char date[128];
    bool ok = false;
    if (http_header(&hs, "Date", date, sizeof(date))) {
        time_t t;
        if (timesync_parse_http_date(date, &t)) {
            time_t local = time(NULL);

            if (!timesync_should_adopt(t, local)) {
                /* Either we are already in step, or we are ahead of the server.
                 * We do not rewind for the reason given in timesync.h, so a
                 * clock running fast stays fast; say so, because from here it
                 * surfaces as certificates that look expired. */
                if (local - t > SKEW_TOLERANCE_SEC)
                    LOGW(TAG, "system clock is %lds ahead of %s and will not be "
                              "rewound; certificates may appear expired",
                         (long)(local - t), host);
                else
                    LOGI(TAG, "system clock agrees with %s", host);
                ok = true;
            } else {
                struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
                if (settimeofday(&tv, NULL) == 0) {
                    LOGI(TAG, "system clock advanced %lds from HTTP Date: %s",
                         (long)(t - local), date);
                    ok = true;
                } else {
                    /* Typically means we are not root. We know the clock is
                     * behind and cannot fix it, so validity dates are no longer
                     * something we can judge -- TLS relaxes them instead. */
                    LOGE(TAG, "clock is %lds behind and could not be set "
                              "(need root?); TLS date checks will be relaxed "
                              "instead", (long)(t - local));
                    g_dates_untrusted = true;
                }
            }
        } else {
            LOGE(TAG, "could not parse Date header '%s'", date);
            g_dates_untrusted |= was_unset;
        }
    } else {
        LOGE(TAG, "response from %s carried no Date header", host);
        g_dates_untrusted |= was_unset;
    }

    conn_close(conn);
    return ok;
}
