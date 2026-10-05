/* SPDX-License-Identifier: GPL-3.0-only */
/* PLAY MODE on the host (firmware/src/ui_play.c, world_store.c; design 8, 10.4; Phase 9): the whole firmware through
 * host/core.c (FELUCCA_WORLD 1, the real UI, flash, audio between the main-loop passes as on the device), the panel
 * driven as a player drives it. Every scenario runs in its own process (the firmware's state is static: one boot each):
 *   first     a first boot (erased flash): PLAY MODE, NEON RAIN, the FIRST screen; PLAY -> HOME
 *   screens   every screen and its texts (a text hook, PL_TEXT_HOOK: what ui_play.c drew), a few pixels; the control
 *             map (each knob, button and hold); CHOOSE WORLD: PLAY switches on the next bar while playing and at once
 *             while stopped, HOME and the timeout cancel; scenes and variations on the bar; the overlays' timeouts;
 *             EDIT 2 s untouched -> the dialog, an EDIT tap -> ADVANCED, a key cancels the detector, back to PLAY;
 *             H17 / H26 in ADVANCED; LEDs; the cost of each redraw (pixels sent to the panel)
 *   leave     LEAVE WORLD: a SLOOP project parked by PLAY MODE comes back bit-identically (its project_t, and its
 *             render against the same render without the excursion; design 13)
 *   persist   the session across a reboot (the flash image): World, scene, variation, controls, PULSE, BEAT, octave,
 *             tempo; the mode SLOOP; the parked project; offset 0xF00 of the region's sectors left erased
 *   fuzz      20,000 frames of random PLAY MODE input with audio: no crash, the state machine always valid, nothing
 *             drawn off the screen, no note left sounding after STOP
 *   build/host/ui_play_test OUTDIR [scenario]     (PPMs: OUTDIR/play-*.ppm) */
#include <stdint.h>
#include <sys/wait.h>
static void pt_text(const char *s);
#define PL_TEXT_HOOK(s) pt_text(s)
#include "../host/core.c"

static const char *outdir = "build/host";
static int fails;
static void check(int ok, const char *what)
{
    uint32_t n = 0;
    printf("play: ");
    for (; *what; what++, n++)                   /* (the firmware's Latin-1 middle dot, as UTF-8) */
        if ((uint8_t)*what == 0xB7)
            printf("\xc2\xb7");
        else
            putchar(*what);
    printf("%*s %s\n", n < 96 ? (int)(96 - n) : 0, "", ok ? "ok" : "FAIL");
    fails += !ok;
}

/* ---- what ui_play.c drew since pt_reset */
#define PT_N 256
static char pt_seen_s[PT_N][48];
static uint32_t pt_n;
static void pt_text(const char *s)
{
    if (pt_n < PT_N && s[0])
        snprintf(pt_seen_s[pt_n++], sizeof pt_seen_s[0], "%s", s);
}
static void pt_reset(void) { pt_n = 0; }
static int seen(const char *s)
{
    uint32_t i;
    for (i = 0; i < pt_n; i++)
        if (!strcmp(pt_seen_s[i], s))
            return 1;
    return 0;
}

/* ---- the panel and time */
static void frame(void)
{
    static int16_t pcm[2 * HOST_BLOCK * HOST_FRAME_BLOCKS];
    host_audio(pcm, HOST_BLOCK * HOST_FRAME_BLOCKS);
    host_ui_frame();
}
static void frames(uint32_t n)
{
    while (n--)
        frame();
}
static void wait_ms(uint32_t ms)
{
    uint32_t t0 = fm1_ms;
    while (fm1_ms - t0 < ms)
        frame();
}
static void press(uint32_t b) { host_button(b, 1); frame(); }
static void release(uint32_t b) { host_button(b, 0); frame(); }
static void tap(uint32_t b) { press(b); release(b); }
static void turn(uint32_t role, int32_t s) { host_turn(role, s); frame(); }
static void key_tap(uint32_t k) { host_key(k, 1); frame(); frame(); host_key(k, 0); frame(); }
static void redraw(void)                         /* every band again, the texts recorded */
{
    pt_reset();
    pl.force = 1;
    frame();
}
static void shot(const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/play-%s.ppm", outdir, name);
    host_lcd_ppm(p);
}
static uint32_t lit(int x, int y, int w, int h)  /* pixels not black in a rectangle */
{
    uint32_t n = 0;
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            n += host_fb[j * 240 + i] != 0;
    return n;
}
static int fidx(const char *name) { return host_world_factory_find(name); }
static uint32_t wid(const char *name) { return WORLD_INDEX[fidx(name)].id; }
static void wait_bar(void)                       /* to just after the next bar line */
{
    uint32_t b = clk_beat >> 2;
    while ((clk_beat >> 2) == b && song.playing)
        frame();
    frames(2);
}
static void stop_quiet(void)
{
    host_stop();
    wait_ms(8000);
}
/* the cost of what follows: pixels sent to the panel */
static uint64_t px0;
static void cost_begin(void) { px0 = host_lcd_px; }
static void cost_end(const char *what)
{
    uint64_t px = host_lcd_px - px0;
    printf("play: cost  %-58s %7llu px  %7llu B  %5.1f ms SPI\n", what, (unsigned long long)px,
           (unsigned long long)px * 2u, px * 0.67 / 1000.0);
}

/* ---- the scenarios */
static void boot(int device, const char *flash)
{
    host_boot_device(device);
    if (host_boot(flash)) {
        printf("play: cannot boot\n");
        exit(1);
    }
    frames(2);
}

