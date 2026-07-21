#include "net/conn.h"
#include "util/log.h"
#include "net/timesync.h"
#include "util/mono.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define TAG "conn"

struct conn_tls_ctx {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_x509_crt         ca;
    mbedtls_ssl_config       conf;
    bool                     insecure;
    bool                     have_ca;
};

typedef enum {
    CONN_KIND_SOCKET = 0,
    CONN_KIND_MEMORY
} conn_kind_t;

struct conn {
    conn_kind_t kind;

    /* CONN_KIND_MEMORY */
    unsigned char *mem;
    size_t mem_len, mem_pos, mem_max_read;

    int  fd;
    bool tls;
    bool ssl_ready;
    mbedtls_ssl_context ssl;
    /* Sized so a hostname plus a worst-case strerror()/x509 verify string fits
     * without truncation; these strings are the only diagnosis available from a
     * log file on the device, so it is worth the stack. */
    char err[512];
};

static void conn_set_err(conn_t *c, const char *what, int code)
{
    if (!c)
        return;
    if (code != 0) {
        char buf[128];
        mbedtls_strerror(code, buf, sizeof(buf));
        snprintf(c->err, sizeof(c->err), "%s: %s (-0x%04x)", what, buf,
                 (unsigned)-code);
    } else {
        snprintf(c->err, sizeof(c->err), "%s: %s", what, strerror(errno));
    }
}

const char *conn_last_error(const conn_t *c)
{
    return (c && c->err[0]) ? c->err : "no error";
}

/* ------------------------------------------------------------------- chaos */

static double   g_chaos_severity;
static unsigned g_chaos_state = 1;

void conn_chaos_enable(double severity, unsigned seed)
{
    g_chaos_severity = severity;
    g_chaos_state = seed ? seed : 1;
    if (severity > 0)
        LOGW(TAG, "CHAOS ENABLED: %.1f%% of reads will be disrupted",
             severity * 100.0);
}

static unsigned chaos_rand(void)
{
    /* xorshift32 -- deterministic given the seed, so a failure found under
     * chaos can be reproduced with the same --chaos-seed. */
    unsigned x = g_chaos_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_chaos_state = x;
    return x;
}

typedef enum {
    CHAOS_NONE = 0,
    CHAOS_SHORT,    /* return less than asked for */
    CHAOS_JITTER,   /* pause, then read normally */
    CHAOS_DROP,     /* hard error */
    CHAOS_TIMEOUT   /* report a timeout without waiting for one */
} chaos_t;

/* Weighted, not uniform. An even split was actively bad: a stall that slept the
 * full 10s read timeout ate the entire run, so a 60s test produced six stalls,
 * zero drops and zero short reads -- it exercised one path and starved the rest.
 *
 * Short reads dominate because they dominate in reality and because they are
 * what broke the MP3 decoder. Jitter pauses briefly and then delivers, which
 * drains the buffer without forcing a reconnect. Only the two rarest outcomes
 * end the connection. */
static chaos_t chaos_roll(void)
{
    if (g_chaos_severity <= 0)
        return CHAOS_NONE;
    if ((double)(chaos_rand() % 100000u) / 100000.0 >= g_chaos_severity)
        return CHAOS_NONE;

    unsigned r = chaos_rand() % 100u;
    if (r < 55) return CHAOS_SHORT;
    if (r < 80) return CHAOS_JITTER;
    if (r < 93) return CHAOS_DROP;
    return CHAOS_TIMEOUT;
}

/* Per-thread so the audio and metadata threads cannot overwrite each other. */
static __thread char g_open_err[256];

const char *conn_last_open_error(void)
{
    return g_open_err[0] ? g_open_err : "";
}

static void set_open_err(const char *msg)
{
    snprintf(g_open_err, sizeof(g_open_err), "%s", msg ? msg : "");
}

/* ---------------------------------------------------------------- TLS context */

/* Certificate validity dates cannot be judged without a trustworthy clock, and
 * this device boots with none. If timesync could not fix it (no root, or the
 * bootstrap host unreachable), the choice is between ignoring the date window
 * and not running at all.
 *
 * We ignore only the dates, and only while the clock is known to be unset.
 * Chain-to-pinned-root, signature and hostname checks all still apply, so a
 * certificate must still genuinely be for this host and issued by ISRG -- just
 * possibly expired. That is a far smaller concession than the alternative
 * anyone reaching for in this situation, which is disabling verification. */
