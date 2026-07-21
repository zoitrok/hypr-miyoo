#ifndef HYPR_SCENE_H
#define HYPR_SCENE_H

#include "ui/gfx.h"

/* The vaporwave backdrop: gradient sky, scanline sun, stars, and a perspective
 * grid scrolling toward the viewer.
 *
 * Split into a static half and a moving half for a reason the probe made
 * concrete. A full-screen per-pixel fill costs ~16.6ms on device -- an entire
 * frame at 60Hz -- so the sky, sun and stars are rasterised **once** into a
 * background surface at startup. Each frame then copies that (a fast path,
 * ~1ms at 16bpp) and draws only what actually moves: about 40 grid lines. */

/* Rasterises the static backdrop. Call once, after gfx_attach. */
void scene_build_background(gfx_t *bg);

/* Draws the moving grid over an already-stamped background.
 *
 * distance is in arbitrary "road" units and should increase monotonically with
 * time; only its fractional part matters for the horizontal lines, so it can
 * grow without bound. Passing a value derived from audio energy is what makes
 * the road speed up with the music. */
void scene_draw_grid(gfx_t *g, double distance);

#endif
