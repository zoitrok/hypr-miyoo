#ifndef HYPR_TIMESYNC_H
#define HYPR_TIMESYNC_H

#include <stdbool.h>
#include <time.h>

/* Bootstraps the system clock from an HTTP Date header.
 *
 * The Miyoo has no RTC, and on this firmware nothing sets the clock at boot --
 * it simply counts from zero, so it reads early January 1970. That breaks TLS
 * completely and permanently: every certificate is "not yet valid", the
 * handshake fails, and the reconnect backoff climbs against something that can
 * never succeed. It is not a network fault and does not heal on its own.
 *
 * The fix has to come from outside, and cannot itself need TLS -- so we ask
 * over plain HTTP, where every response carries a Date header, and set the
 * clock from it.
 *
 * The obvious objection is that plain HTTP is spoofable, and someone who can
 * do that could rewind our clock to revive an expired certificate. The answer
 * is that we never rewind it: the Date header is adopted only when it moves the
 * clock forward, so a spoofer can push us into the future -- which makes
 * certificates look expired and fails closed -- and cannot pull us back. That
 * leaves the risk narrow, and the alternative is worse: without this the only
 * ways to run are to disable certificate verification outright or not to run at
 * all. Everything else about verification stays strict -- we still pin to the
 * ISRG roots and still check the hostname.
 *
 * This asks on every run, not only when the clock reads 1970. A clock can be
 * far enough wrong to break TLS while still being obviously "set"; see the
 * comment on the skew tolerance in timesync.c.
 *
 * Returns true if the clock is trustworthy afterwards.
 */
bool timesync_bootstrap(const char *host);

/* True when the system clock is too early to be real, and therefore when TLS
 * date checks cannot be believed. */
bool timesync_clock_is_plausible(void);

/* True when certificate validity dates can be judged against this clock.
 *
 * Distinct from timesync_clock_is_plausible(): a clock can pass the plausibility
 * floor and still be useless for date checks, either because it was never
 * corrected or because we found it wrong and could not fix it. TLS verification
 * keys its date handling off this, not off plausibility. */
bool timesync_dates_are_trustworthy(void);

/* True when a server-supplied time should replace the local clock: only when it
 * runs ahead of local by more than the skew tolerance. Exposed for testing --
 * this one comparison is the entire security property of the bootstrap, so it
 * is worth pinning down without a network. */
bool timesync_should_adopt(time_t server_now, time_t local_now);

/* Parses an RFC 1123 HTTP date ("Sun, 20 Jul 2026 10:04:33 GMT") to epoch
 * seconds. Exposed for testing: a date parser that quietly returns the wrong
 * year would set the clock wrong and break TLS in a way that looks identical
 * to not having run at all. */
bool timesync_parse_http_date(const char *value, time_t *out);

#endif
