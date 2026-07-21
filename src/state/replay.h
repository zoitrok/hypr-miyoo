#ifndef HYPR_REPLAY_H
#define HYPR_REPLAY_H

#include "state/state.h"

/* Replays a recorded WebSocket transcript into app_state, in place of a live
 * connection. This is what makes the whole UI developable with no network and
 * no server -- record once with `wsdump --record`, then iterate against it.
 *
 * Presents the same start/stop shape as meta.h so main.c can swap one for the
 * other without knowing which it has.
 *
 * Timestamps in the transcript are honoured, so a song change that happened
 * four minutes into the recording happens four minutes into the replay. The
 * file loops when it ends. */

typedef struct replay replay_t;

replay_t *replay_start(const char *path, app_state_t *state);
void replay_stop(replay_t *r);

#endif