static void sc_first(void)
{
    boot(1, NULL);
    check(wrt.mode == WM_PLAY && wrt.active && wrt.id == PL_FIRST_WORLD, "first boot: PLAY MODE, NEON RAIN (by its id)");
    check(pl.scr == PS_FIRST && !song.playing, "first boot: the FIRST screen, stopped");
    redraw();
    check(seen("FLOWSTATE") && seen("READY") && seen("NEON RAIN") && seen("CINEMATIC") && seen("PRESS PLAY") &&
          seen("~ ~ ~~ ~~~ ~~ ~"), "FIRST: FLOWSTATE READY, NEON RAIN, CINEMATIC, the waveform, PRESS PLAY");
    check(seen("COLOR") && seen("MOTION") && seen("SPACE") && seen("ENERGY") && seen("50") && seen("55"),
          "FIRST: the four macros with their values (NEON RAIN: 50 50 50 55)");
    check(lit(0, 20, 240, 36) > 300 && lit(0, 156, 240, 84) > 600, "FIRST: the name and the controls are on the screen");
    shot("first");
    frames(10);
    check(pl.scr == PS_FIRST, "FIRST stays until PLAY (no timeout)");
    tap(B_PLAY);
    frames(3);
    check(song.playing && pl.scr == PS_HOME && pl.played, "PLAY: the music starts, HOME");
    redraw();
    check(seen("NEON RAIN") && seen("CINEMATIC \xB7 SCENE B") && seen("KEYS: SMART MELODY") && seen("FLOWSTATE"),
          "HOME: FLOWSTATE, NEON RAIN, CINEMATIC \xB7 SCENE B, KEYS: SMART MELODY");
    check(host_fb[10 * 240 + 224] == 0xFFFF, "HOME: the play triangle in the top band");
}

