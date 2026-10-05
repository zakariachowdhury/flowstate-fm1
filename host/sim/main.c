/* SPDX-License-Identifier: GPL-3.0-only */
/* flowstate-sim: the FM-1 firmware (SLOOP 2.1, the shared core of Flowstate) in real time on the Mac,
 * through SDL2. Two views: FLOWSTATE STUDIO (UI spec §12: the World, scenes, the four macros, performance
 * buttons, tracks, the keys) and ADVANCED (the FM-1 itself: the LCD and the whole panel); Tab or the tabs
 * switch. The computer keyboard and the mouse play it, the audio goes to the default output.
 * docs/simulator.md.
 *
 *   flowstate-sim [options]
 *   --world NAME          load this (stand-in) World at boot, with its four scenes in sections A..D
 *   --demo                --world GROOVE (examples/projects/groove.fun4)
 *   --worlds DIR          the stand-in Worlds: the projects in DIR (default examples/projects)
 *   --project FILE.fun4   load this project at boot (as LOAD does), no World
 *   --flash FILE          the 1 MiB NOR image: read at boot, written after the firmware writes and at exit
 *                         (SAVE, autosave, sections, settings and user presets survive a restart)
 *   --buffer FRAMES       the audio device's buffer (default 672: 15.2 ms); also the null sink's period
 *   --fifo FRAMES         the FIFO fill the firmware thread keeps (default: buffer + 64; whole blocks)
 *   --headless SECONDS    no window: play SECONDS of audio into the null sink (a wall-clocked device), then
 *                         stop. --device: into the audio device instead (SDL_AUDIODRIVER=disk writes a file)
 *   --fast                (headless, null sink) as fast as the firmware renders, not in real time
 *   --mute-output         the device plays silence; everything else as usual (measurements)
 *   --script TEXT|FILE    timed commands (host/sim/cmd.c): "1 play; 3.1 scene B; +2 expect scene = B"
 *   --stats               the metrics once a second (they are also on the status line)
 *   --wav FILE            what the sink played (headless: all of it; with a window: up to 120 s)
 *   --screen FILE.ppm     the LCD at the end;  --shot FILE.bmp  the whole window at the end (no window needed)
 *   --advanced            start in ADVANCED (and --shot draws it);  --inspect  the inspector open
 *   --verbose
 * Exit status: 0, or 1 when a script expectation failed or a file could not be read. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include "sim.h"

/* SDL 2 on macOS keeps 2 * ceil(15 ms / buffer) AudioQueue buffers in flight under 15 ms, 2 from 15 ms
 * on (audio.c): 672 frames (15.2 ms, 21 blocks) is the smallest buffer of whole blocks with only 2. The
 * AudioQueue pulls in bursts at the HAL's IO cycle, never closer than ~7 ms with this size, so the FIFO
 * needs one buffer and a small margin: measured in docs/simulator.md */
#define DEFAULT_BUFFER 672
#define FIFO_MARGIN 64

static void usage(void)
{
    fprintf(stderr, "usage: flowstate-sim [--world NAME | --demo | --project FILE.fun4] [--worlds DIR] [--flash FILE]\n"
                    "                     [--buffer FRAMES] [--fifo FRAMES] [--advanced] [--inspect]\n"
                    "                     [--headless SECONDS [--device] [--fast]] [--mute-output] [--script TEXT|FILE]\n"
                    "                     [--stats] [--wav FILE] [--screen FILE.ppm] [--shot FILE.bmp] [--verbose]\n");
    exit(2);
}

static sim_dev_t dev;
static sim_opts_t opt;
static struct {                                  /* the window: frames shown and the drawing's cost */
    uint32_t frames;
    double draw_sum, draw_max, up_sum;           /* drawing the view; handing it to SDL (the texture) */
} disp;

