#include "audio/ring.h"
#include "tap.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* Concurrent stress: one producer, one consumer, a ring far smaller than the
 * data pushed through it, so it wraps thousands of times and both sides
 * repeatedly hit full and empty. The payload is a known sequence, so any lost,
 * duplicated or reordered sample is caught exactly where it happens.
 *
 * This is the test that justifies the lock-free implementation. Run it under
 * `make SANITIZE=1 test` -- ThreadSanitizer aside, ASan plus a wrapping ring
 * catches the index arithmetic errors that are otherwise found only as
 * occasional audible clicks on the device. */

#define STRESS_SAMPLES (1u << 20)

typedef struct {
    ring_t *ring;
    size_t  total;
} stress_arg_t;

static void *producer(void *varg)
{
    stress_arg_t *a = varg;
    int16_t chunk[257]; /* deliberately not a divisor of the capacity */
    size_t written = 0;

    while (written < a->total) {
        size_t n = a->total - written;
        if (n > sizeof(chunk) / sizeof(chunk[0]))
            n = sizeof(chunk) / sizeof(chunk[0]);

        for (size_t i = 0; i < n; i++)
            chunk[i] = (int16_t)((written + i) & 0x7fff);

        size_t off = 0;
        while (off < n) {
            size_t got = ring_write(a->ring, chunk + off, n - off);
            off += got;
            if (got == 0)
                sched_yield();
        }
        written += n;
    }
    return NULL;
}

int main(void)
{
    /* --- basic behaviour ------------------------------------------------- */
    ring_t r;
    CHECK(ring_init(&r, 100), "init rounds up to a power of two");
    CHECK_INT(ring_capacity(&r), 128);
    CHECK_INT(ring_available(&r), 0);
    CHECK_INT(ring_space(&r), 128);

    int16_t in[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    int16_t out[8] = { 0 };

    CHECK_INT(ring_write(&r, in, 8), 8);
    CHECK_INT(ring_available(&r), 8);
    CHECK_INT(ring_read(&r, out, 8), 8);
    CHECK(memcmp(in, out, sizeof(in)) == 0, "round-trip preserves samples");
    CHECK_INT(ring_available(&r), 0);

    /* Overfilling must return a short count, never overwrite unread audio. */
    int16_t big[200];
    for (int i = 0; i < 200; i++)
        big[i] = (int16_t)i;
    CHECK_INT(ring_write(&r, big, 200), 128);
    CHECK_INT(ring_space(&r), 0);
    CHECK_INT(ring_write(&r, big, 1), 0);

    /* A short read is how the audio callback learns to pad with silence. */
    int16_t drain[200];
    CHECK_INT(ring_read(&r, drain, 200), 128);
    CHECK(memcmp(drain, big, 128 * sizeof(int16_t)) == 0, "no corruption when full");
    CHECK_INT(ring_read(&r, drain, 1), 0);

    /* Wrapping: write and read in sizes that do not divide the capacity, so
     * every operation straddles the buffer end sooner or later. */
    ring_reset(&r);
    {
        int16_t expect = 0, produce = 0;
        bool ok = true;
        for (int iter = 0; iter < 1000 && ok; iter++) {
            int16_t tmp[37];
            for (int i = 0; i < 37; i++)
                tmp[i] = produce++;
            size_t w = ring_write(&r, tmp, 37);

            int16_t got[37];
            size_t rd = ring_read(&r, got, w);
            if (rd != w)
                ok = false;
            for (size_t i = 0; i < rd && ok; i++)
                if (got[i] != expect++)
                    ok = false;
        }
        CHECK(ok, "wrapping reads and writes stay in sequence");
    }
    ring_free(&r);

    /* --- concurrent producer/consumer ------------------------------------ */
    {
        ring_t sr;
        CHECK(ring_init(&sr, 4096), "stress ring allocated");

        stress_arg_t arg = { .ring = &sr, .total = STRESS_SAMPLES };
        pthread_t tid;
        CHECK_INT(pthread_create(&tid, NULL, producer, &arg), 0);

        size_t read_total = 0;
        size_t mismatches = 0;
        size_t empty_polls = 0;
        int16_t buf[193]; /* also coprime with the capacity */

        while (read_total < STRESS_SAMPLES) {
            size_t n = ring_read(&sr, buf, sizeof(buf) / sizeof(buf[0]));
            if (n == 0) {
                empty_polls++;
                sched_yield();
                continue;
            }
            for (size_t i = 0; i < n; i++) {
                int16_t want = (int16_t)((read_total + i) & 0x7fff);
                if (buf[i] != want)
                    mismatches++;
            }
            read_total += n;
        }

        pthread_join(tid, NULL);

        CHECK_INT(read_total, STRESS_SAMPLES);
        CHECK(mismatches == 0, "%zu samples were lost, duplicated or reordered "
                               "across %u concurrent samples", mismatches,
                               STRESS_SAMPLES);
        /* If this never happened the ring was never actually contended and the
         * test proved much less than it appears to. */
        CHECK(empty_polls > 0, "consumer hit an empty ring at least once "
                               "(contention actually occurred)");
        ring_free(&sr);
    }

    TAP_DONE();
}