static int verify_cb(void *user, mbedtls_x509_crt *crt, int depth, uint32_t *flags)
{
    (void)user;
    (void)crt;
    (void)depth;

    if (timesync_clock_is_plausible())
        return 0;

    uint32_t dates = *flags & (MBEDTLS_X509_BADCERT_EXPIRED |
                               MBEDTLS_X509_BADCERT_FUTURE);
    if (dates) {
        *flags &= ~dates;
        LOGW(TAG, "ignoring certificate validity dates at depth %d: the system "
                  "clock is unset, so the dates cannot be checked", depth);
    }
    return 0;
}

conn_tls_ctx_t *conn_tls_ctx_new(const char *ca_file, bool insecure)
{
    conn_tls_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->insecure = insecure;

    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->drbg);
    mbedtls_x509_crt_init(&ctx->ca);
    mbedtls_ssl_config_init(&ctx->conf);

    static const char PERS[] = "hypr-radio";
    int rc = mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func,
                                   &ctx->entropy, (const unsigned char *)PERS,
                                   sizeof(PERS) - 1);
    if (rc != 0) {
        LOGE(TAG, "ctr_drbg_seed failed (-0x%04x)", (unsigned)-rc);
        goto fail;
    }

    if (ca_file && *ca_file) {
        rc = mbedtls_x509_crt_parse_file(&ctx->ca, ca_file);
        if (rc < 0) {
            LOGE(TAG, "failed to parse CA file '%s' (-0x%04x)", ca_file,
                 (unsigned)-rc);
            goto fail;
        }
        /* A positive return means "parsed, but this many certs were skipped".
         * Worth warning about: a partly-parsed bundle still verifies, so this
         * would otherwise be a silent way to end up trusting less than we meant. */
        if (rc > 0)
            LOGW(TAG, "CA file '%s': %d certificate(s) failed to parse", ca_file, rc);
        ctx->have_ca = true;
    }

    rc = mbedtls_ssl_config_defaults(&ctx->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        LOGE(TAG, "ssl_config_defaults failed (-0x%04x)", (unsigned)-rc);
        goto fail;
    }

    if (insecure) {
        LOGW(TAG, "TLS certificate verification DISABLED (insecure=true)");
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        if (!ctx->have_ca) {
            LOGE(TAG, "no CA file configured and insecure=false; refusing to run");
            goto fail;
        }
        mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca, NULL);
    }

    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->drbg);

    if (!insecure)
        mbedtls_ssl_conf_verify(&ctx->conf, verify_cb, NULL);

    /* The Cortex-A7 has no ARMv8 crypto extensions, so AES runs as software
     * table lookups while ChaCha20 is plain ARM arithmetic and comes out ahead.
     * At our ~24KB/s this is not a throughput concern either way; we express
     * the preference only because it is free to do so. mbedTLS orders by our
     * list, but the server picks, so this is a hint and not a guarantee. */
    static const int CIPHERSUITES[] = {
        MBEDTLS_TLS1_3_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS1_3_AES_128_GCM_SHA256,
        MBEDTLS_TLS1_3_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
        0
    };
    mbedtls_ssl_conf_ciphersuites(&ctx->conf, CIPHERSUITES);

    return ctx;

fail:
    conn_tls_ctx_free(ctx);
    return NULL;
}

void conn_tls_ctx_free(conn_tls_ctx_t *ctx)
{
    if (!ctx)
        return;
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->ca);
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    free(ctx);
}

/* --------------------------------------------------------------- socket setup */

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags != -1 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

/* Waits for the socket to become ready. Returns 1 ready, 0 timeout, -1 error. */
static int wait_ready(int fd, bool for_write, int timeout_ms)
{
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = for_write ? POLLOUT : POLLIN;
    pfd.revents = 0;

    for (;;) {
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc > 0)
            return 1;
        if (rc == 0)
            return 0;
        if (errno != EINTR)
            return -1;
        /* On EINTR we retry with the full timeout rather than tracking the
         * remainder. Signals are rare here and the callers all treat the
         * timeout as a watchdog bound, not a precise deadline. */
    }
}