/* ---- the metrics, as words */
static double latency_ms(const sim_sink_stats_t *k, double *fifo_ms, double *queue_ms)
{
    *fifo_ms = k->fill_avg * 1e3 / HOST_FS;
    *queue_ms = dev.queue_frames * 1e3 / dev.freq;
    return *fifo_ms + *queue_ms;
}
static void status_lines(const sim_snap_t *s, const sim_sink_stats_t *k, char *l1, char *l2, size_t n)
{
    const host_state_t *st = &s->st;
    double fm, qm, lat = latency_ms(k, &fm, &qm);
    char mute[5], hal[24] = "", drift[24];
    int i;
    if (dev.hal_rate)
        snprintf(hal, sizeof hal, " + %.1f hal", dev.hal_frames * 1e3 / dev.hal_rate);
    lat += dev.hal_rate ? dev.hal_frames * 1e3 / dev.hal_rate : 0;
    for (i = 0; i < 4; i++)
        mute[i] = st->t[i].mute ? '-' : (char)('1' + i);
    mute[4] = 0;
    snprintf(l1, n, "%s  SCENE %s%s%s  %d BPM  FILTER %+d  TRACKS %s  MASTER %d%%  %.1f s  |  %s %d Hz, %d fr%s",
             st->playing ? "PLAYING" : "STOPPED", cmd_scene_name(st->scene), st->scene_next >= 0 ? " > " : "",
             st->scene_next >= 0 ? cmd_scene_name(st->scene_next) : "", st->bpm, st->filter, mute,
             s->master_adc * 100 / 1023, (double)s->frames / HOST_FS, dev.driver, dev.freq, dev.samples,
             opt.mute_output ? " MUTED" : "");
    if (!k->windows) {
        snprintf(l2, n, "measuring ...");
        return;
    }
    snprintf(drift, sizeof drift, opt.fast ? "-" : "%+.0fppm", k->drift_ppm);
    snprintf(l2, n, "LATENCY %.1f ms = %.1f fifo + %.1f queue%s  UNDERRUNS %llu  FILL %.0f..%.0f  CPU %.1f%% max %.0f  "
             "PASS sd %.1f  DRIFT %s", lat, fm, qm, hal, (unsigned long long)k->underruns_total, k->fill_min,
             k->fill_max, s->fw.cpu_avg, s->fw.cpu_max, s->fw.pass_ms_sd, drift);
}
static void print_stats(const sim_snap_t *s, const sim_sink_stats_t *k)
{
    double fm, qm, lat = latency_ms(k, &fm, &qm);
    char drift[24];
    snprintf(drift, sizeof drift, opt.fast ? "-" : "%+.0f ppm", k->drift_ppm);
    printf("[%3u s] latency %5.1f ms (fifo %4.1f + queue %4.1f) | fill %4.0f/%4.0f/%4.0f | pulls %3u x %u..%u, "
           "%.2f..%.2f ms | underruns %u (%u fr), total %llu | cpu %4.1f%%, per 5.8 ms %4.1f%% max %5.1f%% | "
           "block %4.1f us max %6.1f | ui %4.0f us max %5.0f | pass %5.2f ms [%5.2f..%5.2f] sd %4.2f | drift %s | "
           "rms %5.1f peak %5.1f dBFS\n",
           k->windows, lat, fm, qm, k->fill_min, k->fill_avg, k->fill_max, k->calls, k->req_min, k->req_max,
           k->period_min, k->period_max, k->underruns, k->missing, (unsigned long long)k->underruns_total,
           s->fw.thread_cpu, s->fw.cpu_avg, s->fw.cpu_max, s->fw.block_us, s->fw.block_us_max, s->fw.ui_us,
           s->fw.ui_us_max, s->fw.pass_ms, s->fw.pass_ms_min, s->fw.pass_ms_max, s->fw.pass_ms_sd, drift,
           s->fw.rms_db, s->fw.peak_db);
    if (!opt.headless) {                         /* (once a second, with the sink's windows) */
        printf("        window: %u frames shown, drawing %.2f ms (max %.2f), texture upload %.2f ms\n", disp.frames,
               disp.frames ? 1e3 * disp.draw_sum / disp.frames : 0, 1e3 * disp.draw_max,
               disp.frames ? 1e3 * disp.up_sum / disp.frames : 0);
        disp.frames = 0;
        disp.draw_sum = disp.draw_max = disp.up_sum = 0;
    }
    fflush(stdout);
}
static void print_device(void)
{
    printf("audio: %s", dev.driver);
    if (dev.open) {
        printf(" \"%s\", %d Hz stereo int16, %d frames (%.1f ms) a callback", dev.name, dev.freq, dev.samples,
               dev.samples * 1e3 / dev.freq);
        if (dev.queue_bufs)
            printf("; SDL's AudioQueue: %d buffers, %d frames (%.1f ms) ahead of the callback's", dev.queue_bufs,
                   dev.queue_frames, dev.queue_frames * 1e3 / dev.freq);
        if (dev.native_rate && dev.native_rate != dev.freq)
            printf("; the device runs at %d Hz: CoreAudio converts", dev.native_rate);
        if (dev.hal_rate)
            printf("; HAL %d frames (%.1f ms: IO buffer %d, device %d, stream %d, safety offset %d)", dev.hal_frames,
                   dev.hal_frames * 1e3 / dev.hal_rate, dev.hal_io, dev.hal_device, dev.hal_stream, dev.hal_safety);
    } else {
        printf(" sink, %d frames a pull%s", dev.samples, opt.fast ? ", as fast as it renders" : ", in real time");
    }
    printf("; FIFO target %d frames (%.1f ms)%s\n", opt.fifo, opt.fifo * 1e3 / HOST_FS,
           opt.mute_output ? "; output muted" : "");
    fflush(stdout);
}
static void report(const sim_snap_t *s, const sim_sink_stats_t *k, uint64_t rendered)
{
    double fm, qm, lat = latency_ms(k, &fm, &qm), windows = s->fw.windows ? s->fw.windows : 1;
    printf("flowstate-sim: rendered %llu frames (%.3f s), the sink played %llu (%.3f s); underruns %llu, %llu frames "
           "missing\n", (unsigned long long)rendered, (double)rendered / HOST_FS, (unsigned long long)k->consumed,
           (double)k->consumed / HOST_FS, (unsigned long long)k->underruns_total, (unsigned long long)k->missing_total);
    if (k->windows)
        printf("  latency  %.1f ms estimated: FIFO %.1f (fill %.0f/%.0f/%.0f, last second) + SDL queue %.1f%s\n", lat,
               fm, k->fill_min, k->fill_avg, k->fill_max, qm, dev.hal_rate ? "" : "");
    if (dev.hal_rate)
        printf("           + CoreAudio HAL %.1f ms (IO buffer, device and stream latency, safety offset)\n",
               dev.hal_frames * 1e3 / dev.hal_rate);
    if (s->fw.windows)
        printf("  cpu      %.2f %% of realtime on average, %.1f %% at most (audio + UI per 5.8 ms, wall time); last "
               "second: thread %.1f %%, block %.1f us (max %.1f), UI pass %.0f us (max %.0f)\n",
               s->fw.cpu_sum_all / windows, s->fw.cpu_max_all, s->fw.thread_cpu, s->fw.block_us, s->fw.block_us_max,
               s->fw.ui_us, s->fw.ui_us_max);
    if (s->fw.windows)
        printf("  passes   %.2f ms apart on average (wall; 15.96 ms of audio each), %.2f..%.2f, sd %.2f (last second)\n",
               s->fw.pass_ms, s->fw.pass_ms_min, s->fw.pass_ms_max, s->fw.pass_ms_sd);
    if (k->windows && !opt.fast)
        printf("  clock    the device against the wall clock: %+.0f ppm over %.1f s\n", k->drift_ppm,
               (double)k->consumed / HOST_FS);
    printf("  state    %s, scene %s, %d BPM, rms %.1f dBFS (last second)\n", s->st.playing ? "playing" : "stopped",
           cmd_scene_name(s->st.scene), s->st.bpm, s->fw.rms_db);
    fflush(stdout);
}

