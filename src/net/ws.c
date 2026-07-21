#include "net/ws.h"
#include "net/http.h"
#include "net/url.h"
#include "util/log.h"
#include "util/mono.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

#define TAG "ws"

/* A malformed length field must not be able to make us allocate the world.
 * The largest thing the protocol legitimately sends is the initial snapshot at
 * a few hundred KB, so 1MB is generous. */
#define WS_MAX_MESSAGE (1u << 20)

#define WS_READ_BUF 8192

/* RFC 6455 magic, appended to the client key before hashing. */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

struct ws {
    conn_t *conn;

    unsigned char in[WS_READ_BUF];
    size_t in_head, in_tail;

    /* Reassembly across continuation frames. */
    unsigned char *msg;
    size_t msg_len, msg_cap;
    uint8_t msg_opcode;
    bool    in_fragment;

    uint64_t last_activity_ms;
    char err[256];
};

/* --------------------------------------------------------------- frame codec */

int ws_parse_header(const uint8_t *buf, size_t len, ws_frame_hdr_t *out)
{
    if (len < 2)
        return 0;

    uint8_t b0 = buf[0], b1 = buf[1];

    /* RSV1-3 must be zero: we never negotiate an extension, so a peer setting
     * them is either confused or compressing data we cannot read. */
    if (b0 & 0x70)
        return -1;

    out->fin = (b0 & 0x80) != 0;
    out->opcode = b0 & 0x0f;
    out->masked = (b1 & 0x80) != 0;

    uint64_t plen = b1 & 0x7f;
    size_t pos = 2;

    if (plen == 126) {
        if (len < pos + 2)
            return 0;
        plen = ((uint64_t)buf[pos] << 8) | buf[pos + 1];
        pos += 2;
        /* Lengths must use the shortest encoding that fits. */
        if (plen < 126)
            return -1;
    } else if (plen == 127) {
        if (len < pos + 8)
            return 0;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | buf[pos + i];
        pos += 8;
        if (plen <= 0xffff)
            return -1;
        /* The high bit must be clear per RFC 6455. */
        if (plen & 0x8000000000000000ULL)
            return -1;
    }

    if (out->masked) {
        if (len < pos + 4)
            return 0;
        memcpy(out->mask_key, buf + pos, 4);
        pos += 4;
    } else {
        memset(out->mask_key, 0, 4);
    }

    /* Control frames carry at most 125 bytes and are never fragmented. */
    if (out->opcode & 0x8) {
        if (plen > 125 || !out->fin)
            return -1;
    }

    out->payload_len = plen;
    out->header_len = pos;
    return 1;
}

size_t ws_build_header(uint8_t *buf, uint8_t opcode, bool fin,
                       uint64_t payload_len, const uint8_t mask[4])
{
    size_t pos = 0;
    buf[pos++] = (uint8_t)((fin ? 0x80 : 0x00) | (opcode & 0x0f));

    if (payload_len < 126) {
        buf[pos++] = (uint8_t)(0x80 | payload_len);
    } else if (payload_len <= 0xffff) {
        buf[pos++] = 0x80 | 126;
        buf[pos++] = (uint8_t)(payload_len >> 8);
        buf[pos++] = (uint8_t)(payload_len);
    } else {
        buf[pos++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            buf[pos++] = (uint8_t)(payload_len >> (i * 8));
    }

    memcpy(buf + pos, mask, 4);
    pos += 4;
    return pos;
}

void ws_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4],
                   uint64_t offset)
{
    for (size_t i = 0; i < len; i++)
        data[i] ^= mask[(offset + i) & 3];
}

bool ws_compute_accept(const char *client_key_b64, char *out, size_t outlen)
{
    unsigned char concat[128];
    int n = snprintf((char *)concat, sizeof(concat), "%s%s", client_key_b64,
                     WS_GUID);
    if (n < 0 || (size_t)n >= sizeof(concat))
        return false;

    unsigned char digest[20];
    if (mbedtls_sha1(concat, (size_t)n, digest) != 0)
        return false;

    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, outlen, &olen, digest,
                              sizeof(digest)) != 0)
        return false;

    out[olen] = '\0';
    return true;
}

/* ------------------------------------------------------------------- random */

