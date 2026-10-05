/* SPDX-License-Identifier: GPL-3.0-only */
/* The host unity root: the firmware as src/felucca.c builds it, in its order, on host/hal_host.h instead
 * of the hardware, plus the entry points of host/host.h. Nothing here makes sound or plays steps: that
 * is all the firmware's own code.
 *
 * Left out, hardware only: lcd.c and felucca.c's flash glue (hal_host.h has both), main.c (host_boot and
 * host_ui_frame mirror fm1_main; its felucca_init is copied verbatim by host/Makefile into fw_main.h),
 * and with FELUCCA_UART / OTA / CDC = 0: midi_uart.c, ota.c, editor.c, console.c, recovery.c. usb.c is
 * built (the MIDI rings, the USB state the UI shows) but never started. audio.c is built as it is: its
 * interrupt handler never runs, host_audio renders through its audio_block. */
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
/* felucca.c's flash glue (st_read, st_erase, st_prog): hal_host.h */
#include "storage.c"
#include "upreset.c"
#include "project.c"
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
static int booted, halted;

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
    if (fmt > 0)
        project_apply(&host_proj);               /* project.c: what project_load does with a slot */
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
