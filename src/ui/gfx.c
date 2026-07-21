#include "ui/gfx.h"
#include "util/log.h"

#include <stdlib.h>
#include <string.h>

#define TAG "gfx"

bool gfx_attach(gfx_t *g, SDL_Surface *s)
{
    memset(g, 0, sizeof(*g));
    if (!s)
        return false;

    /* Checked rather than assumed: every inner loop below writes uint16_t and
     * packs colours by hand, so a surface in another format would render
     * garbage silently instead of failing here. */
    if (s->format->BitsPerPixel != 16 ||
        s->format->Rmask != 0xf800 ||
        s->format->Gmask != 0x07e0 ||
        s->format->Bmask != 0x001f) {
        LOGE(TAG, "surface is not RGB565 (%d bpp, masks %08x/%08x/%08x)",
             s->format->BitsPerPixel, (unsigned)s->format->Rmask,
             (unsigned)s->format->Gmask, (unsigned)s->format->Bmask);
        return false;
    }

    g->surface = s;
    g->pixels = (uint16_t *)s->pixels;
    g->w = s->w;
    g->h = s->h;
    g->stride = s->pitch / (int)sizeof(uint16_t);
    return true;
}

uint16_t gfx_rgb(uint32_t rgb)
{
    unsigned r = (rgb >> 16) & 0xff;
    unsigned gg = (rgb >> 8) & 0xff;
    unsigned b = rgb & 0xff;
    return (uint16_t)(((r & 0xf8) << 8) | ((gg & 0xfc) << 3) | (b >> 3));
}

uint16_t gfx_lerp(uint32_t a, uint32_t b, int t)
{
    if (t < 0) t = 0;
    if (t > 255) t = 255;

    int ar = (int)((a >> 16) & 0xff), ag = (int)((a >> 8) & 0xff), ab = (int)(a & 0xff);
    int br = (int)((b >> 16) & 0xff), bg = (int)((b >> 8) & 0xff), bb = (int)(b & 0xff);

    int r = ar + (br - ar) * t / 255;
    int gg = ag + (bg - ag) * t / 255;
    int bl = ab + (bb - ab) * t / 255;

    return (uint16_t)(((r & 0xf8) << 8) | ((gg & 0xfc) << 3) | (bl >> 3));
}

void gfx_clear(gfx_t *g, uint16_t c)
{
    /* memset only works when both bytes match; otherwise fill a row and
     * replicate it, which is still far cheaper than a per-pixel loop. */
    if ((c & 0xff) == (c >> 8)) {
        memset(g->pixels, c & 0xff, (size_t)g->stride * (size_t)g->h * sizeof(uint16_t));
        return;
    }
    for (int x = 0; x < g->w; x++)
        g->pixels[x] = c;
    for (int y = 1; y < g->h; y++)
        memcpy(g->pixels + (size_t)y * g->stride, g->pixels,
               (size_t)g->w * sizeof(uint16_t));
}

void gfx_hline(gfx_t *g, int x0, int x1, int y, uint16_t c)
{
    if (y < 0 || y >= g->h)
        return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= g->w)
        return;
    if (x0 < 0) x0 = 0;
    if (x1 >= g->w) x1 = g->w - 1;

    uint16_t *row = g->pixels + (size_t)y * g->stride;
    for (int x = x0; x <= x1; x++)
        row[x] = c;
}

void gfx_vline(gfx_t *g, int x, int y0, int y1, uint16_t c)
{
    if (x < 0 || x >= g->w)
        return;
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (y1 < 0 || y0 >= g->h)
        return;
    if (y0 < 0) y0 = 0;
    if (y1 >= g->h) y1 = g->h - 1;

    uint16_t *p = g->pixels + (size_t)y0 * g->stride + x;
    for (int y = y0; y <= y1; y++, p += g->stride)
        *p = c;
}

void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c)
{
    if (y0 == y1) { gfx_hline(g, x0, x1, y0, c); return; }
    if (x0 == x1) { gfx_vline(g, x0, y0, y1, c); return; }

    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        if ((unsigned)x0 < (unsigned)g->w && (unsigned)y0 < (unsigned)g->h)
            g->pixels[(size_t)y0 * g->stride + x0] = c;
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_rect(gfx_t *g, int x, int y, int w, int h, uint16_t c)
{
    for (int i = 0; i < h; i++)
        gfx_hline(g, x, x + w - 1, y + i, c);
}

void gfx_frame(gfx_t *g, int x, int y, int w, int h, uint16_t c)
{
    gfx_hline(g, x, x + w - 1, y, c);
    gfx_hline(g, x, x + w - 1, y + h - 1, c);
    gfx_vline(g, x, y, y + h - 1, c);
    gfx_vline(g, x + w - 1, y, y + h - 1, c);
}

void gfx_vgradient(gfx_t *g, int y0, int y1, uint32_t top, uint32_t bottom)
{
    if (y1 < y0)
        return;
    int span = y1 - y0;

    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= g->h)
            continue;
        int t = span > 0 ? (y - y0) * 255 / span : 0;
        gfx_hline(g, 0, g->w - 1, y, gfx_lerp(top, bottom, t));
    }
}

/* Unpacks, mixes and repacks. Only used for text and bar edges -- a few
 * thousand pixels a frame, not a full screen. */
