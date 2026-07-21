#ifndef HYPR_LOG_H
#define HYPR_LOG_H

typedef enum {
    LOG_ERROR = 0,
    LOG_WARN,
    LOG_INFO,
    LOG_DEBUG
} log_level_t;

/* Logging is written to stderr, which launch.sh redirects to log.txt on the
 * device. Timestamps are monotonic-since-start, deliberately not wall clock:
 * on a device with no RTC a wall-clock timestamp is actively misleading. */
void log_init(log_level_t level);
void log_set_level(log_level_t level);
log_level_t log_get_level(void);

/* Returns LOG_INFO for an unrecognised name. */
log_level_t log_level_from_name(const char *name);

void log_write(log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define LOGE(tag, ...) log_write(LOG_ERROR, (tag), __VA_ARGS__)
#define LOGW(tag, ...) log_write(LOG_WARN, (tag), __VA_ARGS__)
#define LOGI(tag, ...) log_write(LOG_INFO, (tag), __VA_ARGS__)
#define LOGD(tag, ...) log_write(LOG_DEBUG, (tag), __VA_ARGS__)

#endif
