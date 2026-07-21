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

bool timesync_clock_is_plausible(void)
{
    return (long)time(NULL) >= PLAUSIBLE_AFTER;
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

bool timesync_bootstrap(const char *host)
{
    if (timesync_clock_is_plausible())
        return true;

    LOGW(TAG, "system clock is unset; asking http://%s for the time", host);

    char url_str[320];
    snprintf(url_str, sizeof(url_str), "http://%s/", host);

    url_t url;
    if (!url_parse(url_str, &url))
        return false;

    /* Plain HTTP on purpose: this exists precisely because TLS cannot work
     * yet, so it must not depend on it. */
    conn_t *conn = conn_open(NULL, &url, 8000);
    if (!conn) {
        LOGE(TAG, "could not reach %s over plain HTTP: %s", host,
             conn_last_open_error());
        return false;
    }

    const http_header_t extra[] = { { "Connection", "close" } };

    http_stream_t hs;
    int rc = http_get(&hs, conn, &url, extra, 1, 8000);
    if (rc != 0) {
        LOGE(TAG, "no response from %s: %s", host, conn_last_error(conn));
        conn_close(conn);
        return false;
    }

    /* Any status will do -- a redirect or even a 404 still carries Date. */
    char date[128];
    bool ok = false;
    if (http_header(&hs, "Date", date, sizeof(date))) {
        time_t t;
        if (timesync_parse_http_date(date, &t)) {
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            if (settimeofday(&tv, NULL) == 0) {
                LOGI(TAG, "system clock set from HTTP Date: %s", date);
                ok = true;
            } else {
                /* Typically means we are not root. The caller falls back to
                 * tolerating certificate date failures. */
                LOGE(TAG, "could not set the clock (need root?); "
                          "TLS date checks will be relaxed instead");
            }
        } else {
            LOGE(TAG, "could not parse Date header '%s'", date);
        }
    } else {
        LOGE(TAG, "response from %s carried no Date header", host);
    }

    conn_close(conn);
    return ok;
}