/* Last address that connected successfully, per host, so a reconnect can skip
 * DNS entirely.
 *
 * This is not a micro-optimisation, it is the fix for the dominant outage this
 * app actually hits. On the device, WiFi glitches take the resolver down with
 * them, and a failing getaddrinfo() blocks ~10s before returning -- it is
 * synchronous and ignores our connect timeout. So a momentary blip that the
 * server would have ridden out becomes a 10s+ hole in the audio, repeated as
 * the backoff climbs. Reusing the last good IP means the common reconnect does
 * no name lookup at all, and only a genuine server move forces us back through
 * DNS.
 *
 * One entry is enough: the app talks to exactly one host over two connections.
 * The mutex covers that shared entry across the audio and metadata threads. */
static pthread_mutex_t g_dns_lock = PTHREAD_MUTEX_INITIALIZER;
static char            g_dns_host[256];
static int             g_dns_port;
static struct sockaddr_storage g_dns_addr;
static socklen_t       g_dns_addrlen;
static bool            g_dns_valid;

static bool dns_cache_get(const char *host, int port, struct sockaddr_storage *out,
                          socklen_t *outlen)
{
    bool hit = false;
    pthread_mutex_lock(&g_dns_lock);
    if (g_dns_valid && g_dns_port == port && strcmp(g_dns_host, host) == 0) {
        *out = g_dns_addr;
        *outlen = g_dns_addrlen;
        hit = true;
    }
    pthread_mutex_unlock(&g_dns_lock);
    return hit;
}

static void dns_cache_put(const char *host, int port,
                          const struct sockaddr *addr, socklen_t addrlen)
{
    pthread_mutex_lock(&g_dns_lock);
    snprintf(g_dns_host, sizeof(g_dns_host), "%s", host);
    g_dns_port = port;
    memcpy(&g_dns_addr, addr, addrlen);
    g_dns_addrlen = addrlen;
    g_dns_valid = true;
    pthread_mutex_unlock(&g_dns_lock);
}

static void dns_cache_clear(const char *host, int port)
{
    pthread_mutex_lock(&g_dns_lock);
    if (g_dns_valid && g_dns_port == port && strcmp(g_dns_host, host) == 0)
        g_dns_valid = false;
    pthread_mutex_unlock(&g_dns_lock);
}

/* Opens a non-blocking socket and drives the connect to completion. Returns the
 * fd, or -1 with err filled. Shared by the cached-address and freshly-resolved
 * paths so their connect behaviour cannot drift apart. */
static int connect_one(const struct sockaddr *addr, socklen_t addrlen,
                       const char *host, int port, int timeout_ms,
                       char *err, size_t errlen)
{
    int fd = socket(addr->sa_family, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errlen, "socket: %s", strerror(errno));
        return -1;
    }
    if (!set_nonblocking(fd)) {
        snprintf(err, errlen, "set nonblocking: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (connect(fd, addr, addrlen) != 0) {
        if (errno != EINPROGRESS) {
            snprintf(err, errlen, "connect %s:%d: %s", host, port, strerror(errno));
            close(fd);
            return -1;
        }
        int ready = wait_ready(fd, true, timeout_ms);
        if (ready <= 0) {
            snprintf(err, errlen, "connect %s:%d: %s", host, port,
                     ready == 0 ? "timed out" : strerror(errno));
            close(fd);
            return -1;
        }
        /* POLLOUT only means the attempt finished, not that it succeeded -- a
         * refused connection is also "writable". SO_ERROR has the verdict. */
        int soerr = 0;
        socklen_t l = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l) != 0 || soerr != 0) {
            snprintf(err, errlen, "connect %s:%d: %s", host, port,
                     strerror(soerr ? soerr : errno));
            close(fd);
            return -1;
        }
    }

    /* We send small, latency-sensitive frames (WebSocket pongs, pings). Nagle
     * would coalesce a pong with nothing and delay it. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

static int tcp_connect(const char *host, int port, int timeout_ms, char *err,
                       size_t errlen, uint64_t *resolve_ms, uint64_t *connect_ms)
{
    uint64_t t_start = mono_ms();
    if (resolve_ms)
        *resolve_ms = 0;
    if (connect_ms)
        *connect_ms = 0;

    /* Fast path: reuse the last good address and skip the resolver. If it fails
     * to connect the address may be stale (server moved), so we drop it and
     * fall through to a real lookup -- self-healing, at the cost of one failed
     * attempt the first time an address goes bad. */
    struct sockaddr_storage cached;
    socklen_t cached_len;
    if (dns_cache_get(host, port, &cached, &cached_len)) {
        int fd = connect_one((struct sockaddr *)&cached, cached_len, host, port,
                             timeout_ms, err, errlen);
        if (connect_ms)
            *connect_ms = mono_ms() - t_start;
        if (fd >= 0)
            return fd;
        LOGW(TAG, "cached address failed (%s); re-resolving %s", err, host);
        dns_cache_clear(host, port);
    }

    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    uint64_t t_resolve0 = mono_ms();
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, portstr, &hints, &res);
    uint64_t t_resolved = mono_ms();
    if (resolve_ms)
        *resolve_ms = t_resolved - t_resolve0;
    if (rc != 0) {
        snprintf(err, errlen, "resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }

    int fd = -1;
    snprintf(err, errlen, "connect %s:%d: no addresses", host, port);

    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = connect_one(ai->ai_addr, ai->ai_addrlen, host, port, timeout_ms,
                         err, errlen);
        if (fd >= 0) {
            /* Remember what worked, so the next reconnect needs no lookup. */
            dns_cache_put(host, port, ai->ai_addr, ai->ai_addrlen);
            break;
        }
    }

    freeaddrinfo(res);

    if (connect_ms)
        *connect_ms = mono_ms() - t_resolved;

    return fd;
}

