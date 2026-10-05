/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on the host: the real firmware (host/core.c: engines, voices, drums, FX, sequencer, arranger,
 * projects, storage, UI) behind a small API, for programs that render, simulate or author without the
 * FM-1. host/hal_host.h stands in for the hardware.
 *
 * One instance per process: the firmware keeps its state in statics, so host_boot() runs once.
 * One thread, lockstep, as on the device's single core: host_audio() is the audio interrupt (mix_block,
 * HOST_BLOCK frames at a time), host_ui_frame() one pass of the main loop. Never call them at once.
 * Time (fm1_ms, the 24 MHz ticks) is the audio rendered so far. A threaded program (host/sim) makes every
 * call from one firmware thread; only the constant-data calls at the end (names, font) are safe elsewhere.
 *
 * Programs that need the firmware's internals (the engine and preset tables, track parameters, steps)
 * #include "core.c" instead of linking it, as the tests include hostsim.c (host/examples.c does). */
#pragma once
#include <stdint.h>

#define HOST_FS 44100            /* frames per second */
#define HOST_BLOCK 32            /* frames per mix_block (CTL) */
#define HOST_FRAME_BLOCKS 22     /* blocks per main-loop pass (15.96 ms; the device's loop takes >= 15 ms) */
#define HOST_NTRK 4              /* tracks 1..3 synth parts, 4 the drums */

/* ---- boot */
/* main.c fm1_main up to its loop, without the hardware: settings, panel, power-on sounds, autosave
 * resume, the 930 ms the logo stays (rendered). flash_image: a 1 MiB NOR image file to boot from (it need
 * not exist; host_flash_write saves it), NULL = an erased chip. 0 = ok */
int host_boot(const char *flash_image);
int host_flash_write(const char *path);

/* ---- audio */
/* frames (a multiple of HOST_BLOCK) of the mix, interleaved L R: the raw Q15 mix as the host tests write
 * it (audio.c audio_block; the DAC gets it 6 dB lower, OUT_SHIFT) */
void host_audio(int16_t *stereo, uint32_t frames);
uint64_t host_frames(void);      /* rendered since boot */
/* while the main loop waits (fm1_delay_ms, calibration), audio goes on: each block rendered then goes
 * to fn (NULL: dropped) */
void host_set_yield(void (*fn)(const int16_t *stereo, uint32_t frames, void *ctx), void *ctx);

/* ---- the main loop: one pass of main.c's loop body (ADCs, ui_input, ui_leds, ui_draw, autosave,
 * sections). Run it every HOST_FRAME_BLOCKS blocks, as the device does. */
void host_ui_frame(void);

/* ---- the panel (labels and roles as panel.c B_* / EN_*; through the panel table, as calibrated) */
enum { HOST_B_FX, HOST_B_SCL, HOST_B_ENV, HOST_B_LFO, HOST_B_EDIT, HOST_B_GLO, HOST_B_HOME, HOST_B_SAVE,
       HOST_B_ARP, HOST_B_SEQ, HOST_B_PLAY, HOST_B_REC, HOST_B_OCTDN, HOST_B_OCTUP, HOST_NB };
enum { HOST_EN_SELECT, HOST_EN_ALGO, HOST_EN_PRESET, HOST_EN_K1, HOST_EN_K2, HOST_EN_K3, HOST_EN_K4, HOST_NE };
void host_key(uint32_t k, int down);          /* key k: 0 = F3 .. 26 = G5 */
void host_button(uint32_t label, int down);
void host_turn(uint32_t role, int32_t steps);  /* detents, + = clockwise */
void host_master_pot(int32_t adc);           /* the MASTER pot as the main loop reads it, 0..1023 (boot: 1023) */
void host_master_volume(uint32_t q12);       /* the master gain itself (4096 = unity); the pot reads nothing after */
int host_led(uint32_t id);                   /* button id (matrix 0..13) or 14 + key: 0 off, 1 dim, 2 on */
const uint16_t *host_lcd(void);              /* 240 x 240 RGB565 */
int host_lcd_ppm(const char *path);

/* ---- transport, projects (the FUN4 project_t of project.c; FUN1..3 are converted), song */
void host_play(void);                        /* PLAY: transport_req, as the panel asks for it */
void host_stop(void);
int host_playing(void);
int host_project_load(const char *path);     /* the working project, as a load (project_apply: stops, releases).
                                              * The format read (1..4), or < 0: -1 file, -2 not a project */
