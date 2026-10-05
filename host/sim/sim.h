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
 * Firmware thread: engine.c the lockstep, studio.c the Studio's model of what plays (Worlds, scenes, macros,
 * tracks, keys). Sink: audio.c. Main thread: main.c the program and its two views, studio_view.c FLOWSTATE
 * STUDIO, panel.c the FM-1 itself (ADVANCED), keys.c the computer keyboard, what is held and the virtual
 * keyboard, canvas.c drawing and the firmware font. cmd.c: commands and scripts (any thread). */
#pragma once
#include <stdint.h>
#include "host.h"

#define SIM_KEYS 27
#define SIM_NENC (HOST_NE + 1)           /* the 7 encoders, then the MASTER pot */
#define SIM_MASTER HOST_NE
#define SIM_PASS_FRAMES (HOST_FRAME_BLOCKS * HOST_BLOCK)   /* 704: 15.96 ms of audio per main-loop pass */
#define SIM_HALF 256                     /* the device's DMA half buffer (5.8 ms): the unit of the CPU figure */
#define SIM_FIFO_CAP 16384u              /* frames (a power of two, 371 ms) */
#define SIM_W 1000                       /* the window, in its own units (scaled to fit the screen) */
#define SIM_H 872

/* ---- options */
typedef struct {
    const char *project, *flash, *script, *wav, *screen, *shot, *world, *worlds;
    int demo, stats, mute_output, device, fast, verbose, advanced, inspect;
    double headless;                     /* seconds (> 0: no window) */
    int buffer;                          /* the sink's buffer, frames (SDL obtained.samples) */
    int fifo;                            /* the FIFO fill the firmware thread keeps, frames */
    const char *base;                    /* the executable's directory (SDL_GetBasePath), for the examples */
} sim_opts_t;

/* ---- commands: the views, the computer keyboard and scripts all speak these to the firmware thread */
enum {
    OP_KEY, OP_BUTTON,                   /* a = key 0..26 / button label, v = 1 down, 0 up (held by the user) */
    OP_KEY_TAP, OP_BUTTON_TAP,           /* down and up (scripts): held for one main-loop pass */
    OP_TURN,                             /* a = encoder role (SIM_MASTER: the pot, 16 per step), v = steps */
    OP_RELEASE_ALL,                      /* the window lost the focus */
    OP_PLAY, OP_STOP, OP_PLAYSTOP,       /* transport */
    OP_SCENE, OP_STORE,                  /* a = scene / section 0..3 */
    OP_MUTE,                             /* a = track 0..3, v = 0 / 1, -1 toggles */
    OP_LEVEL,                            /* a = track, v; rel: v is a step */
    OP_GLOBAL,                           /* a = HOST_G_*, v; rel: v is a step */
    OP_MASTER,                           /* v = the MASTER pot, 0..1023 */
    OP_MACRO,                            /* a = COLOR MOTION SPACE ENERGY (0..3), v = 0..100; rel: a step */
    OP_WORLD,                            /* a = WA_*: choose (the current one plays on), confirm, cancel */
    OP_SELECT,                           /* a = the track the keys play */
    OP_PRINT, OP_EXPECT, OP_QUIT,
};
enum { WA_STEP, WA_PICK, WA_NAME, WA_CONFIRM, WA_CANCEL };   /* OP_WORLD: v = step / index; s = name */
enum { F_PLAYING, F_SCENE, F_NEXT, F_BPM, F_FILTER, F_MUTE, F_LEVEL, F_RMS, F_PEAK, F_TIME, F_MASTER,
       F_MACRO, F_SEL, F_WORLD, F_BROWSE, F_PENDING, F_NF };   /* F_WORLD.. compare names */
enum { CMP_EQ, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE };
typedef struct {
    uint8_t op, a, rel, cmp;             /* OP_EXPECT: a = field, cmp; F_MUTE / F_LEVEL / F_MACRO: index in rel */
    int32_t v;
    char s[16];                          /* a World's name (OP_WORLD WA_NAME, the name fields of OP_EXPECT) */
} sim_cmd_t;
typedef struct {
    double t;                            /* seconds of audio since power-on */
    sim_cmd_t c;
} sim_step_t;
int cmd_script(const char *text, sim_step_t **steps, int *n, char *err, int errlen);   /* 0 = ok */
const char *cmd_field_name(int f);
const char *cmd_scene_name(int s);       /* "A".."D", "-" */
extern const char *const MACRO_NAME[4];  /* COLOR MOTION SPACE ENERGY */