static void sc_screens(void)
{
    static step_t keys_steps[NSTEP];
    int i, ok;
    uint32_t v, px, t0;
    char b[64];
    boot(1, NULL);
    cost_begin();
    redraw();
    cost_end("FIRST, full redraw");
    wait_ms(500);
    cost_begin();
    wait_ms(2000);
    cost_end("FIRST stopped, 2 s: nothing changes, nothing sent");
    check(host_lcd_px == px0, "stopped and untouched: no redraw");
    tap(B_PLAY);
    wait_ms(1000);
    for (i = 0; i < 4; i++)                      /* a phrase on the Smart Keys: the middle bar moves */
        host_key((uint32_t)(5 + 2 * i), 1), frames(6), host_key((uint32_t)(5 + 2 * i), 0), frames(3);
    host_key(9, 1);
    frames(8);
    shot("home");
    cost_begin();
    t0 = fm1_ms;
    wait_ms(2000);
    cost_end("HOME playing: the activity bars, 2 s (15 fps at most)");
    px = (uint32_t)(host_lcd_px - px0);
    check(px <= 2u * 15u * 152u * 46u + 60u * 2400u, "HOME: the bars redraw at most 15 times a second, only their band");
    host_key(9, 0);
    {   /* the key LEDs: the chord's tones and the tonic dim, a key held on */
        uint32_t ck = sk_chord_keys();
        ok = ck != 0 && host_led(14 + 7) >= 1;   /* the C4 key: the tonic */
        host_key(3, 1);
        frames(2);
        ok = ok && host_led(14 + 3) == 2;
        host_key(3, 0);
        frames(2);
        check(ok, "LEDs: the tonic and the chord's tones dim, a key held lit");
        for (i = 0, ok = 0; i < 60; i++, frame())
            ok |= 1 << host_button_led(B_PLAY);
        check(ok == 5, "LEDs: PLAY flashes with the beat (on and off within a second)");
    }
    /* MACRO */
    cost_begin();
    turn(EN_K3, 1);
    cost_end("MACRO opens (SPACE, first detent)");
    wait_ms(100);
    cost_begin();
    turn(EN_K3, 1);
    cost_end("MACRO, one more detent (the bar, the digits, the column)");
    check(macro_pos(2) == 520 && pl.scr == PS_MACRO, "K3: SPACE, 10 units a detent, the MACRO overlay");
    redraw();
    check(seen("SPACE") && seen("CLOSE") && seen("HUGE") && seen("52") && seen("NEON RAIN \xB7 SCENE B"),
          "MACRO: SPACE, CLOSE \xe2\x94\x81\xe2\x97\x8f HUGE, 52, NEON RAIN \xB7 SCENE B");
    turn(EN_K3, 2);
    turn(EN_K3, 2);
    check(macro_pos(2) > 520 + 40, "K3 turned fast: accelerated (x3)");
    shot("macro");
    for (i = 0; i < 4; i++) {
        static const char *const E[4][2] = {{"DARK", "BRIGHT"}, {"STILL", "ALIVE"}, {"CLOSE", "HUGE"}, {"SPARSE", "INTENSE"}};
        wait_ms(1600);
        turn(EN_K1 + (uint32_t)i, -1);
        redraw();
        check(seen(E[i][0]) && seen(E[i][1]) && pl.ctl == (uint32_t)i, i == 0 ? "MACRO: COLOR's ends DARK / BRIGHT" :
              i == 1 ? "MACRO: MOTION's STILL / ALIVE" : i == 2 ? "MACRO: SPACE's CLOSE / HUGE" : "MACRO: ENERGY's SPARSE / INTENSE");
    }
    wait_ms(1400);
    check(pl.scr == PS_MACRO, "MACRO: still up 1.4 s after the last detent");
    wait_ms(200);
    check(pl.scr == PS_HOME, "MACRO: back to HOME 1.5 s after it");
    /* CHOOSE WORLD */
    cost_begin();
    turn(EN_PRESET, 1);
    cost_end("CHOOSE WORLD opens (full body)");
    check(pl.scr == PS_WORLDS && pl.browse == (uint32_t)fidx("NEON RAIN") + 1u, "PRESETS: CHOOSE WORLD, one detent moves");
    redraw();
    check(seen("CHOOSE WORLD") && seen("Neon Rain") && seen("Frozen Lake") && seen("Dusty Cafe") && seen("LO-FI") &&
          seen("PRESS PLAY"), "CHOOSE WORLD: the Worlds, the highlighted one's category, PRESS PLAY");
    shot("worlds");
    cost_begin();
    turn(EN_PRESET, 1);
    cost_end("CHOOSE WORLD, one detent (two rows, the category)");
    check(wrt.id == PL_FIRST_WORLD && song.playing, "CHOOSE WORLD: the World playing plays on");
    tap(B_HOME);
    check(pl.scr == PS_HOME && wrt.id == PL_FIRST_WORLD && !wreq.sw, "HOME: the choice is forgotten");
    turn(EN_PRESET, 2);
    wait_ms(3900);
    check(pl.scr == PS_WORLDS, "CHOOSE WORLD: still up 3.9 s after");
    wait_ms(200);
    check(pl.scr == PS_HOME && wrt.id == PL_FIRST_WORLD, "CHOOSE WORLD: 4 s idle cancels");
    turn(EN_PRESET, 3);                          /* MIDNIGHT DRIVE (the last) */
    check(pl.browse == (uint32_t)fidx("MIDNIGHT DRIVE"), "CHOOSE WORLD: clamps at the list's end");
    tap(B_PLAY);
    check(wreq.sw == 1 && wrt.id == PL_FIRST_WORLD && song.playing, "PLAY: playing, the switch waits for the bar");
    wait_bar();
    frames(3);
    check(wrt.id == wid("MIDNIGHT DRIVE") && song.playing && song.g[G_BPM] == 100 && wrt.mode == WM_PLAY,
          "PLAY: MIDNIGHT DRIVE from the next bar, still playing, its tempo");
    stop_quiet();
    turn(EN_PRESET, -3);
    tap(B_PLAY);
    frames(2);
    check(wrt.id == wid("FROZEN LAKE") && song.playing, "stopped: PLAY loads the World at once and starts it");
    /* SCENES */
    cost_begin();
    turn(EN_SELECT, 1);
    cost_end("SCENES opens (full body)");
    frames(1);
    {
        uint32_t ps = 0, pv = 0;
        check(pl.scr == PS_SCENES && world_pending(&ps, &pv) && ps == 2 && wrt.scene == 1,
              "SELECT: SCENES, C asked for, B plays until the bar");
    }
    redraw();
    snprintf(b, sizeof b, "%s", world_scene_name(2));
    check(seen("FROZEN LAKE") && seen("A") && seen("D") && seen(b) && seen("CHANGES NEXT BAR"),
          "SCENES: the World, A..D with their names, CHANGES NEXT BAR");
    shot("scenes");
    cost_begin();
    turn(EN_SELECT, 1);
    frames(1);
    cost_end("SCENES, one detent (the rows that changed)");
    turn(EN_SELECT, -1);
    wait_bar();
    check(wrt.scene == 2, "SCENES: C from the next bar");
    wait_ms(2600);
    check(pl.scr == PS_HOME, "SCENES: back to HOME 2.5 s after");
    /* VARIATION */
    turn(EN_ALGO, 1);
    redraw();
    snprintf(b, sizeof b, "%s", world_var_name(1));
    check(pl.scr == PS_VARS && seen("VARIATION 02") && seen(b) && seen("SAME WORLD \xB7 NEW FEEL"),
          "ALGORITHM: VARIATION 02, its name, SAME WORLD \xB7 NEW FEEL");
    shot("variation");
    wait_bar();
    check(wrt.var == 1, "VARIATION: from the next bar");
    wait_ms(2600);
    check(pl.scr == PS_HOME, "VARIATION: back to HOME");
    redraw();
    snprintf(b, sizeof b, "AMBIENT \xB7 SCENE C \xB7 %s", world_var_name(1));
    check(seen(b), "HOME: AMBIENT \xB7 SCENE C \xB7 the variation");
    /* PULSE, BEAT */
    tap(B_ARP);
    check(pl.scr == PS_PULSE && pl_pulse() == 0, "ARP: the PULSE list, OFF");
    tap(B_ARP);
    check(pl_pulse() == 1 && trk[wrt.keys_trk].p[P_AMODE] == WB_PULSE[1][0], "ARP again: SLOW (the keys track's arp)");
    turn(EN_K2, 1);
    check(pl_pulse() == 2, "a knob steps it: PULSE");
    redraw();
    check(seen("PULSE") && seen("OFF") && seen("SLOW") && seen("DRIVE") && seen("HOLD KEYS AND LISTEN"),
          "PULSE: OFF SLOW PULSE DRIVE, HOLD KEYS AND LISTEN");
    shot("pulse");
    check(host_button_led(B_ARP) == 2, "LED: ARP lit (PULSE not OFF)");
    wait_ms(2600);
    check(pl.scr == PS_HOME, "PULSE: closes after 2.5 s");
    tap(B_SEQ);
    check(pl.scr == PS_BEAT && wrt.beat == 1, "SEQ: the BEAT list, GROOVE");
    tap(B_SEQ);
    check(wrt.beat == 2, "SEQ again: BUSY");
    redraw();
    check(seen("BEAT") && seen("MINIMAL") && seen("GROOVE") && seen("BUSY") && seen("BREAK") &&
          seen("TURN TO CHANGE FEEL"), "BEAT: MINIMAL GROOVE BUSY BREAK, TURN TO CHANGE FEEL");
    shot("beat");
    check(host_button_led(B_SEQ) == 2, "LED: SEQ lit (BEAT not GROOVE)");
    turn(EN_K1, -1);
    check(wrt.beat == 1 && host_button_led(B_SEQ) == 0, "a knob back: GROOVE, SEQ unlit");
    /* the pages */
    wait_ms(100);
    cost_begin();
    press(B_FX);
    cost_end("LIVE FX opens (FX held: the middle, the CONTROLS relabelled)");
    cost_begin();
    turn(EN_K2, 3);
    cost_end("LIVE FX, a knob (the digits and the bar of its column)");
    turn(EN_K1, -1);
    redraw();
    check(pl.scr == PS_PAGE && seen("LIVE FX") && seen("FILTER") && seen("ECHO") && seen("CRUSH") && seen("FREEZE") &&
          seen("K1") && seen("K4") && seen("49") && seen("03") && seen("00") && seen("KEYS: PUNCH FX"),
          "FX held: LIVE FX, FILTER ECHO CRUSH FREEZE on K1..K4: 49 03 00 00");
    check(macro_pos(13) == 30 && macro_pos(12) == 490 && punch.hold && host_button_led(B_FX) == 2,
          "FX held: K2 = ECHO (control 13), K1 = FILTER (12); the punch layer on");
    shot("livefx");
    release(B_FX);
    frames(2);
    check(macro_pos(13) == 0 && macro_pos(12) == 500 && !punch.hold && pl.scr == PS_HOME,
          "FX let go: LIVE FX back home (the clean way back), HOME");
    wait_ms(100);
    press(B_ENV);
    turn(EN_K1, 3);
    redraw();
    check(seen("SOUND SHAPE") && seen("SOFT") && seen("SHORT") && seen("BODY") && seen("TAIL") && macro_pos(4) == 30,
          "ENV held: SOUND SHAPE, SOFT SHORT BODY TAIL; K1 = SOFT (control 4)");
    shot("shape");
    release(B_ENV);
    frames(2);
    check(macro_pos(4) == 30 && host_button_led(B_ENV) == 1, "ENV let go: SOFT stays, ENV dim (off home)");
    press(B_LFO);
    turn(EN_K4, -2);
    redraw();
    check(seen("MOVEMENT") && seen("DRIFT") && seen("WOBBLE") && seen("PULSE") && seen("RATE") && macro_pos(11) == 480,
          "LFO held: MOVEMENT, DRIFT WOBBLE PULSE RATE; K4 = RATE (control 11)");
    shot("movement");
    release(B_LFO);
    tap(B_ENV);
    check(pl.scr == PS_PAGE && pl.ctl == PG_SHAPE, "ENV tapped: SOUND SHAPE stays a moment");
    turn(EN_K2, 1);
    check(macro_pos(5) == 10, ".. its knobs edit it meanwhile");
    wait_ms(2600);
    check(pl.scr == PS_HOME, ".. then HOME");
    /* REC (play_rec.c; the recording itself, timing and guard: tests/play_rec_test.c) */
    memcpy(keys_steps, trk[wrt.keys_trk].step, sizeof keys_steps);
    tap(B_REC);
    check(prec.st == PR_TAKE && pl.scr == PS_REC && host_button_led(B_REC) == 2, "REC playing: RECORDING, REC lit");
    redraw();
    check(seen("RECORDING...") && seen("PLAY SOMETHING") && seen("SMART KEYS ACTIVE") && prec_bars() == 0,
          "RECORDING...: the dots (none filled before a note), PLAY SOMETHING, SMART KEYS ACTIVE");
    wait_bar();
    for (i = 0; i < 4; i++)                      /* a phrase over two bars: the first */
        key_tap((uint32_t)(7 + (i * 3) % 8)), wait_ms(700);
    wait_bar();
    wait_ms(400);                                /* (past the bar's first step: REC closes on the next line) */
    redraw();
    check(prec_bars() == 2 && host_button_led(B_EDIT) == 0, "RECORDING: a dot a bar (2 of 4 after a bar and a bit); "
          "no UNDO while the take runs");
    shot("recording");
    tap(B_REC);
    v = clk_beat;
    for (i = 0; i < 3; i++)                      /* .. the second: recorded until the bar line */
        key_tap((uint32_t)(9 + (i * 5) % 9)), wait_ms(600);
    for (i = 0; i < 2000 && prec.st == PR_TAKE; i++)
        frame();
    check(prec.st == PR_LOOP && pl.scr == PS_LOOP && prec.layers == 1 && (clk_beat & 3u) == 0 && clk_beat > v,
          "REC: the take closes on the next bar line and loops (LOOP 1)");
    check(trk[wrt.keys_trk].p[P_SLEN] == 32 && prec_has(&trk[wrt.keys_trk]), "the loop: 2 bars of what was played");
    frames(2);
    check(host_button_led(B_REC) == 1 && host_button_led(B_EDIT) == 1, "LOOP: REC dim (a loop), EDIT dim (UNDO ready)");
    tap(B_REC);
    key_tap(12);
    wait_ms(300);
    redraw();
    check(prec.st == PR_OVERDUB && seen("LOOP 2") && seen("PLAYING + REC") && seen("TAP KEYS TO ADD MORE") &&
          seen("UNDO READY"), "REC: overdub, a note: LOOP 2, PLAYING + REC, TAP KEYS TO ADD MORE \xB7 UNDO READY");
    for (i = 0, ok = 0; i < 40; i++, frame())
        ok |= 1 << host_button_led(B_REC);
    check(ok == 5, "overdub: REC blinks");
    shot("loop");
    tap(B_REC);
    check(prec.st == PR_LOOP && pl.scr == PS_LOOP, "REC: overdub off (LOOP a moment)");
    tap(B_EDIT);
    redraw();
    check(prec.layers == 1 && seen("UNDONE") && seen("LOOP 1"), "EDIT tapped: UNDONE (LOOP 1 again)");
    press(B_EDIT);
    tap(B_OCTUP);
    release(B_EDIT);
    redraw();
    check(prec.layers == 2 && seen("REDONE") && song.octave == 0 && pl.scr != PS_ADVDLG,
          "EDIT held + OCT+: REDONE (no octave, no ADVANCED)");
    press(B_REC);
    wait_ms(1000);
    check(prec.st == PR_LOOP && pl.hold == PH_CLEAR, "REC held: the press undone, the clear ring");
    redraw();
    check(seen("CLEAR LOOP"), "the ring: CLEAR LOOP");
    shot("clear");
    wait_ms(700);
    release(B_REC);
    redraw();
    check(prec.st == PR_EMPTY && pl.hold == PH_NONE && !prec_has(&trk[wrt.keys_trk]) && seen("LOOP CLEARED"),
          "REC held 1.5 s: LOOP CLEARED");
    tap(B_EDIT);
    check(prec.st == PR_LOOP && prec.layers == 2, "EDIT: the clear undone, the loop back");
    press(B_REC);
    wait_ms(1600);
    release(B_REC);
    check(prec.st == PR_EMPTY && !memcmp(keys_steps, trk[wrt.keys_trk].step, sizeof keys_steps),
          "cleared again: the keys track as before REC");
    /* SAVE */
    tap(B_SAVE);
    redraw();
    check(pl.scr == PS_SAVE && seen("SAVE") && seen("SAVE AS USER WORLD") && seen("RESET WORLD"), "SAVE: the menu");
    shot("save");
    tap(B_HOME);
    check(pl.scr == PS_HOME, "HOME: the menu closes");
    /* SCL, GLO, OCT */
    tap(B_SCL);
    redraw();
    check(seen("KEYS: SMART MELODY") && pl.toast_t, "SCL: KEYS: SMART MELODY (the one mode yet)");
    v = (uint32_t)song.g[G_BPM];
    press(B_GLO);
    turn(EN_SELECT, 3);
    release(B_GLO);
    check((uint32_t)song.g[G_BPM] == v + 3 && wrt.scene == 2, "GLO + SELECT: the tempo, not the scene");
    press(B_GLO);
    turn(EN_SELECT, 40);
    release(B_GLO);
    {
        uint32_t lo, hi;
        world_tempo(&lo, &hi);
        check((uint32_t)song.g[G_BPM] == hi, "GLO + SELECT: within the World's range");
    }
    redraw();
    check(seen("SMART MELODY"), "HOME: the tempo shown in the KEYS band");
    wait_ms(500);
    tap(B_GLO);
    tap(B_GLO);
    check((uint32_t)song.g[G_BPM] == world_bpm(), "GLO tapped twice: the World's tempo");
    tap(B_OCTUP);
    check(song.octave == 1 && host_button_led(B_OCTUP) == 2, "OCT+: an octave up, lit");
    press(B_OCTDN);
    press(B_OCTUP);
    release(B_OCTUP);
    release(B_OCTDN);
    check(song.octave == 0, "OCT- + OCT+: back to 0");
    /* EDIT: the dialog, ADVANCED and back */
    press(B_EDIT);
    wait_ms(1000);
    check(pl.hold == PH_EDIT, "EDIT held 1 s: the ring (HOLD FOR ADVANCED)");
    redraw();
    check(seen("HOLD FOR ADVANCED"), "the ring: HOLD FOR ADVANCED");
    cost_begin();
    wait_ms(1100);
    cost_end("EDIT held 1 to 2 s: the ring (5 % steps), the dialog");
    check(pl.scr == PS_ADVDLG, "EDIT held 2 s untouched: the ADVANCED dialog");
    release(B_EDIT);
    redraw();
    check(seen("ADVANCED MODE") && seen("FULL SLOOP CONTROL") && seen("ENTER / CANCEL"),
          "ADVANCED MODE, FULL SLOOP CONTROL, ENTER / CANCEL");
    shot("advanced");
    tap(B_SCL);
    check(pl.scr == PS_HOME && wrt.mode == WM_PLAY, "another button: cancelled");
    press(B_EDIT);
    wait_ms(1000);
    key_tap(10);
    wait_ms(1500);
    check(pl.scr != PS_ADVDLG, "EDIT held with a key played meanwhile: no dialog");
    release(B_EDIT);
    press(B_EDIT);
    wait_ms(2100);
    release(B_EDIT);
    wait_ms(5100);
    check(pl.scr == PS_HOME, "the dialog: 5 s and it is gone");
    press(B_EDIT);
    wait_ms(2100);
    release(B_EDIT);
    tap(B_EDIT);
    frames(2);
    check(wrt.mode == WM_ADV && wrt.active && !wrt.keys_on && ly_bit[LY_ERASE] && ly_bit[LY_STEP],
          "EDIT tapped in the dialog: ADVANCED (SLOOP's UI, kb_map, every layer)");
    v = macro_pos(2);
    turn(EN_K3, 2);
    check(macro_pos(2) == v, "ADVANCED: the macros frozen (K3 is SLOOP's)");
    shot("adv-sloop");
    /* H17: SAVE + key 1..4 in a World session: its scenes; store refused */
    press(B_SAVE);
    key_tap((uint32_t)key_of_white(0));
    {
        uint32_t ps = 9, pv = 9;
        check(world_pending(&ps, &pv) && ps == 0, "ADVANCED: SAVE + key 1 asks for World scene A (H17)");
    }
    key_tap((uint32_t)key_of_white(4));
    check(!strcmp(ui.msg, "SCENES ARE THE WORLD'S"), "ADVANCED: SAVE + key 5 (store) refused");
    key_tap((uint32_t)key_of_white(12));
    check(!strcmp(ui.msg, "NO SONG IN A WORLD") && !arrangement_enabled, "ADVANCED: song mode refused (H26)");
    release(B_SAVE);
    press(B_EDIT);
    wait_ms(1000);
    key_tap(12);
    wait_ms(1200);
    release(B_EDIT);
    check(wrt.mode == WM_ADV, "ADVANCED: EDIT held with a key (erase): stays");
    press(B_EDIT);
    wait_ms(2100);
    check(wrt.mode == WM_PLAY && wrt.keys_on && !ly_bit[LY_ERASE] && ly_bit[LY_FX], "ADVANCED: EDIT held 2 s: PLAY again");
    release(B_EDIT);
    frames(2);
    check(pl.scr != PS_ADVDLG && wrt.mode == WM_PLAY, ".. its release is no tap");
    /* the menu: LEAVE WORLD */
    press(B_HOME);
    wait_ms(800);
    release(B_HOME);
    check(ui.menu == 1, "HOME held: the menu");
    redraw();
    draw_menu();
    shot("menu");
    {
        uint8_t ids[MI_COUNT];
        uint32_t n = menu_items(ids);
        check(n == 7 && ids[0] == MI_LEAVE, "PLAY MODE's menu: LEAVE WORLD first, then SLOOP's settings");
    }
    tap(B_OCTUP);
    frames(2);
    check(wrt.mode == WM_SLOOP && !wrt.active && !song.playing, "LEAVE WORLD: SLOOP (stopped)");
    press(B_HOME);
    wait_ms(800);
    release(B_HOME);
    {
        uint8_t ids[MI_COUNT];
        check(menu_items(ids) == 7 && ids[0] == MI_PLAY, "SLOOP's menu: PLAY MODE first");
    }
    tap(B_OCTUP);
    frames(2);
    check(wrt.mode == WM_PLAY && wrt.id == wid("FROZEN LAKE"), "PLAY MODE: the last World again");
    /* EDIT 2 s in SLOOP enters PLAY too */
    play_leave();
    frames(2);
    press(B_EDIT);
    wait_ms(2100);
    release(B_EDIT);
    frames(2);
    check(wrt.mode == WM_PLAY && pl.scr != PS_ADVDLG, "SLOOP: EDIT held 2 s untouched: PLAY MODE");
}

