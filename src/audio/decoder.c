#include "audio/decoder.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

/* Drop the MP1/MP2 tables; the stream is MP3 and they are dead weight on a
 * 128MB device. SIMD is deliberately left enabled -- the Cortex-A7 has NEON
 * and minimp3's NEON path is a large part of why decode costs only a few
 * percent of one core. */
#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
/* Vendored third-party code; its warnings are not ours to fix, and leaving
 * them on trains us to ignore output from our own build. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include "minimp3/minimp3.h"
#pragma GCC diagnostic pop

#define TAG "decode"

struct decoder {
    mp3dec_t mp3d;
    int  sample_rate;
    int  channels;
    int  bitrate_kbps;
    bool have_format;
};

decoder_t *decoder_new(void)
{
    decoder_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;
    mp3dec_init(&d->mp3d);
    return d;
}

void decoder_free(decoder_t *d)
{
    free(d);
}

void decoder_reset(decoder_t *d)
{
    mp3dec_init(&d->mp3d);
    /* Format is intentionally retained: the audio device is already open at
     * that rate, and a live radio stream does not change format across a
     * reconnect. If it ever does, stream.c notices and says so. */
}

/* Decodes at most one frame. Returns interleaved sample count (0 is normal --
 * minimp3 consumes ID3 tags and resyncs over junk without producing audio) and
 * always sets *consumed to how many input bytes to drop. */
static int decode_one(decoder_t *d, const uint8_t *in, size_t in_len,
                      size_t *consumed, int16_t *out)
{
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));

    /* minimp3 takes an int length; clamp rather than truncate silently. */
    int len = in_len > INT32_MAX ? INT32_MAX : (int)in_len;

    int samples_per_channel = mp3dec_decode_frame(&d->mp3d, in, len, out, &info);

    *consumed = (size_t)(info.frame_bytes > 0 ? info.frame_bytes : 0);

    if (samples_per_channel <= 0)
        return 0;

    if (!d->have_format) {
        d->have_format = true;
        d->sample_rate = info.hz;
        d->channels = info.channels;
        LOGI(TAG, "stream format: %d Hz, %d channel(s), %d kbit/s",
             info.hz, info.channels, info.bitrate_kbps);
    } else if (info.hz != d->sample_rate || info.channels != d->channels) {
        /* The audio device is already open at the old rate, so this would play
         * at the wrong speed. Worth shouting about rather than silently
         * sounding subtly wrong. */
        LOGW(TAG, "stream format changed: %d Hz/%dch -> %d Hz/%dch",
             d->sample_rate, d->channels, info.hz, info.channels);
        d->sample_rate = info.hz;
        d->channels = info.channels;
    }

    d->bitrate_kbps = info.bitrate_kbps;

    return samples_per_channel * info.channels;
}

size_t decoder_drain(decoder_t *d, const uint8_t *in, size_t in_len,
                     bool at_eof, decoder_sink_fn sink, void *user)
{
    int16_t pcm[DECODER_MAX_SAMPLES_PER_FRAME];
    size_t offset = 0;

    for (;;) {
        size_t remaining = in_len - offset;

        /* The whole point of this function. Below a full frame's worth of
         * bytes, minimp3 would swallow the tail as junk rather than wait, so
         * we stop and let the caller bring more. At EOF there is no more to
         * bring, so we decode whatever is left. */
        if (remaining < DECODER_MAX_FRAME_BYTES && !at_eof)
            break;
        if (remaining == 0)
            break;

        size_t consumed = 0;
        int samples = decode_one(d, in + offset, remaining, &consumed, pcm);

        if (samples > 0 && sink)
            sink(user, pcm, samples);

        /* No progress and nothing decoded: only reachable at EOF with a stub
         * of a frame. Drop it rather than spin. */
        if (consumed == 0)
            break;

        offset += consumed;
    }

    return offset;
}

bool decoder_format(const decoder_t *d, int *sample_rate, int *channels)
{
    if (!d->have_format)
        return false;
    if (sample_rate)
        *sample_rate = d->sample_rate;
    if (channels)
        *channels = d->channels;
    return true;
}

int decoder_bitrate_kbps(const decoder_t *d)
{
    return d->bitrate_kbps;
}