/* ---- the Studio's model of what plays (studio.c fills it on the firmware thread, the views draw it).
 * Today from SLOOP: stand-in Worlds are projects, scenes SLOOP's sections. From Phase 5 on the World runtime
 * fills the same fields; the views stay. */
#define STUDIO_WORLDS 16
typedef struct {
    char name[16], category[12], key[12];   /* "GROOVE", "HOUSE", "A MIN" */
    int bpm;
} studio_world_t;
typedef struct {
    int nworlds;
    studio_world_t w[STUDIO_WORLDS];
    int world;                           /* the one playing (-1: a SLOOP project that is no World) */
    int browse;                          /* highlighted while choosing (-1: not choosing); world keeps playing */
    int pending;                         /* confirmed while playing: loads on the next bar (-1 none) */
    int standin;                         /* 1: stand-in Worlds (SLOOP projects), until Phase 5 */
    char title[16], category[12], key[12];
    char scene_name[4][12];              /* "INTRO" .. ("" for a section that is no World's scene) */
    int scenes;                          /* bit per scene that holds something */
    int scene, scene_next;               /* playing (-1 none), from the next bar (-1 none) */
    int nvar, var;                       /* variations (0: none yet, Phase 12) */
    char var_name[8][12];
    int macro[4];                        /* COLOR MOTION SPACE ENERGY, 0..100 */
    int macro_live;                      /* 0: stand-ins that turn SLOOP's KNOB 1..4 (until Phase 7) */
    char role[HOST_NTRK][8], sound[HOST_NTRK][16];   /* "PAD", "WARM PAD" */
    int mute[HOST_NTRK], level[HOST_NTRK], sel;      /* level 0..127; sel: the track the keys play */
    int keys_smart;                      /* 0: the keys are SLOOP's (Smart Keys: Phase 6) */
    char keys[40];                       /* what the keys do: "A MIN · SNAP" */
    int bpm, playing, recording;
} studio_t;
void studio_init(const sim_opts_t *o);   /* firmware thread, after the boot: the stand-in Worlds */
int studio_load(const char *name);       /* load a World now (stopped or not); 0 = ok */
void studio_exec(const sim_cmd_t *c);    /* OP_MACRO, OP_WORLD, OP_SELECT */
void studio_block(void);                 /* before each block: a World waiting for the bar */
void studio_fill(studio_t *m);           /* the model as it is now */
const char *studio_name_field(int f);    /* F_WORLD, F_BROWSE, F_PENDING: a name or "-" */
int studio_macro(int k);
void studio_project_loaded(const char *path);   /* --project: a SLOOP project, no World */

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
#define SIM_PARAMS 64
typedef struct {
    uint64_t frames;                     /* rendered since power-on */
    uint32_t passes;
    int booted, master_adc, fifo_target;
    uint16_t lcd[240 * 240];             /* RGB565 */
    uint8_t led_btn[HOST_NB], led_key[SIM_KEYS];   /* 0 off, 1 dim, 2 on */
    uint32_t notes, buttons;             /* held as the firmware sees them (buttons by label) */
    host_state_t st;
    studio_t studio;
    int nparams;                         /* the selected track's raw parameters (the inspector) */
    host_param_t params[SIM_PARAMS];
    sim_fw_stats_t fw;
} sim_snap_t;

