#include "state/protocol.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON/cJSON.h"

#define TAG "proto"

/* ---------------------------------------------------------------- helpers */

/* Copies into a fixed buffer, truncating only on a UTF-8 character boundary.
 *
 * snprintf would cut mid-sequence and leave a dangling lead byte, producing
 * invalid UTF-8 that the renderer would then have to defend against. Chat
 * lines and demoscene handles both routinely carry multi-byte characters, and
 * a long line hitting the cap is normal rather than exceptional. */
static void copy_utf8(char *dst, size_t cap, const char *src)
{
    dst[0] = '\0';
    if (!src || cap == 0)
        return;

    size_t out = 0;
    for (size_t i = 0; src[i]; ) {
        unsigned char c = (unsigned char)src[i];

        size_t clen = 1;
        if (c >= 0xf0)      clen = 4;
        else if (c >= 0xe0) clen = 3;
        else if (c >= 0xc0) clen = 2;
        /* A stray continuation byte keeps clen 1 so we always make progress
         * rather than looping on malformed input. */

        if (out + clen + 1 > cap)
            break;

        for (size_t k = 0; k < clen && src[i + k]; k++)
            dst[out++] = src[i + k];
        i += clen;
    }
    dst[out] = '\0';
}

static void copy_str(char *dst, size_t cap, const cJSON *node)
{
    if (cJSON_IsString(node) && node->valuestring)
        copy_utf8(dst, cap, node->valuestring);
    else
        dst[0] = '\0';
}

static int get_int(const cJSON *obj, const char *key, int fallback)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) ? (int)n->valuedouble : fallback;
}

static int64_t get_i64(const cJSON *obj, const char *key, int64_t fallback)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) ? (int64_t)n->valuedouble : fallback;
}

static double get_double(const cJSON *obj, const char *key, double fallback)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) ? n->valuedouble : fallback;
}

void protocol_flatten_tags(const char *raw, char *out, size_t outlen)
{
    out[0] = '\0';
    if (!raw || !*raw)
        return;

    /* The value arrives as a string that itself contains JSON, so it needs a
     * second parse. If that fails, fall back to showing the raw text rather
     * than nothing -- a weird tag line beats a blank one. */
    cJSON *arr = cJSON_Parse(raw);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        snprintf(out, outlen, "%s", raw);
        return;
    }

    size_t used = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (!cJSON_IsString(item) || !item->valuestring)
            continue;
        int n = snprintf(out + used, outlen - used, "%s%s",
                         used ? ", " : "", item->valuestring);
        if (n < 0 || (size_t)n >= outlen - used) {
            out[outlen - 1] = '\0';
            break;
        }
        used += (size_t)n;
    }

    cJSON_Delete(arr);
}

/* Projects one enriched song object into a fixed-size struct. */
static void parse_song(const cJSON *o, song_t *s)
{
    memset(s, 0, sizeof(*s));
    if (!cJSON_IsObject(o))
        return;

    s->valid = true;
    s->id = get_int(o, "id", 0);

    copy_str(s->title, sizeof(s->title), cJSON_GetObjectItemCaseSensitive(o, "title"));
    copy_str(s->artist, sizeof(s->artist),
             cJSON_GetObjectItemCaseSensitive(o, "artist_name"));
    copy_str(s->platform, sizeof(s->platform),
             cJSON_GetObjectItemCaseSensitive(o, "platform"));
    copy_str(s->brief, sizeof(s->brief),
             cJSON_GetObjectItemCaseSensitive(o, "platform_brief"));
    copy_str(s->requested_by, sizeof(s->requested_by),
             cJSON_GetObjectItemCaseSensitive(o, "requested_by_username"));

    /* A human requester takes precedence over the auto-DJ when both are set --
     * "requested by someone" is the more interesting fact to show. */
    const cJSON *human =
        cJSON_GetObjectItemCaseSensitive(o, "human_requester_username");
    if (cJSON_IsString(human) && human->valuestring && human->valuestring[0])
        copy_str(s->requested_by, sizeof(s->requested_by), human);

    const cJSON *tags = cJSON_GetObjectItemCaseSensitive(o, "tags");
    if (cJSON_IsString(tags) && tags->valuestring)
        protocol_flatten_tags(tags->valuestring, s->tags, sizeof(s->tags));

    /* Negative encodes "no votes yet", which must look different from 0. */
    s->avg_vote = get_double(o, "avg_vote", -1.0);
    s->user_vote = get_int(o, "user_vote", 0);

    s->start_time = get_i64(o, "start_time", 0);

    /* "length" is the broadcast length (after any trim); "length_seconds" is
     * the raw file. The progress bar must track what is actually played. */
    s->length = get_int(o, "length", 0);
    if (s->length <= 0)
        s->length = get_int(o, "length_seconds", 0);
}

