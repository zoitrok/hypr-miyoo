#ifndef HYPR_RENDER_H
#define HYPR_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "state/state.h"
#include "ui/gfx.h"

/* Frame composition. Draws from a snapshot copy only -- never from live state
 * and never holding a lock, so a slow network write cannot stall a frame. */

typedef enum {
    PAGE_NOW_PLAYING = 0,
    PAGE_QUEUE,
    PAGE_CHAT,
    PAGE_COUNT
} render_page_t;

typedef struct {
    render_page_t page;
    int scroll;              /* list offset on the queue/chat pages */
    bool show_debug;         /* SELECT toggles the on-device HUD */

    /* Audio health, passed in rather than reached for: the renderer has no
     * business knowing about the ring buffer or the stream. */
    bool     audio_connected;
    double   buffer_seconds;
    uint64_t underruns;
    unsigned reconnects;
    int      bitrate_kbps;
    double   fps;

    /* Why audio last failed to connect; empty when healthy. Shown on screen so
     * a failing device explains itself without needing its log pulled. */
    const char *audio_error;

    /* Spectrum, 0..1 per bar. NULL until the analyser exists. */
    const float *spectrum;
    int          spectrum_bars;
} render_ui_t;

/* Rasterises one frame into g. bg must be the surface built by
 * scene_build_background; it is stamped first and then drawn over. */
void render_frame(gfx_t *g, const gfx_t *bg, const app_snapshot_t *snap,
                  const render_ui_t *ui, uint64_t now_mono_ms);

#endif