int host_project_save(const char *path);     /* the working project (proj_capture) as FUN4; 0 = ok */
int host_section_load(uint32_t slot, const char *path);   /* a file into section A..D (0..3), as SAVE + key stores */
int host_section_apply(uint32_t slot);       /* stopped: section slot becomes the working project (section_load) */
int host_song_set(const uint8_t *scene, const uint8_t *bars, uint32_t n);   /* the chain; 0 = valid */
void host_song_mode(int on);                 /* PLAY plays the chain (song mode) or the loop */

/* ---- live control: what the panel's gestures do, as direct calls (between host_audio calls, as the main
 * loop makes them; the screen shows the same messages) */
int host_scene(uint32_t slot);               /* SAVE held + key 1..4: section A..D (0..3). Playing a loop: from the
                                              * next bar (1); stopped: loaded now (0). -1 empty, -2 the song plays */
int host_section_store(uint32_t slot);       /* SAVE held + key 5..8: the working project into section A..D; 0 = ok */
enum { HOST_T_LEVEL, HOST_T_PAN, HOST_T_MUTE, HOST_T_CHO, HOST_T_DLY, HOST_T_REV, HOST_NT };
/* a track's mix value as host_state reports it (the drum track's level and reverb: GLO > DRUMS), clamped to
 * its range; returns what was set (0 for a bad track or value id) */
int host_track_set(uint32_t track, uint32_t what, int32_t v);
enum { HOST_G_BPM, HOST_G_SWING, HOST_G_FILTER, HOST_G_DUST, HOST_G_DUCK, HOST_NG };
int host_global(uint32_t what);              /* tempo, swing, the DJ filter (FX knob 1: -64..63, 0 off), DUST, DUCK */
int host_global_set(uint32_t what, int32_t v);   /* clamped to its range; returns what was set */
uint32_t host_flash_changes(void);           /* erases + page programs so far (a change to save) */
int host_button_led(uint32_t label);         /* host_led of a button by its label (through the panel table) */
void host_track_select(uint32_t track);      /* ALGORITHM: the selected track (the keys play it, REC follows) */
/* a track's parameters as the EDIT pages label and format them (the engine's own labels for its eight), for
 * inspection: up to max of them into p; returns how many */
typedef struct {
    char label[8], text[10];                 /* "CUT", "64%" */
    int16_t value, min, max;
    uint8_t id;                              /* the index in the track's p[] */
} host_param_t;
int host_track_params(uint32_t track, host_param_t *p, int max);

/* ---- names, colours and the firmware's font, for a host's own drawing. Constant data only: any thread */
const char *host_version(void);              /* "SLOOP 2.1" */
const char *host_button_name(uint32_t label);    /* "FX" .. "OCT+" (panel.c) */
const char *host_encoder_name(uint32_t role);    /* "SELECT" .. "KNOB 4" */
uint32_t host_knob_rgb(uint32_t k);          /* KNOB 1..4's colour on the screens, 0xRRGGBB */
/* s in FONT_S (large: FONT_L, 32 px, upper case) onto a w x h ARGB8888 canvas (stride in pixels) at pen x, top
 * y, alpha-blended with colour 0xRRGGBB and clipped; returns the pen's end x */
int host_text(uint32_t *argb, int stride, int w, int h, int x, int y, int large, const char *s, uint32_t rgb);
int host_text_width(int large, const char *s);
int host_text_height(int large);

/* ---- what is loaded */
typedef struct {
    const char *engine, *sound;              /* engine and preset names; the drum track: "DRUMS" and its kit */
    const char *div, *root, *scale, *quant;  /* quant: how the keys snap (SLOOP KEYS: OFF SNAP WHITE) */
    int steps, notes;                        /* pattern length, steps that play */
    double bars;                             /* one pass of the pattern, in 4/4 bars */
    int level, pan, mute, cho, dly, rev;     /* the drum track: GLO > DRUMS level and reverb */
} host_track_t;
typedef struct {
    int bpm, swing;                          /* swing 0..100 = 50..75 % */
    int playing, song_mode, sections;        /* sections: bit per slot A..D holding a project */
    int song_n;
    uint8_t song_scene[16], song_bars[16];
    host_track_t t[HOST_NTRK];
    int scene, scene_next;                   /* the section playing (last jumped to, loaded or stored; A..D = 0..3,
                                              * -1 none) and the one asked for from the next bar (-1 none) */
    int beat;                                /* beats since PLAY or the section's start (4 a bar) */
    int filter, master;                      /* the DJ filter (-64..63); the master gain (4096 = unity) */
    int sel;                                 /* the selected track (the keys play it) */
} host_state_t;
void host_state(host_state_t *s);
/* what a project file holds, without loading it: tempo, swing and per track the engine, sound, key, mix
 * (the steps, the scene and the clock fields stay 0). The format read, or < 0 as host_project_load */
int host_project_info(const char *path, host_state_t *s);
