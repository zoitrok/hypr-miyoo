#ifndef HYPR_FONT_DATA_H
#define HYPR_FONT_DATA_H

#include <stdint.h>

/* Baked bitmap font atlases. See tools/mkfont.py. */

#define FONT_FIRST 32
#define FONT_LAST  255
#define FONT_COUNT (FONT_LAST - FONT_FIRST + 1)

typedef struct {
    uint16_t x, y;   /* position in the atlas */
    uint8_t  w, h;   /* size of the tight box (0 for whitespace) */
    int8_t   bx;     /* left side bearing */
    int8_t   by;     /* top edge relative to the baseline; negative is above */
    uint8_t  adv;    /* pen advance */
} glyph_t;

typedef struct {
    const unsigned char *pixels;   /* atlas_w * atlas_h, 8-bit coverage */
    int atlas_w, atlas_h;
    int line_height;
    int baseline;
    const glyph_t *glyphs;         /* FONT_COUNT entries, from FONT_FIRST */
} font_t;

extern const font_t font_big;    /* song title */
extern const font_t font_med;    /* artist, labels */
extern const font_t font_small;  /* chat, details */

#endif
