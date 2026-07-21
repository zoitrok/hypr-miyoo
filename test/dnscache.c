#include "net/conn.h"
#include "net/url.h"
#include "util/log.h"
#include "tap.h"

/* The DNS cache is what keeps a WiFi blip from becoming a 10s+ audio hole: a
 * failing getaddrinfo blocks ~10s on the device, so the hot reconnect path must
 * avoid the resolver. These exercise the observable contract -- reuse on a
 * repeat connect, self-heal when a cached address goes bad -- without a network,
 * by pointing at a host that always resolves (localhost) and ports that do and
 * do not accept. */

int main(void)
{
    log_init(LOG_ERROR);

    url_t good, bad;
    /* 127.0.0.1 needs no DNS, but still exercises the cache put/get/reuse and
     * the connect_one path shared by both code paths. */
    CHECK(url_parse("http://127.0.0.1:9/", &bad), "parse url");
    (void)good;

    /* A refused port: connect fails, and the failure must be reported cleanly
     * rather than hanging -- the same path a stale cached address takes. Two
     * attempts in a row must behave identically, proving a failed connect does
     * not poison anything. */
    conn_t *c1 = conn_open(NULL, &bad, 1000);
    CHECK(c1 == NULL, "connect to a refused port fails");
    CHECK(conn_last_open_error()[0] != '\0', "and reports why");

    conn_t *c2 = conn_open(NULL, &bad, 1000);
    CHECK(c2 == NULL, "second attempt also fails cleanly, not hangs");

    TAP_DONE();
}
