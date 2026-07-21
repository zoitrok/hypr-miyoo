#ifndef HYPR_MONO_H
#define HYPR_MONO_H

#include <stdint.h>

/* Monotonic milliseconds since some unspecified epoch (process start, boot --
 * it does not matter, only differences are meaningful).
 *
 * Everything that measures elapsed time in this app must use this rather than
 * time(NULL). The Miyoo Mini Plus has no battery-backed RTC, so its wall clock
 * is arbitrary until NTP runs -- and NTP may not have run when we launch, or
 * may step the clock underneath us while we are running. A wall-clock delta can
 * be negative, or off by years. See state/clock.c for how we reconcile this
 * against the server's timestamps. */
uint64_t mono_ms(void);

/* Sleep for at least ms milliseconds, resuming across EINTR. */
void mono_sleep_ms(uint64_t ms);

#endif
