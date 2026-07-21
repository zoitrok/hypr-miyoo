#include "state/state.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct app_state {
    pthread_mutex_t lock;
    app_snapshot_t  data;
};

app_state_t *state_new(void)
{
    app_state_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    pthread_mutex_init(&s->lock, NULL);
    clock_init(&s->data.clock);
    return s;
}

void state_free(app_state_t *s)
{
    if (!s)
        return;
    pthread_mutex_destroy(&s->lock);
    free(s);
}

app_snapshot_t *state_lock(app_state_t *s)
{
    pthread_mutex_lock(&s->lock);
    return &s->data;
}

void state_unlock(app_state_t *s)
{
    pthread_mutex_unlock(&s->lock);
}

void state_snapshot(app_state_t *s, app_snapshot_t *out)
{
    pthread_mutex_lock(&s->lock);
    memcpy(out, &s->data, sizeof(*out));
    pthread_mutex_unlock(&s->lock);
}