/* ---- engine.c: the firmware thread */
int engine_start(const sim_opts_t *o, sim_step_t *script, int nscript);   /* boots on its thread */
void engine_stop(void);                  /* asks it to stop and joins it */
void engine_send(const sim_cmd_t *c);    /* main thread only (the ring has one producer) */
uint32_t engine_snapshot(sim_snap_t *s); /* the latest; returns its sequence number (0: none yet) */
uint32_t engine_snapshots(void);         /* the sequence number alone (no copy) */
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
void c_copy(canvas_t *dst, const canvas_t *src);    /* the same size */
void c_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col);
void c_rrect(canvas_t *c, int x, int y, int w, int h, float r, uint32_t col);
void c_frame(canvas_t *c, int x, int y, int w, int h, float r, float t, uint32_t col, uint32_t fill);
void c_ring(canvas_t *c, float cx, float cy, float r0, float r1, uint32_t col);
void c_arc(canvas_t *c, float cx, float cy, float r, float t, float a0, float a1, uint32_t col);
void c_line(canvas_t *c, float x0, float y0, float x1, float y1, float w, uint32_t col);
int c_text(canvas_t *c, int x, int y, const char *s, uint32_t col);         /* FONT_S, 16 px */
int c_text_l(canvas_t *c, int x, int y, const char *s, uint32_t col);       /* FONT_L, 32 px, upper case */
int c_text_sp(canvas_t *c, int x, int y, const char *s, int extra, uint32_t col);   /* FONT_S, spaced */
void c_text_c(canvas_t *c, int cx, int y, const char *s, uint32_t col);     /* centred */
void c_text_r(canvas_t *c, int rx, int y, const char *s, uint32_t col);     /* right-aligned at rx */
int c_text_w(const char *s);
int c_text_lw(const char *s);
void c_lcd(canvas_t *c, int x, int y, int scale, const uint16_t *lcd);
int c_save_bmp(const canvas_t *c, const char *path);

/* ---- keys.c: the computer keyboard, what the user holds (every source), the virtual keyboard */
enum { KA_NONE, KA_QUIT, KA_VIEW, KA_INSPECT };
struct SDL_KeyboardEvent;
void keys_init(int have_video);          /* key names (this keyboard layout when there is video) */
void keys_print_mapping(void);
int keys_event(const struct SDL_KeyboardEvent *k, int down, int studio);   /* -> KA_* */
void keys_focus_lost(void);              /* everything held is let go (latches too) */
void keys_mouse_note(int k);             /* the key the mouse holds (-1: none) */
void keys_mouse_button(int b);           /* the button the mouse holds (-1: none) */
void keys_latch_note(int k);             /* right-click: toggles a held key */
void keys_latch_button(int b);
int keys_latched_note(int k);
int keys_latched_button(int b);
void keys_turn(uint32_t role, int32_t steps);   /* an encoder (or the MASTER pot), and its drawn pointer */
int32_t keys_turns(uint32_t role);
double keys_flash(uint32_t role);        /* until when it is drawn lit */
void keys_send(uint8_t op, uint8_t a, int32_t v, uint8_t rel);
const char *keys_name_note(int k);       /* the computer key of key k ("Z") */
const char *keys_name_button(int b);
void keys_name_encoder(uint32_t role, char *b, int n);   /* "sh Z X" */
void keys_legend(int studio, char l[5][192]);
typedef struct {                         /* a virtual keyboard: 16 white keys of ww x h at x, y */
    int x, y, ww, h, bw, bh;
} kbd_t;
void kbd_draw(canvas_t *c, const kbd_t *g, const sim_snap_t *s, uint32_t led_on, uint32_t led_dim);
int kbd_hit(const kbd_t *g, int x, int y);   /* the key there, or -1 */
int kbd_mouse(const kbd_t *g, const void *sdl_event);   /* press, glissando, latch; 1 = it was the keyboard's */

/* ---- panel.c: ADVANCED, the FM-1 itself (LCD x2, buttons with LEDs, encoders, keys) */
void panel_mouse(const void *sdl_event); /* SDL_Event * */
void panel_draw(canvas_t *c, const sim_snap_t *s, const char *status1, const char *status2);

/* ---- studio_view.c: FLOWSTATE STUDIO (UI spec §12) */
void studio_view_mouse(const void *sdl_event, const sim_snap_t *s);
void studio_view_draw(canvas_t *c, const sim_snap_t *s, const char *status1, const char *status2);
void inspector_draw(canvas_t *c, const sim_snap_t *s);   /* the developer's pane (raw parameters) */

/* ---- main.c: the tabs both views share */
#define TAB_X 776
#define TAB_Y 10
#define TAB_W 98
#define TAB_H 30
void tabs_draw(canvas_t *c, int advanced);
