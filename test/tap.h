#ifndef HYPR_TAP_H
#define HYPR_TAP_H

/* A deliberately tiny test harness. The interesting logic in this app is pure
 * (frame codecs, de-framers, delta merges, clock arithmetic) and needs nothing
 * more than "run this, compare, report". Pulling in a framework would be more
 * build surface than the thing being tested. */

#include <stdio.h>
#include <string.h>

static int tap_failures;
static int tap_checks;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        tap_checks++;                                                          \
        if (!(cond)) {                                                         \
            tap_failures++;                                                    \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fputc('\n', stderr);                                               \
        }                                                                      \
    } while (0)

#define CHECK_STR(got, want)                                                   \
    CHECK(strcmp((got), (want)) == 0, "expected \"%s\", got \"%s\"", (want), (got))

#define CHECK_INT(got, want)                                                   \
    CHECK((long long)(got) == (long long)(want),                               \
          "expected %lld, got %lld", (long long)(want), (long long)(got))

#define TAP_DONE()                                                             \
    do {                                                                       \
        printf("%d checks, %d failed\n", tap_checks, tap_failures);            \
        return tap_failures == 0 ? 0 : 1;                                      \
    } while (0)

#endif
