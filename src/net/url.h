#ifndef HYPR_URL_H
#define HYPR_URL_H

#include <stdbool.h>

#define URL_HOST_MAX 256
#define URL_PATH_MAX 512

typedef struct {
    char host[URL_HOST_MAX];
    char path[URL_PATH_MAX];  /* always begins with '/', never empty */
    int  port;
    bool tls;
} url_t;

/* Parses http/https/ws/wss URLs. ws maps to !tls, wss to tls, so the rest of
 * the stack never has to care which scheme family it came from -- a WebSocket
 * connection is just an HTTP connection that gets upgraded.
 *
 * Default ports: 80 for http/ws, 443 for https/wss. An explicit :port wins.
 * Query strings are kept as part of path; fragments are stripped (they are
 * client-side only and must never be sent to the server).
 *
 * Returns false on a malformed or unsupported URL. */
bool url_parse(const char *input, url_t *out);

#endif
