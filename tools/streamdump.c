/* streamdump -- Phase 1 validation tool.
 *
 * Connects to an http(s) URL, reads the body, and writes it to a file or
 * stdout. Its purpose is to exercise the TLS stack and the chunked de-framer
 * against the real backend, which is the pair of things most likely to be
 * subtly wrong and least likely to announce it: bad TLS fails loudly, but a
 * broken chunked de-framer just produces audio that clicks every few seconds.
 *
 *   streamdump --url https://hypr.website/hypr.mp3 --seconds 10 --out /tmp/a.mp3
 *
 * Verify the output is clean MP3 with:
 *   ffprobe /tmp/a.mp3        (should report a sane bitrate and duration)
 */

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "net/conn.h"
#include "net/http.h"
#include "net/url.h"
#include "util/log.h"
#include "util/mono.h"

#define TAG "dump"

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s --url URL [options]\n"
        "\n"
        "  --url URL         http(s) URL to fetch (required)\n"
        "  --out PATH        write body here (default: stdout)\n"
        "  --ca PATH         CA bundle (default: package/App/Hypr/ca.crt)\n"
        "  --seconds N       stop after N seconds (default: 10, 0 = forever)\n"
        "  --bytes N         stop after N bytes (default: unlimited)\n"
        "  --insecure        skip certificate verification (development only)\n"
        "  --verbose         debug logging\n",
        argv0);
}

int main(int argc, char **argv)
{
    const char *url_str = NULL;
    const char *out_path = NULL;
    const char *ca_path = "package/App/Hypr/ca.crt";
    long seconds = 10;
    long long max_bytes = 0;
    bool insecure = false;
    bool verbose = false;

    static const struct option OPTS[] = {
        { "url",      required_argument, 0, 'u' },
        { "out",      required_argument, 0, 'o' },
        { "ca",       required_argument, 0, 'c' },
        { "seconds",  required_argument, 0, 's' },
        { "bytes",    required_argument, 0, 'b' },
        { "insecure", no_argument,       0, 'k' },
        { "verbose",  no_argument,       0, 'v' },
        { "help",     no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:o:c:s:b:kvh", OPTS, NULL)) != -1) {
        switch (opt) {
        case 'u': url_str = optarg; break;
        case 'o': out_path = optarg; break;
        case 'c': ca_path = optarg; break;
        case 's': seconds = strtol(optarg, NULL, 10); break;
        case 'b': max_bytes = strtoll(optarg, NULL, 10); break;
        case 'k': insecure = true; break;
        case 'v': verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (!url_str) {
        usage(argv[0]);
        return 2;
    }

    log_init(verbose ? LOG_DEBUG : LOG_INFO);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    url_t url;
    if (!url_parse(url_str, &url)) {
        LOGE(TAG, "could not parse URL '%s'", url_str);
        return 1;
    }
    LOGI(TAG, "host=%s port=%d tls=%s path=%s", url.host, url.port,
         url.tls ? "yes" : "no", url.path);

    conn_tls_ctx_t *tls = NULL;
    if (url.tls) {
        tls = conn_tls_ctx_new(insecure ? NULL : ca_path, insecure);
        if (!tls) {
            LOGE(TAG, "failed to build TLS context");
            return 1;
        }
    }

    conn_t *conn = conn_open(tls, &url, 10000);
    if (!conn) {
        conn_tls_ctx_free(tls);
        return 1;
    }

    /* Deliberately no "Icy-MetaData: 1": the WebSocket gives us far richer
     * metadata than ICY would, and asking for it would interleave metadata
     * blocks into the audio that we would then have to strip back out. */
    const http_header_t extra[] = {
        { "Accept", "*/*" },
        { "Connection", "close" },
    };

    http_stream_t hs;
    int rc = http_get(&hs, conn, &url, extra, sizeof(extra) / sizeof(extra[0]), 10000);
    if (rc != 0) {
        LOGE(TAG, "request failed (rc=%d): %s", rc, conn_last_error(conn));
        goto done;
    }

    if (hs.status != 200) {
        LOGE(TAG, "unexpected HTTP status %d", hs.status);
        rc = 1;
        goto done;
    }

    char ctype[128];
    if (http_header(&hs, "Content-Type", ctype, sizeof(ctype)))
        LOGI(TAG, "content-type: %s", ctype);
    LOGI(TAG, "framing: %s", hs.chunked ? "chunked" : "identity");

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "wb");
        if (!out) {
            LOGE(TAG, "cannot open '%s': %s", out_path, strerror(errno));
            rc = 1;
            goto done;
        }
    }

    uint64_t start = mono_ms();
    uint64_t deadline = seconds > 0 ? start + (uint64_t)seconds * 1000 : 0;
    long long total = 0;
    unsigned char buf[4096];

    while (!g_stop) {
        if (deadline && mono_ms() >= deadline)
            break;
        if (max_bytes && total >= max_bytes)
            break;

        ssize_t n = http_read(&hs, buf, sizeof(buf), 10000);
        if (n == CONN_EOF) {
            LOGI(TAG, "end of stream");
            break;
        }
        if (n == CONN_TIMEOUT) {
            LOGW(TAG, "read timed out with no data for 10s");
            break;
        }
        if (n < 0) {
            LOGE(TAG, "read error: %s", conn_last_error(conn));
            rc = 1;
            break;
        }

        if (fwrite(buf, 1, (size_t)n, out) != (size_t)n) {
            LOGE(TAG, "short write: %s", strerror(errno));
            rc = 1;
            break;
        }
        total += n;
    }

    uint64_t elapsed = mono_ms() - start;
    if (elapsed == 0)
        elapsed = 1;
    LOGI(TAG, "read %lld bytes in %llu ms (%.1f kbit/s)", total,
         (unsigned long long)elapsed, (double)total * 8.0 / (double)elapsed);

    if (out != stdout)
        fclose(out);

done:
    conn_close(conn);
    conn_tls_ctx_free(tls);
    return rc == 0 ? 0 : 1;
}