/* ---- the window: a view, the tabs over it, the inspector */
void tabs_draw(canvas_t *c, int advanced)
{
    static const char *const T[2] = {"STUDIO", "ADVANCED"};
    int i;
    for (i = 0; i < 2; i++) {
        int x = TAB_X + i * (TAB_W + 4), on = i == advanced;
        uint32_t accent = RGBX(124, 96, 255);
        c_frame(c, x, TAB_Y, TAB_W, TAB_H, 8.0f, 1.0f, on ? accent : RGBX(52, 54, 64),
                on ? c_mix(RGBX(19, 20, 25), accent, 120) : RGBX(22, 23, 28));
        c_text_c(c, x + TAB_W / 2, TAB_Y + 7, T[i], on ? RGBX(240, 240, 246) : RGBX(128, 132, 146));
    }
}
static void draw(canvas_t *c, const sim_snap_t *s, int advanced, int inspect, const char *l1, const char *l2)
{
    if (advanced)
        panel_draw(c, s, l1, l2);
    else
        studio_view_draw(c, s, l1, l2);
    if (inspect)
        inspector_draw(c, s);
    tabs_draw(c, advanced);
}

static const char *base_dir(void)
{
    static char b[1024];
    char *p = SDL_GetBasePath();
    if (p) {
        snprintf(b, sizeof b, "%s", p);
        SDL_free(p);
    }
    return b;
}

