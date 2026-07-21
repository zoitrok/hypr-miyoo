#ifndef HYPR_PROTOCOL_H
#define HYPR_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "state/model.h"

/* Turns a server message into a mutation of playback_t.
 *
 * The merge semantics matter and are easy to get subtly wrong:
 *
 *   playback_update  REPLACES the entire local state
 *   *_update         merges into ONLY its own key(s), leaving the rest alone
 *
 * Getting that backwards does not fail loudly -- it slowly corrupts state, so
 * the queue or history quietly goes stale or blank while everything still
 * looks like it is working. */

typedef enum {
    PROTO_UNKNOWN = 0,
    PROTO_BACKEND_VERSION,
    PROTO_PONG,
    PROTO_PLAYBACK_UPDATE,
    PROTO_NOW_PLAYING_UPDATE,
    PROTO_QUEUE_UPDATE,
    PROTO_HISTORY_UPDATE,
    PROTO_ONELINER_UPDATE,
    PROTO_COMMENTS_UPDATE,
    PROTO_CONNECTIONS_UPDATE
} proto_msg_t;

typedef struct {
    proto_msg_t type;

    /* PROTO_PONG only: server time in epoch MILLISECONDS. Note that song
     * start_time is in SECONDS -- the protocol mixes units. */
    int64_t pong_timestamp_ms;

    /* PROTO_BACKEND_VERSION only. */
    int backend_version;

    /* True if the merge changed the current song, so the caller can resync the
     * progress clock rather than let drift accumulate. */
    bool now_playing_changed;
} proto_result_t;

/* Parses one message and merges it into pb. now_mono_ms is passed in rather
 * than read internally so tests can drive time deterministically.
 *
 * Returns false only if the message is not valid JSON or has no "type"; an
 * unrecognised type is not an error (the backend may add messages we do not
 * care about) and yields PROTO_UNKNOWN. */
bool protocol_apply(playback_t *pb, const char *json, size_t len,
                    uint64_t now_mono_ms, proto_result_t *out);

/* Exposed for tests: the backend encodes tags as a JSON *string* containing a
 * JSON array ("[\"2017\", \"Revision\"]"), so it needs a second parse pass.
 * Flattens to "2017, Revision". Always NUL-terminates. */
void protocol_flatten_tags(const char *raw, char *out, size_t outlen);

#endif
