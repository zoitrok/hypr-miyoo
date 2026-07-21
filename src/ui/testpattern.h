#ifndef HYPR_TESTPATTERN_H
#define HYPR_TESTPATTERN_H

#include "ui/gfx.h"

/* An unmistakable calibration image, for diagnosing display problems from a
 * photograph of the screen.
 *
 * This exists because the Phase 0 probe compared SDL_PixelFormat structs and
 * timed blits, and concluded the display path was fine -- while the real
 * output was garbled. Matching format fields do not prove that pixels land
 * where you think. Geometry does.
 *
 * Every element answers a specific question:
 *
 *   numbered corner boxes    is the image rotated, mirrored, or offset?
 *   1px border               are the edges reachable, or is it over/underscanned?
 *   diagonal corner to corner a stride mismatch turns one line into many
 *   vertical colour bars     is the pixel format right? bars come out wrong
 *                            colours or repeat if bytes-per-pixel is wrong
 *   single-pixel grid        aliasing and scaling show up immediately
 *   centre cross             is the centre where it should be?
 */
void testpattern_draw(gfx_t *g, int frame);

#endif