static inline uint16_t blend565(uint16_t dst, uint16_t src, uint8_t a)
{
    unsigned dr = (dst >> 11) & 0x1f, dg = (dst >> 5) & 0x3f, db = dst & 0x1f;
    unsigned sr = (src >> 11) & 0x1f, sg = (src >> 5) & 0x3f, sb = src & 0x1f;

    unsigned r = (sr * a + dr * (255 - a)) / 255;
    unsigned gg = (sg * a + dg * (255 - a)) / 255;
    unsigned b = (sb * a + db * (255 - a)) / 255;

    return (uint16_t)((r << 11) | (gg << 5) | b);
}

void gfx_blend(gfx_t *g, int x, int y, uint16_t c, uint8_t a)
{
    if ((unsigned)x >= (unsigned)g->w || (unsigned)y >= (unsigned)g->h)
        return;
    if (a == 0)
        return;

    uint16_t *p = &g->pixels[(size_t)y * g->stride + x];
    *p = (a == 255) ? c : blend565(*p, c, a);
}

void gfx_blend_hline(gfx_t *g, int x0, int x1, int y, uint16_t c, uint8_t a)
{
    if (y < 0 || y >= g->h || a == 0)
        return;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (x1 < 0 || x0 >= g->w)
        return;
    if (x0 < 0) x0 = 0;
    if (x1 >= g->w) x1 = g->w - 1;

    uint16_t *row = g->pixels + (size_t)y * g->stride;
    if (a == 255) {
        for (int x = x0; x <= x1; x++)
            row[x] = c;
    } else {
        for (int x = x0; x <= x1; x++)
            row[x] = blend565(row[x], c, a);
    }
}

void gfx_shade(gfx_t *g, int x, int y, int w, int h, uint16_t c, uint8_t a)
{
    for (int i = 0; i < h; i++)
        gfx_blend_hline(g, x, x + w - 1, y + i, c, a);
}

void gfx_blit_full(gfx_t *dst, const gfx_t *src)
{
    if (dst->w != src->w || dst->h != src->h)
        return;

    /* Row by row rather than one memcpy: the surfaces may have different
     * pitches even at the same width. */
    if (dst->stride == src->stride) {
        memcpy(dst->pixels, src->pixels,
               (size_t)dst->stride * (size_t)dst->h * sizeof(uint16_t));
        return;
    }
    for (int y = 0; y < dst->h; y++)
        memcpy(dst->pixels + (size_t)y * dst->stride,
               src->pixels + (size_t)y * src->stride,
               (size_t)dst->w * sizeof(uint16_t));
}

/* Note on cost: this writes to the display surface a pixel at a time, which on
 * the device means uncached video memory. The obvious alternative -- rotate
 * into a second system-RAM surface and let SDL_BlitSurface do the conversion --
 * is not actually cheaper, because a 16bpp source and a 32bpp destination take
 * SDL's software converting path anyway (no hardware blit applies across
 * formats). That would be two passes where this is one. If the frame rate on
 * device says otherwise, the fix is to make the canvas 32bpp so the final blit
 * becomes a same-format hardware copy, at the cost of doubling the bandwidth of
 * every drawing operation. Measure before changing. */
bool gfx_present(SDL_Surface *screen, const gfx_t *canvas, bool rotate180)
{
    if (!screen || screen->w != canvas->w || screen->h != canvas->h)
        return false;

    if (SDL_MUSTLOCK(screen) && SDL_LockSurface(screen) != 0)
        return false;

    const int w = canvas->w, h = canvas->h;
    const int bytespp = screen->format->BytesPerPixel;

    /* A 180 degree rotation is just reading the source backwards, so it costs
     * the same pass either way -- no extra buffer and no second traversal. */
    for (int y = 0; y < h; y++) {
        const uint16_t *src = canvas->pixels +
            (size_t)(rotate180 ? (h - 1 - y) : y) * canvas->stride;
        unsigned char *dstrow = (unsigned char *)screen->pixels +
            (size_t)y * screen->pitch;

        if (bytespp == 2) {
            uint16_t *dst = (uint16_t *)dstrow;
            if (rotate180)
                for (int x = 0; x < w; x++)
                    dst[x] = src[w - 1 - x];
            else
                memcpy(dst, src, (size_t)w * sizeof(uint16_t));
        } else if (bytespp == 4) {
            uint32_t *dst = (uint32_t *)dstrow;
            for (int x = 0; x < w; x++) {
                uint16_t p = src[rotate180 ? (w - 1 - x) : x];

                /* Expand 5/6/5 to 8/8/8 by replicating the high bits into the
                 * low ones, so full-scale stays full-scale (0x1f -> 0xff
                 * rather than 0xf8) and gradients do not lose their top end. */
                unsigned r = (p >> 11) & 0x1f;
                unsigned g = (p >> 5) & 0x3f;
                unsigned b = p & 0x1f;
                r = (r << 3) | (r >> 2);
                g = (g << 2) | (g >> 4);
                b = (b << 3) | (b >> 2);

                dst[x] = ((uint32_t)r << screen->format->Rshift) |
                         ((uint32_t)g << screen->format->Gshift) |
                         ((uint32_t)b << screen->format->Bshift);
            }
        } else {
            if (SDL_MUSTLOCK(screen))
                SDL_UnlockSurface(screen);
            return false;
        }
    }

    if (SDL_MUSTLOCK(screen))
        SDL_UnlockSurface(screen);
    return true;
}