/* ------------------------------------------------------------------- TLS BIO */

/* mbedTLS BIO callbacks over our own non-blocking fd. Returning WANT_READ /
 * WANT_WRITE on EAGAIN is what lets the caller drive readiness with poll(). */

static int bio_send(void *vfd, const unsigned char *buf, size_t len)
{
    int fd = *(int *)vfd;
    ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0)
        return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (errno == EPIPE || errno == ECONNRESET)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *vfd, unsigned char *buf, size_t len)
{
    int fd = *(int *)vfd;
    ssize_t n = recv(fd, buf, len, 0);
    if (n >= 0)
        return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (errno == ECONNRESET)
        return MBEDTLS_ERR_NET_CONN_RESET;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* --------------------------------------------------------------------- open */

conn_t *conn_open(conn_tls_ctx_t *tls, const url_t *url, int timeout_ms)
{
    if (!url)
        return NULL;
    if (url->tls && !tls) {
        LOGE(TAG, "TLS URL requested but no TLS context supplied");
        return NULL;
    }

    conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = -1;

    uint64_t t_open = mono_ms();
    uint64_t resolve_ms = 0, connect_ms = 0;

    c->fd = tcp_connect(url->host, url->port, timeout_ms, c->err, sizeof(c->err),
                        &resolve_ms, &connect_ms);
    if (c->fd < 0) {
        LOGE(TAG, "%s", c->err);
        set_open_err(c->err);
        free(c);
        return NULL;
    }

    if (!url->tls) {
        LOGI(TAG, "connected to %s:%d (plain) in %llums "
                  "(dns %llu, tcp %llu)", url->host, url->port,
             (unsigned long long)(mono_ms() - t_open),
             (unsigned long long)resolve_ms, (unsigned long long)connect_ms);
        set_open_err("");
        return c;
    }

    uint64_t t_tls = mono_ms();
    c->tls = true;
    mbedtls_ssl_init(&c->ssl);
    c->ssl_ready = true;

    int rc = mbedtls_ssl_setup(&c->ssl, &tls->conf);
    if (rc != 0) {
        conn_set_err(c, "ssl_setup", rc);
        goto fail;
    }

    /* Sets both SNI and the name checked against the certificate. hypr.website
     * is served from a multi-domain certificate whose CN is a different host
     * (pilvi.zoi.sk) and which lists hypr.website only in the SAN, so this must
     * be the URL host and verification must be SAN-aware -- which mbedTLS is. */
    rc = mbedtls_ssl_set_hostname(&c->ssl, url->host);
    if (rc != 0) {
        conn_set_err(c, "ssl_set_hostname", rc);
        goto fail;
    }

    mbedtls_ssl_set_bio(&c->ssl, &c->fd, bio_send, bio_recv, NULL);

    uint64_t deadline = mono_ms() + (uint64_t)(timeout_ms > 0 ? timeout_ms : 0);
    for (;;) {
        rc = mbedtls_ssl_handshake(&c->ssl);
        if (rc == 0)
            break;

        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            conn_set_err(c, "tls handshake", rc);
            goto fail;
        }

        int remaining = timeout_ms;
        if (timeout_ms > 0) {
            uint64_t now = mono_ms();
            if (now >= deadline) {
                snprintf(c->err, sizeof(c->err), "tls handshake: timed out");
                goto fail;
            }
            remaining = (int)(deadline - now);
        }

        int ready = wait_ready(c->fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE, remaining);
        if (ready == 0) {
            snprintf(c->err, sizeof(c->err), "tls handshake: timed out");
            goto fail;
        }
        if (ready < 0) {
            conn_set_err(c, "tls handshake poll", 0);
            goto fail;
        }
    }

    uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
    if (flags != 0) {
        char why[256];
        mbedtls_x509_crt_verify_info(why, sizeof(why), "  ", flags);

        /* A certificate that is "expired" or "not yet valid" on a device with
         * no RTC almost always means the clock is wrong, not the certificate.
         * Saying so turns a baffling TLS error into an obvious one. */
        if (flags & (MBEDTLS_X509_BADCERT_EXPIRED | MBEDTLS_X509_BADCERT_FUTURE)) {
            time_t now_wall = time(NULL);
            LOGE(TAG, "certificate rejected on validity dates while the system "
                      "clock reads %ld -- this is a clock problem, not a "
                      "certificate problem, if the network time sync has not "
                      "run yet", (long)now_wall);
        }

        snprintf(c->err, sizeof(c->err), "certificate verification failed: %s", why);
        goto fail;
    }

    /* Broken down by phase because the total is what matters to the listener
     * -- every second here past the audio buffer is an audible gap -- and the
     * three phases have completely different remedies. */
    LOGI(TAG, "connected to %s:%d in %llums (dns %llu, tcp %llu, tls %llu) "
              "[%s, %s]",
         url->host, url->port,
         (unsigned long long)(mono_ms() - t_open),
         (unsigned long long)resolve_ms, (unsigned long long)connect_ms,
         (unsigned long long)(mono_ms() - t_tls),
         mbedtls_ssl_get_version(&c->ssl), mbedtls_ssl_get_ciphersuite(&c->ssl));
    set_open_err("");
    return c;

fail:
    LOGE(TAG, "%s", c->err);
    set_open_err(c->err);
    conn_close(c);
    return NULL;
}

