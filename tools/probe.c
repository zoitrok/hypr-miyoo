/* probe -- Phase 0 hardware reconnaissance for the Miyoo Mini Plus.
 *
 * Everything the renderer and audio output are about to assume, measured on
 * the real device instead of guessed at. Run this once, read log.txt, and the
 * answers feed directly into ui/gfx.c and audio/audio_out.c:
 *
 *   - the screen's actual pixel format, so our offscreen surface matches it and
 *     the per-frame blit is a straight memcpy rather than a conversion pass
 *   - what SDL_OpenAudio actually returns, which SDL 1.2 drivers routinely
 *     disagree with the request about
 *   - the keysym for every physical button, since SDL 1.2 on this device
 *     reports the controls as a keyboard
 *   - how long a full-screen blit and flip really take, which sets the frame
 *     budget for the vaporwave scene
 *
 * It is deliberately standalone and harmless: no network, no writes outside
 * its own directory, and a hard timeout so it can never wedge the handheld.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "util/mono.h"

#define WIDTH  640
#define HEIGHT 480

/* Never let the probe hold the device hostage if input is not what we expect. */
#define HARD_TIMEOUT_MS (120 * 1000)

static void hr(const char *title)
{
    printf("\n=== %s %.*s\n", title, (int)(58 - strlen(title)),
           "------------------------------------------------------------");
    fflush(stdout);
}

static void describe_format(const char *label, const SDL_PixelFormat *f)
{
    if (!f) {
        printf("  %-22s (null)\n", label);
        return;
    }
    printf("  %-22s %d bpp, %d bytes/px\n", label, f->BitsPerPixel,
           f->BytesPerPixel);
    printf("  %-22s R %08x  G %08x  B %08x  A %08x\n", "  masks",
           (unsigned)f->Rmask, (unsigned)f->Gmask, (unsigned)f->Bmask,
           (unsigned)f->Amask);
    printf("  %-22s R<<%d G<<%d B<<%d A<<%d  (loss %d/%d/%d/%d)\n", "  shift/loss",
           f->Rshift, f->Gshift, f->Bshift, f->Ashift,
           f->Rloss, f->Gloss, f->Bloss, f->Aloss);
}

static void describe_surface(const char *label, SDL_Surface *s)
{
    if (!s) {
        printf("  %-22s FAILED: %s\n", label, SDL_GetError());
        return;
    }
    printf("  %-22s %dx%d, pitch %d\n", label, s->w, s->h, s->pitch);
    printf("  %-22s %s%s%s%s\n", "  flags",
           (s->flags & SDL_HWSURFACE) ? "HWSURFACE " : "SWSURFACE ",
           (s->flags & SDL_DOUBLEBUF) ? "DOUBLEBUF " : "",
           (s->flags & SDL_FULLSCREEN) ? "FULLSCREEN " : "",
           (s->flags & SDL_HWACCEL) ? "HWACCEL" : "");
    describe_format("  format", s->format);
}

static void probe_system(void)
{
    hr("system");

    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "MemTotal", 8) == 0 ||
                strncmp(line, "MemFree", 7) == 0 ||
                strncmp(line, "MemAvailable", 12) == 0)
                printf("  %s", line);
        }
        fclose(f);
    }

    /* One line per key, not one per core -- these are identical across cores
     * and eight copies just buries the rest of the report. */
    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        char line[256];
        char last[256] = "";
        int cores = 0;
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, "processor", 9) == 0) {
                cores++;
                continue;
            }
            if (strncmp(line, "model name", 10) == 0 ||
                strncmp(line, "Features", 8) == 0 ||
                strncmp(line, "Hardware", 8) == 0 ||
                strncmp(line, "CPU part", 8) == 0) {
                if (strcmp(line, last) != 0) {
                    printf("  %s", line);
                    snprintf(last, sizeof(last), "%s", line);
                }
            }
        }
        printf("  cores                  %d\n", cores);
        fclose(f);
    }

    /* Confirms whether the NEON path we compile minimp3 with is really there. */
    printf("  SDL version            %d.%d.%d\n",
           SDL_Linked_Version()->major, SDL_Linked_Version()->minor,
           SDL_Linked_Version()->patch);

    const char *vd = getenv("SDL_VIDEODRIVER");
    const char *ad = getenv("SDL_AUDIODRIVER");
    printf("  SDL_VIDEODRIVER env    %s\n", vd ? vd : "(unset)");
    printf("  SDL_AUDIODRIVER env    %s\n", ad ? ad : "(unset)");
}

