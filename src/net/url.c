#include "net/url.h"

#include <stdlib.h>
#include <string.h>

static bool scheme_matches(const char *s, const char *scheme, size_t len)
{
    return strncasecmp(s, scheme, len) == 0 && s[len] == ':'
        && s[len + 1] == '/' && s[len + 2] == '/';
}

bool url_parse(const char *input, url_t *out)
{
    if (!input || !out)
        return false;

    memset(out, 0, sizeof(*out));

    const char *p = input;
    if (scheme_matches(p, "https", 5) || scheme_matches(p, "wss", 3)) {
        out->tls = true;
        out->port = 443;
        p += (*p == 'h' || *p == 'H') ? 8 : 6;
    } else if (scheme_matches(p, "http", 4) || scheme_matches(p, "ws", 2)) {
        out->tls = false;
        out->port = 80;
        p += (*p == 'h' || *p == 'H') ? 7 : 5;
    } else {
        return false;
    }

    /* We deliberately do not support userinfo (user:pass@host). Nothing in this
     * app needs it, and silently ignoring it would be worse than rejecting. */
    const char *authority_end = p + strcspn(p, "/?#");
    if (memchr(p, '@', (size_t)(authority_end - p)) != NULL)
        return false;

    const char *host_end = authority_end;
    const char *colon = memchr(p, ':', (size_t)(authority_end - p));
    if (colon) {
        char *endp = NULL;
        long port = strtol(colon + 1, &endp, 10);
        if (endp != authority_end || port <= 0 || port > 65535)
            return false;
        out->port = (int)port;
        host_end = colon;
    }

    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0 || host_len >= sizeof(out->host))
        return false;
    memcpy(out->host, p, host_len);
    out->host[host_len] = '\0';

    /* Strip any fragment: it is purely client-side and must not be sent. */
    const char *rest = authority_end;
    size_t rest_len = strcspn(rest, "#");

    if (rest_len == 0) {
        out->path[0] = '/';
        out->path[1] = '\0';
    } else {
        if (rest_len >= sizeof(out->path))
            return false;
        memcpy(out->path, rest, rest_len);
        out->path[rest_len] = '\0';
    }

    return true;
}
