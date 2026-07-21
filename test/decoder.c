#include "audio/decoder.h"
#include "util/log.h"
#include "tap.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Regression test for a bug that cost roughly half the audio and was invisible
 * except as a slowly draining buffer.
 *
 * minimp3 requires a COMPLETE frame in its input. Handed a partial one it does
 * not report "need more data" -- it consumes those bytes as junk and returns
 * nothing. So a decode loop that reads N bytes and drains whatever it has
 * silently destroys the frame straddling every read boundary. The damage scales
 * with how small the reads are, which is why it looked like a network problem:
 * 16KB reads lost ~8% of the audio, 512-byte reads lost all of it.
 *
 * decoder_drain() holds back the final DECODER_MAX_FRAME_BYTES unless at EOF.
 * This test pushes the same file through it at wildly different granularities
 * and demands byte-identical PCM out of every one. */

#define FIXTURE "fixtures/sample.mp3"
#define IN_BUF_SIZE 16384

typedef struct {
    int16_t *pcm;
    size_t   len, cap;
} capture_t;

static void capture_frame(void *user, const int16_t *pcm, int samples)
{
    capture_t *c = user;
    if (c->len + (size_t)samples > c->cap) {
        c->cap = (c->cap ? c->cap * 2 : 1 << 16) + (size_t)samples;
        c->pcm = realloc(c->pcm, c->cap * sizeof(int16_t));
    }
    memcpy(c->pcm + c->len, pcm, (size_t)samples * sizeof(int16_t));
    c->len += (size_t)samples;
}

/* Decodes the whole file feeding the decoder `chunk` bytes at a time. */
static bool decode_file(const char *path, size_t chunk, capture_t *out,
                        int *rate, int *channels)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    decoder_t *dec = decoder_new();
    uint8_t in[IN_BUF_SIZE];
    size_t in_len = 0;

    memset(out, 0, sizeof(*out));

    for (;;) {
        size_t want = sizeof(in) - in_len;
        if (want > chunk)
            want = chunk;

        size_t n = want ? fread(in + in_len, 1, want, f) : 0;
        in_len += n;
        bool at_eof = (n == 0);

        size_t consumed = decoder_drain(dec, in, in_len, at_eof,
                                        capture_frame, out);
        if (consumed > 0) {
            memmove(in, in + consumed, in_len - consumed);
            in_len -= consumed;
        }
        if (at_eof)
            break;
        if (in_len == sizeof(in))
            in_len = 0; /* unreachable for valid MP3; avoids a spin if not */
    }

    decoder_format(dec, rate, channels);
    decoder_free(dec);
    fclose(f);
    return true;
}

int main(void)
{
    log_init(LOG_ERROR);

    /* The reference: one big read, the case that was least broken. */
    capture_t ref;
    int rate = 0, channels = 0;
    if (!decode_file(FIXTURE, IN_BUF_SIZE, &ref, &rate, &channels)) {
        fprintf(stderr, "SKIP: %s not found (run from the repo root)\n", FIXTURE);
        return 0;
    }

    CHECK_INT(rate, 44100);
    CHECK_INT(channels, 2);
    CHECK(ref.len > 0, "reference decode produced audio");

    /* ~2s of 44.1kHz stereo. A decode that silently drops frames fails here
     * long before it would be noticed by ear. */
    double seconds = (double)ref.len / (44100.0 * 2.0);
    CHECK(seconds > 1.9 && seconds < 2.1,
          "reference duration %.3f s is within tolerance of 2.0 s", seconds);

    /* 1 byte at a time is absurd for a network but it is the limiting case:
     * every single frame straddles a read boundary. If this matches, nothing
     * realistic can break it. */
    const size_t GRANULARITIES[] = { 1, 7, 64, 512, 1000, 1400, 2883, 2884, 4096, 8192 };

    for (size_t i = 0; i < sizeof(GRANULARITIES) / sizeof(GRANULARITIES[0]); i++) {
        capture_t got;
        int r = 0, c = 0;
        CHECK(decode_file(FIXTURE, GRANULARITIES[i], &got, &r, &c),
              "decode at %zu bytes/read", GRANULARITIES[i]);

        CHECK(got.len == ref.len,
              "at %zu bytes/read: got %zu samples, expected %zu "
              "(%.1f%% of the audio was lost)",
              GRANULARITIES[i], got.len, ref.len,
              100.0 - (double)got.len / (double)ref.len * 100.0);

        if (got.len == ref.len) {
            CHECK(memcmp(got.pcm, ref.pcm, ref.len * sizeof(int16_t)) == 0,
                  "at %zu bytes/read: PCM is byte-identical to the reference",
                  GRANULARITIES[i]);
        }

        free(got.pcm);
    }

    free(ref.pcm);
    TAP_DONE();
}