/* ------------------------------------------------------------ memory backend */

conn_t *conn_open_memory(const void *data, size_t len, size_t max_read)
{
    conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    c->kind = CONN_KIND_MEMORY;
    c->fd = -1;
    c->mem_len = len;
    c->mem_max_read = max_read;

    if (len > 0) {
        c->mem = malloc(len);
        if (!c->mem) {
            free(c);
            return NULL;
        }
        memcpy(c->mem, data, len);
    }
    return c;
}

/* ------------------------------------------------------------------- read/write */

ssize_t conn_read(conn_t *c, void *buf, size_t len, int timeout_ms)
{
    if (!c)
        return CONN_ERR;

    if (c->kind == CONN_KIND_MEMORY) {
        if (len == 0)
            return 0;
        size_t avail = c->mem_len - c->mem_pos;
        if (avail == 0)
            return CONN_EOF;
        size_t n = len < avail ? len : avail;
        if (c->mem_max_read && n > c->mem_max_read)
            n = c->mem_max_read;
        memcpy(buf, c->mem + c->mem_pos, n);
        c->mem_pos += n;
        return (ssize_t)n;
    }

    if (c->fd < 0)
        return CONN_ERR;
    if (len == 0)
        return 0;

    switch (chaos_roll()) {
    case CHAOS_DROP:
        snprintf(c->err, sizeof(c->err), "chaos: simulated connection drop");
        LOGW(TAG, "chaos: dropping the connection");
        return CONN_ERR;

    case CHAOS_TIMEOUT:
        /* Reported without actually waiting: the caller treats a timeout as a
         * dead connection either way, and sleeping the real timeout would make
         * a chaos run mostly sleep. */
        LOGW(TAG, "chaos: reporting a read timeout");
        return CONN_TIMEOUT;

    case CHAOS_JITTER: {
        unsigned ms = 50 + chaos_rand() % 450u;
        LOGW(TAG, "chaos: %u ms of jitter", ms);
        mono_sleep_ms(ms);
        break;      /* then read for real */
    }

    case CHAOS_SHORT:
        /* Legal, common in reality, and exactly what a parser assuming full
         * reads gets wrong. Deliberately silent -- it happens often enough
         * that logging it would bury everything else. */
        if (len > 1)
            len = 1 + (size_t)(chaos_rand() % (unsigned)(len / 2 + 1));
        break;

    case CHAOS_NONE:
    default:
        break;
    }

    for (;;) {
        if (c->tls) {
            int rc = mbedtls_ssl_read(&c->ssl, buf, len);
            if (rc > 0)
                return rc;
            if (rc == 0 || rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
                return CONN_EOF;
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                conn_set_err(c, "ssl_read", rc);
                return CONN_ERR;
            }
            int ready = wait_ready(c->fd, rc == MBEDTLS_ERR_SSL_WANT_WRITE, timeout_ms);
            if (ready == 0)
                return CONN_TIMEOUT;
            if (ready < 0) {
                conn_set_err(c, "read poll", 0);
                return CONN_ERR;
            }
        } else {
            ssize_t n = recv(c->fd, buf, len, 0);
            if (n > 0)
                return n;
            if (n == 0)
                return CONN_EOF;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                conn_set_err(c, "recv", 0);
                return CONN_ERR;
            }
            int ready = wait_ready(c->fd, false, timeout_ms);
            if (ready == 0)
                return CONN_TIMEOUT;
            if (ready < 0) {
                conn_set_err(c, "read poll", 0);
                return CONN_ERR;
            }
        }
    }
}