static void probe_drivers(void)
{
    hr("drivers");

    char name[64];
    if (SDL_VideoDriverName(name, sizeof(name)))
        printf("  video driver           %s\n", name);
    else
        printf("  video driver           (none active)\n");

    if (SDL_AudioDriverName(name, sizeof(name)))
        printf("  audio driver           %s\n", name);
    else
        printf("  audio driver           (none active)\n");
}

static void probe_video(SDL_Surface **out_screen)
{
    hr("video");

    /* Queried before SDL_SetVideoMode, this is the display's own native
     * format -- the thing our offscreen surface should match. */
    const SDL_VideoInfo *vi = SDL_GetVideoInfo();
    if (vi) {
        printf("  hw available           %s\n", vi->hw_available ? "yes" : "no");
        printf("  blit_hw / blit_sw      %s / %s\n",
               vi->blit_hw ? "yes" : "no", vi->blit_sw ? "yes" : "no");
        printf("  video memory           %u KB\n", (unsigned)vi->video_mem);
        describe_format("native vfmt", vi->vfmt);
    }

    /* Try both depths. Whichever gives a HWSURFACE with a matching format and
     * the faster blit is the one the renderer should target: 16bpp halves
     * memory bandwidth, which matters more than colour depth on a device whose
     * rendering cost is bandwidth-bound. */
    static const int DEPTHS[] = { 16, 32 };
    SDL_Surface *screen = NULL;

    for (size_t i = 0; i < sizeof(DEPTHS) / sizeof(DEPTHS[0]); i++) {
        int bpp = SDL_VideoModeOK(WIDTH, HEIGHT, DEPTHS[i],
                                  SDL_HWSURFACE | SDL_DOUBLEBUF);
        printf("\n  SDL_VideoModeOK(%d bpp) -> %d\n", DEPTHS[i], bpp);

        screen = SDL_SetVideoMode(WIDTH, HEIGHT, DEPTHS[i],
                                  SDL_HWSURFACE | SDL_DOUBLEBUF);
        char label[48];
        snprintf(label, sizeof(label), "screen @ %d bpp", DEPTHS[i]);
        describe_surface(label, screen);
    }

    /* Leave the mode at 32bpp for the timing tests unless it failed. */
    if (!screen)
        screen = SDL_SetVideoMode(WIDTH, HEIGHT, 16, SDL_HWSURFACE | SDL_DOUBLEBUF);

    *out_screen = screen;
}

/* Fills a surface with a recognisable gradient so the timing tests are not
 * measuring an all-zero page the memory system might special-case. */
static void fill_gradient(SDL_Surface *s, int phase)
{
    SDL_LockSurface(s);
    for (int y = 0; y < s->h; y++) {
        Uint8 *row = (Uint8 *)s->pixels + (size_t)y * s->pitch;
        for (int x = 0; x < s->w; x++) {
            Uint32 c = SDL_MapRGB(s->format, (Uint8)(x + phase),
                                  (Uint8)(y + phase), (Uint8)(x ^ y));
            if (s->format->BytesPerPixel == 2)
                ((Uint16 *)row)[x] = (Uint16)c;
            else if (s->format->BytesPerPixel == 4)
                ((Uint32 *)row)[x] = c;
        }
    }
    SDL_UnlockSurface(s);
}

static void probe_blit_speed(SDL_Surface *screen)
{
    hr("blit + flip timing");

    if (!screen) {
        printf("  no screen surface; skipping\n");
        return;
    }

    /* SDL_DisplayFormat gives a surface in the screen's exact format, which is
     * what makes the blit a memcpy instead of a per-pixel conversion. */
    SDL_Surface *tmp = SDL_CreateRGBSurface(SDL_SWSURFACE, WIDTH, HEIGHT, 32,
                                            0x00ff0000, 0x0000ff00, 0x000000ff, 0);
    SDL_Surface *matched = SDL_DisplayFormat(tmp);
    SDL_FreeSurface(tmp);

    if (!matched) {
        printf("  SDL_DisplayFormat failed: %s\n", SDL_GetError());
        return;
    }

    describe_surface("offscreen (matched)", matched);
    printf("  %-22s %s\n", "  matches screen?",
           matched->format->BitsPerPixel == screen->format->BitsPerPixel &&
           matched->format->Rmask == screen->format->Rmask
               ? "YES (blit should be a straight copy)"
               : "NO (blit will convert per pixel -- expect it to be slow)");

    fill_gradient(matched, 0);

    const int N = 100;

    uint64_t t0 = mono_ms();
    for (int i = 0; i < N; i++)
        SDL_BlitSurface(matched, NULL, screen, NULL);
    uint64_t t_blit = mono_ms() - t0;

    t0 = mono_ms();
    for (int i = 0; i < N; i++)
        SDL_Flip(screen);
    uint64_t t_flip = mono_ms() - t0;

    t0 = mono_ms();
    for (int i = 0; i < N; i++) {
        SDL_BlitSurface(matched, NULL, screen, NULL);
        SDL_Flip(screen);
    }
    uint64_t t_both = mono_ms() - t0;

    printf("\n  %d full-screen blits    %llu ms (%.2f ms each)\n", N,
           (unsigned long long)t_blit, (double)t_blit / N);
    printf("  %d flips                %llu ms (%.2f ms each)\n", N,
           (unsigned long long)t_flip, (double)t_flip / N);
    printf("  %d blit+flip            %llu ms (%.2f ms each)\n", N,
           (unsigned long long)t_both, (double)t_both / N);

    double per_frame = (double)t_both / N;
    printf("\n  budget at 30 fps       33.3 ms/frame\n");
    printf("  spent on present       %.2f ms (%.0f%% of budget)\n",
           per_frame, per_frame / 33.3 * 100.0);
    printf("  headroom for drawing   %.2f ms\n", 33.3 - per_frame);

    /* Software fill rate, for sizing the grid and spectrum drawing. */
    t0 = mono_ms();
    for (int i = 0; i < 20; i++)
        fill_gradient(matched, i);
    uint64_t t_fill = mono_ms() - t0;
    printf("  full-screen SW fill    %.2f ms each (%d px)\n",
           (double)t_fill / 20, WIDTH * HEIGHT);

    SDL_FreeSurface(matched);
}