/* Mask keys exist to stop intermediaries being confused into treating frame
 * data as a request, not to protect secrecy; /dev/urandom is ample, and the
 * fallback only has to be unpredictable to a cache, not to an attacker. */
static void ws_random(uint8_t *out, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t got = read(fd, out, len);
        close(fd);
        if (got == (ssize_t)len)
            return;
    }

    static uint64_t counter;
    uint64_t seed = mono_ms() ^ (uint64_t)(uintptr_t)out ^ (counter += 0x9e3779b9);
    for (size_t i = 0; i < len; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        out[i] = (uint8_t)(seed >> 24);
    }
}

/* -------------------------------------------------------------- buffered I/O */

static size_t ws_buffered(const ws_t *ws) { return ws->in_tail - ws->in_head; }

/* Pulls more bytes in. Returns 1, WS_CLOSED, WS_TIMEOUT or WS_ERROR. */
static int ws_fill(ws_t *ws, int timeout_ms)
{
    if (ws->in_head == ws->in_tail) {
        ws->in_head = ws->in_tail = 0;
    } else if (ws->in_tail == sizeof(ws->in)) {
        memmove(ws->in, ws->in + ws->in_head, ws_buffered(ws));
        ws->in_tail -= ws->in_head;
        ws->in_head = 0;
    }

    ssize_t n = conn_read(ws->conn, ws->in + ws->in_tail,
                          sizeof(ws->in) - ws->in_tail, timeout_ms);
    if (n == CONN_EOF)
        return WS_CLOSED;
    if (n == CONN_TIMEOUT)
        return WS_TIMEOUT;
    if (n < 0) {
        snprintf(ws->err, sizeof(ws->err), "read: %s", conn_last_error(ws->conn));
        return WS_ERROR;
    }

    ws->in_tail += (size_t)n;
    ws->last_activity_ms = mono_ms();
    return 1;
}

/* Ensures at least `need` bytes are buffered. */
static int ws_need(ws_t *ws, size_t need, int timeout_ms)
{
    while (ws_buffered(ws) < need) {
        if (need > sizeof(ws->in)) {
            snprintf(ws->err, sizeof(ws->err),
                     "frame header larger than read buffer");
            return WS_ERROR;
        }
        int rc = ws_fill(ws, timeout_ms);
        if (rc != 1)
            return rc;
    }
    return 1;
}

/* ---------------------------------------------------------------- send path */

static int ws_send_frame(ws_t *ws, uint8_t opcode, const void *payload,
                         size_t len)
{
    uint8_t mask[4];
    ws_random(mask, sizeof(mask));

    uint8_t header[WS_MAX_HEADER];
    size_t hlen = ws_build_header(header, opcode, true, len, mask);

    if (conn_write_all(ws->conn, header, hlen, 10000) != 0) {
        snprintf(ws->err, sizeof(ws->err), "write header: %s",
                 conn_last_error(ws->conn));
        return WS_ERROR;
    }

    if (len == 0)
        return 0;

    /* Mask a copy in chunks rather than the caller's buffer -- callers pass
     * string literals and shared state, and masking in place would corrupt
     * them. */
    uint8_t chunk[1024];
    size_t sent = 0;
    const uint8_t *src = payload;

    while (sent < len) {
        size_t n = len - sent;
        if (n > sizeof(chunk))
            n = sizeof(chunk);
        memcpy(chunk, src + sent, n);
        ws_apply_mask(chunk, n, mask, sent);

        if (conn_write_all(ws->conn, chunk, n, 10000) != 0) {
            snprintf(ws->err, sizeof(ws->err), "write payload: %s",
                     conn_last_error(ws->conn));
            return WS_ERROR;
        }
        sent += n;
    }
    return 0;
}

int ws_send_text(ws_t *ws, const char *text, size_t len)
{
    return ws_send_frame(ws, WS_OP_TEXT, text, len);
}

/* ---------------------------------------------------------------- handshake */