int main(int argc, char **argv)
{
    static sim_snap_t snap;
    sim_sink_stats_t ks;
    sim_step_t *steps = NULL;
    int nsteps = 0, i, quit = 0, rc = 0;
    uint32_t last_window = 0, seen = 0;
    SDL_Window *win = NULL;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    canvas_t cv = {NULL, SIM_W, SIM_H};
    int advanced, inspect;
    char l1[256] = "", l2[256] = "";
    opt.buffer = DEFAULT_BUFFER;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--project") && v) opt.project = v, i++;
        else if (!strcmp(a, "--demo")) opt.demo = 1;
        else if (!strcmp(a, "--world") && v) opt.world = v, i++;
        else if (!strcmp(a, "--worlds") && v) opt.worlds = v, i++;
        else if (!strcmp(a, "--advanced")) opt.advanced = 1;
        else if (!strcmp(a, "--inspect")) opt.inspect = 1;
        else if (!strcmp(a, "--flash") && v) opt.flash = v, i++;
        else if (!strcmp(a, "--buffer") && v) opt.buffer = atoi(v), i++;
        else if (!strcmp(a, "--fifo") && v) opt.fifo = atoi(v), i++;
        else if (!strcmp(a, "--headless") && v) opt.headless = atof(v), i++;
        else if (!strcmp(a, "--device")) opt.device = 1;
        else if (!strcmp(a, "--fast")) opt.fast = 1;
        else if (!strcmp(a, "--mute-output")) opt.mute_output = 1;
        else if (!strcmp(a, "--script") && v) opt.script = v, i++;
        else if (!strcmp(a, "--stats")) opt.stats = 1;
        else if (!strcmp(a, "--wav") && v) opt.wav = v, i++;
        else if (!strcmp(a, "--screen") && v) opt.screen = v, i++;
        else if (!strcmp(a, "--shot") && v) opt.shot = v, i++;
        else if (!strcmp(a, "--verbose")) opt.verbose = 1;
        else usage();
    }
    if (opt.buffer < 32 || opt.buffer > 8192 || opt.fifo < 0 || opt.fifo > (int)SIM_FIFO_CAP - 1024 ||
        opt.headless < 0 || (opt.fast && (!opt.headless || opt.device)) || (opt.device && !opt.headless))
        usage();
    if (opt.demo && !opt.world)
        opt.world = "GROOVE";
    advanced = opt.advanced;
    inspect = opt.inspect;
    if (opt.script) {
        char err[256];
        if (cmd_script(opt.script, &steps, &nsteps, err, sizeof err)) {
            fprintf(stderr, "flowstate-sim: --script: %s\n", err);
            return 2;
        }
    }
    if (SDL_Init(SDL_INIT_TIMER | (opt.headless ? 0 : SDL_INIT_VIDEO | SDL_INIT_EVENTS)) != 0) {
        fprintf(stderr, "flowstate-sim: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    opt.base = base_dir();
    printf("flowstate-sim: %s, the real firmware (engines, sequencer, FX, UI, flash) in real time\n", host_version());
    keys_init(!opt.headless);
    if (!opt.headless || opt.verbose)
        keys_print_mapping();
    if (!(cv.px = calloc((size_t)SIM_W * SIM_H, 4)))
        return 1;
    if (!opt.headless) {
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);   /* (the window: never an efficiency core) */
#endif
        SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
        SDL_Rect ub;
        int ww = SIM_W, wh = SIM_H;
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");   /* (as the preview: crisp pixels) */
        if (SDL_GetDisplayUsableBounds(0, &ub) == 0 && ub.h - 40 < wh) {   /* a small screen: smaller, same shape */
            wh = ub.h - 40;
            ww = SIM_W * wh / SIM_H;
        }
        win = SDL_CreateWindow("FLOWSTATE STUDIO - flowstate-sim", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, ww, wh, SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
        if (!win) {
            fprintf(stderr, "flowstate-sim: SDL_CreateWindow: %s\n", SDL_GetError());
            return 1;
        }
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren)
            ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        SDL_RenderSetLogicalSize(ren, SIM_W, SIM_H);   /* scaled, letterboxed; the mouse in window units */
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
        SDL_StopTextInput();                     /* no press-and-hold accent popup on held letters */
    }
    sink_open(&opt, &dev);
    opt.buffer = dev.samples;
    if (!opt.fifo)
        opt.fifo = dev.samples + FIFO_MARGIN;
    opt.fifo = (opt.fifo + HOST_BLOCK - 1) / HOST_BLOCK * HOST_BLOCK;   /* whole blocks: the fill it stops at */
    print_device();
    if (opt.wav && sink_record((uint32_t)((opt.headless ? opt.headless + 1.0 : 120.0) * HOST_FS))) {
        fprintf(stderr, "flowstate-sim: --wav: out of memory\n");
        return 1;
    }
    fifo_reset();
    if (engine_start(&opt, steps, nsteps)) {
        fprintf(stderr, "flowstate-sim: cannot start the firmware thread\n");
        return 1;
    }
    sink_start(opt.headless, opt.fast);

    if (opt.headless) {                          /* until the sink has played its seconds */
        double guard = sim_now() + (opt.fast ? 120.0 : opt.headless * 2.0 + 10.0);
        while (!engine_done() && !sink_finished()) {
            sim_sleep_us(opt.fast ? 2000 : 20000);
            sink_stats(&ks);
            if (opt.stats && ks.windows != last_window) {
                last_window = ks.windows;
                engine_snapshot(&snap);
                print_stats(&snap, &ks);
            }
            if (sim_now() > guard) {
                fprintf(stderr, "flowstate-sim: the sink stalled (%.1f s of audio played)\n", (double)ks.consumed / HOST_FS);
                rc = 1;
                break;
            }
        }
    } else {
        while (!quit && !engine_done()) {
            SDL_Event e;
            uint32_t n;
            while (SDL_PollEvent(&e)) {
                switch (e.type) {
                case SDL_QUIT: quit = 1; break;
                case SDL_WINDOWEVENT:
                    if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                        keys_focus_lost();
                    seen = 0;                    /* (redraw) */
                    break;
                case SDL_KEYDOWN:
                case SDL_KEYUP:
                    switch (keys_event(&e.key, e.type == SDL_KEYDOWN, !advanced)) {
                    case KA_QUIT:                /* Esc: the inspector, then a World being chosen, then quit */
                        if (inspect)
                            inspect = 0;
                        else if (snap.studio.browse >= 0 || snap.studio.pending >= 0)
                            keys_send(OP_WORLD, WA_CANCEL, 0, 0);
                        else
                            quit = 1;
                        break;
                    case KA_VIEW: advanced = !advanced; break;
                    case KA_INSPECT: inspect = !inspect; break;
                    default: break;
                    }
                    seen = 0;
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    if (e.button.y >= TAB_Y && e.button.y < TAB_Y + TAB_H && e.button.x >= TAB_X &&
                        e.button.x < TAB_X + 2 * TAB_W + 4) {   /* the tabs */
                        advanced = e.button.x >= TAB_X + TAB_W + 4;
                        seen = 0;
                        break;
                    }
                    if (inspect && e.button.x >= 560)   /* (the inspector covers that side) */
                        break;
                    /* fall through */
                case SDL_MOUSEBUTTONUP:
                case SDL_MOUSEMOTION:
                case SDL_MOUSEWHEEL:
                    if (advanced)
                        panel_mouse(&e);
                    else
                        studio_view_mouse(&e, &snap);
                    seen = 0;
                    break;
                default: break;
                }
            }
            n = engine_snapshot(&snap);
            sink_stats(&ks);
            if (opt.stats && ks.windows != last_window) {
                last_window = ks.windows;
                print_stats(&snap, &ks);
            }
            if (n == seen) {
                SDL_Delay(2);
                continue;
            }
            seen = n;
            {
                double t0 = sim_now(), dt;
                status_lines(&snap, &ks, l1, l2, sizeof l1);
                draw(&cv, &snap, advanced, inspect, l1, l2);
                dt = sim_now() - t0;
                SDL_UpdateTexture(tex, NULL, cv.px, SIM_W * 4);
                disp.up_sum += sim_now() - t0 - dt;
                disp.frames++;
                disp.draw_sum += dt;
                disp.draw_max = dt > disp.draw_max ? dt : disp.draw_max;
            }
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, NULL, NULL);
            SDL_RenderPresent(ren);
        }
    }

    engine_stop();
    sink_close();
    if (engine_finish(opt.flash, opt.screen))
        rc = 1;
    engine_snapshot(&snap);
    sink_stats(&ks);
    if (opt.shot) {
        status_lines(&snap, &ks, l1, l2, sizeof l1);
        draw(&cv, &snap, advanced, inspect, l1, l2);
        if (c_save_bmp(&cv, opt.shot)) {
            fprintf(stderr, "flowstate-sim: cannot write %s\n", opt.shot);
            rc = 1;
        }
    }
    if (opt.wav && sink_write_wav(opt.wav)) {
        fprintf(stderr, "flowstate-sim: cannot write %s\n", opt.wav);
        rc = 1;
    }
    report(&snap, &ks, host_frames());
    if (engine_failures()) {
        printf("flowstate-sim: %d failure(s)\n", engine_failures());
        rc = 1;
    }
    if (tex)
        SDL_DestroyTexture(tex);
    if (ren)
        SDL_DestroyRenderer(ren);
    if (win)
        SDL_DestroyWindow(win);
    SDL_Quit();
    free(cv.px);
    free(steps);
    return rc;
}
