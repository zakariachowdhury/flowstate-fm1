/* SPDX-License-Identifier: GPL-3.0-only */
/* The host unity root: the firmware as src/felucca.c builds it, in its order, on host/hal_host.h instead
 * of the hardware, plus the entry points of host/host.h. Nothing here makes sound or plays steps: that
 * is all the firmware's own code.
 *
 * Left out, hardware only: lcd.c and felucca.c's flash glue (hal_host.h has both), main.c (host_boot and
 * host_ui_frame mirror fm1_main; its felucca_init is copied verbatim by host/Makefile into fw_main.h),
 * and with FELUCCA_UART / OTA / CDC = 0: midi_uart.c, ota.c, editor.c, console.c, recovery.c. usb.c is
 * built (the MIDI rings, the USB state the UI shows) but never started. audio.c is built as it is: its
 * interrupt handler never runs, host_audio renders through its audio_block. The Musical Worlds are built in as
 * on the device (FELUCCA_WORLD 1): with no World active every hook takes SLOOP's path. tests/unity_order_test.py
 * fails when this order and felucca.c's differ. */
#define FELUCCA_WORLD 1              /* Musical Worlds (world.c), as felucca.c */
#define FELUCCA_FLASH 1              /* storage.c, project.c and upreset.c on hal_host.h's NOR image */
#define FELUCCA_OTA 0                /* (no USB on the host: no update, no editor, no console) */
#define FELUCCA_CDC 0
#define FELUCCA_UART 0
#include "hal_host.h"

#define memset felucca_memset        /* libc.c's own, with unsigned sizes: kept apart from the system's */
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "libc.c"
#undef memset
#undef memcpy
#undef memcmp
/* lcd.c: hal_host.h */
#include "gfx.c"
#include "core.h"
#include "engines.c"
#include "drums.c"
#include "params.c"
#include "voice.c"
#include "slicer.c"
#include "fx.c"
#include "usb.c"
#define FELUCCA_ARRANGER 1
#include "arranger.c"
#include "seq.c"
#if FELUCCA_WORLD                    /* the harmony and Smart Keys runtime, as felucca.c */
#include "harmony.c"
#include "guard.c"
#include "smartkeys.c"
#include "macro.c"
#include "arrange.c"
#include "play_rec.c"
#endif
#include "audio.c"
#include "panel.c"
#include "ui.c"
#include "ui_song.c"
#include "ui_studio.c"
#include "icons.c"
#include "ui_draw.c"
#include "ui_layers.c"
#include "ui_menu.c"
#include "ui_input.c"
#if FELUCCA_WORLD
#include "world.c"
#include "ui_play.c"
#endif
/* felucca.c's flash glue (st_read, st_erase, st_prog): hal_host.h */
#include "storage.c"
#include "upreset.c"
#include "project.c"
#if FELUCCA_WORLD
#include "world_store.c"
#endif
/* ota.c, editor.c, console.c, recovery.c: not built */
#include "splash.c"
#include "fw_main.h"                 /* main.c: only felucca_init(), the power-on state (verbatim) */
#undef __attribute__
#include "host.h"

_Static_assert(HOST_FS == FS && HOST_BLOCK == CTL && HOST_NTRK == NTRK, "host.h matches the firmware");
_Static_assert(HOST_B_FX == B_FX && HOST_B_SCL == B_SCL && HOST_B_ENV == B_ENV && HOST_B_LFO == B_LFO &&
               HOST_B_EDIT == B_EDIT && HOST_B_GLO == B_GLO && HOST_B_HOME == B_HOME && HOST_B_SAVE == B_SAVE &&
               HOST_B_ARP == B_ARP && HOST_B_SEQ == B_SEQ && HOST_B_PLAY == B_PLAY && HOST_B_REC == B_REC &&
               HOST_B_OCTDN == B_OCTDN && HOST_B_OCTUP == B_OCTUP && HOST_NB == NB, "host.h labels are panel.c's");
_Static_assert(HOST_EN_SELECT == EN_SELECT && HOST_EN_ALGO == EN_ALGO && HOST_EN_PRESET == EN_PRESET &&
               HOST_EN_K1 == EN_K1 && HOST_EN_K2 == EN_K2 && HOST_EN_K3 == EN_K3 && HOST_EN_K4 == EN_K4 &&
               HOST_NE == NE, "host.h encoder roles are panel.c's");

/* ------------------------------------------------ audio --- */
static void (*yield_fn)(const int16_t *, uint32_t, void *);
static void *yield_ctx;