static void silence_cb(void *user, Uint8 *stream, int len)
{
    (void)user;
    memset(stream, 0, (size_t)len);
}

static void probe_audio(void)
{
    hr("audio");

    /* The stream is 44.1kHz stereo; ask for exactly that and report what comes
     * back. A mismatch here is the difference between playing at the right
     * pitch and the wrong one. */
    static const int BUFFERS[] = { 512, 1024, 2048, 4096 };

    for (size_t i = 0; i < sizeof(BUFFERS) / sizeof(BUFFERS[0]); i++) {
        SDL_AudioSpec want, got;
        memset(&want, 0, sizeof(want));
        memset(&got, 0, sizeof(got));
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = (Uint16)BUFFERS[i];
        /* We only want the negotiated spec and never unpause, so this is
         * never invoked -- but SDL requires it to be non-NULL. */
        want.callback = silence_cb;

        if (SDL_OpenAudio(&want, &got) != 0) {
            printf("  request %4d samples -> FAILED: %s\n", BUFFERS[i],
                   SDL_GetError());
            continue;
        }

        printf("  request %4d samples -> %d Hz, %d ch, fmt 0x%04x, %d samples"
               "%s%s\n",
               BUFFERS[i], got.freq, got.channels, got.format, got.samples,
               got.freq != 44100 ? "  [RATE DIFFERS]" : "",
               got.format != AUDIO_S16SYS ? "  [FORMAT DIFFERS]" : "");

        SDL_CloseAudio();
    }
}

/* Measures how fast the device actually consumes samples, which is not
 * necessarily the rate SDL claims to have given us.
 *
 * The first probe run showed SDL_OpenAudio returning 44100 Hz while the MI_AO
 * backend logged "mi_ao sample rate=48000" after MI_AO_SetPubAttr failed. If
 * the hardware really runs at 48kHz and nothing resamples, our 44.1kHz stream
 * plays about 8.8% fast and sharp -- and no amount of checking `obtained`
 * would reveal it, because obtained says 44100.
 *
 * Counting callback samples against wall time answers it objectively:
 * ~44100/s means something resamples for us, ~48000/s means we must do it. */

static uint64_t g_tone_samples;
static double   g_tone_phase;
static int      g_tone_rate;

static void tone_cb(void *user, Uint8 *stream, int len)
{
    (void)user;
    int16_t *out = (int16_t *)stream;
    size_t n = (size_t)len / sizeof(int16_t);

    /* A 440Hz sine, so the pitch can also be judged by ear: at 48kHz played as
     * 44.1kHz it lands near 479Hz, a bit over a semitone sharp. */
    for (size_t i = 0; i + 1 < n; i += 2) {
        int16_t v = (int16_t)(6000.0 * sin(g_tone_phase));
        out[i] = v;
        out[i + 1] = v;
        g_tone_phase += 2.0 * M_PI * 440.0 / (double)g_tone_rate;
        if (g_tone_phase > 2.0 * M_PI)
            g_tone_phase -= 2.0 * M_PI;
    }

    __atomic_add_fetch(&g_tone_samples, n, __ATOMIC_RELAXED);
}

