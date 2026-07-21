#include "state/replay.h"
#include "state/protocol.h"
#include "util/log.h"
#include "util/mono.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON/cJSON.h"

#define TAG "replay"

struct replay {
    char path[512];
    app_state_t *state;
    pthread_t thread;
    bool thread_started;
    volatile bool stop;
};

/* Each line is {"t": <ms since connect>, "msg": {...}} as written by
 * `wsdump --record`. */
static void *replay_thread(void *arg)
{
    replay_t *r = arg;

    for (;;) {
        FILE *f = fopen(r->path, "rb");
        if (!f) {
            LOGE(TAG, "cannot open %s", r->path);
            return NULL;
        }

        uint64_t started = mono_ms();
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        unsigned applied = 0;

        while (!r->stop && (len = getline(&line, &cap, f)) > 0) {
            cJSON *root = cJSON_ParseWithLength(line, (size_t)len);
            if (!root)
                continue;

            const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "t");
            const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
            if (!cJSON_IsNumber(t) || !msg) {
                cJSON_Delete(root);
                continue;
            }

            /* Wait until this message's recorded moment, in slices so stop is
             * honoured promptly even across a long gap between messages. */
            uint64_t due = started + (uint64_t)t->valuedouble;
            while (!r->stop && mono_ms() < due)
                mono_sleep_ms(20);
            if (r->stop) {
                cJSON_Delete(root);
                break;
            }

            char *text = cJSON_PrintUnformatted(msg);
            cJSON_Delete(root);
            if (!text)
                continue;

            uint64_t now = mono_ms();
            proto_result_t res;

            app_snapshot_t *st = state_lock(r->state);
            protocol_apply(&st->playback, text, strlen(text), now, &res);

            /* Feed recorded pongs through the real clock path. The recorded
             * timestamp is old, but the offset it produces is exactly what
             * makes song progress replay at its true recorded position and
             * then advance in real time -- which is the whole point of
             * replaying rather than faking.
             *
             * (An earlier version pinned the offset so that server_now equalled
             * start_time, which forced every song to read 0:00 forever.) */
            if (res.type == PROTO_PONG) {
                clock_ping_sent(&st->clock, now);
                clock_pong_received(&st->clock, now, res.pong_timestamp_ms);
            }
            st->ws_connected = true;
            state_unlock(r->state);

            free(text);
            applied++;
        }

        free(line);
        fclose(f);

        if (r->stop)
            break;

        LOGI(TAG, "replayed %u messages; looping", applied);
    }

    LOGI(TAG, "replay thread exiting");
    return NULL;
}

replay_t *replay_start(const char *path, app_state_t *state)
{
    replay_t *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;

    snprintf(r->path, sizeof(r->path), "%s", path);
    r->state = state;

    if (pthread_create(&r->thread, NULL, replay_thread, r) != 0) {
        LOGE(TAG, "failed to start replay thread");
        free(r);
        return NULL;
    }
    r->thread_started = true;

    LOGI(TAG, "replaying %s", path);
    return r;
}

void replay_stop(replay_t *r)
{
    if (!r)
        return;
    r->stop = true;
    if (r->thread_started)
        pthread_join(r->thread, NULL);
    free(r);
}