void host_audio(int16_t *pcm, uint32_t frames)
{
    int32_t o[2 * CTL];
    uint32_t f, i;
    if (frames % CTL) {
        fprintf(stderr, "host_audio: %u frames is not a multiple of %u\n", frames, CTL);
        abort();
    }
    for (f = 0; f < frames; f += CTL) {
        audio_block(o, CTL);                     /* audio.c: mix_block, the scope, Q15 -> the DAC's 24 bits */
        for (i = 0; i < 2u * CTL; i++) {
            int32_t v = o[i] >> OUT_SHIFT;       /* (exact: back to the mix) */
            pcm[2u * f + i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        host_t += CTL;
        fm1_ms = (uint32_t)(host_t * 1000u / FS);   /* TIMER5's milliseconds, from the audio rendered */
    }
}
uint64_t host_frames(void) { return host_t; }
void host_set_yield(void (*fn)(const int16_t *, uint32_t, void *), void *ctx)
{
    yield_fn = fn;
    yield_ctx = ctx;
}
static void host_yield(void)                     /* the main loop waits: one block of audio goes on */
{
    int16_t pcm[2 * CTL];
    host_audio(pcm, CTL);
    if (yield_fn)
        yield_fn(pcm, CTL, yield_ctx);
}
static void host_wait_ms(uint32_t ms)
{
    uint32_t t0 = fm1_ms;
    while (fm1_ms - t0 < ms)
        host_yield();
}

/* ------------------------------------------------ boot and the main loop --- */
static const char *flash_file;
static int booted, halted, device_boot;
void host_boot_device(int on) { device_boot = on != 0; }

int host_boot(const char *flash_image)
{
    uintptr_t smp = (uintptr_t)SMP_DATA, nor = (uintptr_t)host_nor;
    if (booted++) {
        fprintf(stderr, "host_boot: once per process (the firmware's state is static)\n");
        return -1;
    }
    if (nor < smp || nor + HOST_NOR_SIZE - smp > 0xFFFFFFFFu) {   /* hal_host.h SMP_USER_XIP */
        fprintf(stderr, "host_boot: the flash image is not within 4 GiB above SMP_DATA (%p, %p): user sample "
                "slots would read the wrong memory (eng_sample.c:89 keeps 32-bit offsets)\n", (void *)host_nor,
                (const void *)SMP_DATA);
        abort();
    }
    memset(host_nor, 0xFF, sizeof host_nor);     /* an erased chip */
    if (flash_image) {
        FILE *f = fopen(flash_image, "rb");
        flash_file = flash_image;
        if (f) {
            size_t n = fread(host_nor, 1, sizeof host_nor, f);
            fclose(f);
            if (n != sizeof host_nor) {
                fprintf(stderr, "host_boot: %s: %zu bytes, a flash image has %u\n", flash_image, n, HOST_NOR_SIZE);
                return -2;
            }
        }
    }
    /* main.c fm1_main up to its loop; the hardware bring-up (input, ADC, audio, USB, TIMER5, interrupts)
     * has nothing to do here, and no button can be held at power-on (no calibration) */
    persist_boot();                              /* flash: settings, panel table, sections, user presets */
    settings_init();
    lcd_init();
    sloop_splash();
    if (felucca_dbg.magic != DBG_MAGIC) {
        memset(&felucca_dbg, 0, sizeof felucca_dbg);
        felucca_dbg.magic = DBG_MAGIC;
    }
    felucca_dbg.boots++;
    panel_init();
#if FELUCCA_WORLD
    wss_sloop = (uint8_t)!device_boot;           /* (world_store.c: SLOOP whatever the session says, unless asked) */
#endif
    felucca_init();
    host_wait_ms(30 + 900);                      /* fm1_delay_ms(30), fm1_delay_ms(900): the logo stays */
    lcd_fill(0, 0, 240, 240, C_BLACK);
    return 0;
}

int host_flash_write(const char *path)
{
    FILE *f = fopen(path ? path : flash_file ? flash_file : "", "wb");
    size_t n;
    if (!f)
        return -1;
    n = fwrite(host_nor, 1, sizeof host_nor, f);
    return fclose(f) || n != sizeof host_nor ? -1 : 0;
}

static void host_blob_gc(void);
/* one pass of fm1_main's loop body. The device then spins on ui_input until 15 ms have passed; here the
 * program renders HOST_FRAME_BLOCKS blocks between two passes. */
void host_ui_frame(void)
{
    static int32_t knob = 512 * 16;
    if (halted)
        return;
    host_in_loop = 1;
    {
        int32_t b = fm1_adc_read(FM1_ADC_BATT);   /* battery: slow IIR */
        if (b > 0)
            song.batt_raw = song.batt_raw ? song.batt_raw + (b - song.batt_raw) / 32 : b;
    }
    {
        int32_t a = fm1_adc_read(FM1_ADC_MASTER);
        if (a >= 0) {
            uint32_t k10;
            knob += (a * 16 - knob) / 8;
            k10 = (uint32_t)(knob / 16);
            song.master_q12 = (k10 * k10) >> 8;
        }
    }
    {   /* OCT- + OCT+ held 5 s: the device enters UBOOT (a reset); the host stops its main loop there */
        static uint32_t t0, shown;
        uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
        if ((fm1_in.buttons & both) != both) {
            if (shown)
                ui_say("UPDATE MODE ", "CANCELLED");
            shown = 0;
            t0 = fm1_ms;
        } else if (fm1_ms - t0 > 2000u && fm1_ms - t0 <= 5000u) {
            uint32_t left = (5000u - (fm1_ms - t0) + 999u) / 1000u;
            if (left != shown) {
                char d[4] = {(char)('0' + left), '.', '.', 0};
                ui_say("UPDATE MODE IN ", d);
                shown = left;
            }
        } else if (fm1_ms - t0 > 5000u) {
            lcd_fill(0, 0, 240, 240, C_BLACK);
            draw_text_box(0, 110, 240, &FONT_S, "UBOOT", RGB(80, 120, 255), 1);
            halted = 1;
            host_in_loop = 0;
            return;
        }
    }
    felucca_dbg.ui_frames++;
    felucca_dbg.page = ui.page;
    felucca_dbg.home = ui.home;
    felucca_dbg.stage = 1;
    ui_input();
    felucca_dbg.stage = 2;
    ui_leds();
    ui_draw();
    felucca_dbg.stage = 8;
    autosave_tick();                             /* the working project into flash, when quiet */
#if FELUCCA_WORLD
    play_service();                              /* (main.c's loop: a World switch on its bar, the modes; the macros) */
    macro_service();
    host_blob_gc();
#endif
    sections_flush();
    felucca_dbg.stage = 9;
    host_in_loop = 0;
}

/* ------------------------------------------------ the panel --- */
void host_key(uint32_t k, int down)
{
    uint32_t n = fm1_in.notes;
    if (k < 27u)
        host_in_set(down ? n | 1u << k : n & ~(1u << k), fm1_in.buttons);
}
void host_button(uint32_t label, int down)
{
    uint32_t b = fm1_in.buttons;
    if (label < NB)
        host_in_set(fm1_in.notes, down ? b | 1u << panel.btn[label] : b & ~(1u << panel.btn[label]));
}
void host_turn(uint32_t role, int32_t steps)
{
    if (role < NE)
        fm1_in.enc_steps[panel.enc[role]] += (int16_t)(steps * panel.dir[role]);
}
void host_master_pot(int32_t adc) { host_adc_master = adc < -1 ? -1 : adc > 1023 ? 1023 : adc; }
void host_master_volume(uint32_t q12)
{
    host_adc_master = -1;                        /* (an ADC timeout: main.c leaves the gain alone) */
    song.master_q12 = q12;
}
int host_led(uint32_t id)
{
    uint32_t c, r;
    for (c = 0; c < FM1_NCOL; c++)
        for (r = 1; r < 5u; r++)
            if (FM1_KEYMAP[r][c] == (int8_t)id)
                return (fm1_led[c] >> r) & 1u ? 2 : (fm1_led_dim[c] >> r) & 1u ? 1 : 0;
    return 0;
}
const uint16_t *host_lcd(void) { return host_fb; }
int host_lcd_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f)
        return -1;
    fprintf(f, "P6\n240 240\n255\n");
    for (i = 0; i < 240u * 240u; i++) {
        uint16_t p = host_fb[i];
        uint8_t rgb[3] = {(uint8_t)((p >> 11) * 255u / 31u), (uint8_t)(((p >> 5) & 63u) * 255u / 63u),
                          (uint8_t)((p & 31u) * 255u / 31u)};
        fwrite(rgb, 1, 3, f);
    }
    return fclose(f) ? -1 : 0;
}

/* ------------------------------------------------ transport, projects, song --- */
void host_play(void) { transport_req = 1; }
void host_stop(void) { transport_req = 2; }
int host_playing(void) { return song.playing; }

