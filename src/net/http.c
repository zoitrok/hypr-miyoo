#include "net/http.h"
#include "util/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "http"

#define HTTP_EBAD (-3)   /* malformed / oversized response */

/* ------------------------------------------------------------- buffered input */

static size_t buffered(const http_stream_t *s) { return s->tail - s->head; }

/* Pulls at least one more byte into the buffer. Returns 1 on success,
 * CONN_EOF, or a negative CONN_* error. */
static int fill(http_stream_t *s, int timeout_ms)
{
    if (s->head == s->tail) {
        s->head = s->tail = 0;
    } else if (s->tail == sizeof(s->buf)) {
        /* Slide the unconsumed remainder down rather than growing. Only the
         * header/chunk-line scanners can push us here, and both are bounded. */
        memmove(s->buf, s->buf + s->head, buffered(s));
        s->tail -= s->head;
        s->head = 0;
    }

    if (s->tail == sizeof(s->buf))
        return HTTP_EBAD; /* a single line longer than the whole buffer */

    ssize_t n = conn_read(s->conn, s->buf + s->tail, sizeof(s->buf) - s->tail,
                          timeout_ms);
    if (n <= 0)
        return (int)n;

    s->tail += (size_t)n;
    return 1;
}

/* Reads one CRLF- (or LF-) terminated line into out, without the terminator.
 * Returns 0 on success, CONN_EOF, or a negative error. */
static int read_line(http_stream_t *s, char *out, size_t outlen, int timeout_ms)
{
    size_t scanned = 0;

    for (;;) {
        unsigned char *base = s->buf + s->head;
        size_t avail = buffered(s);

        unsigned char *nl = memchr(base + scanned, '\n', avail - scanned);
        if (nl) {
            size_t line_len = (size_t)(nl - base);
            s->head += line_len + 1;

            if (line_len > 0 && base[line_len - 1] == '\r')
                line_len--;
            if (line_len >= outlen)
                return HTTP_EBAD;

            memcpy(out, base, line_len);
            out[line_len] = '\0';
            return 0;
        }

        scanned = avail;
        int rc = fill(s, timeout_ms);
        if (rc <= 0)
            return rc == CONN_EOF ? CONN_EOF : rc;
        /* fill() may have slid the buffer down; rescan from the same offset,
         * which is still correct because only leading consumed bytes moved. */
        scanned = (buffered(s) < scanned) ? 0 : scanned;
    }
}

/* ------------------------------------------------------------------- headers */

const char *http_header(const http_stream_t *s, const char *name, char *out,
                        size_t outlen)
{
    size_t namelen = strlen(name);
    const char *p = s->headers;

    while (*p) {
        const char *eol = strstr(p, "\r\n");
        if (!eol)
            eol = p + strlen(p);

        if (strncasecmp(p, name, namelen) == 0) {
            const char *v = p + namelen;
            while (v < eol && (*v == ' ' || *v == '\t'))
                v++;
            if (v < eol && *v == ':') {
                v++;
                while (v < eol && (*v == ' ' || *v == '\t'))
                    v++;
                size_t len = (size_t)(eol - v);
                if (len >= outlen)
                    len = outlen - 1;
                memcpy(out, v, len);
                out[len] = '\0';
                return out;
            }
        }

        if (!*eol)
            break;
        p = eol + 2;
    }
    return NULL;
}

/* ---------------------------------------------------------------------- open */

