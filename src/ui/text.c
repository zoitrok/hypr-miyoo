#include "ui/text.h"

#include <string.h>

/* Decodes one UTF-8 sequence, advancing *p. Returns the codepoint, or '?' for
 * malformed input -- we never want a decoder error to stop a song title from
 * drawing. */
static unsigned next_codepoint(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned c = *s;

    if (c < 0x80) {
        *p += 1;
        return c;
    }

    int extra;
    unsigned cp;
    if ((c & 0xe0) == 0xc0)      { extra = 1; cp = c & 0x1f; }
    else if ((c & 0xf0) == 0xe0) { extra = 2; cp = c & 0x0f; }
    else if ((c & 0xf8) == 0xf0) { extra = 3; cp = c & 0x07; }
    else                         { *p += 1; return '?'; }

    for (int i = 1; i <= extra; i++) {
        if ((s[i] & 0xc0) != 0x80) {   /* truncated or invalid */
            *p += 1;
            return '?';
        }
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    *p += extra + 1;
    return cp;
}

/* Folds to the atlas range. Above Latin-1 there is no glyph, and '?' reads
 * better than a tofu box at this size. */
static const glyph_t *glyph_for(const font_t *f, unsigned cp)
{
    if (cp > FONT_LAST || cp < FONT_FIRST)
        cp = '?';
    return &f->glyphs[cp - FONT_FIRST];
}

int text_width(const font_t *f, const char *utf8)
{
    int w = 0;
    const char *p = utf8;
    while (*p)
        w += glyph_for(f, next_codepoint(&p))->adv;
    return w;
}

/* The one place that touches the atlas. row_tint, when non-NULL, supplies a
 * colour per output row so gradients cost no extra pass. */
static int draw_impl(gfx_t *g, const font_t *f, int x, int baseline_y,
                     const char *utf8, uint16_t flat,
                     const uint16_t *row_tint, int tint_y0, int tint_h)
{
    const char *p = utf8;
    int pen = x;

    while (*p) {
        const glyph_t *gl = glyph_for(f, next_codepoint(&p));

        if (gl->w && gl->h) {
            int gx = pen + gl->bx;
            int gy = baseline_y + gl->by;

            for (int row = 0; row < gl->h; row++) {
                int py = gy + row;
                if (py < 0 || py >= g->h)
                    continue;

                const unsigned char *cov =
                    f->pixels + (size_t)(gl->y + row) * f->atlas_w + gl->x;

                uint16_t colour = flat;
                if (row_tint) {
                    int t = py - tint_y0;
                    if (t < 0) t = 0;
                    if (t >= tint_h) t = tint_h - 1;
                    colour = row_tint[t];
                }

                uint16_t *dst = g->pixels + (size_t)py * g->stride;
                for (int col = 0; col < gl->w; col++) {
                    unsigned char a = cov[col];
                    if (!a)
                        continue;
                    int px = gx + col;
                    if ((unsigned)px >= (unsigned)g->w)
                        continue;
                    if (a == 255)
                        dst[px] = colour;
                    else {
                        unsigned dr = (dst[px] >> 11) & 0x1f;
                        unsigned dg = (dst[px] >> 5) & 0x3f;
                        unsigned db = dst[px] & 0x1f;
                        unsigned sr = (colour >> 11) & 0x1f;
                        unsigned sg = (colour >> 5) & 0x3f;
                        unsigned sb = colour & 0x1f;
                        dst[px] = (uint16_t)
                            ((((sr * a + dr * (255 - a)) / 255) << 11) |
                             (((sg * a + dg * (255 - a)) / 255) << 5) |
                             (((sb * a + db * (255 - a)) / 255)));
                    }
                }
            }
        }
        pen += gl->adv;
    }
    return pen - x;
}

int text_draw(gfx_t *g, const font_t *f, int x, int baseline_y,
              const char *utf8, uint32_t rgb)
{
    return draw_impl(g, f, x, baseline_y, utf8, gfx_rgb(rgb), NULL, 0, 0);
}

int text_draw_shadow(gfx_t *g, const font_t *f, int x, int baseline_y,
                     const char *utf8, uint32_t rgb, uint32_t shadow_rgb,
                     int dx, int dy)
{
    draw_impl(g, f, x + dx, baseline_y + dy, utf8, gfx_rgb(shadow_rgb), NULL, 0, 0);
    return draw_impl(g, f, x, baseline_y, utf8, gfx_rgb(rgb), NULL, 0, 0);
}

int text_draw_gradient(gfx_t *g, const font_t *f, int x, int baseline_y,
                       const char *utf8, uint32_t top_rgb, uint32_t bot_rgb)
{
    /* One colour per row of the em box, computed once for the whole string so
     * every glyph shares the same ramp and the gradient reads as continuous
     * across the line rather than restarting per character. */
    int top = baseline_y - f->baseline;
    int h = f->line_height;
    if (h <= 0 || h > 256)
        return text_draw(g, f, x, baseline_y, utf8, top_rgb);

    uint16_t ramp[256];
    for (int i = 0; i < h; i++)
        ramp[i] = gfx_lerp(top_rgb, bot_rgb, i * 255 / (h - 1 ? h - 1 : 1));

    return draw_impl(g, f, x, baseline_y, utf8, ramp[0], ramp, top, h);
}

int text_draw_ellipsis(gfx_t *g, const font_t *f, int x, int baseline_y,
                       const char *utf8, uint32_t rgb, int max_w)
{
    if (text_width(f, utf8) <= max_w)
        return text_draw(g, f, x, baseline_y, utf8, rgb);

    int ell_w = text_width(f, "...");
    int budget = max_w - ell_w;
    if (budget <= 0)
        return 0;

    /* Walk whole codepoints so truncation never lands inside a sequence. */
    const char *p = utf8;
    const char *cut = utf8;
    int w = 0;
    while (*p) {
        const char *before = p;
        int adv = glyph_for(f, next_codepoint(&p))->adv;
        if (w + adv > budget) {
            cut = before;
            break;
        }
        w += adv;
        cut = p;
    }

    char buf[512];
    size_t n = (size_t)(cut - utf8);
    if (n >= sizeof(buf) - 4)
        n = sizeof(buf) - 4;
    memcpy(buf, utf8, n);
    memcpy(buf + n, "...", 4);

    return text_draw(g, f, x, baseline_y, buf, rgb);
}

int text_draw_centered(gfx_t *g, const font_t *f, int x, int w, int baseline_y,
                       const char *utf8, uint32_t rgb)
{
    int tw = text_width(f, utf8);
    return text_draw(g, f, x + (w - tw) / 2, baseline_y, utf8, rgb);
}