static union {                                   /* a project file, any format (project.c proj_tmp) */
    project_t v4;
    project_v3_t v3;
    project_v2_t v2;
    project_v1_t v1;
    uint8_t b[4096];
} host_pf;
static project_t host_proj;

/* a project file -> *q as format 4; the format read, or < 0 */
static int host_read(const char *path, project_t *q)
{
    FILE *f = fopen(path, "rb");
    int n;
    if (!f)
        return -1;
    n = (int)fread(host_pf.b, 1, sizeof host_pf.b, f);
    fclose(f);
    if (n < 8 || !proj_import(q, host_pf.b, n))
        return -2;
    return host_pf.v4.magic == PROJ_MAGIC ? 4 : host_pf.v4.magic == PROJ_MAGIC_V3 ? 3 :
           host_pf.v4.magic == PROJ_MAGIC_V2 ? 2 : 1;
}
int host_project_load(const char *path)
{
    int fmt = host_read(path, &host_proj);
    if (fmt > 0) {
#if FELUCCA_WORLD
        play_leave();                            /* (a World session: SLOOP again first, as TOOLS > LOAD) */
#endif
        project_apply(&host_proj);               /* project.c: what project_load does with a slot */
    }
    return fmt;
}
int host_project_save(const char *path)
{
    FILE *f = fopen(path, "wb");
    size_t n;
    if (!f)
        return -1;
    proj_capture(&host_proj);
    n = fwrite(&host_proj, 1, sizeof host_proj, f);
    return fclose(f) || n != sizeof host_proj ? -1 : 0;
}
int host_section_load(uint32_t slot, const char *path)
{
    int fmt;
    if (slot >= 4u)
        return -2;
    fmt = host_read(path, &proj_slot[slot]);
    if (fmt > 0)
        sec_dirty |= (uint8_t)(1u << slot);      /* (to flash when quiet, as section_store) */
    else
        proj_slot[slot].magic = 0;
    return fmt;
}
int host_section_apply(uint32_t slot)
{
    if (slot >= 4u || !proj_ok(&proj_slot[slot]) || song.playing)
        return -1;
    section_load(slot);
    return 0;
}
int host_song_set(const uint8_t *scene, const uint8_t *bars, uint32_t n)
{
    uint32_t i;
    if (!n || n > ARR_STEPS)
        return -1;
    arrangement.count = (uint8_t)n;
    arrangement.loop = 0;
    for (i = 0; i < n; i++) {
        arrangement.entry[i].scene = scene[i];
        arrangement.entry[i].bars = bars[i];
    }
    return arr_valid(&arrangement, arrangement_ready()) ? 0 : -1;
}
void host_song_mode(int on) { arrangement_enabled = (uint8_t)(on != 0); }

/* ------------------------------------------------ what is loaded --- */
void host_state(host_state_t *s)
{
    uint32_t i, k;
    memset(s, 0, sizeof *s);
    s->bpm = song.g[G_BPM];
    s->swing = song.g[G_SWING];
    s->playing = song.playing;
    s->song_mode = arrangement_enabled;
    s->sections = (int)arrangement_ready();
    s->song_n = arrangement.count <= ARR_STEPS ? arrangement.count : 0;
    for (i = 0; i < (uint32_t)s->song_n; i++) {
        s->song_scene[i] = arrangement.entry[i].scene;
        s->song_bars[i] = arrangement.entry[i].bars;
    }
    for (i = 0; i < NTRK; i++) {
        const track_t *t = &trk[i];
        host_track_t *d = &s->t[i];
        uint32_t len = trk_len(t), div = (uint32_t)t->p[P_SDIV] % 6u;
        if (is_drum(t)) {
            d->engine = "DRUMS";
            d->sound = DRUM_KIT_NAMES[(uint32_t)t->p[P_E0] % DRUM_KITS];
        } else {
            const engine_t *e = ENGINES[t->eng_req % NENGINES];
            d->engine = e->name;
            d->sound = e->npresets ? e->presets[t->preset % e->npresets].name : "-";
        }
        d->div = N_DIV[div];
        d->root = N_NOTE[(uint32_t)t->p[P_ROOT] % 12u];
        d->scale = N_SCALE[(uint32_t)t->p[P_SCALE] % NSCALES];
        d->quant = N_QUANT[(uint32_t)t->p[P_QUANT] % 3u];
        d->steps = (int)len;
        for (k = 0; k < len && k < NSTEP; k++)
            d->notes += is_drum(t) ? dstep_mask(&t->dstep[k]) != 0u : t->step[k].time == ST_NOTE && t->step[k].n;
        d->bars = (double)len / DIV_DEN[div] / 4.0;
        d->level = is_drum(t) ? song.g[G_DRLVL] : t->p[P_LEVEL];   /* (the drum track: GLO > DRUMS) */
        d->pan = t->p[P_PAN];
        d->mute = t->p[P_MUTE];
        d->cho = t->p[P_CHOR];
        d->dly = t->p[P_DLY];
        d->rev = is_drum(t) ? song.g[G_DRREV] : t->p[P_REV];
    }
    s->scene = live_sec;
    s->scene_next = live_req;
    s->beat = (int)clk_beat;
    s->filter = song.g[G_FILT];
    s->master = (int)song.master_q12;
    s->sel = song.sel;
    for (i = 0; i < NPART; i++)
        for (k = 0; k < NVOICE; k++) {
            s->voices += trk[i].v[k].active != 0;
            s->gated += trk[i].v[k].active && trk[i].v[k].gate;
        }
#if FELUCCA_WORLD
    if (wrt.active) {                            /* a World's scenes, not the sections */
        s->rec = prec.st;
        for (k = 0; k < NSTEP; k++)
            s->loop += trk[wrt.keys_trk % NPART].step[k].time == ST_NOTE ? trk[wrt.keys_trk % NPART].step[k].n : 0;
        s->scene = wrt.scene;
        s->scene_next = wst.st == WST_READY && wst.scene != wrt.scene ? wst.scene : -1;
    }
#endif
}

