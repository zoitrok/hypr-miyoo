#ifndef HYPR_WS_H
#define HYPR_WS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "net/conn.h"

/* A minimal RFC 6455 client: exactly the subset hypr.website's protocol needs,
 * and no more. No extensions, no compression, no server role.
 *
 * The three things here that are easy to get wrong and expensive to diagnose:
 *
 *  - The server sends a *protocol-level* PING frame every 10s and drops any
 *    connection that has not PONGed by the next cycle. This is distinct from
 *    the application-level {"type":"ping"} message. Miss it and the connection
 *    dies every ~20s with no error and no obvious cause.
 *
 *  - The initial playback_update snapshot is large (100-300KB), so the 64-bit
 *    extended length path and continuation-frame reassembly are both on the
 *    normal path, not edge cases.
 *
 *  - We do not offer permessage-deflate, so the server will not compress and
 *    we need no inflate at all.
 *
 * A ws_t is single-threaded: one thread owns both send and receive. */

/* ------------------------------------------------------------- frame codec */
/* Pure functions, exposed so they can be tested without a socket. */

#define WS_OP_CONTINUATION 0x0
#define WS_OP_TEXT         0x1
#define WS_OP_BINARY       0x2
#define WS_OP_CLOSE        0x8
#define WS_OP_PING         0x9
#define WS_OP_PONG         0xA

/* Longest possible client frame header: 2 + 8 length + 4 mask. */
#define WS_MAX_HEADER 14

typedef struct {
    bool     fin;
    uint8_t  opcode;
    bool     masked;
    uint64_t payload_len;
    uint8_t  mask_key[4];
    size_t   header_len;
} ws_frame_hdr_t;

/* Returns 1 with *out filled, 0 if more bytes are needed, -1 on a malformed
 * header (reserved bits set, or a length encoded non-minimally). */
int ws_parse_header(const uint8_t *buf, size_t len, ws_frame_hdr_t *out);

/* Writes a client frame header (always masked, as RFC 6455 requires of
 * clients) into buf, which must have room for WS_MAX_HEADER. Returns the
 * header length. */
size_t ws_build_header(uint8_t *buf, uint8_t opcode, bool fin,
                       uint64_t payload_len, const uint8_t mask[4]);

/* XOR-masks len bytes in place. offset is the byte position within the whole
 * payload, so masking can be applied across chunk boundaries. */
void ws_apply_mask(uint8_t *data, size_t len, const uint8_t mask[4],
                   uint64_t offset);

/* Computes the Sec-WebSocket-Accept value for a given client key, per RFC 6455.
 * out must hold at least 29 bytes + NUL. Returns false on failure. */
bool ws_compute_accept(const char *client_key_b64, char *out, size_t outlen);

/* ------------------------------------------------------------- connection */

typedef struct ws ws_t;

/* ws_recv return codes. */
#define WS_MSG      1   /* a complete text/binary message is available */
#define WS_TIMEOUT  0   /* nothing complete within the deadline (not an error) */
#define WS_CLOSED (-1)  /* peer closed cleanly */
#define WS_ERROR  (-2)

/* Connects, performs the HTTP Upgrade handshake, and validates the accept key.
 * url may be ws:// or wss://. Returns NULL on failure. */
ws_t *ws_connect(conn_tls_ctx_t *tls, const char *url, int timeout_ms);

void ws_close(ws_t *ws);

/* Reads until one complete application message is reassembled, the deadline
 * passes, or the connection ends.
 *
 * Control frames are handled internally and never surface: PING is answered
 * with a PONG echoing its payload, PONG is recorded, CLOSE ends the
 * connection. On WS_MSG, *payload points into ws-owned memory valid only
 * until the next ws_recv call. */
int ws_recv(ws_t *ws, const char **payload, size_t *len, int timeout_ms);

/* Sends a text message. Returns 0 on success. */
int ws_send_text(ws_t *ws, const char *text, size_t len);

/* mono_ms() of the most recent frame of any kind received. The staleness
 * watchdog uses this: a connection can be silently dead while the socket
 * still looks fine. */
uint64_t ws_last_activity_ms(const ws_t *ws);

const char *ws_last_error(const ws_t *ws);

#endif