static void probe_audio_rate(int request_hz)
{
    SDL_AudioSpec want, got;
    memset(&want, 0, sizeof(want));
    memset(&got, 0, sizeof(got));
    want.freq = request_hz;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = tone_cb;

    g_tone_samples = 0;
    g_tone_phase = 0;
    g_tone_rate = request_hz;

    if (SDL_OpenAudio(&want, &got) != 0) {
        printf("  %d Hz -> could not open: %s\n", request_hz, SDL_GetError());
        return;
    }

    g_tone_rate = got.freq;

    printf("\n  --- requested %d Hz, SDL reports %d Hz ---\n", request_hz, got.freq);
    printf("  playing a 440 Hz tone for 5 s (listen: is it in tune?)\n");
    fflush(stdout);

    uint64_t t0 = mono_ms();
    SDL_PauseAudio(0);
    SDL_Delay(5000);
    SDL_PauseAudio(1);
    uint64_t elapsed = mono_ms() - t0;
    SDL_CloseAudio();

    uint64_t samples = __atomic_load_n(&g_tone_samples, __ATOMIC_RELAXED);
    double frames = (double)samples / (double)got.channels;
    double actual_hz = frames / ((double)elapsed / 1000.0);

    printf("  consumed %.0f frames in %llu ms\n", frames,
           (unsigned long long)elapsed);
    printf("  ACTUAL RATE ~= %.0f Hz (SDL claimed %d Hz)\n", actual_hz, got.freq);

    double ratio = actual_hz / (double)got.freq;
    if (ratio > 0.97 && ratio < 1.03) {
        printf("  => matches. Feeding %d Hz audio plays at the right pitch.\n",
               got.freq);
    } else {
        printf("  => MISMATCH by %.1f%%. Audio fed at %d Hz would play %s.\n",
               (ratio - 1.0) * 100.0, got.freq,
               ratio > 1 ? "FAST and SHARP" : "SLOW and FLAT");
        printf("  => the app must resample to %.0f Hz.\n", actual_hz);
    }
}

static void probe_input(SDL_Surface *screen)
{
    hr("input");

    printf("  Press every button in turn. Each keypress is logged with its\n"
           "  SDL keysym, which is what platform.h needs to map the controls.\n"
           "  The screen flashes a colour per press so you can tell it is alive.\n"
           "\n"
           "  Press ESCAPE or MENU to finish (auto-exits after %d s).\n\n",
           HARD_TIMEOUT_MS / 1000);
    fflush(stdout);

    uint64_t start = mono_ms();
    int presses = 0;

    for (;;) {
        if (mono_ms() - start > HARD_TIMEOUT_MS) {
            printf("\n  timeout reached; exiting\n");
            break;
        }

        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT)
                return;

            if (e.type == SDL_KEYDOWN) {
                SDLKey sym = e.key.keysym.sym;
                printf("  keydown  sym=%-6d name=%-16s scancode=%d\n",
                       (int)sym, SDL_GetKeyName(sym), e.key.keysym.scancode);
                fflush(stdout);
                presses++;

                if (screen) {
                    Uint32 c = SDL_MapRGB(screen->format,
                                          (Uint8)(sym * 37), (Uint8)(sym * 91),
                                          (Uint8)(sym * 53));
                    SDL_FillRect(screen, NULL, c);
                    SDL_Flip(screen);
                }

                if (sym == SDLK_ESCAPE || sym == SDLK_HOME) {
                    printf("\n  exit key pressed after %d presses\n", presses);
                    return;
                }
            }

            if (e.type == SDL_JOYBUTTONDOWN) {
                printf("  joybutton %d (device reports a joystick too)\n",
                       e.jbutton.button);
                fflush(stdout);
            }
        }
        SDL_Delay(10);
    }
}

int main(void)
{
    /* Line buffering so a crash still leaves a usable log. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("hypr probe -- Miyoo Mini Plus hardware reconnaissance\n");

    probe_system();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        printf("\nSDL_Init failed: %s\n", SDL_GetError());
        /* Video may be unavailable while audio is fine; try audio alone so the
         * run still produces something useful. */
        if (SDL_Init(SDL_INIT_AUDIO) != 0) {
            printf("SDL_Init(AUDIO) also failed: %s\n", SDL_GetError());
            return 1;
        }
    }

    probe_drivers();

    SDL_Surface *screen = NULL;
    probe_video(&screen);
    probe_blit_speed(screen);
    probe_audio();

    /* The question the first probe run could not answer: SDL said 44100 while
     * the MI_AO backend logged 48000. Measure what is really consumed. */
    hr("actual audio rate");
    probe_audio_rate(44100);
    probe_audio_rate(48000);

    SDL_EnableKeyRepeat(0, 0);
    probe_input(screen);

    hr("done");
    printf("  Copy log.txt back and hand it over.\n");

    SDL_Quit();
    return 0;
}