static int parse_song_array(const cJSON *arr, song_t *dst, int max)
{
    int n = 0;
    if (!cJSON_IsArray(arr))
        return 0;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (n >= max)
            break;
        parse_song(item, &dst[n]);
        if (dst[n].valid)
            n++;
    }
    return n;
}

static void parse_oneliner(const cJSON *o, oneliner_t *m)
{
    memset(m, 0, sizeof(*m));
    if (!cJSON_IsObject(o))
        return;

    m->valid = true;
    m->id = get_int(o, "id", 0);
    m->timestamp = get_double(o, "timestamp", 0.0);

    copy_str(m->author, sizeof(m->author),
             cJSON_GetObjectItemCaseSensitive(o, "author"));
    copy_str(m->country, sizeof(m->country),
             cJSON_GetObjectItemCaseSensitive(o, "country"));
    copy_str(m->message, sizeof(m->message),
             cJSON_GetObjectItemCaseSensitive(o, "message"));

    /* The song a line was posted against is nested twice: {"song":{"song":{…}}}.
     * Both levels are optional -- a line posted with nothing playing has
     * neither -- so each is checked rather than assumed. */
    const cJSON *outer = cJSON_GetObjectItemCaseSensitive(o, "song");
    if (cJSON_IsObject(outer)) {
        const cJSON *inner = cJSON_GetObjectItemCaseSensitive(outer, "song");
        if (cJSON_IsObject(inner)) {
            copy_str(m->song_title, sizeof(m->song_title),
                     cJSON_GetObjectItemCaseSensitive(inner, "title"));
            /* Here the display name is "artist", not "artist_name" as on a
             * song object. */
            copy_str(m->song_artist, sizeof(m->song_artist),
                     cJSON_GetObjectItemCaseSensitive(inner, "artist"));
        }
    }
}

static int parse_oneliner_array(const cJSON *arr, oneliner_t *dst, int max)
{
    int n = 0;
    if (!cJSON_IsArray(arr))
        return 0;

    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (n >= max)
            break;
        parse_oneliner(item, &dst[n]);
        if (dst[n].valid)
            n++;
    }
    return n;
}

static void parse_listeners(const cJSON *o, listeners_t *l)
{
    if (!cJSON_IsObject(o))
        return;
    l->icecast = get_int(o, "icecast", 0);
    l->hls = get_int(o, "hls", 0);
    l->total = get_int(o, "total", 0);
}

/* --------------------------------------------------------------- dispatch */

static proto_msg_t classify(const char *type)
{
    if (strcmp(type, "playback_update") == 0)    return PROTO_PLAYBACK_UPDATE;
    if (strcmp(type, "now_playing_update") == 0) return PROTO_NOW_PLAYING_UPDATE;
    if (strcmp(type, "queue_update") == 0)       return PROTO_QUEUE_UPDATE;
    if (strcmp(type, "history_update") == 0)     return PROTO_HISTORY_UPDATE;
    if (strcmp(type, "oneliner_update") == 0)    return PROTO_ONELINER_UPDATE;
    if (strcmp(type, "comments_update") == 0)    return PROTO_COMMENTS_UPDATE;
    if (strcmp(type, "connections_update") == 0) return PROTO_CONNECTIONS_UPDATE;
    if (strcmp(type, "backend_version") == 0)    return PROTO_BACKEND_VERSION;
    if (strcmp(type, "pong") == 0)               return PROTO_PONG;
    return PROTO_UNKNOWN;
}

/* Replaces now_playing, reporting whether the song actually changed so the
 * caller can resync the progress clock. */
static bool set_now_playing(playback_t *pb, const cJSON *node,
                            uint64_t now_mono_ms)
{
    song_t next;
    parse_song(node, &next);

    bool changed = next.id != pb->now_playing.id ||
                   next.start_time != pb->now_playing.start_time ||
                   next.valid != pb->now_playing.valid;

    pb->now_playing = next;
    if (changed)
        pb->now_playing_received_ms = now_mono_ms;

    return changed;
}

