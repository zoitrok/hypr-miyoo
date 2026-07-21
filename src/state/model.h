#ifndef HYPR_MODEL_H
#define HYPR_MODEL_H

#include <stdbool.h>
#include <stdint.h>

/* The application's view of playback state.
 *
 * Everything is fixed-size and POD on purpose. The backend sends a ~33KB
 * snapshot on connect and deltas thereafter; parsing those into a tree of
 * allocations and holding it would put thousands of small mallocs through a
 * long-lived heap on a 128MB device, and fragmentation over a multi-hour
 * listening session is a real risk. Instead cJSON output is projected into
 * these structs immediately and the parse tree is freed before the state lock
 * is released.
 *
 * Being POD also makes the renderer's job trivial: it takes one memcpy
 * snapshot under the lock and then draws from it without holding anything. */

#define SONG_TITLE_MAX     128
#define SONG_ARTIST_MAX    128
#define SONG_PLATFORM_MAX   40
#define SONG_BRIEF_MAX      16
#define SONG_USER_MAX       48
#define SONG_TAGS_MAX      128

/* The UI shows "previous" and the next song or two. The server sends 20 of
 * each; keeping 8 is more than the screen can use and bounds the struct at a
 * few KB so snapshotting it is cheap. */
#define MAX_QUEUE    8
#define MAX_HISTORY  8

typedef struct {
    bool valid;

    int  id;
    char title[SONG_TITLE_MAX];
    char artist[SONG_ARTIST_MAX];
    char platform[SONG_PLATFORM_MAX];
    char brief[SONG_BRIEF_MAX];       /* platform_brief: "IT" vs "PC-IT" */
    char requested_by[SONG_USER_MAX];
    char tags[SONG_TAGS_MAX];         /* flattened to "2017, Revision" */

    /* Negative means the song has no votes yet, which is different from a
     * rating of zero and must display differently. */
    double avg_vote;
    int    user_vote;                 /* 0 = not voted */

    /* Epoch SECONDS on the server's clock -- note the units differ from the
     * pong timestamp, which is milliseconds. Never compare these directly to
     * the local wall clock; the device has no RTC. See state/clock.h. */
    int64_t start_time;
    int     length;                   /* broadcast length in seconds */
} song_t;

typedef struct {
    int icecast;
    int hls;
    int total;
} listeners_t;

#define ONELINER_MSG_MAX     192
#define ONELINER_AUTHOR_MAX   40
#define MAX_ONELINER           8

/* A chat line. The server sends 20, newest first; we keep the newest few
 * because that is all a 640x480 screen can show. */
typedef struct {
    bool valid;
    int  id;

    char author[ONELINER_AUTHOR_MAX];
    char country[4];              /* ISO 2-letter, e.g. "FI" */
    char message[ONELINER_MSG_MAX];

    /* Epoch seconds, fractional. A third unit in this protocol: song
     * start_time is integer seconds and pong timestamps are milliseconds. */
    double timestamp;

    /* The song that was playing when the line was posted. Often the most
     * interesting part of an old message, and free to carry. */
    char song_title[SONG_TITLE_MAX];
    char song_artist[SONG_ARTIST_MAX];
} oneliner_t;

typedef struct {
    song_t now_playing;

    song_t queue[MAX_QUEUE];
    int    queue_len;

    song_t history[MAX_HISTORY];
    int    history_len;

    /* Newest first, matching the order the server sends. */
    oneliner_t oneliner[MAX_ONELINER];
    int        oneliner_len;

    listeners_t listeners;
    int  active_users;
    int  lurkers;

    int  backend_version;

    /* mono_ms() at which the current now_playing arrived. Song progress is
     * derived from this plus the server clock offset rather than from the
     * local wall clock, which on this device is arbitrary until NTP runs. */
    uint64_t now_playing_received_ms;

    /* True once a full snapshot has been merged. Until then the UI should say
     * "connecting" rather than render an empty song. */
    bool have_snapshot;
} playback_t;

#endif