static project_t host_info_proj;                 /* (host_project_info: never the working project) */
int host_project_info(const char *path, host_state_t *s)
{
    FILE *f = fopen(path, "rb");
    project_t *q = &host_info_proj;
    uint8_t *b;
    int n, fmt;
    uint32_t i;
    memset(s, 0, sizeof *s);
    s->scene = s->scene_next = -1;
    if (!f)
        return -1;
    b = malloc(sizeof host_pf);
    n = b ? (int)fread(b, 1, sizeof host_pf, f) : 0;
    fclose(f);
    if (n < 8 || !proj_import(q, b, n)) {
        free(b);
        return -2;
    }
    fmt = ((const project_t *)(const void *)b)->magic == PROJ_MAGIC ? 4 :
          ((const project_t *)(const void *)b)->magic == PROJ_MAGIC_V3 ? 3 :
          ((const project_t *)(const void *)b)->magic == PROJ_MAGIC_V2 ? 2 : 1;
    free(b);
    s->bpm = q->g[G_BPM];
    s->swing = q->g[G_SWING];
    s->sel = q->sel < NTRK ? q->sel : 0;
    for (i = 0; i < NTRK; i++) {
        const proj_trk_t *t = &q->t[i];
        host_track_t *d = &s->t[i];
        if (i == TRK_DRUM) {
            d->engine = "DRUMS";
            d->sound = DRUM_KIT_NAMES[(uint32_t)t->p[P_E0] % DRUM_KITS];
        } else {
            const engine_t *e = ENGINES[t->engine % NENGINES];
            d->engine = e->name;
            d->sound = e->npresets ? e->presets[t->preset % e->npresets].name : "-";
        }
        d->div = N_DIV[(uint32_t)t->p[P_SDIV] % 6u];
        d->root = N_NOTE[(uint32_t)t->p[P_ROOT] % 12u];
        d->scale = N_SCALE[(uint32_t)t->p[P_SCALE] % NSCALES];
        d->quant = N_QUANT[(uint32_t)t->p[P_QUANT] % 3u];
        d->level = i == TRK_DRUM ? q->g[G_DRLVL] : t->p[P_LEVEL];
        d->pan = t->p[P_PAN];
        d->mute = t->p[P_MUTE];
        d->cho = t->p[P_CHOR];
        d->dly = t->p[P_DLY];
        d->rev = i == TRK_DRUM ? q->g[G_DRREV] : t->p[P_REV];
    }
    return fmt;
}

/* ------------------------------------------------ live control (the gestures of ui_layers.c, direct) --- */
int host_scene(uint32_t slot)                    /* ui_layers.c LY_SONG, white keys 1..4 */
{
    char b[2] = {(char)('A' + (slot & 3u)), 0};
    if (slot >= 4u)
        return -1;
#if FELUCCA_WORLD
    if (wrt.active) {                            /* a World: its scene (design H17), on the next bar */
        int rc = host_world_request((int)slot, -1);
        if (rc >= 0)
            ui_say(rc ? "NEXT: " : "SCENE ", b);
        return rc < 0 ? -1 : rc;
    }
#endif
    if (arrangement_clock.running) {
        ui_message("SONG PLAYS");
        return -2;
    }
    if (!((arrangement_ready() >> slot) & 1u)) {
        ui_say("EMPTY ", b);
        return -1;
    }
    if (song.playing) {
        live_req = (int8_t)slot;                 /* seq.c live_block: on the next bar */
        ui_say("NEXT: ", b);
        return 1;
    }
    section_load(slot);
    ui_say("LOADED ", b);
    return 0;
}
int host_section_store(uint32_t slot)            /* white keys 5..8 (without the device's "AGAIN" over a used one) */
{
    char b[2] = {(char)('A' + (slot & 3u)), 0};
    if (slot >= 4u)
        return -1;
    section_store(slot);
    ui_say("SAVED ", b);
    return 0;
}
int host_track_set(uint32_t k, uint32_t what, int32_t v)
{
    static const uint8_t ID[HOST_NT] = {P_LEVEL, P_PAN, P_MUTE, P_CHOR, P_DLY, P_REV};
    int16_t *p;
    const param_desc_t *d;
    if (k >= NTRK || what >= HOST_NT)
        return 0;
    if (is_drum(&trk[k]) && (what == HOST_T_LEVEL || what == HOST_T_REV)) {   /* GLO > DRUMS */
        uint32_t g = what == HOST_T_LEVEL ? G_DRLVL : G_DRREV;
        p = &song.g[g];
        d = &GP[g];
    } else {
        p = &trk[k].p[ID[what]];
        d = &TP[ID[what]];
    }
    *p = (int16_t)clamp(v, d->min, d->max);
    return *p;
}
static const uint8_t HOST_GID[HOST_NG] = {G_BPM, G_SWING, G_FILT, G_DUST, G_DUCK};
int host_global(uint32_t what) { return what < HOST_NG ? song.g[HOST_GID[what]] : 0; }
int host_global_set(uint32_t what, int32_t v)
{
    uint32_t g;
    if (what >= HOST_NG)
        return 0;
    g = HOST_GID[what];
    song.g[g] = (int16_t)clamp(v, GP[g].min, GP[g].max);
    return song.g[g];
}
uint32_t host_flash_changes(void) { return host_nor_erases + host_nor_progs; }
int host_button_led(uint32_t label) { return label < NB ? host_led(panel.btn[label]) : 0; }
void host_track_select(uint32_t k) { track_select(k); }   /* ui.c: what ALGORITHM does */
int host_track_params(uint32_t k, host_param_t *out, int max)
{
    uint32_t id;
    int n = 0;
    if (k >= NTRK)
        return 0;
    for (id = 0; id < P_COUNT && n < max; id++) {
        const param_desc_t *d = track_desc(&trk[k], id);
        const char *unit;
        char v[16];
        host_param_t *p = &out[n++];
        if (!d->label || !d->label[0] || !strcmp(d->label, "-")) {   /* (an unused engine slot) */
            n--;
            continue;
        }
        str_cpy(p->label, d->label, sizeof p->label);
        param_format(d, trk[k].p[id], v, &unit);
        snprintf(p->text, sizeof p->text, "%s%s", v, unit ? unit : "");
        p->value = trk[k].p[id];
        p->min = d->min;
        p->max = d->max;
        p->id = (uint8_t)id;
    }
    return n;
}

/* ------------------------------------------------ Musical Worlds (world.c) --- */
#define HOST_BLOBS 8                             /* World blobs from files: kept while world.c points into them */
static uint8_t *host_blob[HOST_BLOBS];
static void host_blob_gc(void)
{
    uint32_t i;
    for (i = 0; i < HOST_BLOBS; i++)
        if (host_blob[i] && host_blob[i] != wctx.b && (!wreq.sw || host_blob[i] != wnext.b)) {
            free(host_blob[i]);
            host_blob[i] = NULL;
        }
}
static int host_blob_keep(uint8_t *b)            /* 0, or -1: no room (b freed) */
{
    uint32_t i;
    host_blob_gc();
    for (i = 0; i < HOST_BLOBS && host_blob[i]; i++)
        ;
    if (i == HOST_BLOBS) {
        free(b);
        return -1;
    }
    host_blob[i] = b;
    return 0;
}
static int host_wrc(int rc) { return rc ? -rc : wreq.sw || wst.st == WST_READY ? 1 : 0; }