int http_get(http_stream_t *s, conn_t *conn, const url_t *url,
             const http_header_t *extra, size_t n_extra, int timeout_ms)
{
    memset(s, 0, sizeof(*s));
    s->conn = conn;
    s->content_length = -1;
    s->chunk_remaining = 0;

    char req[2048];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "User-Agent: hypr-miyoo/1.0\r\n",
                     url->path, url->host);
    if (n < 0 || (size_t)n >= sizeof(req))
        return HTTP_EBAD;

    for (size_t i = 0; i < n_extra; i++) {
        int m = snprintf(req + n, sizeof(req) - (size_t)n, "%s: %s\r\n",
                         extra[i].name, extra[i].value);
        if (m < 0 || (size_t)(n + m) >= sizeof(req))
            return HTTP_EBAD;
        n += m;
    }

    if ((size_t)n + 2 >= sizeof(req))
        return HTTP_EBAD;
    memcpy(req + n, "\r\n", 2);
    n += 2;

    int rc = conn_write_all(conn, req, (size_t)n, timeout_ms);
    if (rc != 0)
        return rc;

    /* Status line: HTTP/1.x <code> <reason> */
    char line[1024];
    rc = read_line(s, line, sizeof(line), timeout_ms);
    if (rc != 0)
        return rc == CONN_EOF ? HTTP_EBAD : rc;

    if (strncasecmp(line, "HTTP/1.", 7) != 0)
        return HTTP_EBAD;
    const char *sp = strchr(line, ' ');
    if (!sp)
        return HTTP_EBAD;
    s->status = (int)strtol(sp + 1, NULL, 10);
    if (s->status < 100 || s->status > 599)
        return HTTP_EBAD;

    /* Headers, accumulated verbatim so http_header() can query them lazily. */
    for (;;) {
        rc = read_line(s, line, sizeof(line), timeout_ms);
        if (rc != 0)
            return rc == CONN_EOF ? HTTP_EBAD : rc;
        if (line[0] == '\0')
            break;

        size_t len = strlen(line);
        if (s->headers_len + len + 3 > sizeof(s->headers)) {
            LOGW(TAG, "response headers exceed %zu bytes; truncating",
                 sizeof(s->headers));
            continue;
        }
        memcpy(s->headers + s->headers_len, line, len);
        s->headers_len += len;
        memcpy(s->headers + s->headers_len, "\r\n", 2);
        s->headers_len += 2;
        s->headers[s->headers_len] = '\0';
    }

    char val[256];
    if (http_header(s, "Transfer-Encoding", val, sizeof(val))) {
        /* The header is a comma-separated list and chunked, when present, is
         * always the last encoding applied -- so a substring test is enough
         * for our purposes and tolerates "gzip, chunked". */
        for (char *q = val; *q; q++)
            *q = (char)tolower((unsigned char)*q);
        if (strstr(val, "chunked")) {
            s->chunked = true;
            s->chunk_need_crlf = false;
        }
    }
    if (!s->chunked && http_header(s, "Content-Length", val, sizeof(val)))
        s->content_length = strtoll(val, NULL, 10);

    LOGI(TAG, "%s%s -> %d%s%s", url->host, url->path, s->status,
         s->chunked ? " (chunked)" : "",
         s->content_length >= 0 ? " (length-delimited)" : "");

    return 0;
}

/* ------------------------------------------------------------ chunked reading */

/* Ensures chunk_remaining > 0, or sets body_eof. Returns 0, CONN_EOF, or error. */
static int chunk_advance(http_stream_t *s, int timeout_ms)
{
    char line[128];

    while (s->chunk_remaining == 0 && !s->body_eof) {
        if (s->chunk_need_crlf) {
            int rc = read_line(s, line, sizeof(line), timeout_ms);
            if (rc != 0)
                return rc == CONN_EOF ? CONN_EOF : rc;
            if (line[0] != '\0') {
                LOGE(TAG, "chunk not terminated by CRLF");
                return HTTP_EBAD;
            }
            s->chunk_need_crlf = false;
        }

        int rc = read_line(s, line, sizeof(line), timeout_ms);
        if (rc != 0)
            return rc == CONN_EOF ? CONN_EOF : rc;

        /* "<hex-size>[;chunk-extension]" -- strtoll stops at ';' for us. */
        char *end = NULL;
        long long size = strtoll(line, &end, 16);
        if (end == line || size < 0) {
            LOGE(TAG, "malformed chunk size line: '%s'", line);
            return HTTP_EBAD;
        }

        if (size == 0) {
            /* Final chunk: consume trailers up to the blank line. */
            for (;;) {
                rc = read_line(s, line, sizeof(line), timeout_ms);
                if (rc != 0)
                    break; /* EOF right after the terminator is common enough */
                if (line[0] == '\0')
                    break;
            }
            s->body_eof = true;
            return CONN_EOF;
        }

        s->chunk_remaining = size;
    }

    return s->body_eof ? CONN_EOF : 0;
}

ssize_t http_read(http_stream_t *s, void *out, size_t len, int timeout_ms)
{
    if (s->body_eof)
        return CONN_EOF;
    if (len == 0)
        return 0;

    size_t want = len;

    if (s->chunked) {
        int rc = chunk_advance(s, timeout_ms);
        if (rc != 0)
            return rc;
        if ((long long)want > s->chunk_remaining)
            want = (size_t)s->chunk_remaining;
    } else if (s->content_length >= 0) {
        if (s->content_length == 0) {
            s->body_eof = true;
            return CONN_EOF;
        }
        if ((long long)want > s->content_length)
            want = (size_t)s->content_length;
    }

    size_t avail = buffered(s);
    if (avail == 0) {
        int rc = fill(s, timeout_ms);
        if (rc <= 0) {
            if (rc == CONN_EOF)
                s->body_eof = true;
            return rc;
        }
        avail = buffered(s);
    }

    size_t n = want < avail ? want : avail;
    memcpy(out, s->buf + s->head, n);
    s->head += n;

    if (s->chunked) {
        s->chunk_remaining -= (long long)n;
        if (s->chunk_remaining == 0)
            s->chunk_need_crlf = true;
    } else if (s->content_length >= 0) {
        s->content_length -= (long long)n;
    }

    return (ssize_t)n;
}

const unsigned char *http_buffered(const http_stream_t *s, size_t *len)
{
    *len = s->tail - s->head;
    return s->buf + s->head;
}
