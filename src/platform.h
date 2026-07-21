#ifndef HYPR_PLATFORM_H
#define HYPR_PLATFORM_H

/* The only file that differs per target, and it contains nothing but input
 * mapping and screen constants. Keeping every #ifdef here is what stops the
 * desktop build drifting into a different program from the device build.
 *
 * On the Miyoo the controls are reported as a *keyboard*, not a joystick --
 * the probe saw no SDL_JOYBUTTONDOWN events at all.
 *
 * Provenance, because it differs per line and matters if something feels wrong:
 *
 *   Every keysym here was observed on real hardware (docs/probe-log.txt). The
 *   device has 17 buttons and the probe runs produced exactly 17 distinct
 *   keysyms, so the set is complete and nothing is guessed.
 *
 *   The D-pad, shoulders, SELECT, START and the volume keys are unambiguous --
 *   their Linux scancodes name them outright (e.g. 115 = KEY_VOLUMEUP).
 *
 *   A/B/X/Y are the exception. The four keysyms are certainly those four
 *   buttons, but which is which comes from the documented Miyoo community
 *   mapping, not from the probe: the log records keysyms, not which button was
 *   under the thumb. If the face buttons turn out transposed, swap them here --
 *   it is a one-line fix and nothing else depends on the pairing. */

#include <SDL.h>

#define SCREEN_W 640
#define SCREEN_H 480

/* The display mode we ask SDL for.
 *
 * 32bpp is the panel's *native* format, and that turns out to be the only one
 * that works. An earlier build asked for 16bpp: SDL_VideoModeOK accepts it and
 * SDL_SetVideoMode hands back a perfectly plausible RGB565 surface with a
 * 1280-byte pitch, and on real hardware the result was garbled, repeating
 * across the screen. The panel scans out 32bpp regardless, so every hardware
 * pixel swallows two of ours.
 *
 * The probe missed this for a subtle reason worth remembering: it *set* 16bpp
 * and dutifully printed the resulting format, but every pixel it actually drew
 * was drawn after it had switched to 32bpp. It validated the mode it was not
 * using. Printing a format struct proves nothing; only putting a known image
 * on the glass does -- which is what --testpattern is for.
 *
 * We still *draw* at 16bpp into our own RGB565 canvas, which keeps the
 * software rendering cheap in cached RAM. Only the final blit converts, and
 * SDL does that for us. */
#define SCREEN_BPP 32

/* Our offscreen canvas is always RGB565 regardless of the display mode: every
 * inner loop in gfx.c writes uint16_t. */
#define CANVAS_BPP 16

/* The panel is mounted upside down and SDL's fbcon driver does not compensate,
 * so everything we draw arrives rotated. Confirmed on hardware with
 * --testpattern: the numbered corners came back 4/3 on top and 2/1 below.
 * The rotation is folded into the final present, costing nothing extra. */
#ifdef TARGET_MIYOO
#define SCREEN_ROTATE180 1
#else
#define SCREEN_ROTATE180 0
#endif

#ifdef TARGET_MIYOO

/* Miyoo Mini Plus, OnionOS. */
#define KEY_UP        SDLK_UP          /* 273 */
#define KEY_DOWN      SDLK_DOWN        /* 274 */
#define KEY_LEFT      SDLK_LEFT        /* 276 */
#define KEY_RIGHT     SDLK_RIGHT       /* 275 */

#define KEY_A         SDLK_SPACE       /* 32  */
#define KEY_B         SDLK_LCTRL       /* 306 */
#define KEY_X         SDLK_LSHIFT      /* 304 */
#define KEY_Y         SDLK_LALT        /* 308 */

#define KEY_L1        SDLK_e           /* 101 */
#define KEY_R1        SDLK_t           /* 116 */
#define KEY_L2        SDLK_TAB         /* 9   */
#define KEY_R2        SDLK_BACKSPACE   /* 8   */

#define KEY_SELECT    SDLK_RCTRL       /* 305 */
#define KEY_START     SDLK_RETURN      /* 13  */

/* MENU reports as ESCAPE, not HOME as planning assumed. By elimination: with
 * the volume keys identified below, escape is the only keysym left unaccounted
 * for, and it is what arrived when MENU was pressed. */
#define KEY_MENU      SDLK_ESCAPE      /* 27  */

/* The volume rocker reaches SDL rather than being swallowed by the OS --
 * scancodes 115/114 are KEY_VOLUMEUP/KEY_VOLUMEDOWN. Note the apparent
 * inversion (volume *up* reports as *left* super) is what the hardware sends;
 * trust the scancodes, not the key names.
 *
 * We do not act on these: OnionOS handles system volume itself, and a second
 * in-app volume control layered on top would fight it. They are named so the
 * renderer can ignore them deliberately rather than treat them as unknown. */
#define KEY_VOLUP     SDLK_LSUPER      /* 311, scancode 115 */
#define KEY_VOLDOWN   SDLK_RSUPER      /* 312, scancode 114 */

#else /* TARGET_DESKTOP */

/* Chosen so the device layout is reachable on a keyboard without contorting:
 * the face buttons sit under the right hand, shoulders on the number row. */
#define KEY_UP        SDLK_UP
#define KEY_DOWN      SDLK_DOWN
#define KEY_LEFT      SDLK_LEFT
#define KEY_RIGHT     SDLK_RIGHT

#define KEY_A         SDLK_SPACE
#define KEY_B         SDLK_LCTRL
#define KEY_X         SDLK_LSHIFT
#define KEY_Y         SDLK_LALT

#define KEY_L1        SDLK_e
#define KEY_R1        SDLK_t
#define KEY_L2        SDLK_TAB
#define KEY_R2        SDLK_BACKSPACE

#define KEY_SELECT    SDLK_RCTRL
#define KEY_START     SDLK_RETURN
#define KEY_MENU      SDLK_ESCAPE

/* No volume rocker on a keyboard; these exist only so shared code can name
 * them. Bound to keys the app does not otherwise use. */
#define KEY_VOLUP     SDLK_PAGEUP
#define KEY_VOLDOWN   SDLK_PAGEDOWN

#endif

#endif
