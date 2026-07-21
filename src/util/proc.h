#ifndef HYPR_PROC_H
#define HYPR_PROC_H

#include <stdbool.h>

/* Self-measurement for soak runs.
 *
 * A multi-hour run exists to answer "does this leak?", and without these
 * numbers in the log it cannot: everything would look fine right up until the
 * device ran out of memory. Both are cheap enough to sample every 30 seconds
 * and are read from /proc, so they cost no bookkeeping in the hot paths.
 *
 * Resident size catches the heap growth that fixed-size PODs and prompt
 * cJSON_Delete are supposed to prevent. Open descriptors catch a leaked socket
 * per reconnect, which is the specific failure a chaos soak provokes and which
 * would otherwise stay invisible until connections started failing. */

/* Resident set size in KB, or 0 if unavailable. */
long proc_rss_kb(void);

/* Number of open file descriptors, or -1 if unavailable. */
int proc_open_fds(void);

#endif
