#include "util/mono.h"

#include <errno.h>
#include <time.h>

uint64_t mono_ms(void)
{
    struct timespec ts;
    /* CLOCK_MONOTONIC does not tick while suspended, which is what we want:
     * if the handheld sleeps mid-song we would rather under-report elapsed
     * time and have the next now_playing resync us than jump forward. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

void mono_sleep_ms(uint64_t ms)
{
    struct timespec req;
    req.tv_sec = (time_t)(ms / 1000u);
    req.tv_nsec = (long)((ms % 1000u) * 1000000L);

    while (nanosleep(&req, &req) != 0 && errno == EINTR)
        ; /* resume with the remaining time nanosleep wrote back */
}
