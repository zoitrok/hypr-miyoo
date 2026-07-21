/* wsdump -- connect to the Hypr WebSocket, report what arrives, and optionally
 * record it.
 *
 * Two jobs. First, it validates the WebSocket client against the real server:
 * the handshake, the large fragmented initial snapshot, and the protocol-level
 * ping the server drops us for ignoring.
 *
 * Second, --record writes a transcript that the offline harness replays, so the
 * whole metadata and UI layer can be developed with no network at all. The
 * format is JSON Lines: {"t": <ms since connect>, "msg": <the message>}.
 *
 *   wsdump --record fixtures/session.jsonl --seconds 300
 */

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net/conn.h"
#include "net/ws.h"
#include "util/log.h"
#include "util/mono.h"

#define TAG "wsdump"

#define DEFAULT_WS_URL "wss://hypr.website/ws"

static volatile sig_atomic_t g_stop;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Pulls "type" out of a message without a JSON parser -- this tool is meant to
 * work even when the parser is what is broken. */
static void extract_type(const char *msg, char *out, size_t outlen)
{
    out[0] = '\0';
    const char *p = strstr(msg, "\"type\"");
    if (!p)
        return;
    p = strchr(p + 6, '"');
    if (!p)
        return;
    p++;
    const char *end = strchr(p, '"');
    if (!end)
        return;
    size_t n = (size_t)(end - p);
    if (n >= outlen)
        n = outlen - 1;
    memcpy(out, p, n);
    out[n] = '\0';
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "\n"
        "  --url URL       WebSocket URL (default: %s)\n"
        "  --ca PATH       CA bundle (default: package/App/Hypr/ca.crt)\n"
        "  --record PATH   write a JSONL transcript for offline replay\n"
        "  --seconds N     stop after N seconds (default 60, 0 = forever)\n"
        "  --ping N        send an app-level ping every N seconds (default 20)\n"
        "  --print         print full message bodies\n"
        "  --insecure      skip certificate verification\n"
        "  --verbose\n",
        argv0, DEFAULT_WS_URL);
}

int main(int argc, char **argv)
{
    const char *url = DEFAULT_WS_URL;
    const char *ca = "package/App/Hypr/ca.crt";
    const char *record = NULL;
    long seconds = 60;
    long ping_interval = 20;
    bool print_body = false, insecure = false, verbose = false;

    static const struct option OPTS[] = {
        { "url",      required_argument, 0, 'u' },
        { "ca",       required_argument, 0, 'c' },
        { "record",   required_argument, 0, 'r' },
        { "seconds",  required_argument, 0, 's' },
        { "ping",     required_argument, 0, 'P' },
        { "print",    no_argument,       0, 'p' },
        { "insecure", no_argument,       0, 'k' },
        { "verbose",  no_argument,       0, 'v' },
        { "help",     no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "u:c:r:s:P:pkvh", OPTS, NULL)) != -1) {
        switch (opt) {
        case 'u': url = optarg; break;
        case 'c': ca = optarg; break;
        case 'r': record = optarg; break;
        case 's': seconds = strtol(optarg, NULL, 10); break;
        case 'P': ping_interval = strtol(optarg, NULL, 10); break;
        case 'p': print_body = true; break;
        case 'k': insecure = true; break;
        case 'v': verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    log_init(verbose ? LOG_DEBUG : LOG_INFO);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    conn_tls_ctx_t *tls = conn_tls_ctx_new(insecure ? NULL : ca, insecure);
    if (!tls)
        return 1;

    ws_t *ws = ws_connect(tls, url, 15000);
    if (!ws) {
        conn_tls_ctx_free(tls);
        return 1;
    }

    FILE *rec = NULL;
    if (record) {
        rec = fopen(record, "wb");
        if (!rec) {
            perror(record);
            ws_close(ws);
            conn_tls_ctx_free(tls);
            return 1;
        }
        LOGI(TAG, "recording to %s", record);
    }

    uint64_t start = mono_ms();
    uint64_t deadline = seconds > 0 ? start + (uint64_t)seconds * 1000 : 0;
    uint64_t next_ping = ping_interval > 0
        ? start + (uint64_t)ping_interval * 1000 : 0;

    unsigned long count = 0, bytes = 0;
    int rc_exit = 0;

    while (!g_stop) {
        uint64_t now = mono_ms();
        if (deadline && now >= deadline)
            break;

        /* The application-level ping doubles as the clock-offset probe the
         * renderer needs, since the device has no usable wall clock. */
        if (next_ping && now >= next_ping) {
            static const char PING[] = "{\"type\":\"ping\"}";
            if (ws_send_text(ws, PING, sizeof(PING) - 1) != 0) {
                LOGE(TAG, "ping failed: %s", ws_last_error(ws));
                rc_exit = 1;
                break;
            }
            LOGD(TAG, "sent app-level ping");
            next_ping = now + (uint64_t)ping_interval * 1000;
        }

        const char *payload = NULL;
        size_t len = 0;
        int rc = ws_recv(ws, &payload, &len, 1000);

        if (rc == WS_TIMEOUT)
            continue;
        if (rc == WS_CLOSED) {
            LOGI(TAG, "connection closed by server");
            break;
        }
        if (rc < 0) {
            LOGE(TAG, "receive failed: %s", ws_last_error(ws));
            rc_exit = 1;
            break;
        }

        count++;
        bytes += len;

        char type[64];
        extract_type(payload, type, sizeof(type));
        LOGI(TAG, "%-22s %7zu bytes", type[0] ? type : "(no type)", len);

        if (print_body)
            printf("%.*s\n", (int)len, payload);

        if (rec) {
            /* The message is already valid JSON, so it can be embedded
             * directly rather than re-escaped. */
            fprintf(rec, "{\"t\":%llu,\"msg\":%.*s}\n",
                    (unsigned long long)(mono_ms() - start), (int)len, payload);
        }
    }

    LOGI(TAG, "%lu messages, %lu bytes total", count, bytes);

    if (rec)
        fclose(rec);
    ws_close(ws);
    conn_tls_ctx_free(tls);
    return rc_exit;
}
