/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP on the host: the real firmware (host/core.c: engines, voices, drums, FX, sequencer, arranger,
 * projects, storage, UI) behind a small API, for programs that render, simulate or author without the
 * FM-1. host/hal_host.h stands in for the hardware.
 *
 * One instance per process: the firmware keeps its state in statics, so host_boot() runs once.
 * One thread, lockstep, as on the device's single core: host_audio() is the audio interrupt (mix_block,
 * HOST_BLOCK frames at a time), host_ui_frame() one pass of the main loop. Never call them at once.
 * Time (fm1_ms, the 24 MHz ticks) is the audio rendered so far.
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

/* ---- what is loaded */
typedef struct {
    const char *engine, *sound;              /* engine and preset names; the drum track: "DRUMS" and its kit */
    const char *div, *root, *scale;
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
} host_state_t;
void host_state(host_state_t *s);
