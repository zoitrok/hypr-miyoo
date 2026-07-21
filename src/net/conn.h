#ifndef HYPR_CONN_H
#define HYPR_CONN_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "net/url.h"

/* A byte-stream connection, either plain TCP or TLS, behind one interface.
 *
 * Both users of this -- the audio stream and the WebSocket -- talk to the same
 * host over the same scheme, so putting TLS here rather than in each of them
 * means neither has to know it exists. It is also the single choke point where
 * the --chaos test harness injects stalls and truncations. */

typedef struct conn conn_t;
typedef struct conn_tls_ctx conn_tls_ctx_t;

/* Return values for conn_read / conn_write. Positive means bytes transferred. */
#define CONN_EOF      0   /* peer closed cleanly */
#define CONN_ERR    (-1)  /* fatal; the connection must be torn down */
#define CONN_TIMEOUT (-2) /* nothing transferred within the deadline */

/* The entropy pool, RNG and parsed CA chain are expensive to set up and to hold
 * (a full Mozilla bundle is ~200KB of parsed x509), so they are created once
 * and shared by every connection.
 *
 * ca_file should be a trimmed bundle containing only the roots our backend
 * actually chains to -- for hypr.website that is ISRG Root X1 and X2, about 2KB,
 * versus ~1.5MB of TLS heap for the full bundle. Roots only, never
 * intermediates: Let's Encrypt rotates intermediates and pinning one would
 * break the app on renewal.
 *
 * insecure=true disables verification. Development only; never ship it. */
conn_tls_ctx_t *conn_tls_ctx_new(const char *ca_file, bool insecure);
void conn_tls_ctx_free(conn_tls_ctx_t *ctx);

/* Resolves, connects, and performs the TLS handshake if url->tls.
 * tls may be NULL only for plain connections. Returns NULL on failure. */
conn_t *conn_open(conn_tls_ctx_t *tls, const url_t *url, int timeout_ms);

/* A connection backed by an in-memory buffer instead of a socket. Reads drain
 * the buffer and then report EOF; writes are discarded.
 *
 * This is the seam the offline harness is built on -- it is what --fake-stream
 * replays a local MP3 through, and what the unit tests feed crafted HTTP
 * responses into, without either needing a network or a server.
 *
 * max_read bounds how much a single conn_read() will return (0 = unbounded).
 * Setting it small is the point: it forces the callers' parsers to handle
 * records split across read boundaries, which is where framing bugs actually
 * live and which a real fast connection almost never exercises.
 *
 * The buffer is copied, so the caller need not keep it alive. */
conn_t *conn_open_memory(const void *data, size_t len, size_t max_read);

/* Both block up to timeout_ms. A short read is normal and not an error.
 * conn_write may also return a short count; use conn_write_all for headers. */
ssize_t conn_read(conn_t *c, void *buf, size_t len, int timeout_ms);
ssize_t conn_write(conn_t *c, const void *buf, size_t len, int timeout_ms);

/* Writes the whole buffer or fails. Returns 0 on success, CONN_ERR/CONN_TIMEOUT. */
int conn_write_all(conn_t *c, const void *buf, size_t len, int timeout_ms);

void conn_close(conn_t *c);

/* Human-readable description of the last failure, for logging. */
const char *conn_last_error(const conn_t *c);

/* Fault injection, for exercising the paths that are otherwise nearly
 * untestable: reconnect, buffer underrun, partial reads.
 *
 * Applied here because conn is the single choke point every byte passes
 * through, so one switch covers the audio stream and the WebSocket alike, and
 * neither of them needs to know it exists.
 *
 * `severity` is the probability (0..1) that any single read is disrupted;
 * 0 disables it. Roughly a third of disruptions are a hard drop, a third a
 * stall (the read times out), and a third a short read. Short reads are the
 * most valuable of the three: they are common in reality, they are what broke
 * the MP3 decoder, and nothing else provokes them reliably. */
void conn_chaos_enable(double severity, unsigned seed);

/* Why the most recent conn_open() on *this thread* failed.
 *
 * conn_open returns NULL and logs, but the caller has no object left to ask.
 * The UI needs the reason -- "OFFLINE" alone sends you hunting for a log file
 * when the device could just say "certificate expired" or "resolve failed".
 * Thread-local because the audio and metadata threads connect independently. */
const char *conn_last_open_error(void);

#endif
