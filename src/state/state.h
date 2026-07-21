#ifndef HYPR_STATE_H
#define HYPR_STATE_H

#include <stdbool.h>

#include "state/clock.h"
#include "state/model.h"

/* The one piece of state shared between the metadata thread (writer) and the
 * renderer (reader).
 *
 * The renderer takes a whole-struct snapshot under the lock and then draws from
 * its private copy. That is deliberate: holding a lock across drawing would let
 * a slow network write stall a frame, and the struct is a few KB, so copying it
 * is far cheaper than the contention it avoids. */

typedef struct {
    playback_t   playback;
    hypr_clock_t clock;

    bool     ws_connected;
    unsigned ws_reconnects;
    unsigned backoff_ms;

    /* Why the WebSocket last failed to connect. Empty when healthy. */
    char ws_error[160];
} app_snapshot_t;

typedef struct app_state app_state_t;

app_state_t *state_new(void);
void state_free(app_state_t *s);

/* Writer side. Mutate the returned struct, then unlock. Keep the critical
 * section short -- never parse or draw inside it. */
app_snapshot_t *state_lock(app_state_t *s);
void state_unlock(app_state_t *s);

/* Reader side: one memcpy under the lock. */
void state_snapshot(app_state_t *s, app_snapshot_t *out);

#endif