int host_world_factory_count(void) { return (int)world_factory_count(); }
int host_world_factory(int i, host_world_entry_t *e)
{
    uint32_t bpm = 0;
    memset(e, 0, sizeof *e);
    if (i < 0 || (uint32_t)i >= WORLD_NFACTORY || world_factory_info((uint32_t)i, e->name, e->category, &bpm))
        return -1;
    e->id = WORLD_INDEX[i].id;
    e->bpm = (int)bpm;
    return 0;
}
int host_world_factory_find(const char *name)
{
    host_world_entry_t e;
    char *end;
    unsigned long id = strtoul(name, &end, 0);
    int i, k;
    if (*name && !*end && end != name)
        return world_factory_find((uint32_t)id);
    for (i = 0; i < (int)WORLD_NFACTORY; i++) {
        if (host_world_factory(i, &e))
            continue;
        for (k = 0; e.name[k] && name[k] && toupper((unsigned char)e.name[k]) == toupper((unsigned char)name[k]); k++)
            ;
        if (!e.name[k] && !name[k])
            return i;
    }
    return -1;
}
int host_world_load(int i)                       /* (ui_play.c play_world: from SLOOP, the project parked first) */
{
    const uint8_t *b;
    uint32_t n;
    if (i < 0 || world_factory((uint32_t)i, &b, &n))
        return -WE_STATE;
    return host_wrc(play_world(b, n));
}
int host_world_load_blob(uint8_t *b, uint32_t n)
{
    int rc;
    if (host_blob_keep(b))
        return -WE_BUSY;
    rc = play_world(b, n);
    host_blob_gc();                              /* (b too, when it was refused) */
    return host_wrc(rc);
}
int host_world_reload_blob(uint8_t *b, uint32_t n)
{
    int rc;
    if (!wrt.loaded)
        return host_world_load_blob(b, n);
    if (host_blob_keep(b))
        return -WE_BUSY;
    rc = world_hot_reload(b, n);
    host_blob_gc();
    return rc ? -rc : 0;
}
static void host_sh_quote(char *d, size_t n, const char *s)   /* 'it'\''s' for the shell */
{
    size_t k = 0;
    d[k++] = '\'';
    for (; *s && k + 5 < n; s++) {
        if (*s == '\'') {
            memcpy(d + k, "'\\''", 4);
            k += 4;
        } else {
            d[k++] = *s;
        }
    }
    d[k++] = '\'';
    d[k] = 0;
}
static int host_read_file(const char *path, uint8_t **b, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    size_t got;
    if (!f)
        return -1;
    *b = malloc(WF_MAX_LEN + 1u);
    got = *b ? fread(*b, 1, WF_MAX_LEN + 1u, f) : 0;
    fclose(f);
    if (!*b || got > WF_MAX_LEN) {
        free(*b);
        *b = NULL;
        return -2;
    }
    *n = (uint32_t)got;
    return 0;
}
int host_world_compile(const char *path, uint8_t **b, uint32_t *n, char *msg, int mlen)
{
    char tool[1100], dir[PATH_MAX], tmp[1100], cmd[4096], q1[1200], q2[1200], q3[1200], *slash;
    const char *root = getenv("FLOWSTATE_ROOT"), *td = getenv("TMPDIR");
    size_t l = strlen(path);
    FILE *p;
    int fd, rc, k, up;
    *b = NULL;
    msg[0] = 0;
    if (l < 11 || strcmp(path + l - 11, ".world.json")) {   /* a blob */
        rc = host_read_file(path, b, n);
        if (rc)
            snprintf(msg, (size_t)mlen, "%s: %s", path, rc == -1 ? "cannot read" : "larger than a World blob");
        return rc;
    }
    tool[0] = 0;                                 /* tools/worldc.py: $FLOWSTATE_ROOT, from the file upwards, here */
    if (root)
        snprintf(tool, sizeof tool, "%s/tools/worldc.py", root);
    if (!root || access(tool, R_OK)) {
        tool[0] = 0;
        if (realpath(path, dir))
            for (up = 0; up < 8 && (slash = strrchr(dir, '/')) && slash != dir; up++) {
                *slash = 0;
                snprintf(tool, sizeof tool, "%s/tools/worldc.py", dir);
                if (!access(tool, R_OK))
                    break;
                tool[0] = 0;
            }
        if (!tool[0])
            snprintf(tool, sizeof tool, "tools/worldc.py");
    }
    snprintf(tmp, sizeof tmp, "%s/flowstate-world-XXXXXX", td && *td ? td : "/tmp");
    if ((fd = mkstemp(tmp)) < 0) {
        snprintf(msg, (size_t)mlen, "cannot make a temporary file in %s", td ? td : "/tmp");
        return -1;
    }
    close(fd);
    host_sh_quote(q1, sizeof q1, tool);
    host_sh_quote(q2, sizeof q2, path);
    host_sh_quote(q3, sizeof q3, tmp);
    snprintf(cmd, sizeof cmd, "python3 %s compile %s -o %s 2>&1", q1, q2, q3);
    if (!(p = popen(cmd, "r"))) {
        snprintf(msg, (size_t)mlen, "cannot run python3");
        remove(tmp);
        return -1;
    }
    for (k = 0; k < mlen - 1; ) {                /* what worldc says (its errors name the place) */
        int ch = fgetc(p);
        if (ch == EOF)
            break;
        msg[k++] = (char)(ch == '\n' ? ' ' : ch);
    }
    msg[k > 0 ? k : 0] = 0;
    rc = pclose(p);
    if (rc) {
        remove(tmp);
        return -1;
    }
    rc = host_read_file(tmp, b, n);
    remove(tmp);
    if (rc)
        snprintf(msg, (size_t)mlen, "%s: worldc wrote no blob", path);
    return rc;
}
int host_world_load_file(const char *path, char *msg, int mlen)
{
    uint8_t *b;
    uint32_t n;
    if (host_world_compile(path, &b, &n, msg, mlen))
        return -WE_SIZE;
    return host_world_load_blob(b, n);
}
int host_world_request(int scene, int var)
{
    uint32_t ps = WF_NONE, pv = WF_NONE;
    int pend = world_pending(&ps, &pv) && ps != WF_NONE;   /* (a request on its way: the other half stays) */
    if (!wrt.loaded)
        return -WE_STATE;
    return host_wrc(world_request(scene >= 0 ? (uint32_t)scene : pend ? ps : wrt.scene,
                                  var >= 0 ? (uint32_t)var : pend ? pv : wrt.var));
}
void host_world_unload(void) { play_leave(); }    /* (LEAVE WORLD: the parked SLOOP project back) */
void host_world_service(void)
{
    world_service();
    macro_service();                             /* (main.c's loop: the macros' table when a control moved) */
    host_blob_gc();
}
void host_world(host_world_t *w)
{
    uint32_t i, ps = 0, pv = 0;
    memset(w, 0, sizeof *w);
    w->loaded = wrt.loaded;
    w->active = wrt.active;
    w->mode = wrt.mode;
    w->pending_scene = w->pending_var = -1;
    if (world_pending(&ps, &pv)) {
        int ph = 0;
        w->bars_left = (int)world_bars_left(&ph);
        w->phrase = ph;
        w->pending = ps == WF_NONE ? 2 : 1;
        w->pending_scene = ps == WF_NONE ? -1 : (int)ps;
        w->pending_var = pv == WF_NONE ? -1 : (int)pv;
    }
    if (!wrt.loaded)
        return;
    w->id = wrt.id;
    snprintf(w->name, sizeof w->name, "%s", world_name());
    snprintf(w->category, sizeof w->category, "%s", world_category());
    snprintf(w->blurb, sizeof w->blurb, "%s", world_blurb());
    w->bpm = (int)world_bpm();
    w->scene = wrt.scene;
    w->var = wrt.var;
    w->nvar = (int)world_nvar();
    for (i = 0; i < world_nscenes() && i < 4u; i++)
        snprintf(w->scene_name[i], sizeof w->scene_name[i], "%s", world_scene_name(i));
    for (i = 0; i < (uint32_t)w->nvar && i < 8u; i++)
        snprintf(w->var_name[i], sizeof w->var_name[i], "%s", world_var_name(i));
    for (i = 0; i < NTRK; i++)
        snprintf(w->role[i], sizeof w->role[i], "%s", world_role_label(world_track_role(i)));
    w->keys_track = (int)world_keys_track();
    w->keys_on = wrt.keys_on;
    harm_chord_name(w->chord);                   /* harmony.c: the chord at the clock ("" with none) */
}
/* ------------------------------------------------ the macros (macro.c, arrange.c) --- */
static const char *const HOST_CTL_NAME[WF_NCTL] = {"COLOR", "MOTION", "SPACE", "ENERGY", "SOFT", "SHORT", "BODY", "TAIL",
                                                   "DRIFT", "WOBBLE", "PULSE", "RATE", "FILTER", "ECHO", "CRUSH", "FREEZE"};
