/* HYPR demoscene radio -- an 80s-vaporwave internet radio client for the Miyoo Mini Plus.
 *
 * Four concurrent parts, none of which can stall the others:
 *
 *   stream thread    TLS -> chunked de-frame -> MP3 decode -> ring buffer
 *   metadata thread  WebSocket -> JSON -> shared state (or a recorded replay)
 *   audio callback   drains the ring; memcpy only, never blocks
 *   this thread      snapshots state, draws, flips
 *
 * Frame pacing comes from SDL_Flip, which blocks on vblank at 60Hz on this
 * hardware (measured -- see docs/hardware-findings.md). Adding our own sleep
 * on top would wait twice, so there is deliberately no frame limiter here.
 */

#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#include "audio/audio_out.h"
#include "audio/ring.h"
#include "audio/stream.h"
#include "dsp/spectrum.h"
#include "net/conn.h"
#include "net/timesync.h"
#include "net/url.h"
#include "util/cfg.h"
#include "platform.h"
#include "state/meta.h"
#include "state/replay.h"
#include "state/state.h"
#include "ui/gfx.h"
#include "ui/render.h"
#include "ui/scene.h"
#include "ui/testpattern.h"
#include "util/log.h"
#include "util/mono.h"
#include "util/power.h"
#include "util/proc.h"

#define TAG "main"

#define DEFAULT_STREAM_URL "https://hypr.website/hypr.mp3"
#define DEFAULT_WS_URL     "wss://hypr.website/ws"
#define DEFAULT_CA_FILE    "ca.crt"

/* Roughly 3.5s of 44.1kHz stereo. Deep enough to ride out a WiFi stall or a
 * reconnect without a gap, shallow enough that ~600KB is a rounding error
 * against the 82MB the device has free. */
#define RING_SECONDS   3.5
#define RING_SAMPLES   ((size_t)(44100 * 2 * RING_SECONDS))

/* How much audio to accumulate before starting playback. Starting into a
 * nearly-empty ring guarantees an immediate underrun; the server's ~2s connect
 * burst means this is reached almost at once in practice. */
#define PRIME_SECONDS  1.5

#define SPECTRUM_BARS  40

static volatile sig_atomic_t g_quit;

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* Run off the main thread so a slow or unreachable bootstrap host cannot delay
 * the first frame. The stream and metadata threads will fail their first
 * attempt or two and then succeed once the clock lands -- which is exactly what
 * their existing backoff is for, so nothing needs to coordinate. */
static char g_timesync_host[256];

static void *timesync_thread(void *arg)
{
    (void)arg;
    timesync_bootstrap(g_timesync_host);
    return NULL;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "usage: %s [options]\n"
        "\n"
        "  --url URL       stream URL (default: %s)\n"
        "  --ws URL        WebSocket URL (default: %s)\n"
        "  --ca PATH       CA bundle (default: %s)\n"
        "  --fake-ws PATH  replay a recorded transcript instead of connecting\n"
        "  --fake-stream   no audio; UI only\n"
        "  --frames N      render N frames then exit (for headless checks)\n"
        "  --shot PATH     save the final frame as a BMP\n"
        "  --page N        start on page N (0 now playing, 1 queue, 2 chat)\n"
        "  --testpattern   show a display calibration pattern instead of the UI\n"
        "  --rotate 0|1    rotate the display 180 degrees (default: %d here)\n"
        "  --keep-awake 0|1  stop the device auto-sleeping while playing (default 1)\n"
        "  --chaos P       disrupt this fraction of reads (0.02 = 2%%), for\n"
        "                  exercising reconnect and underrun handling\n"
        "  --chaos-seed N  make a chaos run reproducible\n"
        "  --insecure      skip certificate verification (development only)\n"
        "  --verbose\n"
        "  --help\n",
        argv0, DEFAULT_STREAM_URL, DEFAULT_WS_URL, DEFAULT_CA_FILE,
        SCREEN_ROTATE180);
}

