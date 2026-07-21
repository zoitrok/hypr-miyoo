#include "state/protocol.h"
#include "util/log.h"
#include "tap.h"

#include <stdio.h>
#include <string.h>

/* The merge semantics are the thing to protect here. Getting them backwards
 * does not fail loudly -- it slowly corrupts state, so the queue or history
 * quietly goes stale while everything still looks like it is working. */

static bool apply(playback_t *pb, const char *json, uint64_t now,
                  proto_result_t *res)
{
    return protocol_apply(pb, json, strlen(json), now, res);
}

static const char SNAPSHOT[] =
    "{\"type\":\"playback_update\","
    " \"now_playing\":{\"id\":59119,\"title\":\"Overhead\","
    "   \"artist_name\":\"Bryface\",\"platform\":\"PC-IT\","
    "   \"platform_brief\":\"IT\",\"avg_vote\":4.5,\"user_vote\":0,"
    "   \"start_time\":1784468956,\"length\":200,\"length_seconds\":204,"
    "   \"requested_by_username\":\"DJ Hypr\","
    "   \"tags\":\"[\\\"2017\\\", \\\"Revision\\\"]\"},"
    " \"queue\":[{\"id\":2,\"title\":\"Next\",\"artist_name\":\"A\",\"length\":100}],"
    " \"history\":[{\"id\":1,\"title\":\"Prev\",\"artist_name\":\"B\",\"length\":90}],"
    " \"listeners\":{\"icecast\":3,\"hls\":2,\"total\":5},"
    " \"active_users\":[{\"username\":\"x\"},{\"username\":\"y\"}],"
    " \"lurkers\":7}";

