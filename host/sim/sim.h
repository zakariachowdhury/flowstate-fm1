/* SPDX-License-Identifier: GPL-3.0-only */
/* flowstate-sim: the shared SLOOP core (host/core.c, through host/host.h) played in real time on the Mac.
 * docs/simulator.md has the picture; in short, three threads and three lock-free hand-overs:
 *
 *   main thread    SDL events -> commands ---- cmd ring (SPSC) ----> firmware thread
 *                  draws the window   <------- snapshot (two slots, seqlock) ---
 *   firmware       the device's single core: host_audio blocks and host_ui_frame passes in lockstep
 *                  (a pass every HOST_FRAME_BLOCKS blocks), all firmware state; pushes the output
 *                  ---------- audio FIFO (SPSC, int16 stereo) ------------------> sink
 *   sink           the SDL audio callback (or the null sink thread): pops the FIFO, never touches the
 *                  firmware; counts underruns
 *
 * engine.c the firmware thread, audio.c the sinks, cmd.c commands and scripts, canvas.c drawing primitives
 * and the firmware font, panel.c the FM-1 panel view and its key map, main.c the program. */
#pragma once
#include <stdint.h>
#include "host.h"

#define SIM_KEYS 27
#define SIM_NENC (HOST_NE + 1)           /* the 7 encoders, then the MASTER pot */
#define SIM_MASTER HOST_NE
#define SIM_PASS_FRAMES (HOST_FRAME_BLOCKS * HOST_BLOCK)   /* 704: 15.96 ms of audio per main-loop pass */
#define SIM_HALF 256                     /* the device's DMA half buffer (5.8 ms): the unit of the CPU figure */
#define SIM_FIFO_CAP 16384u              /* frames (a power of two, 371 ms) */

/* ---- options */
typedef struct {
    const char *project, *flash, *script, *wav, *screen, *shot;
    int demo, stats, mute_output, device, fast, verbose;
    double headless;                     /* seconds (> 0: no window) */
    int buffer;                          /* the sink's buffer, frames (SDL obtained.samples) */
    int fifo;                            /* the FIFO fill the firmware thread keeps, frames */
    const char *base;                    /* the executable's directory (SDL_GetBasePath), for --demo */
} sim_opts_t;

/* ---- commands: the panel, the computer keyboard and scripts all speak these to the firmware thread */
enum {
    OP_KEY, OP_BUTTON,                   /* a = key 0..26 / button label, v = 1 down, 0 up (held by the user) */
    OP_KEY_TAP, OP_BUTTON_TAP,           /* down and up (scripts): held for one main-loop pass */
    OP_TURN,                             /* a = encoder role (SIM_MASTER: the pot, 16 per step), v = steps */
    OP_RELEASE_ALL,                      /* the window lost the focus */
    OP_PLAY, OP_STOP, OP_PLAYSTOP,       /* transport */
    OP_SCENE, OP_STORE,                  /* a = section 0..3 */
    OP_MUTE,                             /* a = track 0..3, v = 0 / 1, -1 toggles */
    OP_LEVEL,                            /* a = track, v; rel: v is a step */
    OP_GLOBAL,                           /* a = HOST_G_*, v; rel: v is a step */
    OP_MASTER,                           /* v = the MASTER pot, 0..1023 */
    OP_PRINT, OP_EXPECT, OP_QUIT,
};
enum { F_PLAYING, F_SCENE, F_NEXT, F_BPM, F_FILTER, F_MUTE, F_LEVEL, F_RMS, F_PEAK, F_TIME, F_MASTER, F_NF };
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE };
typedef struct {
    uint8_t op, a, rel, cmp;             /* OP_EXPECT: a = field, cmp; F_MUTE / F_LEVEL: track in rel */
    int32_t v;
} sim_cmd_t;
typedef struct {
    double t;                            /* seconds of audio since power-on */
    sim_cmd_t c;
} sim_step_t;
int cmd_script(const char *text, sim_step_t **steps, int *n, char *err, int errlen);   /* 0 = ok */
const char *cmd_field_name(int f);
const char *cmd_scene_name(int s);       /* "A".."D", "-" */

/* ---- what the firmware thread publishes after each main-loop pass */
typedef struct {
    uint32_t windows;                    /* 1 s windows of audio completed (0: none yet) */
    double cpu_avg, cpu_max;             /* % of realtime: audio + UI per SIM_HALF frames (wall time) */
    double thread_cpu;                   /* % of realtime: the thread's CPU time over the window */
    double block_us, block_us_max;       /* one host_audio block (HOST_BLOCK frames, 726 us of audio) */
    double ui_us, ui_us_max;             /* one host_ui_frame pass */
    double pass_ms, pass_ms_min, pass_ms_max, pass_ms_sd;   /* wall time between passes (15.96 nominal) */
    double rms_db, peak_db;              /* the output (after -6 dB), dBFS */
    double cpu_max_all, cpu_sum_all;     /* since boot (avg = sum / windows) */
} sim_fw_stats_t;
typedef struct {
    uint64_t frames;                     /* rendered since power-on */
    uint32_t passes;
    int booted, master_adc, fifo_target;
    uint16_t lcd[240 * 240];             /* RGB565 */
    uint8_t led_btn[HOST_NB], led_key[SIM_KEYS];   /* 0 off, 1 dim, 2 on */
    uint32_t notes, buttons;             /* held as the firmware sees them (buttons by label) */
    host_state_t st;
    sim_fw_stats_t fw;
} sim_snap_t;

