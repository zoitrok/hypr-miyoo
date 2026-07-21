#ifndef HYPR_TEXT_H
#define HYPR_TEXT_H

#include <stdint.h>

#include "ui/font_data.h"
#include "ui/gfx.h"

/* Text drawing from the baked coverage atlases.
 *
 * Input is UTF-8 (song titles and demoscene handles carry accents). Codepoints
 * are folded to Latin-1, which the atlas covers; anything above U+00FF becomes
 * '?' rather than a missing-glyph box, because on a 640x480 screen an
 * occasional '?' is less distracting than a run of tofu. */

/* Advance width of a UTF-8 string, in pixels. */
int text_width(const font_t *f, const char *utf8);

/* Draws at a baseline origin. Returns the x advance. */
int text_draw(gfx_t *g, const font_t *f, int x, int baseline_y,
              const char *utf8, uint32_t rgb);

/* Draws with a hard offset shadow underneath -- the cheap chrome look, and it
 * keeps titles legible over the busy grid. */
int text_draw_shadow(gfx_t *g, const font_t *f, int x, int baseline_y,
                     const char *utf8, uint32_t rgb, uint32_t shadow_rgb,
                     int dx, int dy);

/* Draws with a vertical gradient across the glyph height, top to bottom. The
 * 80s chrome/sunset treatment for headings. */
int text_draw_gradient(gfx_t *g, const font_t *f, int x, int baseline_y,
                       const char *utf8, uint32_t top_rgb, uint32_t bot_rgb);

/* Like text_draw, but truncates with an ellipsis to fit max_w. Song titles
 * routinely exceed the screen -- "Red Sector Theme (aka "Shockwave", ...)". */
int text_draw_ellipsis(gfx_t *g, const font_t *f, int x, int baseline_y,
                       const char *utf8, uint32_t rgb, int max_w);

/* Centres within [x, x+w). */
int text_draw_centered(gfx_t *g, const font_t *f, int x, int w, int baseline_y,
                       const char *utf8, uint32_t rgb);

#endif
