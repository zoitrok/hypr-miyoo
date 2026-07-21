#include "ui/scene.h"
#include "ui/theme.h"

#include <math.h>
#include <stdlib.h>

/* Sun geometry. It sits just above the horizon and is clipped by it, so the
 * disc reads as setting rather than floating. */
#define SUN_R      112
#define SUN_CY     (HORIZON_Y - 18)

/* Grid. GRID_ROWS is how many receding horizontal lines exist at once; beyond
 * about 18 they converge into the horizon haze and cost pixels for nothing. */
#define GRID_ROWS  18
#define GRID_COLS  9      /* rays each side of centre */
#define GRID_SPREAD 210   /* horizontal spacing of the rays at the bottom edge */

/* Deterministic star field: a fixed seed means the sky is identical every run,
 * which matters because the background is rasterised once and never redrawn --
 * a wandering star field would be a bug you could not see until it flickered. */
static unsigned star_rand(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fff;
}

static void draw_stars(gfx_t *bg)
{
    unsigned seed = 0x5eed1e;

    for (int i = 0; i < 150; i++) {
        int x = (int)(star_rand(&seed) % (unsigned)bg->w);
        int y = (int)(star_rand(&seed) % (unsigned)(HORIZON_Y - 40));
        unsigned bright = star_rand(&seed) % 100;

        /* Stars fade out toward the horizon, where the sky is brightest and a
         * star would read as a stuck pixel rather than a star. */
        int fade = 255 - (y * 255 / (HORIZON_Y - 40));
        int a = (int)(bright * fade / 100);
        if (a < 20)
            continue;

        gfx_blend(bg, x, y, gfx_rgb(C_HORIZON), (uint8_t)(a > 255 ? 255 : a));
    }
}

static void draw_sun(gfx_t *bg)
{
    for (int y = SUN_CY - SUN_R; y <= SUN_CY + SUN_R; y++) {
        if (y < 0 || y >= HORIZON_Y)
            continue;   /* clipped by the horizon */

        int dy = y - SUN_CY;
        double half = sqrt((double)(SUN_R * SUN_R - dy * dy));
        if (half <= 0)
            continue;

        /* Position down the disc, 0 at the crown to 1 at the base. */
        double band = (double)(y - (SUN_CY - SUN_R)) / (2.0 * SUN_R);

        /* The detail that makes it read as 80s rather than as a circle:
         * horizontal cuts through the lower half, widening and spacing out
         * toward the bottom. */
        if (band > 0.45) {
            int period = 4 + (int)((band - 0.45) * 26.0);
            int phase = (y - (SUN_CY - SUN_R)) % period;
            if (phase < period / 2)
                continue;
        }

        uint16_t c = gfx_lerp(C_SUN_TOP, C_SUN_BOT, (int)(band * 255));
        gfx_hline(bg, bg->w / 2 - (int)half, bg->w / 2 + (int)half, y, c);
    }
}

static void draw_sun_glow(gfx_t *bg)
{
    /* A soft bloom above the horizon so the sun sits in the sky rather than on
     * top of it. Cheap: a handful of blended spans, done once. */
    for (int y = HORIZON_Y - 60; y < HORIZON_Y; y++) {
        if (y < 0)
            continue;
        int d = HORIZON_Y - y;
        uint8_t a = (uint8_t)(60 - d);
        if (d >= 60)
            continue;
        gfx_blend_hline(bg, 0, bg->w - 1, y, gfx_rgb(C_SUN_BOT), a / 2);
    }
}

void scene_build_background(gfx_t *bg)
{
    gfx_vgradient(bg, 0, HORIZON_Y - 1, C_SKY_TOP, C_SKY_BOT);
    gfx_vgradient(bg, HORIZON_Y, bg->h - 1, C_GROUND_TOP, C_GROUND_BOT);

    draw_stars(bg);
    draw_sun(bg);
    draw_sun_glow(bg);

    /* The horizon itself: a bright line with a short falloff, which is what
     * sells the ground and sky as separate planes. */
    gfx_hline(bg, 0, bg->w - 1, HORIZON_Y, gfx_rgb(C_HORIZON));
    gfx_blend_hline(bg, 0, bg->w - 1, HORIZON_Y - 1, gfx_rgb(C_HORIZON), 120);
    gfx_blend_hline(bg, 0, bg->w - 1, HORIZON_Y + 1, gfx_rgb(C_HORIZON), 90);
}

void scene_draw_grid(gfx_t *g, double distance)
{
    const int cx = g->w / 2;
    const int depth = g->h - HORIZON_Y;

    /* Only the fractional part matters: the rows are identical modulo one
     * step, so wrapping there makes the scroll seamless and lets `distance`
     * grow forever without precision trouble. */
    double frac = distance - floor(distance);

    /* Receding horizontal lines. y = horizon + depth / z places z=1 at the
     * bottom edge and z -> infinity at the horizon, which is the correct
     * perspective foreshortening and is what makes the spacing accelerate
     * toward the viewer. */
    for (int i = 0; i < GRID_ROWS; i++) {
        double z = (double)i + 1.0 - frac;
        if (z < 0.35)
            continue;

        int y = HORIZON_Y + (int)((double)depth / z);
        if (y >= g->h || y <= HORIZON_Y)
            continue;

        /* Fade with distance, so far rows sink into the haze instead of
         * stopping abruptly at the horizon. */
        int t = (int)(255.0 / z);
        if (t > 255) t = 255;
        uint16_t c = gfx_lerp(C_GRID_FAR, C_GRID, t);

        gfx_hline(g, 0, g->w - 1, y, c);
    }

    /* Rays converging on the vanishing point. These do not move -- in a
     * straight-ahead drive only the cross-lines advance -- so they are plain
     * lines from the horizon to the bottom edge. */
    for (int i = -GRID_COLS; i <= GRID_COLS; i++) {
        int x_bottom = cx + i * GRID_SPREAD;

        /* Perspective: everything converges on (cx, HORIZON_Y). Drawing to the
         * bottom edge rather than clipping keeps the outer rays' slope right. */
        uint16_t c = gfx_lerp(C_GRID_FAR, C_GRID, 200);
        gfx_line(g, cx, HORIZON_Y, x_bottom, g->h - 1, c);
    }
}