static const char *const HOST_CURVE[WF_NBUILTIN_CURVES] = {"lin", "exp", "log", "s", "late"};
static const char *const HOST_CLASS[4] = {"fast", "medium", "slow", "stepped"};
int host_macro_set(uint32_t ctl, int32_t pos)
{
    if (!wrt.active || ctl >= WF_NCTL)
        return -1;
    macro_set(ctl, pos);
    return (int)macro_pos(ctl);
}
int host_macro(uint32_t ctl) { return wrt.active && ctl < WF_NCTL ? (int)macro_pos(ctl) : -1; }
int host_macro_snap(void)
{
    if (!wrt.active)
        return -1;
    mac.snap = mac.dirty = 1;
    return macro_eval() ? -2 : 0;                /* (-2: the last table is not taken yet) */
}
const char *host_macro_name(uint32_t ctl) { return ctl < WF_NCTL ? HOST_CTL_NAME[ctl] : "?"; }

/* names for the inspector: the track by its role, the parameter by what it does */
static void host_lower(char *d, const char *s, size_t n)
{
    size_t k;
    for (k = 0; s[k] && k + 1 < n; k++)
        d[k] = (char)tolower((unsigned char)s[k]);
    d[k] = 0;
}
static void host_track_name(uint32_t t, char *d, size_t n)
{
    uint32_t r = world_track_role(t), k, twice = 0;
    for (k = 0; k < NTRK; k++)
        twice += k != t && world_track_role(k) == r;
    host_lower(d, world_role_label(r), n);
    if (twice)
        snprintf(d + strlen(d), n - strlen(d), "%u", t + 1);
}
static void host_param_name(uint32_t kind, uint32_t t, uint32_t id, char *d, size_t n)
{
    static const struct { uint8_t id; const char *name; } TPN[] = {
        {P_LEVEL, "level"}, {P_ATK, "attack"}, {P_DEC, "decay"}, {P_SUS, "sustain"}, {P_REL, "release"},
        {P_ED_FLT, "env_filter"}, {P_ED_PIT, "env_pitch"}, {P_ED_SHP, "env_shape"}, {P_LRATE, "lfo_rate"},
        {P_LPHASE, "lfo_phase"}, {P_LFADE, "lfo_fade"}, {P_LD_PIT, "lfo_pitch"}, {P_LD_FLT, "lfo_filter"},
        {P_LD_SHP, "lfo_shape"}, {P_LD_AMP, "lfo_amp"}, {P_SGATE, "gate"}, {P_DIST, "drive"}, {P_CHOR, "chorus"},
        {P_DLY, "delay"}, {P_REV, "reverb"}, {P_GLIDE, "glide"}, {P_PAN, "pan"}, {P_DETUNE, "detune"},
        {P_SLDEPTH, "slicer_depth"}};
    static const struct { uint8_t id; const char *name; } GPN[] = {
        {G_DFDBK, "fx.delay_feedback"}, {G_DCOLOR, "fx.delay_color"}, {G_DMIX, "fx.delay_mix"},
        {G_RSIZE, "fx.reverb_size"}, {G_RDAMP, "fx.reverb_damp"}, {G_CRATE, "fx.chorus_rate"},
        {G_CDEPTH, "fx.chorus_depth"}, {G_DRLVL, "drums.level"}, {G_DRREV, "drums.reverb"}, {G_DUST, "fx.dust"},
        {G_DUCK, "fx.duck"}, {G_FILT, "fx.filter"}, {G_SWING, "fx.swing"}, {G_DTIME, "fx.delay_time"}};
    static const struct { const char *label, *name; } EPN[] = {
        {"CUT", "cutoff"}, {"RES", "resonance"}, {"DRV", "drive"}, {"IDX", "fm_index"}, {"FB", "fm_feedback"},
        {"DTN", "detune"}, {"NOIS", "noise"}, {"Q", "resonance"}, {"BRTH", "breath"}, {"VOWL", "vowel"},
        {"SPRD", "spread"}, {"RAND", "random"}, {"DENS", "density"}, {"CRSH", "crush"}, {"VIB", "vibrato"}};
    char tn[16], pn[24];
    uint32_t k;
    if (kind == OV_G) {
        for (k = 0; k < sizeof GPN / sizeof GPN[0] && GPN[k].id != id; k++)
            ;
        if (k < sizeof GPN / sizeof GPN[0])
            snprintf(d, n, "%s", GPN[k].name);
        else
            host_lower(pn, GP[id].label, sizeof pn), snprintf(d, n, "fx.%s", pn);
        return;
    }
    host_track_name(t, tn, sizeof tn);
    if (kind == OV_VCUT || kind == OV_VSHP) {
        snprintf(d, n, "%s.%s", tn, kind == OV_VCUT ? "brightness" : "timbre");
        return;
    }
    if (id >= P_E0 && t < NPART) {
        const char *l = ENGINES[trk[t].eng_req % NENGINES]->edit[id - P_E0].label;
        for (k = 0; k < sizeof EPN / sizeof EPN[0] && strcmp(EPN[k].label, l); k++)
            ;
        if (k < sizeof EPN / sizeof EPN[0])
            snprintf(pn, sizeof pn, "%s", EPN[k].name);
        else
            host_lower(pn, l, sizeof pn);
    } else {
        for (k = 0; k < sizeof TPN / sizeof TPN[0] && TPN[k].id != id; k++)
            ;
        if (k < sizeof TPN / sizeof TPN[0])
            snprintf(pn, sizeof pn, "%s", TPN[k].name);
        else
            host_lower(pn, TP[id].label, sizeof pn);
    }
    snprintf(d, n, "%s.%s", tn, pn);
}
static void host_slot_id(const ov_slot_t *s, uint32_t *t, uint32_t *id)   /* a slot's track (or part) and parameter */
{
    *t = s->part;
    *id = s->kind == OV_P ? (uint32_t)(s->ptr - trk[s->part % NTRK].p) : s->kind == OV_G ? (uint32_t)(s->ptr - song.g) : 0u;
}
/* the slot of the live table that a (kind, track, id) target resolves to, -1 none */
static int host_slot_of(uint32_t kind, uint32_t t, uint32_t id)
{
    const ov_tab_t *tb = &ovb[ov_live];
    int16_t *ptr = kind == OV_P ? &trk[t].p[id] : kind == OV_G ? &song.g[id] : 0;
    uint32_t i;
    for (i = 0; i < tb->n; i++)
        if (tb->s[i].kind == kind && (ptr ? tb->s[i].ptr == ptr : tb->s[i].part == t))
            return (int)i;
    return -1;
}
/* the targets of a MAPS / RULES target byte, as macro.c resolves them: up to 4 (kind, track, id) */
static int host_targets(uint32_t tg, uint32_t id, uint32_t kind[4], uint32_t trkn[4], uint32_t pid[4])
{
    uint32_t k = tg >> 5, m = tg & WF_TMASK, t, n = 0, e;
    if (k == WF_K_GLOBAL) {
        kind[0] = OV_G, trkn[0] = WF_NONE, pid[0] = id;
        return 1;
    }
    for (t = 0; t < NTRK; t++) {
        if (!(m >> t & 1u))
            continue;
        if (k == WF_K_PARAM) {
            kind[n] = OV_P, trkn[n] = t, pid[n++] = id;
        } else if (t < NPART && k == WF_K_ROLE) {
            e = trk[t].eng_req % NENGINES;
            kind[n] = OV_P, trkn[n] = t;
            pid[n++] = e < MC_NROLE && id < WF_NEROLES && MC_ROLE[e][id] != WF_NONE ? P_E0 + MC_ROLE[e][id] : P_COUNT;
        } else if (t < NPART) {
            kind[n] = k == WF_K_BRIGHT ? OV_VCUT : OV_VSHP, trkn[n] = t, pid[n++] = 0;
        }
    }
    return (int)n;
}
int host_macro_slots(host_slot_t *out, int max)
{
    const ov_tab_t *tb = &ovb[ov_live];
    const uint8_t *p;
    uint32_t i, k, j, t, id, kd[4], tr[4], pd[4];
    int n = 0;
    if (!wrt.active)
        return 0;
    for (i = 0; i < tb->n && n < max; i++) {
        const ov_slot_t *s = &tb->s[i];
        host_slot_t *o = &out[n++];
        int32_t c = ov_cur[ov_live][i];
        memset(o, 0, sizeof *o);
        host_slot_id(s, &t, &id);
        host_param_name(s->kind, t, id, o->name, sizeof o->name);
        o->offset = c / 256.0;
        o->target = s->tgt / 256.0;
        snprintf(o->smooth, sizeof o->smooth, "%s", HOST_CLASS[s->cls & 3u]);
        o->lo = s->lo;
        o->hi = s->hi;
        if (s->kind >= OV_VCUT) {
            o->effective = (c + 128) >> 8;
            o->norm = 0.5 + c / (512.0 * GL_VMOD_MAX);
        } else {
            const param_desc_t *d = s->kind == OV_G ? &GP[id] : t == TRK_DRUM || id < P_E0 ? &TP[id] :
                                    &ENGINES[trk[t].eng_req % NENGINES]->edit[id - P_E0];
            o->base = *s->ptr;                   /* (between blocks: the authored value) */
            o->effective = ov_effective(s, o->base, c);
            o->norm = d->max > d->min ? (double)(o->effective - d->min) / (d->max - d->min) : 0;
        }
        for (k = 0, p = mac.maps; k < mac.nmaps; k++, p += WF_MAP_LEN)   /* who moves it */
            for (j = 0; j < (uint32_t)host_targets(p[1], p[2], kd, tr, pd); j++)
                if (host_slot_of(kd[j], tr[j], pd[j]) == (int)i)
                    o->ctls |= 1u << (p[0] % WF_NCTL);
        for (k = 0, p = mac.rules; k < mac.nrules; k++, p += WF_RULE_HDR + WF_ACT_LEN * p[3])
            for (j = 0; j < p[3]; j++) {
                const uint8_t *a = p + WF_RULE_HDR + WF_ACT_LEN * j;
                uint32_t q, nt = (uint32_t)host_targets(a[0], a[1], kd, tr, pd);
                for (q = 0; q < nt; q++)
                    if (host_slot_of(kd[q], tr[q], pd[q]) == (int)i)
                        o->ctls |= 1u << 16;
            }
    }
    return n;
}
int host_macro_mappings(host_mapping_t *out, int max)
{
    const uint8_t *p;
    uint32_t k, j, kd[4], tr[4], pd[4];
    int n = 0;
    if (!wrt.active)
        return 0;
    for (k = 0, p = mac.maps; k < mac.nmaps; k++, p += WF_MAP_LEN) {
        uint32_t nt = (uint32_t)host_targets(p[1], p[2], kd, tr, pd), cv = p[3] & WF_CURVE_MASK;
        for (j = 0; j < nt && n < max; j++) {
            host_mapping_t *o = &out[n++];
            memset(o, 0, sizeof *o);
            o->ctl = p[0] % WF_NCTL;
            o->min = (int8_t)p[4];
            o->max = (int8_t)p[5];
            snprintf(o->curve, sizeof o->curve, "%s", cv < WF_NBUILTIN_CURVES ? HOST_CURVE[cv] : "lut");
            o->offset = mc_offset(p) / 256.0;
            o->slot = -1;
            if (pd[j] >= P_COUNT) {              /* (a role this engine lacks) */
                char tn[16];
                host_track_name(tr[j], tn, sizeof tn);
                snprintf(o->target, sizeof o->target, "%s.(no role)", tn);
                continue;
            }
            o->role = 1;
            host_param_name(kd[j], tr[j], pd[j], o->target, sizeof o->target);
            o->slot = host_slot_of(kd[j], tr[j], pd[j]);
            if (kd[j] >= OV_VCUT) {              /* the value now: the slot's, else the base (at home) */
                int32_t c = o->slot >= 0 ? ov_cur[ov_live][o->slot] : 0;
                o->effective = (c + 128) >> 8;
                o->norm = 0.5 + c / (512.0 * GL_VMOD_MAX);
            } else {
                int16_t *ptr = kd[j] == OV_G ? &song.g[pd[j]] : &trk[tr[j]].p[pd[j]];
                const param_desc_t *d = kd[j] == OV_G ? &GP[pd[j]] : tr[j] == TRK_DRUM || pd[j] < P_E0 ? &TP[pd[j]] :
                                        &ENGINES[trk[tr[j]].eng_req % NENGINES]->edit[pd[j] - P_E0];
                o->effective = o->slot >= 0 ? ov_effective(&ovb[ov_live].s[o->slot], *ptr, ov_cur[ov_live][o->slot]) : *ptr;
                o->norm = d->max > d->min ? (double)(o->effective - d->min) / (d->max - d->min) : 0;
            }
        }
    }
    return n;
}
int host_macro_rules(host_rule_t *out, int max)
{
    const uint8_t *p;
    uint32_t k, j, kd[4], tr[4], pd[4];
    int n = 0;
    if (!wrt.active)
        return 0;
    for (k = 0, p = mac.rules; k < mac.nrules && n < max; k++, p += WF_RULE_HDR + WF_ACT_LEN * p[3]) {
        host_rule_t *o = &out[n++];
        memset(o, 0, sizeof *o);
        o->a = p[0] >> 4;
        o->b = p[0] & 15;
        o->ta = p[1] / 250.0;
        o->tb = p[2] / 250.0;
        o->strength = mc_strength(p) / 4096.0;
        for (j = 0; j < p[3] && j < 4u; j++) {
            const uint8_t *a = p + WF_RULE_HDR + WF_ACT_LEN * j;
            uint32_t nt = (uint32_t)host_targets(a[0], a[1], kd, tr, pd);
            char nm[32] = "-", one[32];
            uint32_t q;
            if (nt > 1 && kd[0] == OV_P && pd[0] < P_COUNT) {   /* several tracks: "pad+bass+lead.level" */
                nm[0] = 0;
                for (q = 0; q < nt; q++) {
                    host_track_name(tr[q], one, sizeof one);
                    snprintf(nm + strlen(nm), sizeof nm - strlen(nm), "%s%s", q ? "+" : "", one);
                }
                host_param_name(OV_P, tr[0], pd[0], one, sizeof one);
                snprintf(nm + strlen(nm), sizeof nm - strlen(nm), "%s", strchr(one, '.') ? strchr(one, '.') : "");
            } else if (nt && pd[0] < P_COUNT) {
                host_param_name(kd[0], tr[0], pd[0], nm, sizeof nm);
            }
            snprintf(o->act[o->nact++], sizeof o->act[0], "%s %+d", nm, (int8_t)a[2]);
        }
    }
    return n;
}
void host_energy(host_energy_t *e)
{
    memset(e, 0, sizeof *e);
    if (!wrt.active)
        return;
    e->nbands = arr.et.n;
    e->band = arr.mb;
    e->sel = arr.sel;
    e->pos = (int)arr_pos();
    e->layers = 15 & ~wrt.mute;
    e->fill = arr.fill_now;
    e->lanes = arr.lanes;
    e->density = arr.dens;
}

