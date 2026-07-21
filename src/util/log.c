#include "util/log.h"
#include "util/mono.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static log_level_t g_level = LOG_INFO;
static uint64_t g_start_ms;

static const char *const LEVEL_NAME[] = { "ERROR", "WARN ", "INFO ", "DEBUG" };

void log_init(log_level_t level)
{
    g_level = level;
    g_start_ms = mono_ms();
    /* The device log is a file, so it would be block-buffered by default and
     * would lose everything buffered if we crash. Line buffering costs nothing
     * at our log rates and means the tail of the log is always the truth. */
    setvbuf(stderr, NULL, _IOLBF, 0);
}

void log_set_level(log_level_t level) { g_level = level; }
log_level_t log_get_level(void) { return g_level; }

log_level_t log_level_from_name(const char *name)
{
    if (!name)
        return LOG_INFO;
    if (strcasecmp(name, "error") == 0) return LOG_ERROR;
    if (strcasecmp(name, "warn") == 0)  return LOG_WARN;
    if (strcasecmp(name, "info") == 0)  return LOG_INFO;
    if (strcasecmp(name, "debug") == 0) return LOG_DEBUG;
    return LOG_INFO;
}

void log_write(log_level_t level, const char *tag, const char *fmt, ...)
{
    if (level > g_level)
        return;

    uint64_t t = mono_ms() - g_start_ms;
    fprintf(stderr, "[%4llu.%03llu] %s %-8s ",
            (unsigned long long)(t / 1000), (unsigned long long)(t % 1000),
            LEVEL_NAME[level], tag ? tag : "-");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fputc('\n', stderr);
}
