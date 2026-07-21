#ifndef HYPR_META_H
#define HYPR_META_H

#include "net/conn.h"
#include "state/state.h"

/* Owns a background thread that keeps app_state fed from the WebSocket:
 * connect -> handshake -> receive -> merge, reconnecting forever on failure,
 * and pinging periodically both to keep the connection honest and to estimate
 * the server clock offset.
 *
 * Mirrors audio/stream.h in shape on purpose: a dead network shows up as
 * ws_connected going false and the UI saying so, never as a stall in the
 * renderer. */

typedef struct meta meta_t;

/* Starts the thread immediately; it connects asynchronously.
 * url must outlive the returned handle. */
meta_t *meta_start(const char *url, conn_tls_ctx_t *tls, app_state_t *state);

/* Signals the thread to stop and joins it. Safe to call once. */
void meta_stop(meta_t *m);

#endif
