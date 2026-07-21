#include "net/url.h"
#include "tap.h"

int main(void)
{
    url_t u;

    /* The two URLs this app actually uses. */
    CHECK(url_parse("https://hypr.website/hypr.mp3", &u), "parse stream url");
    CHECK_STR(u.host, "hypr.website");
    CHECK_STR(u.path, "/hypr.mp3");
    CHECK_INT(u.port, 443);
    CHECK_INT(u.tls, 1);

    CHECK(url_parse("wss://hypr.website/ws", &u), "parse websocket url");
    CHECK_STR(u.host, "hypr.website");
    CHECK_STR(u.path, "/ws");
    CHECK_INT(u.port, 443);
    CHECK_INT(u.tls, 1);

    /* ws/wss must collapse onto the same tls flag as http/https, so nothing
     * downstream has to special-case the WebSocket scheme family. */
    CHECK(url_parse("ws://localhost:4000/ws", &u), "parse plain ws");
    CHECK_INT(u.tls, 0);
    CHECK_INT(u.port, 4000);
    CHECK_STR(u.host, "localhost");

    CHECK(url_parse("http://example.com", &u), "parse http default port");
    CHECK_INT(u.port, 80);
    CHECK_INT(u.tls, 0);
    CHECK_STR(u.path, "/");

    CHECK(url_parse("HTTPS://Example.COM/Path", &u), "scheme is case-insensitive");
    CHECK_INT(u.tls, 1);
    CHECK_STR(u.path, "/Path");

    /* Query strings belong to the path; fragments must never be sent. */
    CHECK(url_parse("https://h/a?b=c&d=e", &u), "query retained");
    CHECK_STR(u.path, "/a?b=c&d=e");

    CHECK(url_parse("https://h/a#frag", &u), "fragment stripped");
    CHECK_STR(u.path, "/a");

    CHECK(url_parse("https://h?q=1", &u), "query with empty path");
    CHECK_STR(u.path, "?q=1");

    CHECK(url_parse("https://h:8443/", &u), "explicit port overrides default");
    CHECK_INT(u.port, 8443);

    /* Rejections. Each of these would otherwise fail somewhere less obvious. */
    CHECK(!url_parse("ftp://h/x", &u), "unsupported scheme rejected");
    CHECK(!url_parse("hypr.website/x", &u), "scheme-less rejected");
    CHECK(!url_parse("https://", &u), "empty host rejected");
    CHECK(!url_parse("https://h:0/", &u), "port 0 rejected");
    CHECK(!url_parse("https://h:99999/", &u), "out-of-range port rejected");
    CHECK(!url_parse("https://h:abc/", &u), "non-numeric port rejected");
    CHECK(!url_parse("https://user:pw@h/", &u), "userinfo rejected, not ignored");
    CHECK(!url_parse(NULL, &u), "NULL rejected");

    TAP_DONE();
}