/* LEAVE WORLD brings the SLOOP project back bit-identically (design 13: load then unload) */
static uint32_t render_hash(uint32_t blocks)
{
    static int16_t pcm[2 * HOST_BLOCK];
    uint32_t h = 2166136261u, i, k;
    host_play();
    for (i = 0; i < blocks; i++) {
        host_audio(pcm, HOST_BLOCK);
        if (i % HOST_FRAME_BLOCKS == 0)
            host_ui_frame();
        for (k = 0; k < 2u * HOST_BLOCK; k++)
            h = (h ^ (uint16_t)pcm[k]) * 16777619u;
    }
    return h;
}
static void sc_leave(void)
{
    static project_t p0, p1;
    int fd[2], st;
    uint32_t ha = 0, hb, i;
    pid_t pid;
    boot(0, NULL);
    check(wrt.mode == WM_SLOOP, "host boot: SLOOP (host_boot_device 0)");
    if (host_project_load("examples/projects/groove.fun4") < 0) {
        check(0, "examples/projects/groove.fun4");
        return;
    }
    song.octave = -1;
    song.solo = 4;
    frames(10);
    proj_capture(&p0);
    if (pipe(fd))
        return;
    fflush(stdout);
    if (!(pid = fork())) {                       /* the reference: no World */
        frames(50);
        ha = render_hash(4000);
        fflush(stdout);
        if (write(fd[1], &ha, 4) != 4)
            _exit(1);
        _exit(0);
    }
    close(fd[1]);
    /* design 13: a World loaded, then unloaded (PLAY MODE's way in and out: the project parked, put back) */
    check(play_from_sloop() == 0 && wrt.mode == WM_PLAY && wrt.active && wrt.id == PL_FIRST_WORLD,
          "SLOOP -> PLAY MODE: NEON RAIN loaded over the parked project");
    play_leave();
    check(wrt.mode == WM_SLOOP && !wrt.active, "LEAVE WORLD: SLOOP");
    proj_capture(&p1);
    check(!memcmp(&p0, &p1, sizeof p0) && song.octave == -1 && song.solo == 4 && song.sel == p0.sel,
          "LEAVE WORLD: the SLOOP project bit-identical (project_t, octave, solo, track)");
    frames(50);
    hb = render_hash(4000);
    if (read(fd[0], &ha, 4) != 4)
        ha = ~hb;
    waitpid(pid, &st, 0);
    check(ha == hb, "load then LEAVE WORLD: it renders bit-identically to the project with no World (4000 blocks)");
    /* through the panel, the World heard, then LEAVE WORLD: the project as it was (the LFOs and the noise generator ran
     * on meanwhile, as they do in SLOOP: only the project is compared) */
    host_stop();
    frames(20);
    proj_capture(&p0);
    press(B_HOME);
    wait_ms(800);
    release(B_HOME);
    tap(B_OCTUP);                                /* the menu's PLAY MODE */
    check(wrt.mode == WM_PLAY && wrt.active, "SLOOP's menu: PLAY MODE");
    host_world_load(fidx("DUSTY CAFE"));
    host_play();
    for (i = 0; i < 300; i++) {
        if (i % 20 == 0)
            host_key(i % 27, 1);
        if (i % 20 == 10)
            host_key((i - 10) % 27, 0);
        frame();
    }
    turn(EN_K1, 5);
    turn(EN_SELECT, 1);
    tap(B_ARP);
    tap(B_ARP);
    wait_ms(2000);
    press(B_HOME);
    wait_ms(800);
    release(B_HOME);
    tap(B_OCTUP);                                /* LEAVE WORLD */
    frames(4);
    proj_capture(&p1);
    check(!memcmp(&p0, &p1, sizeof p0) && wrt.mode == WM_SLOOP && !song.playing,
          "a World played, then the menu's LEAVE WORLD: the project as it was, stopped");
}