ssize_t conn_write(conn_t *c, const void *buf, size_t len, int timeout_ms)
{
    if (!c)
        return CONN_ERR;

    /* A memory conn swallows writes: the tests and --fake-stream are replaying
     * a canned response, so the request that would have produced it is moot. */
    if (c->kind == CONN_KIND_MEMORY)
        return (ssize_t)len;

    if (c->fd < 0)
        return CONN_ERR;
    if (len == 0)
        return 0;

    for (;;) {
        if (c->tls) {
            int rc = mbedtls_ssl_write(&c->ssl, buf, len);
            if (rc > 0)
                return rc;
            if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
                conn_set_err(c, "ssl_write", rc);
                return CONN_ERR;
            }
            int ready = wait_ready(c->fd, rc != MBEDTLS_ERR_SSL_WANT_READ, timeout_ms);
            if (ready == 0)
                return CONN_TIMEOUT;
            if (ready < 0) {
                conn_set_err(c, "write poll", 0);
                return CONN_ERR;
            }
        } else {
            ssize_t n = send(c->fd, buf, len, MSG_NOSIGNAL);
            if (n > 0)
                return n;
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                conn_set_err(c, "send", 0);
                return CONN_ERR;
            }
            int ready = wait_ready(c->fd, true, timeout_ms);
            if (ready == 0)
                return CONN_TIMEOUT;
            if (ready < 0) {
                conn_set_err(c, "write poll", 0);
                return CONN_ERR;
            }
        }
    }
}

int conn_write_all(conn_t *c, const void *buf, size_t len, int timeout_ms)
{
    const unsigned char *p = buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = conn_write(c, p + sent, len - sent, timeout_ms);
        if (n < 0)
            return (int)n;
        sent += (size_t)n;
    }
    return 0;
}

void conn_close(conn_t *c)
{
    if (!c)
        return;

    if (c->kind == CONN_KIND_MEMORY) {
        free(c->mem);
        free(c);
        return;
    }

    if (c->ssl_ready) {
        /* Best-effort close_notify. We do not retry on WANT_* -- if the peer is
         * gone or the socket is full, closing the fd is a perfectly acceptable
         * outcome and blocking a teardown path on a dead network is not. */
        if (c->fd >= 0)
            mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        c->ssl_ready = false;
    }
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    free(c);
}
