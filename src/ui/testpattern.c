#include "ui/testpattern.h"
#include "ui/text.h"
#include "ui/theme.h"

#include <stdio.h>

/* Primaries and secondaries at full and half intensity. If bytes-per-pixel or
 * the channel masks are wrong these come out as the wrong colours, or the set
 * repeats across the screen. */
static const uint32_t BARS[] = {
    0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
    0xff00ff, 0xff0000, 0x0000ff, 0x000000,
    0x808080, 0x804000, 0x004080, 0x408000,
};
#define NBARS ((int)(sizeof(BARS) / sizeof(BARS[0])))

static void corner_box(gfx_t *g, int x, int y, const char *label, uint32_t c)
{
    gfx_rect(g, x, y, 58, 34, gfx_rgb(c));
    gfx_frame(g, x, y, 58, 34, gfx_rgb(0xffffff));
    text_draw(g, &font_med, x + 8, y + 26, label, 0x000000);
}

void testpattern_draw(gfx_t *g, int frame)
{
    const int w = g->w, h = g->h;

    /* Colour bars across the full width. */
    int bar_w = w / NBARS;
    for (int i = 0; i < NBARS; i++)
        gfx_rect(g, i * bar_w, 0, (i == NBARS - 1) ? w - i * bar_w : bar_w,
                 h, gfx_rgb(BARS[i]));

    /* Single-pixel grid every 32px. Any scaling or stride error turns these
     * from crisp lines into moire or diagonals. */
    for (int x = 0; x < w; x += 32)
        gfx_vline(g, x, 0, h - 1, gfx_rgb(0x303030));
    for (int y = 0; y < h; y += 32)
        gfx_hline(g, 0, w - 1, y, gfx_rgb(0x303030));

    /* Diagonals corner to corner. A stride mismatch is unmistakable here: one
     * clean line becomes a fan of many. */
    gfx_line(g, 0, 0, w - 1, h - 1, gfx_rgb(0xffffff));
    gfx_line(g, w - 1, 0, 0, h - 1, gfx_rgb(0xffffff));

    /* Centre cross and a box, to check centring and aspect. */
    gfx_hline(g, w / 2 - 40, w / 2 + 40, h / 2, gfx_rgb(0xffffff));
    gfx_vline(g, w / 2, h / 2 - 40, h / 2 + 40, gfx_rgb(0xffffff));
    gfx_frame(g, w / 2 - 80, h / 2 - 60, 160, 120, gfx_rgb(0xffff00));

    /* A 1px border. If any edge is missing, the panel is cropping. */
    gfx_frame(g, 0, 0, w, h, gfx_rgb(0xff0000));
    gfx_frame(g, 1, 1, w - 2, h - 2, gfx_rgb(0x00ff00));

    /* Numbered corners: rotation and mirroring show up instantly. */
    corner_box(g, 6, 6, "1", 0xffffff);
    corner_box(g, w - 64, 6, "2", 0x00ffff);
    corner_box(g, 6, h - 40, "3", 0xffff00);
    corner_box(g, w - 64, h - 40, "4", 0xff00ff);

    /* Dimensions and a moving marker, so a photo also proves the app is
     * actually running rather than showing one stuck frame. */
    char info[96];
    snprintf(info, sizeof(info), "%dx%d  canvas RGB565  frame %d", w, h, frame);
    text_draw_shadow(g, &font_med, w / 2 - text_width(&font_med, info) / 2,
                     h / 2 - 74, info, 0xffffff, 0x000000, 2, 2);

    int mx = 20 + (frame * 3) % (w - 40);
    gfx_rect(g, mx, h / 2 + 70, 16, 16, gfx_rgb(0xffffff));
}