int main(void)
{
    log_init(LOG_ERROR);

    playback_t pb;
    memset(&pb, 0, sizeof(pb));
    proto_result_t res;

    /* --- snapshot --------------------------------------------------------- */
    CHECK(apply(&pb, SNAPSHOT, 1000, &res), "snapshot parsed");
    CHECK_INT(res.type, PROTO_PLAYBACK_UPDATE);
    CHECK_INT(pb.have_snapshot, 1);
    CHECK_INT(res.now_playing_changed, 1);
    CHECK_INT(pb.now_playing_received_ms, 1000);

    CHECK_STR(pb.now_playing.title, "Overhead");
    CHECK_STR(pb.now_playing.artist, "Bryface");
    CHECK_STR(pb.now_playing.platform, "PC-IT");
    CHECK_STR(pb.now_playing.brief, "IT");
    CHECK_STR(pb.now_playing.requested_by, "DJ Hypr");
    CHECK_INT(pb.now_playing.start_time, 1784468956);
    CHECK_INT(pb.now_playing.id, 59119);

    /* "length" is the broadcast length; "length_seconds" is the raw file. The
     * progress bar must follow what is actually played. */
    CHECK_INT(pb.now_playing.length, 200);

    /* tags arrive as a JSON string containing a JSON array, so they need a
     * second parse pass before they are displayable. */
    CHECK_STR(pb.now_playing.tags, "2017, Revision");

    CHECK_INT(pb.queue_len, 1);
    CHECK_INT(pb.history_len, 1);
    CHECK_STR(pb.queue[0].title, "Next");
    CHECK_STR(pb.history[0].title, "Prev");
    CHECK_INT(pb.listeners.total, 5);
    CHECK_INT(pb.listeners.icecast, 3);
    CHECK_INT(pb.active_users, 2);
    CHECK_INT(pb.lurkers, 7);

    /* --- deltas merge into their own key only ----------------------------- */

    /* queue_update must not disturb now_playing or history. */
    CHECK(apply(&pb, "{\"type\":\"queue_update\",\"queue\":"
                     "[{\"id\":9,\"title\":\"Later\",\"artist_name\":\"C\"}]}",
                2000, &res), "queue_update parsed");
    CHECK_INT(res.type, PROTO_QUEUE_UPDATE);
    CHECK_INT(pb.queue_len, 1);
    CHECK_STR(pb.queue[0].title, "Later");
    CHECK_STR(pb.now_playing.title, "Overhead");
    CHECK_STR(pb.history[0].title, "Prev");
    CHECK_INT(pb.listeners.total, 5);

    /* history_update must not disturb the queue. */
    CHECK(apply(&pb, "{\"type\":\"history_update\",\"history\":"
                     "[{\"id\":8,\"title\":\"Older\",\"artist_name\":\"D\"}]}",
                3000, &res), "history_update parsed");
    CHECK_STR(pb.history[0].title, "Older");
    CHECK_STR(pb.queue[0].title, "Later");
    CHECK_STR(pb.now_playing.title, "Overhead");

    /* now_playing_update must not clear queue or history. */
    CHECK(apply(&pb, "{\"type\":\"now_playing_update\",\"now_playing\":"
                     "{\"id\":77,\"title\":\"New Song\",\"artist_name\":\"E\","
                     "\"start_time\":1784469999,\"length\":150},"
                     "\"listeners\":{\"icecast\":1,\"hls\":1,\"total\":2}}",
                4000, &res), "now_playing_update parsed");
    CHECK_INT(res.now_playing_changed, 1);
    CHECK_INT(pb.now_playing_received_ms, 4000);
    CHECK_STR(pb.now_playing.title, "New Song");
    CHECK_INT(pb.listeners.total, 2);
    CHECK_STR(pb.queue[0].title, "Later");
    CHECK_STR(pb.history[0].title, "Older");

    /* Re-sending the same song must not count as a change, or the progress
     * clock would resync on every redundant update and never advance. */
    CHECK(apply(&pb, "{\"type\":\"now_playing_update\",\"now_playing\":"
                     "{\"id\":77,\"title\":\"New Song\",\"artist_name\":\"E\","
                     "\"start_time\":1784469999,\"length\":150}}",
                5000, &res), "duplicate now_playing parsed");
    CHECK_INT(res.now_playing_changed, 0);
    CHECK_INT(pb.now_playing_received_ms, 4000); /* unchanged */

    /* connections_update touches only the counts. */
    CHECK(apply(&pb, "{\"type\":\"connections_update\","
                     "\"active_users\":[{\"username\":\"a\"}],\"lurkers\":3}",
                6000, &res), "connections_update parsed");
    CHECK_INT(pb.active_users, 1);
    CHECK_INT(pb.lurkers, 3);
    CHECK_STR(pb.now_playing.title, "New Song");

    /* --- snapshot replaces everything -------------------------------------
     * A snapshot with an empty queue must clear the queue, not leave the old
     * one in place. This is the direction that silently goes stale. */
    CHECK(apply(&pb, "{\"type\":\"playback_update\","
                     "\"now_playing\":{\"id\":1,\"title\":\"Only\",\"length\":10},"
                     "\"queue\":[],\"history\":[],"
                     "\"listeners\":{\"total\":0},\"active_users\":[],\"lurkers\":0}",
                7000, &res), "replacing snapshot parsed");
    CHECK_INT(pb.queue_len, 0);
    CHECK_INT(pb.history_len, 0);
    CHECK_STR(pb.now_playing.title, "Only");
    CHECK_INT(pb.active_users, 0);

    /* --- field edge cases -------------------------------------------------- */

    /* A null avg_vote means "no votes yet", which must not read as zero stars. */
    CHECK(apply(&pb, "{\"type\":\"now_playing_update\",\"now_playing\":"
                     "{\"id\":5,\"title\":\"Unrated\",\"avg_vote\":null,"
                     "\"length\":60}}", 8000, &res), "null avg_vote parsed");
    CHECK(pb.now_playing.avg_vote < 0, "unrated is negative, not 0");

    CHECK(apply(&pb, "{\"type\":\"now_playing_update\",\"now_playing\":"
                     "{\"id\":6,\"title\":\"Rated\",\"avg_vote\":0,"
                     "\"length\":60}}", 9000, &res), "zero avg_vote parsed");
    CHECK(pb.now_playing.avg_vote == 0, "an actual zero rating stays zero");

    /* A human requester is more interesting than the auto-DJ. */
    CHECK(apply(&pb, "{\"type\":\"now_playing_update\",\"now_playing\":"
                     "{\"id\":7,\"title\":\"X\",\"requested_by_username\":\"DJ Hypr\","
                     "\"human_requester_username\":\"zoi\",\"length\":60}}",
                10000, &res), "human requester parsed");
    CHECK_STR(pb.now_playing.requested_by, "zoi");

    /* Falling back to length_seconds when length is absent. */
    CHECK(apply(&pb, "{\"type\":\"now_playing_update\",\"now_playing\":"
                     "{\"id\":10,\"title\":\"Y\",\"length_seconds\":123}}",
                11000, &res), "length fallback parsed");
    CHECK_INT(pb.now_playing.length, 123);

    /* More songs than we keep must truncate, not overflow. */
    {
        char big[4096];
        int n = snprintf(big, sizeof(big), "{\"type\":\"queue_update\",\"queue\":[");
        for (int i = 0; i < 40; i++)
            n += snprintf(big + n, sizeof(big) - (size_t)n,
                          "%s{\"id\":%d,\"title\":\"T%d\"}", i ? "," : "", i, i);
        snprintf(big + n, sizeof(big) - (size_t)n, "]}");

        CHECK(apply(&pb, big, 12000, &res), "oversized queue parsed");
        CHECK_INT(pb.queue_len, MAX_QUEUE);
        CHECK_STR(pb.queue[0].title, "T0");
    }

    /* --- robustness -------------------------------------------------------- */

    /* An unknown type is not an error: the backend may add messages we do not
     * care about, and refusing them would break on a routine deploy. */
    CHECK(apply(&pb, "{\"type\":\"something_new\",\"data\":1}", 13000, &res),
          "unknown type accepted");
    CHECK_INT(res.type, PROTO_UNKNOWN);
    CHECK_STR(pb.now_playing.title, "Y"); /* untouched */

    CHECK(!apply(&pb, "{not json", 14000, &res), "malformed JSON rejected");
    CHECK(!apply(&pb, "{\"no\":\"type\"}", 14000, &res), "missing type rejected");

    /* Metadata messages. */
    CHECK(apply(&pb, "{\"type\":\"backend_version\",\"version\":7}", 15000, &res),
          "backend_version parsed");
    CHECK_INT(res.backend_version, 7);
    CHECK_INT(pb.backend_version, 7);

    CHECK(apply(&pb, "{\"type\":\"pong\",\"timestamp\":1784469127362}", 16000, &res),
          "pong parsed");
    CHECK_INT(res.type, PROTO_PONG);
    CHECK_INT(res.pong_timestamp_ms, 1784469127362LL);

    /* --- oneliner (chat) ---------------------------------------------------- */
    {
        playback_t p;
        memset(&p, 0, sizeof(p));

        static const char WITH_CHAT[] =
            "{\"type\":\"playback_update\",\"now_playing\":{\"id\":1},"
            " \"oneliner\":["
            "  {\"id\":51508,\"timestamp\":1784464409.816,"
            "   \"message\":\":banana-dance:\",\"author\":\"zoi\",\"country\":\"FI\","
            "   \"song\":{\"song\":{\"id\":37929,\"title\":\"Dupa\",\"artist\":\"505\"}}},"
            "  {\"id\":51507,\"timestamp\":1784463162.345,\"message\":\"o/\","
            "   \"author\":\"HaCKa\",\"country\":\"BR\"}"
            " ]}";

        CHECK(apply(&p, WITH_CHAT, 1000, &res), "snapshot with chat parsed");
        CHECK_INT(p.oneliner_len, 2);

        /* Newest first, as the server sends them. */
        CHECK_INT(p.oneliner[0].id, 51508);
        CHECK_STR(p.oneliner[0].author, "zoi");
        CHECK_STR(p.oneliner[0].country, "FI");
        CHECK_STR(p.oneliner[0].message, ":banana-dance:");
        CHECK(p.oneliner[0].timestamp > 1784464409.0 &&
              p.oneliner[0].timestamp < 1784464410.0,
              "fractional epoch-seconds timestamp preserved");

        /* The associated song is nested twice, and uses "artist" rather than
         * the "artist_name" a song object carries. */
        CHECK_STR(p.oneliner[0].song_title, "Dupa");
        CHECK_STR(p.oneliner[0].song_artist, "505");

        /* A line posted with nothing playing has no song at all. */
        CHECK_STR(p.oneliner[1].song_title, "");
        CHECK_STR(p.oneliner[1].message, "o/");

        /* oneliner_update merges into its own key only. */
        CHECK(apply(&p, "{\"type\":\"oneliner_update\",\"oneliner\":"
                        "[{\"id\":9,\"message\":\"new\",\"author\":\"a\"}],"
                        "\"dj_hypr_mood\":null}", 2000, &res),
              "oneliner_update parsed");
        CHECK_INT(res.type, PROTO_ONELINER_UPDATE);
        CHECK_INT(p.oneliner_len, 1);
        CHECK_STR(p.oneliner[0].message, "new");
        CHECK_INT(p.now_playing.id, 1); /* untouched */

        /* More lines than we keep must truncate, not overflow. */
        {
            char big[8192];
            int n = snprintf(big, sizeof(big),
                             "{\"type\":\"oneliner_update\",\"oneliner\":[");
            for (int i = 0; i < 20; i++)
                n += snprintf(big + n, sizeof(big) - (size_t)n,
                              "%s{\"id\":%d,\"message\":\"m%d\",\"author\":\"a\"}",
                              i ? "," : "", i, i);
            snprintf(big + n, sizeof(big) - (size_t)n, "]}");
            CHECK(apply(&p, big, 3000, &res), "oversized chat parsed");
            CHECK_INT(p.oneliner_len, MAX_ONELINER);
        }
    }

    /* --- UTF-8 safe truncation ----------------------------------------------
     * Chat lines and demoscene handles carry multi-byte characters, and hitting
     * the field cap is routine rather than exceptional. Cutting mid-sequence
     * would emit invalid UTF-8 that the renderer then has to defend against,
     * so truncation must land on a character boundary. */
    {
        playback_t p;
        memset(&p, 0, sizeof(p));

        /* Build a message of 3-byte characters long enough to overflow the
         * field, so the cut necessarily lands inside a sequence. */
        char msg[1024] = "";
        for (int i = 0; i < 120; i++)
            strcat(msg, "\xe2\x99\xaa"); /* U+266A EIGHTH NOTE */

        char json[2048];
        snprintf(json, sizeof(json),
                 "{\"type\":\"oneliner_update\",\"oneliner\":"
                 "[{\"id\":1,\"author\":\"a\",\"message\":\"%s\"}]}", msg);

        CHECK(apply(&p, json, 1000, &res), "long UTF-8 message parsed");
        CHECK_INT(p.oneliner_len, 1);

        const char *m = p.oneliner[0].message;
        size_t len = strlen(m);
        CHECK(len < ONELINER_MSG_MAX, "message truncated within bounds");
        CHECK(len % 3 == 0, "truncated on a character boundary (len %zu)", len);

        /* Validate the whole string decodes: every lead byte must be followed
         * by exactly the continuation bytes it promises. */
        bool valid = true;
        for (size_t i = 0; i < len; ) {
            unsigned char c = (unsigned char)m[i];
            size_t need = c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 :
                          c >= 0xc0 ? 2 : 0;
            if (need == 0 || i + need > len) { valid = false; break; }
            for (size_t k = 1; k < need; k++)
                if (((unsigned char)m[i + k] & 0xc0) != 0x80) { valid = false; break; }
            if (!valid) break;
            i += need;
        }
        CHECK(valid, "truncated message is still valid UTF-8");
    }

    /* --- tag flattening ---------------------------------------------------- */
    {
        char out[128];

        protocol_flatten_tags("[\"a\", \"b\", \"c\"]", out, sizeof(out));
        CHECK_STR(out, "a, b, c");

        protocol_flatten_tags("[]", out, sizeof(out));
        CHECK_STR(out, "");

        /* Not valid JSON: show the raw text rather than nothing. A weird tag
         * line beats a blank one. */
        protocol_flatten_tags("chiptune", out, sizeof(out));
        CHECK_STR(out, "chiptune");

        /* Must truncate rather than overflow. */
        char small[8];
        protocol_flatten_tags("[\"aaaa\", \"bbbb\", \"cccc\"]", small, sizeof(small));
        CHECK(strlen(small) < sizeof(small), "truncated within bounds");
    }

    TAP_DONE();
}
