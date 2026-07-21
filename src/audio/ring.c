#include "audio/ring.h"

#include <stdlib.h>
#include <string.h>

/* __atomic builtins rather than <stdatomic.h>: the device toolchain is
 * GCC 8.3 and these are the portable-across-both spelling. On ARMv7 an
 * acquire/release pair compiles to a dmb, which is what we want -- the
 * Cortex-A7 is dual-core and genuinely reorders. */
#define LOAD_ACQUIRE(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define LOAD_RELAXED(p)      __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE_RELEASE(p, v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static size_t round_up_pow2(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

bool ring_init(ring_t *r, size_t capacity_samples)
{
    memset(r, 0, sizeof(*r));

    size_t cap = round_up_pow2(capacity_samples ? capacity_samples : 2);
    r->buf = calloc(cap, sizeof(int16_t));
    if (!r->buf)
        return false;

    r->capacity = cap;
    r->mask = cap - 1;
    return true;
}

void ring_free(ring_t *r)
{
    free(r->buf);
    r->buf = NULL;
    r->capacity = r->mask = 0;
}

void ring_reset(ring_t *r)
{
    STORE_RELEASE(&r->head, 0);
    STORE_RELEASE(&r->tail, 0);
}

size_t ring_capacity(const ring_t *r) { return r->capacity; }

size_t ring_available(const ring_t *r)
{
    size_t head = LOAD_ACQUIRE(&r->head);
    size_t tail = LOAD_ACQUIRE(&r->tail);
    return head - tail;
}

size_t ring_space(const ring_t *r)
{
    return r->capacity - ring_available(r);
}

size_t ring_write(ring_t *r, const int16_t *src, size_t n)
{
    /* The producer owns head, so it can read its own copy relaxed. tail must
     * be acquired: we need the consumer's reads to be visible before we
     * overwrite the space it has freed. */
    size_t head = LOAD_RELAXED(&r->head);
    size_t tail = LOAD_ACQUIRE(&r->tail);

    size_t space = r->capacity - (head - tail);
    if (n > space)
        n = space;
    if (n == 0)
        return 0;

    size_t offset = head & r->mask;
    size_t first = r->capacity - offset;
    if (first > n)
        first = n;

    memcpy(r->buf + offset, src, first * sizeof(int16_t));
    if (n > first)
        memcpy(r->buf, src + first, (n - first) * sizeof(int16_t));

    /* Release: the sample writes above must be visible to the consumer before
     * the head advance that publishes them. */
    STORE_RELEASE(&r->head, head + n);
    return n;
}

size_t ring_read(ring_t *r, int16_t *dst, size_t n)
{
    size_t tail = LOAD_RELAXED(&r->tail);
    size_t head = LOAD_ACQUIRE(&r->head);

    size_t avail = head - tail;
    if (n > avail)
        n = avail;
    if (n == 0)
        return 0;

    size_t offset = tail & r->mask;
    size_t first = r->capacity - offset;
    if (first > n)
        first = n;

    memcpy(dst, r->buf + offset, first * sizeof(int16_t));
    if (n > first)
        memcpy(dst + first, r->buf, (n - first) * sizeof(int16_t));

    STORE_RELEASE(&r->tail, tail + n);
    return n;
}