const char *host_world_error(int code)
{
    static char b[16];
    const char *s = WE_NAMES;
    int k = 0;
    code = code < 0 ? -code : code;
    for (; *s && k < code; s++)
        k += *s == ' ';
    for (k = 0; *s && *s != ' ' && k < (int)sizeof b - 1; s++)
        b[k++] = *s;
    b[k] = 0;
    return k ? b : "?";
}

/* ------------------------------------------------ names, colours, the font (constant data) --- */
const char *host_version(void) { return FELUCCA_VERSION; }
const char *host_button_name(uint32_t label) { return label < NB ? B_NAME[label] : "?"; }
const char *host_encoder_name(uint32_t role) { return role < NE ? E_NAME[role] : "?"; }
uint32_t host_knob_rgb(uint32_t k)
{
    uint32_t c = TE_COL[k & 3u];
    return ((c >> 11) * 255u / 31u) << 16 | ((c >> 5 & 63u) * 255u / 63u) << 8 | (c & 31u) * 255u / 31u;
}
static uint32_t host_mix(uint32_t a, uint32_t b, uint32_t t)   /* 0xRRGGBB, t 0..255: a -> b */
{
    uint32_t r = ((a >> 16 & 255u) * (255u - t) + (b >> 16 & 255u) * t) / 255u;
    uint32_t g = ((a >> 8 & 255u) * (255u - t) + (b >> 8 & 255u) * t) / 255u;
    return 0xFF000000u | r << 16 | g << 8 | ((a & 255u) * (255u - t) + (b & 255u) * t) / 255u;
}
int host_text(uint32_t *px, int stride, int w, int h, int x, int y, int large, const char *s, uint32_t rgb)
{
    const felucca_font_t *f = large ? &FONT_L : &FONT_S;   /* as gfx.c cv_text: 4-bit alpha */
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), bw = f->bw[gi], bpr = (bw + 1u) / 2u, gx, gy;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < bw; gx++) {
                uint32_t a = gd[gy * bpr + gx / 2u];
                int cx = x - f->pad + (int)gx, cy = y + (int)gy;
                a = (gx & 1u) ? (a & 15u) : (a >> 4);
                if (a && cx >= 0 && cx < w && cy >= 0 && cy < h) {
                    uint32_t *d = &px[cy * stride + cx];
                    *d = host_mix(*d, rgb, a * 17u);
                }
            }
        x += f->adv[gi];
    }
    return x;
}
int host_text_width(int large, const char *s) { return (int)text_w(large ? &FONT_L : &FONT_S, s); }
int host_text_height(int large) { return large ? FONT_L.h : FONT_S.h; }
