#ifndef HYPR_GFX_H
#define HYPR_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include <SDL.h>

/* Software drawing onto a 16bpp RGB565 surface.
 *
 * Everything here writes uint16_t directly rather than going through
 * SDL_MapRGB per pixel. The probe measured a full-screen per-pixel MapRGB loop
 * at 16.6ms on device -- an entire frame's budget for one fill. Colours are
 * packed once, up front, and the inner loops are plain stores.
 *
 * RGB565 is assumed rather than handled generically. The device offers it as
 * HWSURFACE|DOUBLEBUF (masks f800/07e0/001f) and it halves memory traffic
 * against the panel's native 32bpp, which matters because rendering here is
 * bandwidth-bound. gfx_init() verifies the assumption instead of trusting it. */

typedef struct {
    SDL_Surface *surface;
    uint16_t *pixels;
    int w, h;
    int stride;      /* in uint16_t units, not bytes */
} gfx_t;

/* Wraps an existing surface. Returns false if it is not 16bpp RGB565. */
bool gfx_attach(gfx_t *g, SDL_Surface *s);

/* Packs an 0xRRGGBB constant (see theme.h) into RGB565. */
uint16_t gfx_rgb(uint32_t rgb888);

/* Interpolates between two 0xRRGGBB colours in 8-bit space, then packs.
 * Blending before packing avoids the banding that interpolating packed
 * RGB565 would produce. t is 0..255. */
uint16_t gfx_lerp(uint32_t a, uint32_t b, int t);

void gfx_clear(gfx_t *g, uint16_t c);

/* Clipped primitives. All coordinates may lie outside the surface. */
void gfx_hline(gfx_t *g, int x0, int x1, int y, uint16_t c);
void gfx_vline(gfx_t *g, int x, int y0, int y1, uint16_t c);
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c);
void gfx_rect(gfx_t *g, int x, int y, int w, int h, uint16_t c);
void gfx_frame(gfx_t *g, int x, int y, int w, int h, uint16_t c);

/* Vertical gradient between two 0xRRGGBB colours, inclusive of both rows. */
void gfx_vgradient(gfx_t *g, int y0, int y1, uint32_t top, uint32_t bottom);

/* Blends c over the pixel at (x,y) with coverage a (0..255). This is the
 * primitive text rendering is built on -- glyphs are 8-bit coverage masks. */
void gfx_blend(gfx_t *g, int x, int y, uint16_t c, uint8_t a);

/* Blends a horizontal run at uniform coverage; cheaper than per-pixel calls. */
void gfx_blend_hline(gfx_t *g, int x0, int x1, int y, uint16_t c, uint8_t a);

/* A translucent panel. Used to hold text away from the grid: the scene is busy
 * by design, and a thin scrim keeps small type legible over it without hiding
 * the artwork the way an opaque box would. */
void gfx_shade(gfx_t *g, int x, int y, int w, int h, uint16_t c, uint8_t a);

/* Copies src over the whole of g. Both must be the same size and format;
 * used to stamp the pre-rendered background at the start of each frame. */
void gfx_blit_full(gfx_t *dst, const gfx_t *src);

/* Copies our RGB565 canvas onto the display surface, converting to the screen's
 * format and optionally rotating 180 degrees.
 *
 * This replaces SDL_BlitSurface for the final present. The Miyoo's panel is
 * mounted upside down and the fbcon driver does not compensate, so a rotation
 * is needed somewhere; folding it into the format conversion keeps it to the
 * single full-screen pass we were already paying for, rather than adding a
 * separate rotate pass on top.
 *
 * Handles 16bpp RGB565 and 32bpp 8-8-8 destinations. Returns false if the
 * screen format is neither, so an unexpected mode fails loudly. */
bool gfx_present(SDL_Surface *screen, const gfx_t *canvas, bool rotate180);

#endif
