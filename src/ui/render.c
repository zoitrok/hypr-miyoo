#include "ui/render.h"
#include "ui/scene.h"
#include "ui/text.h"
#include "ui/theme.h"
#include "state/clock.h"
#include "util/mono.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MARGIN 18

static void fmt_mmss(char *out, size_t n, double seconds)
{
    if (seconds < 0)
        seconds = 0;
    int s = (int)seconds;
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

/* ------------------------------------------------------------------ chrome */

/* The road speeds up with the music: louder passages drive faster. Derived
 * from the spectrum rather than a timer so the motion is tied to what is
 * actually playing. */
static double road_distance(const render_ui_t *ui, uint64_t now_ms)
{
    static double distance;
    static uint64_t last_ms;

    double dt = last_ms ? (double)(now_ms - last_ms) / 1000.0 : 0.0;
    last_ms = now_ms;
    if (dt > 0.25)
        dt = 0.25;   /* a stall must not teleport the road */

    double energy = 0.0;
    if (ui->spectrum && ui->spectrum_bars > 0) {
        /* Low bands only: bass is what a driving scene should respond to. */
        int n = ui->spectrum_bars / 3;
        if (n < 1)
            n = 1;
        for (int i = 0; i < n; i++)
            energy += ui->spectrum[i];
        energy /= n;
    }

    /* A floor so the road never stops entirely, even in silence. */
    double speed = 0.55 + energy * 2.2;
    distance += dt * speed;
    return distance;
}

static void draw_spectrum(gfx_t *g, const render_ui_t *ui)
{
    if (!ui->spectrum || ui->spectrum_bars <= 0)
        return;

    /* Bars stand on the horizon, growing upward into the sky -- the skyline
     * silhouette that reads as a city in the distance. */
    const int base_y = HORIZON_Y - 1;
    const int max_h = 96;
    const int n = ui->spectrum_bars;
    const int gap = 2;
    const int bw = (g->w - MARGIN * 2 - gap * (n - 1)) / n;
    if (bw < 1)
        return;

    int x = MARGIN;
    for (int i = 0; i < n; i++) {
        double v = ui->spectrum[i];
        if (v < 0) v = 0;
        if (v > 1) v = 1;

        int h = (int)(v * max_h);
        for (int row = 0; row < h; row++) {
            int y = base_y - row;
            uint16_t c = gfx_lerp(C_BAR_LOW, C_BAR_HIGH, row * 255 / max_h);
            /* Translucent so the sun and stars show through, which keeps the
             * analyser part of the scene rather than pasted on top. */
            gfx_blend_hline(g, x, x + bw - 1, y, c, 165);
        }
        x += bw + gap;
    }
}

static void draw_progress(gfx_t *g, const song_t *song, double elapsed,
                          int y, int w)
{
    const int x = MARGIN;
    const int h = 6;

    /* An outlined trough: against the grid, an unbordered bar reads as just
     * another horizontal line. */
    gfx_rect(g, x, y, w, h, gfx_rgb(C_PROGRESS_BG));
    gfx_frame(g, x - 1, y - 1, w + 2, h + 2, gfx_rgb(C_TEXT_DIM));

    if (song->length > 0) {
        double frac = elapsed / (double)song->length;
        if (frac < 0) frac = 0;
        if (frac > 1) frac = 1;
        int fill = (int)(frac * w);

        for (int i = 0; i < fill; i++)
            gfx_vline(g, x + i, y, y + h - 1,
                      gfx_lerp(C_BAR_LOW, C_PROGRESS, i * 255 / (w ? w : 1)));

        /* A bright head on the bar, so position is readable at a glance on a
         * 3.5" screen without reading the numbers. */
        if (fill > 0)
            gfx_vline(g, x + fill - 1, y - 2, y + h + 1, gfx_rgb(C_TEXT));
    }

    char left[16], right[16];
    fmt_mmss(left, sizeof(left), elapsed);
    fmt_mmss(right, sizeof(right), (double)song->length);

    int by = y + h + font_small.baseline + 4;
    text_draw(g, &font_small, x, by, left, C_TEXT_DIM);

    char total[24];
    snprintf(total, sizeof(total), "-%s", right);
    int tw = text_width(&font_small, total);
    text_draw(g, &font_small, x + w - tw, by, total, C_TEXT_DIM);
}

static void draw_rating(gfx_t *g, double avg, int x, int baseline)
{
    if (avg < 0) {
        text_draw(g, &font_small, x, baseline, "unrated", C_TEXT_DIM);
        return;
    }

    /* Five pips, partially lit. Cheaper to read at a glance than a number, and
     * it looks more at home in the scene. */
    const int pip = 9, gap = 3;
    for (int i = 0; i < 5; i++) {
        double fill = avg - i;
        if (fill < 0) fill = 0;
        if (fill > 1) fill = 1;

        int px = x + i * (pip + gap);
        int top = baseline - pip;

        gfx_frame(g, px, top, pip, pip, gfx_rgb(C_TEXT_DIM));
        if (fill > 0) {
            int fw = (int)(fill * (pip - 2));
            if (fw > 0)
                gfx_rect(g, px + 1, top + 1, fw, pip - 2, gfx_rgb(C_TEXT_ACCENT));
        }
    }

    char num[16];
    snprintf(num, sizeof(num), "%.1f", avg);
    text_draw(g, &font_small, x + 5 * (pip + gap) + 4, baseline, num, C_TEXT_DIM);
}

/* --------------------------------------------------------------- the pages */

static void page_now_playing(gfx_t *g, const app_snapshot_t *snap,
                             const render_ui_t *ui, uint64_t now_ms)
{
    const song_t *np = &snap->playback.now_playing;
    const int usable = g->w - MARGIN * 2;

    if (!snap->playback.have_snapshot || !np->valid) {
        const char *msg = snap->ws_connected ? "TUNING IN" : "OFFLINE";
        text_draw_centered(g, &font_big, 0, g->w, HORIZON_Y + 90, msg, C_TEXT);

        /* Say *why*, not just that. "OFFLINE" alone means pulling a log file
         * off the SD card to learn it was a clock skew or a DNS failure; the
         * device already knows, so it may as well say. */
        const char *why = (snap->ws_error[0] ? snap->ws_error
                          : (ui->audio_error && ui->audio_error[0] ? ui->audio_error
                                                                   : NULL));
        if (why) {
            gfx_shade(g, 0, HORIZON_Y + 104, g->w, 52, gfx_rgb(C_GROUND_BOT), 190);
            text_draw_ellipsis(g, &font_small, MARGIN, HORIZON_Y + 126, why,
                               C_WARN, usable);
        }
        return;
    }

    /* Title and artist sit in the sky above the horizon, where the background
     * is darkest and the text is most legible. */
    int y = 58;
    text_draw_shadow(g, &font_big, MARGIN, y, np->title[0] ? np->title : "(untitled)",
                     C_TEXT, C_SHADOW, 2, 2);
    if (text_width(&font_big, np->title) > usable) {
        /* Redraw clipped rather than let it run off the edge. */
        gfx_rect(g, 0, y - font_big.baseline - 2, g->w, font_big.line_height + 6,
                 gfx_rgb(C_SKY_TOP));
        text_draw_ellipsis(g, &font_big, MARGIN, y, np->title, C_TEXT, usable);
    }

    y += 34;
    text_draw_ellipsis(g, &font_med, MARGIN, y, np->artist, C_TEXT_ACCENT, usable);

    /* Platform and tags on one line; both are short and neither is essential,
     * so they share the space and truncate together. */
    y += 26;
    char meta[192];
    if (np->tags[0])
        snprintf(meta, sizeof(meta), "%s  ·  %s",
                 np->brief[0] ? np->brief : np->platform, np->tags);
    else
        snprintf(meta, sizeof(meta), "%s",
                 np->brief[0] ? np->brief : np->platform);
    text_draw_ellipsis(g, &font_small, MARGIN, y, meta, C_TEXT_DIM, usable);

    y += 22;
    draw_rating(g, np->avg_vote, MARGIN, y);

    if (np->requested_by[0]) {
        char req[96];
        snprintf(req, sizeof(req), "req %s", np->requested_by);
        int rw = text_width(&font_small, req);
        text_draw(g, &font_small, g->w - MARGIN - rw, y, req, C_TEXT_DIM);
    }

    /* Below the horizon everything competes with the grid, which is bright and
     * high-contrast by design. A scrim over the lower band keeps small type
     * legible without hiding the road the way an opaque panel would. */
    gfx_shade(g, 0, HORIZON_Y + 12, g->w, 136, gfx_rgb(C_GROUND_BOT), 148);

    double elapsed = clock_song_elapsed(&snap->clock, now_ms, np->start_time,
                                        np->length,
                                        snap->playback.now_playing_received_ms);
    draw_progress(g, np, elapsed, HORIZON_Y + 22, usable);

    int ly = HORIZON_Y + 74;
    char line[320];

    if (snap->playback.history_len > 0) {
        const song_t *h = &snap->playback.history[0];
        snprintf(line, sizeof(line), "PREV   %s - %s", h->artist, h->title);
        text_draw_ellipsis(g, &font_small, MARGIN, ly, line, C_TEXT_DIM, usable);
        ly += 22;
    }

    if (snap->playback.queue_len > 0) {
        const song_t *q = &snap->playback.queue[0];
        snprintf(line, sizeof(line), "NEXT   %s - %s", q->artist, q->title);
        text_draw_ellipsis(g, &font_small, MARGIN, ly, line, C_TEXT, usable);
        ly += 22;

        if (snap->playback.queue_len > 1) {
            const song_t *q2 = &snap->playback.queue[1];
            snprintf(line, sizeof(line), "       %s - %s", q2->artist, q2->title);
            text_draw_ellipsis(g, &font_small, MARGIN, ly, line, C_TEXT_DIM, usable);
        }
    } else {
        /* The queue is often empty -- the auto-DJ picks the next song at the
         * last moment -- so this is a normal state, not an error. */
        text_draw(g, &font_small, MARGIN, ly, "NEXT   up to DJ Hypr", C_TEXT_DIM);
    }
}

static void draw_list_page(gfx_t *g, const char *heading, const song_t *songs,
                           int count, int scroll, const char *empty_msg)
{
    const int usable = g->w - MARGIN * 2;

    gfx_shade(g, 0, 16, g->w, g->h - 32, gfx_rgb(C_GROUND_BOT), 180);
    text_draw_gradient(g, &font_med, MARGIN, 40, heading, C_SUN_TOP, C_SUN_BOT);

    if (count == 0) {
        text_draw(g, &font_small, MARGIN, 90, empty_msg, C_TEXT_DIM);
        return;
    }

    int y = 84;
    for (int i = scroll; i < count && y < g->h - 20; i++) {
        const song_t *s = &songs[i];
        char line[320];
        snprintf(line, sizeof(line), "%d.  %s - %s", i + 1, s->artist, s->title);

        text_draw_ellipsis(g, &font_small, MARGIN, y, line,
                           i == scroll ? C_TEXT : C_TEXT_DIM, usable - 60);

        if (s->length > 0) {
            char dur[16];
            fmt_mmss(dur, sizeof(dur), (double)s->length);
            int dw = text_width(&font_small, dur);
            text_draw(g, &font_small, g->w - MARGIN - dw, y, dur, C_TEXT_DIM);
        }
        y += 24;
    }
}

static void page_chat(gfx_t *g, const app_snapshot_t *snap,
                      const render_ui_t *ui, uint64_t now_ms)
{
    const int usable = g->w - MARGIN * 2;

    gfx_shade(g, 0, 16, g->w, g->h - 32, gfx_rgb(C_GROUND_BOT), 180);
    text_draw_gradient(g, &font_med, MARGIN, 40, "ONELINER", C_SUN_TOP, C_SUN_BOT);

    if (snap->playback.oneliner_len == 0) {
        text_draw(g, &font_small, MARGIN, 90, "nothing said yet", C_TEXT_DIM);
        return;
    }

    /* Newest at the top, which is how the oneliner has always read on HYPR --
     * and it is the order the server sends, so this is also the one that needs
     * no reversing. An earlier version flipped it to read downward like a
     * chat window, which was neither the convention nor the wire order. */
    int y = 84;
    for (int i = 0; i < snap->playback.oneliner_len && y < g->h - 20; i++) {
        const oneliner_t *o = &snap->playback.oneliner[i];

        char age[16];
        clock_format_age(clock_age_seconds(&snap->clock, now_ms, o->timestamp),
                         age, sizeof(age));

        char who[96];
        snprintf(who, sizeof(who), "%s%s%s", o->author,
                 o->country[0] ? "/" : "", o->country);

        int x = MARGIN;
        x += text_draw(g, &font_small, x, y, age, C_TEXT_DIM) + 8;
        x += text_draw(g, &font_small, x, y, who, C_TEXT_ACCENT) + 8;
        text_draw_ellipsis(g, &font_small, x, y, o->message, C_TEXT,
                           usable - (x - MARGIN));

        y += 22;
    }
}

/* ------------------------------------------------------------------- debug */

static void draw_debug(gfx_t *g, const app_snapshot_t *snap,
                       const render_ui_t *ui)
{
    char line[160];
    int y = g->h - 46;

    gfx_rect(g, 0, y - 14, g->w, 46, gfx_rgb(C_GROUND_BOT));

    snprintf(line, sizeof(line),
             "%.0f fps  buf %.1fs  ur %llu  rc %u  %dk  %s",
             ui->fps, ui->buffer_seconds,
             (unsigned long long)ui->underruns, ui->reconnects,
             ui->bitrate_kbps, ui->audio_connected ? "audio ok" : "AUDIO DOWN");
    text_draw(g, &font_small, MARGIN, y, line, C_WARN);

    y += 20;
    if (snap->ws_error[0] || (ui->audio_error && ui->audio_error[0])) {
        text_draw_ellipsis(g, &font_small, MARGIN, y,
                           snap->ws_error[0] ? snap->ws_error : ui->audio_error,
                           C_WARN, g->w - MARGIN * 2);
        return;
    }
    snprintf(line, sizeof(line), "ws %s  rtt %llums  off %s  listeners %d",
             snap->ws_connected ? "up" : "DOWN",
             (unsigned long long)snap->clock.last_rtt_ms,
             snap->clock.have_offset ? "synced" : "UNSYNCED",
             snap->playback.listeners.total);
    text_draw(g, &font_small, MARGIN, y, line, C_WARN);
}

/* A persistent wordmark, so a photo of the screen always shows where to tune
 * in, followed by the live listener count. The audience is half the appeal of a
 * demoscene radio, and the number rides in on every snapshot for free.
 *
 * Sits top-left on every page; the status banner keeps the top-right, so the
 * two never collide. */
static void draw_header(gfx_t *g, const app_snapshot_t *snap)
{
    const int y = font_small.baseline + 7;

    int x = MARGIN;
    /* Spelled with the scheme: ".website" is an unfamiliar TLD, and "https://"
     * is the unambiguous signal that this is an address to type, not a label. */
    x += text_draw(g, &font_small, x, y, "https://hypr.website", C_TEXT_ACCENT);

    /* Only once a snapshot has landed: a "0 listeners" flashed during connect
     * would read as "nobody here" rather than "not known yet". */
    if (snap->playback.have_snapshot) {
        int n = snap->playback.listeners.total;
        char who[32];
        snprintf(who, sizeof(who), "   %d listeners", n);
        text_draw(g, &font_small, x, y, who, C_TEXT_DIM);
    }
}

/* Shown whenever audio is not actually flowing, so a silent device always
 * explains itself rather than just being silent. */
static void draw_status_banner(gfx_t *g, const app_snapshot_t *snap,
                               const render_ui_t *ui)
{
    const char *msg = NULL;

    if (!ui->audio_connected)
        msg = "RECONNECTING";
    else if (ui->buffer_seconds < 0.35)
        msg = "BUFFERING";
    else if (!snap->ws_connected)
        msg = "METADATA OFFLINE";

    if (!msg)
        return;

    int w = text_width(&font_small, msg) + 20;
    int x = g->w - MARGIN - w;
    int y = 8;

    gfx_rect(g, x, y, w, 22, gfx_rgb(C_SHADOW));
    gfx_frame(g, x, y, w, 22, gfx_rgb(C_WARN));
    text_draw_centered(g, &font_small, x, w, y + 16, msg, C_WARN);
}

/* -------------------------------------------------------------------- frame */

void render_frame(gfx_t *g, const gfx_t *bg, const app_snapshot_t *snap,
                  const render_ui_t *ui, uint64_t now_mono_ms)
{
    gfx_blit_full(g, bg);

    scene_draw_grid(g, road_distance(ui, now_mono_ms));
    draw_spectrum(g, ui);

    switch (ui->page) {
    case PAGE_QUEUE:
        draw_list_page(g, "UP NEXT", snap->playback.queue,
                       snap->playback.queue_len, ui->scroll,
                       "queue is empty -- DJ Hypr picks the next one");
        break;
    case PAGE_CHAT:
        page_chat(g, snap, ui, now_mono_ms);
        break;
    case PAGE_NOW_PLAYING:
    default:
        page_now_playing(g, snap, ui, now_mono_ms);
        break;
    }

    draw_header(g, snap);
    draw_status_banner(g, snap, ui);

    if (ui->show_debug)
        draw_debug(g, snap, ui);
}
