#ifndef HYPR_HTTP_H
#define HYPR_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "net/conn.h"
#include "net/url.h"

#define HTTP_BUF_SIZE     8192
#define HTTP_HEADERS_MAX  4096

typedef struct {
    const char *name;
    const char *value;
} http_header_t;

/* A buffered HTTP/1.1 response reader that transparently de-frames chunked
 * transfer encoding.
 *
 * The chunked path is not defensive programming: hypr.website serves the
 * Icecast MP3 stream through nginx, which re-frames it as
 * "Transfer-Encoding: chunked". Feeding the chunk-length lines into an MP3
 * decoder produces periodic clicks and slow drift that are extremely
 * unpleasant to diagnose, so we strip them here, once, for every consumer.
 *
 * The same reader is used for the WebSocket handshake response; after
 * http_open() returns 101, call http_take_conn() and switch to frame parsing.
 * Any bytes already buffered past the response head are handed back with it,
 * which matters because the server may pack its first frame into the same
 * TCP segment as the 101. */
typedef struct {
    conn_t *conn;

    unsigned char buf[HTTP_BUF_SIZE];
    size_t head, tail;              /* buffered, unconsumed bytes: [head, tail) */

    int  status;
    char headers[HTTP_HEADERS_MAX]; /* raw block, NUL-terminated, CRLFs intact */
    size_t headers_len;

    bool      chunked;
    long long content_length;       /* -1 when absent */

    /* chunked de-framer state */
    long long chunk_remaining;
    bool      chunk_need_crlf;      /* consume CRLF trailing the previous chunk */
    bool      body_eof;
} http_stream_t;

/* Sends "GET <path> HTTP/1.1" with Host, plus any extra headers, and reads the
 * status line and headers. Returns 0 on success (any HTTP status -- inspect
 * s->status), CONN_ERR / CONN_TIMEOUT on transport failure, or -3 on a
 * malformed or oversized response head.
 *
 * Takes ownership of nothing: the caller still owns conn. */
int http_get(http_stream_t *s, conn_t *conn, const url_t *url,
             const http_header_t *extra, size_t n_extra, int timeout_ms);

/* Reads de-framed body bytes. Returns >0 bytes, CONN_EOF at end of body,
 * or a negative CONN_* error. Short reads are normal. */
ssize_t http_read(http_stream_t *s, void *out, size_t len, int timeout_ms);

/* Case-insensitive header lookup. Returns NULL if absent. The value is copied
 * into out and NUL-terminated. */
const char *http_header(const http_stream_t *s, const char *name, char *out,
                        size_t outlen);

/* Bytes already read past the response head. After a 101 the WebSocket layer
 * must consume these before reading from the socket again. */
const unsigned char *http_buffered(const http_stream_t *s, size_t *len);

#endif
