#include "util/cfg.h"
#include "util/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "cfg"

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
    return s;
}

int cfg_load_args(const char *path, char **out, int max)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;   /* absent is normal */

    int n = 0;
    char line[512];

    while (n + 2 <= max && fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (*s == '\0' || *s == '#')
            continue;

        /* Split on the first space only, so a value containing spaces (or a
         * URL with odd characters) survives without quoting rules. */
        char *sp = strchr(s, ' ');
        if (sp) {
            *sp = '\0';
            char *val = trim(sp + 1);
            out[n++] = strdup(s);
            if (*val)
                out[n++] = strdup(val);
        } else {
            out[n++] = strdup(s);
        }
    }

    fclose(f);

    if (n > 0)
        LOGI(TAG, "loaded %d argument(s) from %s", n, path);
    return n;
}