static void sc_persist(void)
{
    char img[600];
    int st;
    pid_t pid;
    snprintf(img, sizeof img, "%s/play-session.bin", outdir);
    fflush(stdout);
    if (!(pid = fork())) {                       /* a session, saved when quiet */
        uint32_t c0, i;
        boot(1, NULL);
        tap(B_PLAY);
        turn(EN_PRESET, 1);                      /* DUSTY CAFE (after NEON RAIN by category) */
        tap(B_PLAY);
        wait_bar();
        frames(3);
        turn(EN_SELECT, 1);
        turn(EN_ALGO, 2);
        wait_bar();
        frames(3);
        turn(EN_K1, -3);
        turn(EN_K4, 2);
        tap(B_ARP);
        tap(B_ARP);
        tap(B_SEQ);
        tap(B_SEQ);
        tap(B_SEQ);
        tap(B_OCTUP);
        press(B_GLO);
        turn(EN_SELECT, 2);
        release(B_GLO);
        press(B_ENV);
        turn(EN_K3, 4);
        release(B_ENV);
        host_stop();
        c0 = host_flash_changes();
        wait_ms(3000);
        check(host_flash_changes() == c0, "the session waits: 20 s since boot, 2.5 s without input");
        for (i = 0; i < 40 && host_flash_changes() == c0; i++)
            wait_ms(1000);
        check(host_flash_changes() != c0, "stopped, quiet, changed: the session saved");
        {
            uint32_t s, ok = 1;
            for (s = 0xE5000; s < 0xFC000; s += 0x1000)
                for (i = 0; i < 80; i++)
                    ok &= host_nor[s + 0xF00 + i] == 0xFF;
            check(ok, "the region's sectors: offset 0xF00 left erased (no update record look-alike)");
        }
        printf("play: saved: %s scene %c var %u ctl %u %u %u pulse %u beat %u oct %d bpm %d\n", world_name(),
               'A' + wrt.scene, wrt.var, macro_pos(0), macro_pos(3), macro_pos(6), pl_pulse(), wrt.beat, song.octave,
               song.g[G_BPM]);
        host_flash_write(img);
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
    fflush(stdout);
    if (!(pid = fork())) {                       /* the reboot */
        boot(1, img);
        check(wrt.mode == WM_PLAY && wrt.id == wid("DUSTY CAFE") && wrt.scene == 2 && wrt.var == 2 && !song.playing,
              "reboot: PLAY MODE, DUSTY CAFE, scene C, variation 3, stopped");
        check(macro_pos(0) == 470 && macro_pos(3) == 520 && macro_pos(6) == 40 && pl_pulse() == 1 && wrt.beat == 3,
              "reboot: the controls, PULSE, BEAT as they were");
        check(song.octave == 1 && song.g[G_BPM] == 84 && pl.scr == PS_HOME && pl.played,
              "reboot: octave, tempo; HOME (not FIRST: PLAY was pressed)");
        redraw();
        check(seen("DUSTY CAFE"), "reboot: HOME shows DUSTY CAFE");
        play_leave();                            /* SLOOP from now on */
        wait_ms(25000);
        host_flash_write(img);
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
    fflush(stdout);
    if (!(pid = fork())) {
        boot(1, img);
        check(wrt.mode == WM_SLOOP && !wrt.active, "reboot after LEAVE WORLD: SLOOP, as SLOOP boots");
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
    fflush(stdout);
    if (!(pid = fork())) {                       /* a project parked, flash, a reboot in PLAY, LEAVE WORLD: it back */
        static project_t p0, p1;
        boot(0, NULL);
        host_project_load("examples/projects/cinematic.fun4");
        frames(4);
        proj_capture(&p0);
        play_menu(0);
        frames(2);
        wait_ms(26000);                          /* (quiet: the parked project and the session go to flash) */
        host_flash_write(img);
        fflush(stdout);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    fflush(stdout);
    if (!(pid = fork())) {
        static project_t p0, p1;
        int fmt;
        boot(1, img);
        check(wrt.mode == WM_PLAY && wrt.id == PL_FIRST_WORLD && pl.scr == PS_FIRST,
              "a project, PLAY MODE, reboot: PLAY MODE, NEON RAIN, FIRST (never played)");
        fmt = host_read("examples/projects/cinematic.fun4", &p0) > 0;
        play_leave();
        frames(2);
        proj_capture(&p1);
        p0.g[G_SLOT] = p1.g[G_SLOT];             /* (not a project's: proj_apply keeps the panel's) */
        p0.g[G_LOAD] = p1.g[G_LOAD];
        p0.g[G_SAVE] = p1.g[G_SAVE];
        check(fmt && !memcmp(p0.t, p1.t, sizeof p0.t) && !memcmp(p0.g, p1.g, sizeof p0.g) && p0.sel == p1.sel,
              "LEAVE WORLD after the reboot: the project parked before it");
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
}

static uint32_t lcg = 12345u;
static uint32_t rnd(uint32_t n)
{
    lcg = lcg * 1103515245u + 12345u;
    return (lcg >> 8) % n;
}
static void sc_fuzz(uint32_t n)
{
    uint32_t i, bad = 0, keys = 0, btn = 0, modes[3] = {0}, scr[PS_COUNT] = {0}, nscr = 0, quiet = 0, hold_edit = 0;
    host_state_t st;
    boot(1, NULL);
    for (i = 0; i < n; i++) {
        uint32_t r = rnd(100), b;
        if (quiet) {                             /* EDIT alone for 2.2 s (the dialog, ADVANCED and back), or nothing */
            if (!--quiet && hold_edit) {
                btn &= ~(1u << B_EDIT);
                host_button(B_EDIT, 0);
                if (rnd(2))
                    quiet = 3, hold_edit = 2;    /* .. then an EDIT tap: ADVANCED (or back to PLAY) */
            } else if (hold_edit == 2 && quiet == 2) {
                host_button(B_EDIT, 1);
            } else if (hold_edit == 2 && quiet == 1) {
                host_button(B_EDIT, 0);
                hold_edit = 0;
            }
            r = 100;
        } else if (rnd(250) == 0) {
            keys = btn = 0;
            host_in_set(0, 0);
            if (ui.menu)
                ui.menu = 0, ui.force = 1;
            hold_edit = rnd(4) != 0;
            if (hold_edit)
                btn |= 1u << B_EDIT, host_button(B_EDIT, 1);
            quiet = 140 + rnd(200);
            r = 100;
        }
        if (r < 30) {                            /* a key */
            keys ^= 1u << rnd(27);
            host_in_set(keys, fm1_in.buttons);
        } else if (r < 52) {                     /* a button down or up (never both OCT buttons held long) */
            b = rnd(NB);
            btn ^= 1u << b;
            host_button(b, (btn >> b) & 1u);
        } else if (r < 85) {
            host_turn(rnd(NE), (int32_t)rnd(7) - 3);
        } else if (r < 86 && rnd(4) == 0) {
            play_menu(rnd(3) == 0);              /* (the menu's PLAY MODE / LEAVE WORLD) */
        } else if (r < 90) {
            btn = 0;
            host_in_set(keys, 0);
        }
        if ((btn >> B_OCTDN & 1u) && (btn >> B_OCTUP & 1u) && rnd(3) == 0) {
            btn &= ~(1u << B_OCTUP);
            host_button(B_OCTUP, 0);
        }
        if (ui.menu && rnd(8) == 0) {            /* (the menu is tested above: no calibration from here) */
            ui.menu = 0;
            ui.force = 1;
        }
        if (ui.menu && (btn >> B_OCTUP & 1u)) {
            btn &= ~(1u << B_OCTUP);
            host_button(B_OCTUP, 0);
        }
        frame();
        modes[wrt.mode % 3u]++;
        if (wrt.mode == WM_PLAY && !scr[pl.scr % PS_COUNT]++)
            nscr++;
        if (wrt.mode > WM_ADV || pl.scr >= PS_COUNT || prec.st >= PR_COUNT || pl.hold > PH_CLEAR || pl.hot > 4 ||
            prec.rc > prec.rn || prec.rn > PR_RING || prec.rh >= PR_RING ||
            (wrt.mode != WM_SLOOP && !wrt.active && !wreq.sw) || (wrt.mode == WM_SLOOP && wrt.active) ||
            (wrt.mode == WM_PLAY && (ly_bit[LY_ERASE] || ly_bit[LY_STEP] || !ly_bit[LY_FX])) ||
            (wrt.mode == WM_PLAY && wrt.active && !wrt.keys_on) || host_lcd_clipped || halted) {
            if (!bad++)
                printf("play: fuzz: invalid at frame %u: mode %u scr %u rec %u active %u sw %u\n", i, wrt.mode, pl.scr,
                       prec.st, wrt.active, wreq.sw);
        }
    }
    host_in_set(0, 0);
    host_stop();
    frames(4);
    if (wrt.mode != WM_PLAY)
        play_menu(0);
    for (i = 0; i < NPART; i++)                  /* (an arp HOLD set in ADVANCED latches by design: off) */
        trk[i].p[P_AHOLD] = 0;
    wait_ms(10000);
    host_state(&st);
    printf("play: fuzz: %u frames: PLAY %u, ADVANCED %u, SLOOP %u frames; %u PLAY screens seen\n", n, modes[1], modes[2],
           modes[0], nscr);
    check(!bad, "fuzz: the state machine always valid, nothing drawn off the screen");
    check(st.voices == 0 && st.gated == 0 && !song.playing, "fuzz: after STOP and 10 s no note left (no stuck notes)");
    check(nscr >= 10 && modes[0] && modes[1] && modes[2], "fuzz: most PLAY screens, and all three modes, reached");
}

static int run(const char *name, void (*fn)(void))
{
    pid_t pid;
    int st;
    fflush(stdout);
    if (!(pid = fork())) {
        fn();
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("play: scenario %s FAILED%s\n", name, WIFEXITED(st) ? "" : " (crashed)");
        return 1;
    }
    return 0;
}
static void sc_fuzz_main(void)
{
    const char *e = getenv("PLAY_FUZZ");
    sc_fuzz(e ? (uint32_t)atoi(e) : 20000u);
}

int main(int argc, char **argv)
{
    int bad = 0;
    const char *only = argc > 2 ? argv[2] : "";
    if (argc > 1)
        outdir = argv[1];
    if (!*only || !strcmp(only, "first"))
        bad += run("first", sc_first);
    if (!*only || !strcmp(only, "screens"))
        bad += run("screens", sc_screens);
    if (!*only || !strcmp(only, "leave"))
        bad += run("leave", sc_leave);
    if (!*only || !strcmp(only, "persist"))
        bad += run("persist", sc_persist);
    if (!*only || !strcmp(only, "fuzz"))
        bad += run("fuzz", sc_fuzz_main);
    printf(bad ? "PLAY MODE TEST FAILED\n" : "play mode test: all checks passed\n");
    return bad != 0;
}