bool protocol_apply(playback_t *pb, const char *json, size_t len,
                    uint64_t now_mono_ms, proto_result_t *out)
{
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) {
        LOGW(TAG, "message is not valid JSON");
        return false;
    }

    const cJSON *type_node = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type_node) || !type_node->valuestring) {
        LOGW(TAG, "message has no type");
        cJSON_Delete(root);
        return false;
    }

    out->type = classify(type_node->valuestring);

    switch (out->type) {
    case PROTO_BACKEND_VERSION:
        out->backend_version = get_int(root, "version", 0);
        pb->backend_version = out->backend_version;
        break;

    case PROTO_PONG:
        /* Milliseconds here, unlike song start_time which is seconds. */
        out->pong_timestamp_ms = get_i64(root, "timestamp", 0);
        break;

    case PROTO_PLAYBACK_UPDATE:
        /* A full snapshot REPLACES everything. Anything not present in it must
         * end up cleared, not carried over from before. */
        out->now_playing_changed =
            set_now_playing(pb, cJSON_GetObjectItemCaseSensitive(root, "now_playing"),
                            now_mono_ms);

        memset(pb->queue, 0, sizeof(pb->queue));
        pb->queue_len = parse_song_array(
            cJSON_GetObjectItemCaseSensitive(root, "queue"), pb->queue, MAX_QUEUE);

        memset(pb->history, 0, sizeof(pb->history));
        pb->history_len = parse_song_array(
            cJSON_GetObjectItemCaseSensitive(root, "history"), pb->history,
            MAX_HISTORY);

        memset(pb->oneliner, 0, sizeof(pb->oneliner));
        pb->oneliner_len = parse_oneliner_array(
            cJSON_GetObjectItemCaseSensitive(root, "oneliner"), pb->oneliner,
            MAX_ONELINER);

        memset(&pb->listeners, 0, sizeof(pb->listeners));
        parse_listeners(cJSON_GetObjectItemCaseSensitive(root, "listeners"),
                        &pb->listeners);

        {
            const cJSON *au = cJSON_GetObjectItemCaseSensitive(root, "active_users");
            pb->active_users = cJSON_IsArray(au) ? cJSON_GetArraySize(au) : 0;
        }
        pb->lurkers = get_int(root, "lurkers", 0);

        pb->have_snapshot = true;
        break;

    /* Every case below merges into its own key(s) only. */

    case PROTO_NOW_PLAYING_UPDATE:
        out->now_playing_changed =
            set_now_playing(pb, cJSON_GetObjectItemCaseSensitive(root, "now_playing"),
                            now_mono_ms);
        parse_listeners(cJSON_GetObjectItemCaseSensitive(root, "listeners"),
                        &pb->listeners);
        break;

    case PROTO_QUEUE_UPDATE:
        memset(pb->queue, 0, sizeof(pb->queue));
        pb->queue_len = parse_song_array(
            cJSON_GetObjectItemCaseSensitive(root, "queue"), pb->queue, MAX_QUEUE);
        break;

    case PROTO_HISTORY_UPDATE:
        memset(pb->history, 0, sizeof(pb->history));
        pb->history_len = parse_song_array(
            cJSON_GetObjectItemCaseSensitive(root, "history"), pb->history,
            MAX_HISTORY);
        break;

    case PROTO_CONNECTIONS_UPDATE: {
        const cJSON *au = cJSON_GetObjectItemCaseSensitive(root, "active_users");
        if (cJSON_IsArray(au))
            pb->active_users = cJSON_GetArraySize(au);
        pb->lurkers = get_int(root, "lurkers", pb->lurkers);
        break;
    }

    case PROTO_ONELINER_UPDATE:
        memset(pb->oneliner, 0, sizeof(pb->oneliner));
        pb->oneliner_len = parse_oneliner_array(
            cJSON_GetObjectItemCaseSensitive(root, "oneliner"), pb->oneliner,
            MAX_ONELINER);
        /* This message also carries dj_hypr_mood, which we do not store: there
         * is nowhere to show it and it is independently nullable. */
        break;

    /* Song comments are a separate feed from the chat and have nowhere to go
     * in a display-only build. Listed explicitly so a genuinely unhandled type
     * still falls through to the default and gets logged. */
    case PROTO_COMMENTS_UPDATE:
        break;

    case PROTO_UNKNOWN:
    default:
        LOGD(TAG, "ignoring unknown message type '%s'", type_node->valuestring);
        break;
    }

    /* Freed before returning: nothing downstream may hold a pointer into the
     * parse tree, which is what keeps the steady-state heap flat. */
    cJSON_Delete(root);
    return true;
}