int main(int argc, char **argv)
{
    const char *stream_url = DEFAULT_STREAM_URL;
    const char *ws_url = DEFAULT_WS_URL;
    const char *ca_file = DEFAULT_CA_FILE;
    const char *fake_ws = NULL;
    const char *shot_path = NULL;
    int start_page = PAGE_NOW_PLAYING;
    bool testpattern = false;
    int rotate = SCREEN_ROTATE180;
    int keep_awake = 1;
    double chaos = 0.0;
    unsigned chaos_seed = 1;
    bool no_audio = false, insecure = false, verbose = false;
    long max_frames = 0;

    static const struct option OPTS[] = {
        { "url",         required_argument, 0, 'u' },
        { "ws",          required_argument, 0, 'w' },
        { "ca",          required_argument, 0, 'c' },
        { "fake-ws",     required_argument, 0, 'F' },
        { "fake-stream", no_argument,       0, 'S' },
        { "frames",      required_argument, 0, 'n' },
        { "shot",        required_argument, 0, 's' },
        { "page",        required_argument, 0, 'p' },
        { "testpattern", no_argument,       0, 'T' },
        { "rotate",      required_argument, 0, 'R' },
        { "keep-awake",  required_argument, 0, 'K' },
        { "chaos",       required_argument, 0, 'C' },
        { "chaos-seed",  required_argument, 0, 'Z' },
        { "insecure",    no_argument,       0, 'k' },
        { "verbose",     no_argument,       0, 'v' },
        { "help",        no_argument,       0, 'h' },
        { 0, 0, 0, 0 }
    };

    /* Started before the options are parsed, since the log level is one of
     * them. Without this the first line logged carries an uptime timestamp
     * instead of a run-relative one, which looks like the log begins 39
     * seconds before the app starts. */
    log_init(LOG_INFO);

    /* Options from hypr.conf come first, so anything on the real command line
     * overrides them -- getopt takes the last occurrence. On the device there
     * is no command line at all, which is the whole point of the file. */
    char *argbuf[64];
    int nfile = cfg_load_args("hypr.conf", argbuf, 60);

    char *merged[80];
    int nmerged = 0;
    merged[nmerged++] = argv[0];
    for (int i = 0; i < nfile; i++)
        merged[nmerged++] = argbuf[i];
    for (int i = 1; i < argc && nmerged < 79; i++)
        merged[nmerged++] = argv[i];
    merged[nmerged] = NULL;

    argc = nmerged;
    argv = merged;

    int opt;
    while ((opt = getopt_long(argc, argv, "u:w:c:F:Sn:s:p:TR:K:C:Z:kvh", OPTS, NULL)) != -1) {
        switch (opt) {
        case 'u': stream_url = optarg; break;
        case 'w': ws_url = optarg; break;
        case 'c': ca_file = optarg; break;
        case 'F': fake_ws = optarg; break;
        case 'S': no_audio = true; break;
        case 'n': max_frames = strtol(optarg, NULL, 10); break;
        case 's': shot_path = optarg; break;
        case 'p': start_page = atoi(optarg) % PAGE_COUNT; break;
        case 'T': testpattern = true; break;
        case 'R': rotate = atoi(optarg) ? 1 : 0; break;
        case 'K': keep_awake = atoi(optarg) ? 1 : 0; break;
        case 'C': chaos = atof(optarg); break;
        case 'Z': chaos_seed = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'k': insecure = true; break;
        case 'v': verbose = true; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    log_set_level(verbose ? LOG_DEBUG : LOG_INFO);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    LOGI(TAG, "hypr radio starting");

    if (chaos > 0.0)
        conn_chaos_enable(chaos, chaos_seed);

    /* Nobody presses buttons while listening to the radio, so Onion's idle
     * timer would suspend the device mid-song for doing exactly what it should. */
    if (keep_awake)
        power_keep_awake();

    /* The device has no RTC and learns the time over the network. If the app
     * starts before that lands, the clock can read 1970 -- and TLS will then
     * reject a perfectly good certificate as not yet valid. Logging it up front
     * makes that diagnosable from log.txt alone. */
    {
        time_t now_wall = time(NULL);
        struct tm tmv;
        char buf[64] = "?";
        if (gmtime_r(&now_wall, &tmv))
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tmv);
        LOGI(TAG, "system clock reads %s", buf);
    }

    Uint32 sdl_flags = SDL_INIT_VIDEO | (no_audio ? 0 : SDL_INIT_AUDIO);
    if (SDL_Init(sdl_flags) != 0) {
        LOGE(TAG, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }
    atexit(SDL_Quit);
    SDL_ShowCursor(SDL_DISABLE);
    SDL_WM_SetCaption("HYPR demoscene radio", NULL);

    SDL_Surface *screen = SDL_SetVideoMode(SCREEN_W, SCREEN_H, SCREEN_BPP,
                                           SDL_HWSURFACE | SDL_DOUBLEBUF);
    if (!screen) {
        LOGE(TAG, "SDL_SetVideoMode failed: %s", SDL_GetError());
        return 1;
    }

    LOGI(TAG, "display: %dx%d %d bpp, pitch %d, masks %06x/%06x/%06x,%s%s",
         screen->w, screen->h, screen->format->BitsPerPixel, screen->pitch,
         (unsigned)screen->format->Rmask, (unsigned)screen->format->Gmask,
         (unsigned)screen->format->Bmask,
         (screen->flags & SDL_HWSURFACE) ? " HW" : " SW",
         (screen->flags & SDL_DOUBLEBUF) ? " DOUBLEBUF" : "");

    /* Draw into our own surface and blit once per frame. SDL_DOUBLEBUF
     * invalidates screen->pixels after every flip, so rendering straight into
     * it means never being able to cache anything -- and the background is
     * precisely a thing worth caching. */
    SDL_Surface *canvas = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_W, SCREEN_H,
                                               16, 0xf800, 0x07e0, 0x001f, 0);
    SDL_Surface *bg_surf = SDL_CreateRGBSurface(SDL_SWSURFACE, SCREEN_W, SCREEN_H,
                                                16, 0xf800, 0x07e0, 0x001f, 0);
    gfx_t canvas_g, bg_g;
    if (!canvas || !bg_surf ||
        !gfx_attach(&canvas_g, canvas) || !gfx_attach(&bg_g, bg_surf)) {
        LOGE(TAG, "could not create RGB565 surfaces");
        return 1;
    }

    /* The expensive half of the scene, rasterised exactly once. A full-screen
     * per-pixel pass costs ~16.6ms on device -- a whole frame at 60Hz. */
    scene_build_background(&bg_g);
    LOGI(TAG, "background rasterised (%dx%d, %d bpp)", SCREEN_W, SCREEN_H,
         SCREEN_BPP);

    conn_tls_ctx_t *tls = NULL;
    ring_t ring;
    stream_t *stream = NULL;
    bool audio_ready = false;
    int rate = 44100, channels = 2;

    /* Before any TLS is attempted: this device has no RTC and its firmware
     * never sets the clock, so without this every certificate looks not-yet-
     * valid and nothing can ever connect. */
    if (!fake_ws || !no_audio) {
        url_t u;
        if (url_parse(stream_url, &u)) {
            snprintf(g_timesync_host, sizeof(g_timesync_host), "%s", u.host);
            pthread_t tid;
            if (pthread_create(&tid, NULL, timesync_thread, NULL) == 0)
                pthread_detach(tid);
        }
    }

    if (!no_audio) {
        tls = conn_tls_ctx_new(insecure ? NULL : ca_file, insecure);
        if (!tls) {
            LOGE(TAG, "failed to initialise TLS");
            return 1;
        }
        if (!ring_init(&ring, RING_SAMPLES)) {
            LOGE(TAG, "failed to allocate ring buffer");
            return 1;
        }
        stream = stream_start(stream_url, tls, &ring);
    }

    app_state_t *state = state_new();
    meta_t *meta = NULL;
    replay_t *replay = NULL;

    if (fake_ws) {
        replay = replay_start(fake_ws, state);
    } else if (!no_audio || tls) {
        if (!tls)
            tls = conn_tls_ctx_new(insecure ? NULL : ca_file, insecure);
        if (tls)
            meta = meta_start(ws_url, tls, state);
    }

    /* Audio is brought up inside the render loop rather than before it.
     *
     * Waiting here for the stream format (up to 20s) and then for the buffer to
     * prime (up to 15s more) meant that on a slow or absent network the device
     * showed a black screen for over half a minute -- indistinguishable from a
     * hang. Starting to draw immediately means the scene is up within a frame
     * and the banner explains what it is waiting for. */
    bool audio_playing = false;
    size_t prime_samples = 0;

    spectrum_t spec;
    spectrum_init(&spec, SPECTRUM_BARS);

    render_ui_t ui;
    memset(&ui, 0, sizeof(ui));
    ui.page = (render_page_t)start_page;
    ui.spectrum = spec.value;
    ui.spectrum_bars = spec.bars;

    uint64_t last_frame_ms = mono_ms();
    uint64_t fps_window_ms = last_frame_ms;
    int fps_frames = 0;
    long frames = 0;
    uint64_t last_status_ms = last_frame_ms;
    long rss0 = 0;

    while (!g_quit) {
        uint64_t now = mono_ms();
        double dt = (double)(now - last_frame_ms) / 1000.0;
        last_frame_ms = now;

        /* ---- input ---- */
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                g_quit = 1;
            } else if (e.type == SDL_KEYDOWN) {
                SDLKey k = e.key.keysym.sym;

                if (k == KEY_MENU || k == KEY_B) {
                    g_quit = 1;
                } else if (k == KEY_A) {
                    ui.page = (render_page_t)((ui.page + 1) % PAGE_COUNT);
                    ui.scroll = 0;
                } else if (k == KEY_SELECT) {
                    ui.show_debug = !ui.show_debug;
                } else if (k == KEY_DOWN) {
                    ui.scroll++;
                } else if (k == KEY_UP) {
                    if (ui.scroll > 0)
                        ui.scroll--;
                }
                /* KEY_VOLUP/KEY_VOLDOWN are ignored on purpose: OnionOS owns
                 * system volume and a second control would fight it. */
            }
        }

        /* Clamp scroll here rather than at every key press, since the list
         * length changes underneath us as the queue updates. */
        app_snapshot_t snap;
        state_snapshot(state, &snap);

        int list_len = ui.page == PAGE_QUEUE ? snap.playback.queue_len
                     : ui.page == PAGE_CHAT  ? snap.playback.oneliner_len : 0;
        if (ui.scroll > list_len - 1)
            ui.scroll = list_len > 0 ? list_len - 1 : 0;

        /* ---- bring audio up opportunistically ----
         * Polled rather than waited on, so a stream that never arrives costs
         * nothing but a "RECONNECTING" banner over a scene that is already
         * drawing. */
        if (stream && !audio_ready &&
            stream_wait_format(stream, 0, &rate, &channels)) {
            if (audio_out_open(rate, channels, &ring)) {
                audio_ready = true;
                prime_samples = (size_t)(rate * channels * PRIME_SECONDS);
                if (prime_samples > ring_capacity(&ring) / 2)
                    prime_samples = ring_capacity(&ring) / 2;
                LOGI(TAG, "priming %.1f s", PRIME_SECONDS);
            } else {
                LOGW(TAG, "audio device unavailable; continuing with the UI only");
                stream_stop(stream);
                stream = NULL;
            }
        }

        if (audio_ready && !audio_playing &&
            ring_available(&ring) >= prime_samples) {
            audio_out_play();
            audio_playing = true;
            LOGI(TAG, "playing");
        }

        /* ---- analysis ---- */
        float mono_samples[SPECTRUM_FFT_SIZE];
        int got = audio_ready
            ? audio_out_tap(mono_samples, SPECTRUM_FFT_SIZE) : 0;
        spectrum_update(&spec, mono_samples, got, rate, dt);

        /* ---- state for the HUD ---- */
        if (stream) {
            stream_stats_t st;
            stream_get_stats(stream, &st);
            ui.audio_connected = st.connected;
            ui.audio_error = st.last_error;
            ui.bitrate_kbps = st.bitrate_kbps;
            ui.reconnects = st.reconnects;
            ui.buffer_seconds =
                (double)ring_available(&ring) / ((double)rate * channels);
            ui.underruns = audio_out_underruns();
        } else {
            ui.audio_connected = true;   /* --fake-stream: nothing to report */
            ui.buffer_seconds = 99.0;
        }

        /* ---- draw ---- */
        if (testpattern)
            testpattern_draw(&canvas_g, (int)frames);
        else
            render_frame(&canvas_g, &bg_g, &snap, &ui, now);

        /* Converts RGB565 -> the screen format and rotates in the same pass. */
        gfx_present(screen, &canvas_g, rotate != 0);

        /* Captured here, from the screen rather than the canvas, and before the
         * flip swaps the buffer out from under us. This is the only place the
         * rotation and format conversion have both been applied -- a shot of
         * the canvas would show neither, which is exactly the mistake that
         * made an earlier rotation check pass against itself. */
        if (shot_path && (max_frames && frames + 1 >= max_frames)) {
            if (SDL_SaveBMP(screen, shot_path) == 0)
                LOGI(TAG, "wrote %s", shot_path);
            else
                LOGE(TAG, "could not write %s: %s", shot_path, SDL_GetError());
            shot_path = NULL;
        }

        SDL_Flip(screen);   /* blocks on vblank; this is the frame limiter */

        /* Periodic health line. The debug HUD covers someone holding the
         * device; this covers a soak run, where nobody is, and it is the only
         * record if the app dies overnight. */
        if (now - last_status_ms >= 30000) {
            last_status_ms = now;
            app_snapshot_t hs;
            state_snapshot(state, &hs);
            /* RSS and descriptor count are what make a soak run mean
             * anything: a leak is invisible in every other number here right
             * up until the device runs out. rss0 is the first sample, so the
             * log carries the drift rather than requiring arithmetic later. */
            long rss = proc_rss_kb();
            if (rss0 == 0)
                rss0 = rss;

            LOGI(TAG, "%s | buf %.1fs | ur %llu | rc %u/%u | %d kbit/s | "
                      "%.0f fps | ws %s | rss %ldk (%+ldk) | fds %d | %s",
                 ui.audio_connected ? "playing" : "OFFLINE",
                 ui.buffer_seconds, (unsigned long long)ui.underruns,
                 ui.reconnects, hs.ws_reconnects, ui.bitrate_kbps, ui.fps,
                 hs.ws_connected ? "up" : "down",
                 rss, rss - rss0, proc_open_fds(),
                 hs.playback.now_playing.title[0] ? hs.playback.now_playing.title
                                                  : "(no metadata)");
        }

        frames++;
        fps_frames++;
        if (now - fps_window_ms >= 1000) {
            ui.fps = fps_frames * 1000.0 / (double)(now - fps_window_ms);
            fps_window_ms = now;
            fps_frames = 0;
        }

        if (max_frames && frames >= max_frames)
            break;
    }

    /* Interrupted before the target frame, so the in-loop capture never ran. */
    if (shot_path) {
        gfx_present(screen, &canvas_g, rotate != 0);
        if (SDL_SaveBMP(screen, shot_path) == 0)
            LOGI(TAG, "wrote %s", shot_path);
        else
            LOGE(TAG, "could not write %s: %s", shot_path, SDL_GetError());
    }

    LOGI(TAG, "shutting down after %ld frames", frames);

    /* Silence the device before tearing down the ring it reads from, or the
     * callback can run against freed memory. */
    if (audio_ready)
        audio_out_close();
    meta_stop(meta);
    replay_stop(replay);
    if (stream)
        stream_stop(stream);
    state_free(state);
    if (!no_audio)
        ring_free(&ring);
    conn_tls_ctx_free(tls);

    SDL_FreeSurface(canvas);
    SDL_FreeSurface(bg_surf);

    /* Before the final log line, so the log shows it happened. */
    power_release();

    LOGI(TAG, "bye");
    return 0;
}