/* ---- engine.c: the firmware thread */
int engine_start(const sim_opts_t *o, sim_step_t *script, int nscript);   /* boots on its thread */
void engine_stop(void);                  /* asks it to stop and joins it */
void engine_send(const sim_cmd_t *c);    /* main thread only (the ring has one producer) */
uint32_t engine_snapshot(sim_snap_t *s); /* the latest; returns its sequence number (0: none yet) */
int engine_done(void);                   /* a script's quit, the UBOOT halt, ... */
int engine_failures(void);               /* script expectations that failed */
int engine_finish(const char *flash, const char *screen);   /* after engine_stop: flash image, LCD */
int64_t engine_sleeps(void);

/* ---- audio.c: the FIFO and the sinks */
void fifo_reset(void);
uint32_t fifo_fill(void);
uint32_t fifo_push(const int16_t *stereo, uint32_t frames);   /* firmware thread; returns frames taken */
void fifo_set_target(uint32_t frames);
typedef struct {
    uint32_t windows;                    /* 1 s windows of audio consumed */
    uint32_t calls, req_min, req_max;    /* sink pulls, frames asked per pull */
    double fill_min, fill_avg, fill_max; /* FIFO frames at each pull, before it */
    uint32_t underruns, missing;         /* pulls that found the FIFO short, frames missing (the window) */
    double period_avg, period_min, period_max;   /* ms between pulls */
    uint64_t underruns_total, missing_total, consumed;
    double drift_ppm;                    /* frames consumed against the wall clock since the FIFO first filled */
} sim_sink_stats_t;
typedef struct {
    int open;                            /* 0: the null sink */
    char driver[32], name[128];
    int freq, samples, queue_frames;     /* what SDL gave; SDL's own queue below the callback (frames ahead) */
    int queue_bufs;                      /* SDL 2 coreaudio: the AudioQueue's buffers (0: not known) */
    int native_rate;                     /* the output device's rate (CoreAudio converts when it differs) */
    int hal_frames;                      /* CoreAudio HAL: IO buffer + device and stream latency + safety */
    int hal_rate, hal_io, hal_device, hal_stream, hal_safety;
} sim_dev_t;
int sink_open(const sim_opts_t *o, sim_dev_t *d);   /* the SDL device (window or --device), else the null sink */
void sink_start(double seconds, int fast);           /* the null sink: stop after seconds of audio (0: never) */
void sink_close(void);
int sink_finished(void);                 /* the null sink consumed its seconds */
void sink_stats(sim_sink_stats_t *s);
int sink_record(uint32_t max_frames);    /* keep what the sink plays, up to max_frames */
int sink_write_wav(const char *path);
double sim_now(void);                    /* seconds, monotonic */
void sim_sleep_us(int us);
void sim_realtime_thread(double period_ms, double compute_ms);

/* ---- canvas.c: an ARGB8888 window drawn in software */
typedef struct {
    uint32_t *px;
    int w, h;
} canvas_t;
#define RGBX(r, g, b) (0xFF000000u | (uint32_t)(r) << 16 | (uint32_t)(g) << 8 | (uint32_t)(b))
uint32_t c_mix(uint32_t a, uint32_t b, uint32_t t);
uint32_t c_565(uint16_t c);
void c_fill(canvas_t *c, uint32_t col);
void c_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void c_rrect(canvas_t *c, int x, int y, int w, int h, float r, uint32_t col);
void c_ring(canvas_t *c, float cx, float cy, float r0, float r1, uint32_t col);
void c_line(canvas_t *c, float x0, float y0, float x1, float y1, float w, uint32_t col);
int c_text(canvas_t *c, int x, int y, const char *s, uint32_t col);         /* FONT_S */
int c_text_l(canvas_t *c, int x, int y, const char *s, uint32_t col);       /* FONT_L */
void c_text_c(canvas_t *c, int cx, int y, const char *s, uint32_t col);     /* centred */
int c_text_w(const char *s);
void c_lcd(canvas_t *c, int x, int y, int scale, const uint16_t *lcd);
int c_save_bmp(const canvas_t *c, const char *path);

/* ---- panel.c: the FM-1 panel view (LCD x2, buttons with LEDs, encoders, keys) and the computer keyboard */
#define PANEL_W 1000
#define PANEL_H 872
struct SDL_KeyboardEvent;
void panel_init(int have_video);         /* key names (this keyboard layout when there is video) */
void panel_print_mapping(void);
void panel_key(const struct SDL_KeyboardEvent *k, int down, int *quit);
void panel_mouse(const void *sdl_event); /* SDL_Event * */
void panel_focus_lost(void);
void panel_draw(canvas_t *c, const sim_snap_t *s, const char *status1, const char *status2);
