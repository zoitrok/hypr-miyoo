#ifndef HYPR_THEME_H
#define HYPR_THEME_H

/* The palette, in one place so the renderer and tools/mkicon.py cannot drift.
 *
 * Vaporwave lives on a narrow set of hues -- magenta/violet sky, cyan grid,
 * hot-pink-to-gold sun -- so these are deliberately saturated and deliberately
 * few. Anything that needs to stand out gets brightness, not a new hue. */

#define RGB(r, g, b) ((r) << 16 | (g) << 8 | (b))

/* Sky: deep violet at the top falling to magenta at the horizon. */
#define C_SKY_TOP     RGB( 26,   8,  56)
#define C_SKY_BOT     RGB( 94,  22, 108)

/* Ground below the horizon, dark enough for the grid to read against. */
#define C_GROUND_TOP  RGB( 40,   6,  62)
#define C_GROUND_BOT  RGB( 12,   2,  28)

/* Sun, gold at the crown through hot pink at the waterline. */
#define C_SUN_TOP     RGB(255, 214,  92)
#define C_SUN_BOT     RGB(255,  45, 122)

/* The grid, and the horizon glow it converges on. */
#define C_GRID        RGB(  0, 240, 232)
#define C_GRID_FAR    RGB( 90,  60, 170)   /* grid fading into the haze */
#define C_HORIZON     RGB(255, 240, 255)

/* Text. Titles are near-white with a magenta drop shadow for the chrome look;
 * secondary text is cyan-tinted so it recedes without becoming unreadable. */
#define C_TEXT        RGB(255, 250, 255)
#define C_TEXT_DIM    RGB(150, 190, 220)
#define C_TEXT_ACCENT RGB(255,  90, 200)
#define C_SHADOW      RGB(120,  10,  90)

/* Spectrum analyser: cyan at the base through magenta at the peak, with a
 * brighter peak-hold marker. */
#define C_BAR_LOW     RGB(  0, 220, 240)
#define C_BAR_HIGH    RGB(255,  60, 180)
#define C_BAR_PEAK    RGB(255, 255, 255)

/* Progress bar. */
#define C_PROGRESS    RGB(255,  90, 200)
#define C_PROGRESS_BG RGB( 60,  20,  80)

/* Warnings (buffering, reconnecting) -- amber reads as "attention" against a
 * palette that has no other warm mid-tone. */
#define C_WARN        RGB(255, 180,  60)

/* Layout. The horizon sits high enough to leave room for the grid to feel like
 * ground, low enough that the sky and sun dominate. */
#define HORIZON_Y     264

#endif
