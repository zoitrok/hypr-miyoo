#include "net/conn.h"
#include "net/http.h"
#include "net/url.h"
#include "util/log.h"
#include "tap.h"

#include <stdlib.h>
#include <string.h>

/* Drains a canned HTTP response through the real parser, via a memory-backed
 * conn, and returns the de-framed body. max_read controls how many bytes each
 * conn_read() hands back -- small values split chunk-size lines and CRLF pairs
 * across reads, which is precisely where de-framers break and precisely what a
 * fast real connection never does. */
static char *drain(const char *response, size_t max_read, size_t *out_len,
                   int *out_status, http_stream_t *out_hs)
{
    url_t url;
    url_parse("http://test/x", &url);

    conn_t *c = conn_open_memory(response, strlen(response), max_read);
    if (!c)
        return NULL;

    http_stream_t hs;
    int rc = http_get(&hs, c, &url, NULL, 0, 1000);
    if (rc != 0) {
        conn_close(c);
        return NULL;
    }

    size_t cap = 256, len = 0;
    char *body = malloc(cap);

    for (;;) {
        if (len + 64 > cap) {
            cap *= 2;
            body = realloc(body, cap);
        }
        ssize_t n = http_read(&hs, body + len, 64, 1000);
        if (n <= 0)
            break;
        len += (size_t)n;
    }

    body[len] = '\0';
    *out_len = len;
    if (out_status)
        *out_status = hs.status;
    if (out_hs)
        *out_hs = hs;

    conn_close(c);
    return body;
}

int main(void)
{
    log_init(LOG_ERROR); /* the parsers log per-request at INFO; not useful here */

    size_t len;
    int status;

    /* --- the shape hypr.website actually serves: chunked audio ------------ */
    static const char CHUNKED[] =
        "HTTP/1.1 200 OK\r\n"
        "Server: nginx/1.22.1\r\n"
        "Content-Type: audio/mpeg\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nHello\r\n"
        "6\r\n World\r\n"
        "0\r\n\r\n";

    http_stream_t hs;
    char *body = drain(CHUNKED, 0, &len, &status, &hs);
    CHECK(body != NULL, "chunked response parsed");
    if (body) {
        CHECK_INT(status, 200);
        CHECK_INT(hs.chunked, 1);
        CHECK_INT(len, 11);
        CHECK_STR(body, "Hello World");

        char val[128];
        CHECK(http_header(&hs, "Content-Type", val, sizeof(val)) != NULL, "ctype found");
        CHECK_STR(val, "audio/mpeg");
        /* Header lookup must be case-insensitive; servers vary. */
        CHECK(http_header(&hs, "cOnTeNt-TyPe", val, sizeof(val)) != NULL, "ctype ci");
        CHECK_STR(val, "audio/mpeg");
        CHECK(http_header(&hs, "X-Absent", val, sizeof(val)) == NULL, "absent header");
        free(body);
    }

    /* The same bytes must de-frame identically no matter how the transport
     * fragments them. Byte-at-a-time is the adversarial case. */
    const size_t GRANULARITIES[] = { 1, 2, 3, 7, 13, 64, 4096 };
    for (size_t i = 0; i < sizeof(GRANULARITIES) / sizeof(GRANULARITIES[0]); i++) {
        body = drain(CHUNKED, GRANULARITIES[i], &len, NULL, NULL);
        CHECK(body != NULL, "drip parse at %zu bytes/read", GRANULARITIES[i]);
        if (body) {
            CHECK(len == 11 && strcmp(body, "Hello World") == 0,
                  "drip at %zu bytes/read produced \"%s\"", GRANULARITIES[i], body);
            free(body);
        }
    }

    /* --- chunk extensions and trailers ----------------------------------- */
    static const char EXTENSIONS[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5;name=value\r\nHello\r\n"
        "0\r\n"
        "X-Checksum: abc123\r\n"
        "\r\n";
    body = drain(EXTENSIONS, 0, &len, NULL, NULL);
    CHECK(body != NULL, "chunk extensions parsed");
    if (body) {
        CHECK_STR(body, "Hello");
        free(body);
    }

    /* A chunk size large enough to need more than one hex digit, and one whose
     * data spans several reads, together catch off-by-ones in the accounting. */
    {
        char big[8192];
        char payload[300];
        memset(payload, 'z', sizeof(payload));
        snprintf(big, sizeof(big),
                 "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                 "%zx\r\n%.*s\r\n0\r\n\r\n",
                 sizeof(payload), (int)sizeof(payload), payload);
        body = drain(big, 5, &len, NULL, NULL);
        CHECK(body != NULL, "multi-digit chunk size parsed");
        if (body) {
            CHECK_INT(len, (long long)sizeof(payload));
            free(body);
        }
    }

    /* --- identity framing, for completeness ------------------------------ */
    static const char LENGTHED[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "HelloTHIS SHOULD NOT BE READ";
    body = drain(LENGTHED, 0, &len, &status, &hs);
    CHECK(body != NULL, "content-length response parsed");
    if (body) {
        CHECK_INT(hs.chunked, 0);
        /* Reading past Content-Length would leak the next pipelined response
         * into the body -- for the audio stream that would be silent corruption. */
        CHECK_INT(len, 5);
        CHECK_STR(body, "Hello");
        free(body);
    }

    /* --- the WebSocket handshake path ------------------------------------ */
    {
        url_t url;
        url_parse("http://test/ws", &url);
        static const char UPGRADE[] =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: upgrade\r\n"
            "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
            "\r\n"
            "\x81\x03""abc";

        conn_t *c = conn_open_memory(UPGRADE, sizeof(UPGRADE) - 1, 0);
        http_stream_t ws;
        int rc = http_get(&ws, c, &url, NULL, 0, 1000);
        CHECK_INT(rc, 0);
        CHECK_INT(ws.status, 101);

        char val[128];
        CHECK(http_header(&ws, "Sec-WebSocket-Accept", val, sizeof(val)) != NULL,
              "accept header found");
        CHECK_STR(val, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

        /* The server may pack its first frame into the same segment as the 101.
         * Those bytes are already in our buffer and would be lost forever if
         * the WebSocket layer went straight back to the socket. */
        size_t nbuf;
        const unsigned char *pending = http_buffered(&ws, &nbuf);
        CHECK_INT(nbuf, 5);
        CHECK(pending[0] == 0x81 && pending[1] == 0x03 && pending[2] == 'a',
              "buffered post-handshake bytes preserved");

        conn_close(c);
    }

    /* --- malformed input is rejected, not misparsed ----------------------- */
    {
        url_t url;
        url_parse("http://test/x", &url);
        static const char *const BAD[] = {
            "NOT-HTTP 200 OK\r\n\r\n",
            "HTTP/1.1 999 Nonsense\r\n\r\n",
            "HTTP/1.1\r\n\r\n",
        };
        for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
            conn_t *c = conn_open_memory(BAD[i], strlen(BAD[i]), 0);
            http_stream_t bad;
            int rc = http_get(&bad, c, &url, NULL, 0, 1000);
            CHECK(rc != 0, "malformed response #%zu rejected", i);
            conn_close(c);
        }
    }

    TAP_DONE();
}
