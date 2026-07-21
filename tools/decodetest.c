/* decodetest -- feed a local MP3 through the exact decode loop stream.c uses
 * and report what came out.
 *
 * Isolates the decoder from the network. If this reports the same duration
 * ffprobe does, the decode path is sound and any shortfall is upstream; if it
 * does not, the bug is here and reproducible without a server.
 *
 *   decodetest /tmp/rate.mp3
 *
 * --chunk N feeds the decoder in N-byte slices to imitate a network dribbling
 * bytes in, which is how partial-frame handling gets exercised.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "audio/decoder.h"
#include "util/log.h"

#define IN_BUF_SIZE 16384

typedef struct { uint64_t samples, frames; FILE *pcm_out; } tally_t;

static void count_frame(void *user, const int16_t *pcm, int samples)
{
    tally_t *t = user;
    t->samples += (uint64_t)samples;
    t->frames++;
    if (t->pcm_out)
        fwrite(pcm, sizeof(int16_t), (size_t)samples, t->pcm_out);
}

int main(int argc, char **argv)
{
    size_t chunk = 4096;
    const char *pcm_path = NULL;
    bool verbose = false;

    static const struct option OPTS[] = {
        { "chunk",   required_argument, 0, 'c' },
        { "pcm",     required_argument, 0, 'p' },
        { "verbose", no_argument,       0, 'v' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:v", OPTS, NULL)) != -1) {
        switch (opt) {
        case 'c': chunk = (size_t)strtoul(optarg, NULL, 10); break;
        case 'p': pcm_path = optarg; break;
        case 'v': verbose = true; break;
        default:
            fprintf(stderr, "usage: %s [--chunk N] [--verbose] FILE.mp3\n", argv[0]);
            return 2;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "usage: %s [--chunk N] [--verbose] FILE.mp3\n", argv[0]);
        return 2;
    }

    log_init(verbose ? LOG_DEBUG : LOG_INFO);

    FILE *f = fopen(argv[optind], "rb");
    if (!f) {
        perror(argv[optind]);
        return 1;
    }

    decoder_t *dec = decoder_new();
    uint8_t in[IN_BUF_SIZE];
    size_t  in_len = 0;

    tally_t tally = { 0, 0, NULL };
    if (pcm_path) {
        tally.pcm_out = fopen(pcm_path, "wb");
        if (!tally.pcm_out) { perror(pcm_path); return 1; }
    }
    uint64_t total_bytes = 0;

    if (chunk > sizeof(in))
        chunk = sizeof(in);

    for (;;) {
        size_t want = sizeof(in) - in_len;
        if (want > chunk)
            want = chunk;

        size_t n = want ? fread(in + in_len, 1, want, f) : 0;
        in_len += n;
        total_bytes += n;

        bool at_eof = (n == 0);

        /* The same drain used by the streaming path, so the two cannot
         * diverge -- which is exactly how the partial-frame bug survived
         * until it was measured. */
        size_t consumed = decoder_drain(dec, in, in_len, at_eof, count_frame, &tally);

        if (consumed > 0) {
            memmove(in, in + consumed, in_len - consumed);
            in_len -= consumed;
        }

        if (at_eof)
            break;

        if (in_len == sizeof(in)) {
            fprintf(stderr, "input buffer full with no decodable frame\n");
            in_len = 0;
        }
    }

    fclose(f);

    uint64_t total_samples = tally.samples;
    uint64_t frames = tally.frames;

    int rate = 0, channels = 0;
    decoder_format(dec, &rate, &channels);

    double seconds = (rate && channels)
        ? (double)total_samples / ((double)rate * channels) : 0;

    printf("chunk size:     %zu bytes\n", chunk);
    printf("bytes read:     %llu\n", (unsigned long long)total_bytes);
    printf("frames decoded: %llu\n", (unsigned long long)frames);
    printf("samples:        %llu\n", (unsigned long long)total_samples);
    printf("format:         %d Hz, %d ch\n", rate, channels);
    printf("duration:       %.2f s\n", seconds);

    if (tally.pcm_out)
        fclose(tally.pcm_out);
    decoder_free(dec);
    return 0;
}