ws_t *ws_connect(conn_tls_ctx_t *tls, const char *url_str, int timeout_ms)
{
    url_t url;
    if (!url_parse(url_str, &url)) {
        LOGE(TAG, "cannot parse URL '%s'", url_str);
        return NULL;
    }

    uint8_t key_raw[16];
    ws_random(key_raw, sizeof(key_raw));

    char key_b64[32];
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)key_b64, sizeof(key_b64), &olen,
                              key_raw, sizeof(key_raw)) != 0) {
        LOGE(TAG, "failed to encode Sec-WebSocket-Key");
        return NULL;
    }
    key_b64[olen] = '\0';

    conn_t *conn = conn_open(tls, &url, timeout_ms);
    if (!conn)
        return NULL;

    /* No Sec-WebSocket-Extensions: not offering permessage-deflate means the
     * server will not compress, and we need no inflate path at all. */
    const http_header_t extra[] = {
        { "Upgrade",               "websocket" },
        { "Connection",            "Upgrade" },
        { "Sec-WebSocket-Version", "13" },
        { "Sec-WebSocket-Key",     key_b64 },
    };

    http_stream_t hs;
    int rc = http_get(&hs, conn, &url, extra,
                      sizeof(extra) / sizeof(extra[0]), timeout_ms);
    if (rc != 0) {
        LOGE(TAG, "handshake request failed: %s", conn_last_error(conn));
        conn_close(conn);
        return NULL;
    }

    if (hs.status != 101) {
        LOGE(TAG, "expected 101 Switching Protocols, got %d", hs.status);
        conn_close(conn);
        return NULL;
    }

    char accept[64];
    if (!http_header(&hs, "Sec-WebSocket-Accept", accept, sizeof(accept))) {
        LOGE(TAG, "server omitted Sec-WebSocket-Accept");
        conn_close(conn);
        return NULL;
    }

    char expect[64];
    if (!ws_compute_accept(key_b64, expect, sizeof(expect))) {
        LOGE(TAG, "failed to compute expected accept key");
        conn_close(conn);
        return NULL;
    }

    /* Verifying this is what distinguishes a real WebSocket peer from a proxy
     * that echoed a 101 without understanding the protocol. */
    if (strcmp(accept, expect) != 0) {
        LOGE(TAG, "accept key mismatch (got '%s', expected '%s')", accept, expect);
        conn_close(conn);
        return NULL;
    }

    ws_t *ws = calloc(1, sizeof(*ws));
    if (!ws) {
        conn_close(conn);
        return NULL;
    }
    ws->conn = conn;
    ws->last_activity_ms = mono_ms();

    /* The server may pack its first frames into the same TCP segment as the
     * 101 response. Those bytes are already in the HTTP reader and would be
     * lost forever if we went straight back to the socket. */
    size_t pending = 0;
    const unsigned char *buf = http_buffered(&hs, &pending);
    if (pending > 0) {
        if (pending > sizeof(ws->in)) {
            LOGE(TAG, "too many bytes buffered after handshake (%zu)", pending);
            ws_close(ws);
            return NULL;
        }
        memcpy(ws->in, buf, pending);
        ws->in_tail = pending;
        LOGD(TAG, "carried %zu bytes over from the handshake", pending);
    }

    LOGI(TAG, "connected to %s", url_str);
    return ws;
}

void ws_close(ws_t *ws)
{
    if (!ws)
        return;
    if (ws->conn) {
        ws_send_frame(ws, WS_OP_CLOSE, NULL, 0);
        conn_close(ws->conn);
    }
    free(ws->msg);
    free(ws);
}

/* ---------------------------------------------------------------- recv path */

static bool msg_reserve(ws_t *ws, size_t extra)
{
    if (ws->msg_len + extra <= ws->msg_cap)
        return true;

    size_t cap = ws->msg_cap ? ws->msg_cap : 16384;
    while (cap < ws->msg_len + extra)
        cap *= 2;

    if (cap > WS_MAX_MESSAGE) {
        snprintf(ws->err, sizeof(ws->err),
                 "message exceeds %u byte cap", WS_MAX_MESSAGE);
        return false;
    }

    unsigned char *p = realloc(ws->msg, cap);
    if (!p) {
        snprintf(ws->err, sizeof(ws->err), "out of memory reassembling message");
        return false;
    }
    ws->msg = p;
    ws->msg_cap = cap;
    return true;
}

/* Reads payload_len bytes of a frame, appending to dst (or discarding if NULL),
 * unmasking as it goes. */
static int read_payload(ws_t *ws, const ws_frame_hdr_t *h, unsigned char *dst,
                        int timeout_ms)
{
    uint64_t remaining = h->payload_len;
    uint64_t offset = 0;

    while (remaining > 0) {
        if (ws_buffered(ws) == 0) {
            int rc = ws_fill(ws, timeout_ms);
            if (rc != 1)
                return rc;
        }

        size_t avail = ws_buffered(ws);
        size_t n = remaining < avail ? (size_t)remaining : avail;

        if (dst) {
            memcpy(dst + offset, ws->in + ws->in_head, n);
            if (h->masked)
                ws_apply_mask(dst + offset, n, h->mask_key, offset);
        }

        ws->in_head += n;
        offset += n;
        remaining -= n;
    }
    return 1;
}

int ws_recv(ws_t *ws, const char **payload, size_t *len, int timeout_ms)
{
    uint64_t deadline = mono_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);

    for (;;) {
        int remaining_ms = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t now = mono_ms();
            if (now >= deadline)
                return WS_TIMEOUT;
            remaining_ms = (int)(deadline - now);
        }

        /* Header: peek, growing the buffer until the whole thing is present. */
        ws_frame_hdr_t h;
        int parsed;
        for (;;) {
            parsed = ws_parse_header(ws->in + ws->in_head, ws_buffered(ws), &h);
            if (parsed == 1)
                break;
            if (parsed < 0) {
                snprintf(ws->err, sizeof(ws->err), "malformed frame header");
                return WS_ERROR;
            }
            int rc = ws_need(ws, ws_buffered(ws) + 1, remaining_ms);
            if (rc != 1)
                return rc;
        }
        ws->in_head += h.header_len;

        switch (h.opcode) {
        case WS_OP_PING: {
            /* The one obligation that keeps the connection alive: the server
             * pings every 10s and drops us if we have not ponged by the next
             * cycle. The pong must echo the ping's payload. */
            unsigned char pong[125];
            int rc = read_payload(ws, &h, pong, remaining_ms);
            if (rc != 1)
                return rc;
            LOGD(TAG, "ping -> pong (%llu bytes)",
                 (unsigned long long)h.payload_len);
            if (ws_send_frame(ws, WS_OP_PONG, pong,
                              (size_t)h.payload_len) != 0)
                return WS_ERROR;
            continue;
        }

        case WS_OP_PONG: {
            unsigned char discard[125];
            int rc = read_payload(ws, &h, discard, remaining_ms);
            if (rc != 1)
                return rc;
            continue;
        }

        case WS_OP_CLOSE: {
            unsigned char reason[125];
            read_payload(ws, &h, reason, remaining_ms);
            if (h.payload_len >= 2) {
                unsigned code = ((unsigned)reason[0] << 8) | reason[1];
                LOGI(TAG, "server closed (code %u)", code);
            } else {
                LOGI(TAG, "server closed");
            }
            return WS_CLOSED;
        }

        case WS_OP_TEXT:
        case WS_OP_BINARY:
            if (ws->in_fragment) {
                snprintf(ws->err, sizeof(ws->err),
                         "new data frame arrived mid-fragment");
                return WS_ERROR;
            }
            ws->msg_len = 0;
            ws->msg_opcode = h.opcode;
            break;

        case WS_OP_CONTINUATION:
            if (!ws->in_fragment) {
                snprintf(ws->err, sizeof(ws->err),
                         "continuation frame with nothing to continue");
                return WS_ERROR;
            }
            break;

        default:
            snprintf(ws->err, sizeof(ws->err), "unknown opcode 0x%x", h.opcode);
            return WS_ERROR;
        }

        if (!msg_reserve(ws, (size_t)h.payload_len + 1))
            return WS_ERROR;

        int rc = read_payload(ws, &h, ws->msg + ws->msg_len, remaining_ms);
        if (rc != 1)
            return rc;
        ws->msg_len += (size_t)h.payload_len;

        if (!h.fin) {
            /* Large snapshots really do arrive fragmented; this is a normal
             * path, not an error case. */
            ws->in_fragment = true;
            continue;
        }

        ws->in_fragment = false;
        ws->msg[ws->msg_len] = '\0'; /* callers treat text as a C string */

        *payload = (const char *)ws->msg;
        *len = ws->msg_len;
        return WS_MSG;
    }
}

uint64_t ws_last_activity_ms(const ws_t *ws)
{
    return ws->last_activity_ms;
}

const char *ws_last_error(const ws_t *ws)
{
    return (ws && ws->err[0]) ? ws->err : "no error";
}
