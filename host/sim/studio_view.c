/* SPDX-License-Identifier: GPL-3.0-only */
/* FLOWSTATE STUDIO (UI spec §12), main thread: everything drawn comes from the snapshot's Studio model
 * (sim.h studio_t), everything done goes to the firmware thread as commands.
 *
 *   top          the World, its category, key and tempo, the scene; < > choose a World (the current one plays
 *                on), LOAD confirms (on the next bar while playing), CANCEL forgets; the device's screen
 *   middle       scenes A B C D (the one playing filled, the one asked for marked: it changes on the next bar)
 *                and the variation selector (greyed until variations exist: Phase 12)
 *   controls     COLOR MOTION SPACE ENERGY, 0..100 with the spec's end words: a World's macros (what they move is
 *                the World's: the inspector shows it), a SLOOP project's KNOB 1..4; the line under them says which
 *   performance  PLAY REC PULSE BEAT FX: the FM-1's PLAY REC ARP SEQ FX buttons with their LEDs, held while
 *                the mouse is down (right-click latches)
 *   tracks       by role: mute, level, the sound; a click on the sound gives the keys that track
 *   input        KEYS: what the keys do (SLOOP's keys until SMART MELODY, Phase 6), the 27 keys
 * The static parts (background, cards, labels, knob tracks, legend) are drawn once into a cached layer. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sim.h"

/* ------------------------------------------------ look --- */
#define BG RGBX(13, 14, 17)
#define HEAD RGBX(19, 20, 25)
#define CARD RGBX(23, 24, 30)
#define EDGE RGBX(42, 44, 54)
#define TEXT RGBX(232, 232, 238)
#define DIM RGBX(128, 132, 146)
#define FAINT RGBX(78, 81, 94)
#define ACCENT RGBX(124, 96, 255)                /* the spec's violet */
#define LAV RGBX(178, 168, 255)                  /* the spec's KEYS line */
#define LED_ON RGBX(255, 176, 46)                /* the device's LEDs */
#define LED_DIM RGBX(120, 84, 28)
#define LED_OFF RGBX(52, 54, 62)

/* ------------------------------------------------ layout (window units) --- */
#define WX 24                                    /* the World card */
#define WY 62
#define WW 688
#define WH 124
#define ARROW 36
#define SX 24                                    /* scenes */
#define SY 198
#define SW 132
#define SH 64
#define SGAP 8
#define VX 588                                   /* the variation selector */
#define VW 124
#define LX 730                                   /* the device's screen */
#define LY 62
#define MY 334                                   /* macros */
#define MH 136
#define MR 38
#define PY 500                                   /* performance and tracks */
#define PH 92
#define PBX 36
#define PBW 78
#define PBH 52
#define TX 478
#define TSX 490
#define TSW 118
#define KY 604                                   /* the KEYS line */
#define BRX 140                                  /* the World browser */
#define BRY 66
#define BRW 456
#define BRH 268
#define BROWS 8                                  /* rows the World browser shows */
static const kbd_t KBD = {28, 638, 59, 122, 36, 76};
static const uint8_t PERF[5] = {HOST_B_PLAY, HOST_B_REC, HOST_B_ARP, HOST_B_SEQ, HOST_B_FX};
static const char *const PERF_NAME[5] = {"PLAY", "REC", "PULSE", "BEAT", "FX"};
static const char *const PERF_SUB[5] = {"", "", "ARP", "SEQ", "HOLD"};
static const char *const MACRO_LO[4] = {"DARK", "STILL", "CLOSE", "SPARSE"};
static const char *const MACRO_HI[4] = {"BRIGHT", "ALIVE", "HUGE", "INTENSE"};
#define A0 ((float)(0.75 * M_PI))                /* knob travel: 135 .. 405 degrees, clockwise */
#define A1 ((float)(2.25 * M_PI))

static int macro_cx(int k) { return 24 + 119 + k * 238; }
static int scene_x(int i) { return SX + i * (SW + SGAP); }
static int perf_x(int i) { return PBX + i * (PBW + 6); }
static int strip_x(int k) { return TSX + k * (TSW + 3); }
static int in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

/* ------------------------------------------------ small shapes --- */
static void chevron(canvas_t *c, int cx, int cy, int dir, uint32_t col)   /* < (dir -1) or > (dir 1) */
{
    c_line(c, (float)(cx - 4 * dir), (float)(cy - 9), (float)(cx + 4 * dir), (float)cy, 3.0f, col);
    c_line(c, (float)(cx + 4 * dir), (float)cy, (float)(cx - 4 * dir), (float)(cy + 9), 3.0f, col);
}
static void check(canvas_t *c, int x, int y, int on, uint32_t col)   /* an 18 x 18 box, ticked when on */
{
    c_frame(c, x, y, 18, 18, 4.0f, 2.0f, on ? col : FAINT, on ? c_mix(CARD, col, 40) : CARD);
    if (on) {
        c_line(c, (float)x + 4.5f, (float)y + 9.5f, (float)x + 8.0f, (float)y + 13.0f, 2.4f, TEXT);
        c_line(c, (float)x + 8.0f, (float)y + 13.0f, (float)x + 14.0f, (float)y + 5.0f, 2.4f, TEXT);
    }
}
static void card(canvas_t *c, int x, int y, int w, int h) { c_frame(c, x, y, w, h, 10.0f, 1.0f, EDGE, CARD); }
static void label(canvas_t *c, int x, int y, const char *s) { c_text_sp(c, x, y, s, 2, FAINT); }

/* ------------------------------------------------ the static layer --- */
static canvas_t layer;
static void draw_static(int w, int h)
{
    static char lg[5][192];
    int k;
    if (!(layer.px = malloc((size_t)w * (size_t)h * 4u)))
        return;
    layer.w = w;
    layer.h = h;
    c_fill(&layer, BG);
    c_rect(&layer, 0, 0, w, 50, HEAD);
    c_rect(&layer, 0, 50, w, 1, EDGE);
    c_text_sp(&layer, 24, 17, "FLOWSTATE STUDIO", 3, TEXT);
    card(&layer, WX, WY, WW, WH);
    card(&layer, LX, LY, 246, 246);
    c_text_c(&layer, LX + 123, LY + 252, "DEVICE SCREEN  \xB7  TAB: THE FM-1", FAINT);
    card(&layer, 24, MY, 952, MH);
    for (k = 0; k < 4; k++) {
        int cx = macro_cx(k), cy = MY + 70;
        c_text_sp(&layer, cx - (c_text_w(MACRO_NAME[k]) + 2 * ((int)strlen(MACRO_NAME[k]) - 1)) / 2, MY + 12,
                  MACRO_NAME[k], 2, TEXT);
        c_arc(&layer, (float)cx, (float)cy, (float)MR, 6.0f, A0, A1, RGBX(40, 42, 52));
        c_text(&layer, cx - 84, MY + 108, MACRO_LO[k], DIM);
        c_text_r(&layer, cx + 84, MY + 108, MACRO_HI[k], DIM);
    }
    card(&layer, 24, PY, 442, PH);
    card(&layer, TX, PY, 498, PH);
    label(&layer, 36, PY + 8, "PERFORMANCE");
    label(&layer, TSX, PY + 8, "TRACKS");
    keys_legend(1, lg);
    c_text(&layer, KBD.x - 4, 782, lg[1], FAINT);
    c_text(&layer, KBD.x - 4, 800, lg[2], FAINT);
    c_rect(&layer, 0, 820, w, h - 820, RGBX(10, 10, 13));
}

/* ------------------------------------------------ dynamic parts --- */
static void draw_world(canvas_t *c, const studio_t *m)
{
    char b[64], sc[16];
    int cx = WX + WW / 2, tw, x;
    double now = sim_now();
    c_frame(c, WX + 12, WY + 34, ARROW, ARROW, 8.0f, 1.0f, EDGE, RGBX(30, 31, 38));
    c_frame(c, WX + WW - 12 - ARROW, WY + 34, ARROW, ARROW, 8.0f, 1.0f, EDGE, RGBX(30, 31, 38));
    chevron(c, WX + 12 + ARROW / 2, WY + 34 + ARROW / 2, -1, m->nworlds ? TEXT : FAINT);
    chevron(c, WX + WW - 12 - ARROW / 2, WY + 34 + ARROW / 2, 1, m->nworlds ? TEXT : FAINT);
    snprintf(sc, sizeof sc, "SCENE %s", cmd_scene_name(m->scene));
    tw = c_text_lw(m->title) + (m->scene >= 0 ? 28 + c_text_lw(sc) : 0);
    x = c_text_l(c, cx - tw / 2, WY + 16, m->title, TEXT);
    if (m->scene >= 0) {
        c_ring(c, (float)x + 14.0f, (float)WY + 32.0f, 0.0f, 4.0f, DIM);
        c_text_l(c, x + 28, WY + 16, sc, TEXT);
    }
    snprintf(b, sizeof b, "%s  \xB7  %s  \xB7  %d BPM", m->category, m->key, m->bpm);
    c_text_c(c, cx, WY + 60, b, DIM);
    if (m->pending >= 0) {
        snprintf(b, sizeof b, "LOADS ON THE NEXT BAR: %s", m->w[m->pending].name);
        c_text_c(c, cx, WY + 92, b, fmod(now, 0.8) < 0.5 ? ACCENT : c_mix(CARD, ACCENT, 140));
    } else if (m->msg[0]) {
        c_text_c(c, cx, WY + 92, m->msg, ACCENT);
    } else if (m->kind == SK_PROJECT || m->world < 0) {
        c_text_c(c, cx, WY + 92, m->world >= 0 ? "A SLOOP PROJECT: ITS SCENES ARE SECTIONS A-D" :
                 "SLOOP, NO WORLD:  < >  TO CHOOSE ONE", FAINT);
    } else {
        snprintf(b, sizeof b, "\"%s\"%s", m->blurb, m->kind == SK_FILE ? "  \xB7  AUTHORING: RELOADS ON SAVE" : "");
        c_text_c(c, cx, WY + 92, b, DIM);
    }
}

static void draw_scenes(canvas_t *c, const studio_t *m)
{
    double now = sim_now();
    int i;
    for (i = 0; i < 4; i++) {
        int x = scene_x(i), used = m->scenes >> i & 1, playing = m->scene == i, next = m->scene_next == i;
        char l[2] = {(char)('A' + i), 0};
        uint32_t edge = playing ? ACCENT : next ? (fmod(now, 0.6) < 0.35 ? ACCENT : EDGE) : EDGE;
        c_frame(c, x, SY, SW, SH, 10.0f, next ? 2.0f : 1.0f, edge, playing ? c_mix(CARD, ACCENT, 70) : CARD);
        c_text_l(c, x + 14, SY + 16, l, used ? TEXT : FAINT);
        c_text(c, x + 44, SY + 14, m->scene_name[i][0] ? m->scene_name[i] : "SECTION", used ? TEXT : FAINT);
        c_text(c, x + 44, SY + 34, !used ? "EMPTY" : playing ? "PLAYING" : next ? m->when : "", playing ? LAV : next ? ACCENT : FAINT);
        if (playing)
            c_ring(c, (float)(x + SW - 14), (float)(SY + 14), 0.0f, 4.5f, TEXT);
    }
    if (m->nvar > 0) {                           /* VAR: < name >, the one asked for marked */
        int next = m->var_next >= 0;
        char b[16];
        c_frame(c, VX, SY, VW, SH, 10.0f, next ? 2.0f : 1.0f, next && fmod(now, 0.6) < 0.35 ? ACCENT : EDGE, CARD);
        label(c, VX + 12, SY + 10, "VAR");
        snprintf(b, sizeof b, "%d/%d", (next ? m->var_next : m->var) + 1, m->nvar);
        c_text_r(c, VX + VW - 12, SY + 10, next ? "NEXT BAR" : b, next ? ACCENT : FAINT);
        chevron(c, VX + 12, SY + 42, -1, DIM);
        chevron(c, VX + VW - 12, SY + 42, 1, DIM);
        c_text_c(c, VX + VW / 2, SY + 34, m->var_name[next ? m->var_next : m->var], next ? ACCENT : TEXT);
    } else {
        c_frame(c, VX, SY, VW, SH, 10.0f, 1.0f, EDGE, RGBX(18, 19, 23));
        label(c, VX + 12, SY + 10, "VAR");
        c_text(c, VX + 12, SY + 34, "NONE", FAINT);
    }
    if (m->scene_next >= 0 || m->var_next >= 0) {
        char b[64];
        if (m->scene_next >= 0)
            snprintf(b, sizeof b, "CHANGES %s:  %s %s%s%s", m->when, cmd_scene_name(m->scene_next),
                     m->scene_name[m->scene_next], m->var_next >= 0 ? "  \xB7  " : "",
                     m->var_next >= 0 ? m->var_name[m->var_next] : "");
        else
            snprintf(b, sizeof b, "CHANGES NEXT BAR:  %s", m->var_name[m->var_next]);
        c_text_c(c, (SX + VX + VW) / 2, SY + SH + 10, b, ACCENT);
    } else {
        c_text_c(c, (SX + VX + VW) / 2, SY + SH + 10, "scenes change on their transition's bar, variations on the next "
                 "(Option+1..4, V)", FAINT);
    }
}

static void draw_macros(canvas_t *c, const studio_t *m, double now)
{
    int k;
    for (k = 0; k < 4; k++) {
        int cx = macro_cx(k), cy = MY + 70, v = m->macro[k];
        uint32_t col = 0xFF000000u | host_knob_rgb((uint32_t)k);
        float a = A0 + (A1 - A0) * (float)v / 100.0f;
        char b[8];
        if (keys_flash(HOST_EN_K1 + (uint32_t)k) > now)
            c_ring(c, (float)cx, (float)cy, (float)MR + 7.0f, (float)MR + 9.0f, c_mix(CARD, col, 90));
        if (v > 0)
            c_arc(c, (float)cx, (float)cy, (float)MR, 6.0f, A0, a, col);
        c_ring(c, (float)cx + (float)MR * cosf(a), (float)cy + (float)MR * sinf(a), 0.0f, 5.5f, TEXT);
        snprintf(b, sizeof b, "%d", v);
        c_text_l(c, cx - c_text_lw(b) / 2, cy - 16, b, TEXT);
    }
}

static void draw_macro_line(canvas_t *c, const sim_snap_t *s)   /* under the knobs: what they are */
{
    const studio_t *m = &s->studio;
    const host_energy_t *e = &s->energy;
    char b[160], lay[48] = "";
    int k;
    if (!m->macro_live) {
        c_text_c(c, 500, MY + MH + 8, "a SLOOP project: these turn SLOOP's KNOB 1-4 (the device screen shows what they "
                 "change)", FAINT);
        return;
    }
    for (k = 0; k < HOST_NTRK; k++)
        if (e->layers >> k & 1)
            snprintf(lay + strlen(lay), sizeof lay - strlen(lay), "%s%s", lay[0] ? " " : "", m->role[k]);
    if (e->nbands)
        snprintf(b, sizeof b, "%s's own mappings (Option+I shows them)  \xB7  ENERGY band %d/%d: %s%s", m->title,
                 e->band + 1, e->nbands, lay, e->sel != e->band ? "  (changing)" : "");
    else
        snprintf(b, sizeof b, "%s's own mappings (Option+I shows them)", m->title);
    c_text_c(c, 500, MY + MH + 8, b, FAINT);
}

static void draw_perf(canvas_t *c, const sim_snap_t *s)
{
    int i;
    for (i = 0; i < 5; i++) {
        int x = perf_x(i), y = PY + 30, b = PERF[i], led = s->led_btn[b], held = (int)(s->buttons >> b & 1u);
        uint32_t face = held ? RGBX(58, 60, 74) : RGBX(32, 33, 41);
        char sub[16];
        if (led == 2)
            face = c_mix(face, LED_ON, 40);
        if (keys_latched_button(b))
            c_rrect(c, x - 3, y - 3, PBW + 6, PBH + 6, 9.0f, RGBX(80, 200, 255));
        c_frame(c, x, y, PBW, PBH, 8.0f, 1.0f, held ? TEXT : EDGE, face);
        c_ring(c, (float)x + 12.0f, (float)y + 12.0f, 0.0f, 4.5f, led == 2 ? LED_ON : led ? LED_DIM : LED_OFF);
        c_text_c(c, x + PBW / 2 + 4, y + 7, PERF_NAME[i], TEXT);
        snprintf(sub, sizeof sub, "%s%s%s", PERF_SUB[i], PERF_SUB[i][0] ? " " : "", keys_name_button(b));
        c_text_c(c, x + PBW / 2, y + 29, sub, FAINT);
    }
}

static void draw_tracks(canvas_t *c, const studio_t *m)
{
    int k;
    for (k = 0; k < HOST_NTRK; k++) {
        int x = strip_x(k), y = PY + 30, on = !m->mute[k], lv = m->level[k], sel = m->sel == k;
        uint32_t col = 0xFF000000u | host_knob_rgb((uint32_t)k);
        char snd[16];
        check(c, x, y, on, col);
        c_text(c, x + 26, y + 1, m->role[k], on ? TEXT : DIM);
        c_rrect(c, x, y + 28, 106, 6, 3.0f, RGBX(40, 42, 52));
        c_rrect(c, x, y + 28, 6 + 100 * lv / 127, 6, 3.0f, on ? col : c_mix(CARD, col, 90));
        c_ring(c, (float)(x + 3 + 100 * lv / 127), (float)(y + 31), 0.0f, 6.0f, on ? TEXT : DIM);
        snprintf(snd, sizeof snd, "%.13s", m->sound[k]);
        c_text(c, x, y + 42, snd, sel ? LAV : FAINT);
        if (sel)
            c_rect(c, x, y + 60, c_text_w(snd), 2, LAV);
    }
}

static void draw_keys_line(canvas_t *c, const studio_t *m)   /* SMART MELODY once Smart Keys map the keys */
{
    char a[64], b[48];
    int w;
    if (m->keys_smart)
        snprintf(a, sizeof a, "KEYS: %s", m->keys);
    else
        snprintf(a, sizeof a, "KEYS: SLOOP  %s  %s", m->role[m->sel], m->keys);
    snprintf(b, sizeof b, "%s%s%s", m->chord[0] ? "   CHORD " : "", m->chord, m->keys_smart ? "" : "   (SMART MELODY: PHASE 6)");
    w = c_text_w(a) + c_text_w(b);
    c_text(c, c_text(c, 500 - w / 2, KY, a, LAV), KY, b, FAINT);
}

/* the browser's rows: the entries, with a heading before the SLOOP projects (-1); the window around the cursor */
static int browser_rows(const studio_t *m, int *row, int *first)
{
    int i, n = 0, cur = 0;
    for (i = 0; i < m->nworlds && n < STUDIO_WORLDS + 1; i++) {
        if ((m->w[i].kind == SK_PROJECT || m->w[i].kind == SK_USER) && (!i || m->w[i - 1].kind != m->w[i].kind))
            row[n++] = m->w[i].kind == SK_USER ? -2 : -1;
        if (i == m->browse)
            cur = n;
        row[n++] = i;
    }
    *first = cur - BROWS / 2;
    *first = *first + BROWS > n ? n - BROWS : *first;
    *first = *first < 0 ? 0 : *first;
    return n;
}

static void draw_browser(canvas_t *c, const studio_t *m)
{
    int row[STUDIO_WORLDS + 2], r, first, n = browser_rows(m, row, &first);
    c_frame(c, BRX - 2, BRY - 2, BRW + 4, BRH + 4, 12.0f, 2.0f, ACCENT, RGBX(17, 17, 23));
    label(c, BRX + 18, BRY + 14, "CHOOSE WORLD");
    c_text_r(c, BRX + BRW - 18, BRY + 14, "THE CURRENT ONE PLAYS ON", FAINT);
    for (r = first; r < n && r < first + BROWS; r++) {
        int i = row[r], y = BRY + 42 + (r - first) * 22, sel = i == m->browse;
        char b[48];
        if (i < 0) {
            label(c, BRX + 40, y + 2, i == -2 ? "MY WORLDS" : "SLOOP PROJECTS");
            continue;
        }
        if (sel)
            c_rrect(c, BRX + 10, y - 3, BRW - 20, 22, 6.0f, c_mix(RGBX(17, 17, 23), ACCENT, 70));
        c_text(c, BRX + 20, y, sel ? "\xBB" : " ", ACCENT);
        snprintf(b, sizeof b, "%s", m->w[i].name);
        c_text(c, BRX + 40, y, b, sel ? TEXT : DIM);
        if (m->w[i].kind == SK_FILE)
            snprintf(b, sizeof b, "FILE%s", i == m->world ? "  \xB7 PLAYING" : "");
        else if (m->w[i].kind == SK_USER)
            snprintf(b, sizeof b, "USER WORLD%s", i == m->world ? "  \xB7 PLAYING" : "");
        else
            snprintf(b, sizeof b, "%s \xB7 %d BPM%s", m->w[i].category, m->w[i].bpm, i == m->world ? "  \xB7 PLAYING" : "");
        c_text_r(c, BRX + BRW - 20, y, b, i == m->world ? LAV : FAINT);
    }
    c_frame(c, BRX + BRW - 220, BRY + BRH - 46, 96, 32, 8.0f, 1.0f, ACCENT, c_mix(RGBX(17, 17, 23), ACCENT, 110));
    c_text_c(c, BRX + BRW - 172, BRY + BRH - 38, "LOAD", TEXT);
    c_frame(c, BRX + BRW - 114, BRY + BRH - 46, 96, 32, 8.0f, 1.0f, EDGE, RGBX(30, 31, 38));
    c_text_c(c, BRX + BRW - 66, BRY + BRH - 38, "CANCEL", DIM);
    c_text(c, BRX + 18, BRY + BRH - 38, m->playing ? "LOADS ON THE NEXT BAR" : "LOADS NOW", FAINT);
}

void studio_view_draw(canvas_t *c, const sim_snap_t *s, const char *status1, const char *status2)
{
    const studio_t *m = &s->studio;
    double now = sim_now();
    char b[32];
    if (!layer.px)
        draw_static(c->w, c->h);
    if (layer.px)
        c_copy(c, &layer);
    snprintf(b, sizeof b, "%d BPM", m->bpm);
    c_text_r(c, TAB_X - 18, 17, b, TEXT);
    c_text_r(c, TAB_X - 30 - c_text_w(b), 17, m->playing ? "PLAYING" : "STOPPED", m->playing ? LAV : FAINT);
    draw_world(c, m);
    c_lcd(c, LX + 3, LY + 3, 1, s->lcd);
    draw_scenes(c, m);
    draw_macros(c, m, now);
    draw_macro_line(c, s);
    draw_perf(c, s);
    draw_tracks(c, m);
    draw_keys_line(c, m);
    kbd_draw(c, &KBD, s, LED_ON, LED_DIM);
    if (m->browse >= 0)
        draw_browser(c, m);
    c_text(c, 10, 826, status1, RGBX(232, 232, 236));
    c_text(c, 10, 846, status2, RGBX(150, 200, 160));
}

/* ------------------------------------------------ the inspector (developer only) --- */
static void meter_bar(canvas_t *c, int x, int y, int w, double v, uint32_t col)   /* 0..1 */
{
    int f = (int)(w * (v < 0 ? 0 : v > 1 ? 1 : v) + 0.5);
    c_rect(c, x, y + 7, w, 3, RGBX(40, 42, 52));
    c_rect(c, x, y + 7, f, 3, col);
}
/* a World: each macro's hidden mappings with their parameter's effective value now (normalised to its range: "pad.cutoff
 * 0.64"), the rules and how strongly they act, the ENERGY band; else the selected track's raw parameters */
void inspector_draw(canvas_t *c, const sim_snap_t *s)
{
    const studio_t *m = &s->studio;
    int x0 = 560, y0 = 56, w = 416, h = 756, i, k, y;
    char b[96];
    c_frame(c, x0, y0, w, h, 10.0f, 2.0f, ACCENT, RGBX(9, 9, 12));
    label(c, x0 + 16, y0 + 14, "INSPECTOR");
    c_text_r(c, x0 + w - 16, y0 + 14, "OPTION+I CLOSES", FAINT);
    if (!m->macro_live) {
        snprintf(b, sizeof b, "TRACK %d  %s  %s", m->sel + 1, m->role[m->sel], m->sound[m->sel]);
        c_text(c, x0 + 16, y0 + 40, b, LAV);
        c_text(c, x0 + 16, y0 + 60, "raw SLOOP parameters (no World: no macros)", FAINT);
        for (i = 0; i < s->nparams; i++) {
            const host_param_t *p = &s->params[i];
            int col = i / 31, row = i % 31, x = x0 + 16 + col * 200, yy = y0 + 90 + row * 21;
            int span = p->max - p->min;
            c_text(c, x, yy, p->label, DIM);
            c_text_r(c, x + 128, yy, p->text, TEXT);
            meter_bar(c, x + 136, yy, 48, span > 0 ? (double)(p->value - p->min) / span : 0, ACCENT);
        }
        return;
    }
    snprintf(b, sizeof b, "MACROS  \xB7  %s", m->title);
    c_text(c, x0 + 16, y0 + 40, b, LAV);
    c_text(c, x0 + 16, y0 + 58, "the hidden mappings, live (PLAY never shows them)", FAINT);
    y = y0 + 84;
    for (k = 0; k < 4; k++) {
        uint32_t col = 0xFF000000u | host_knob_rgb((uint32_t)k);
        c_ring(c, (float)x0 + 21.0f, (float)y + 8.0f, 0.0f, 4.0f, col);
        snprintf(b, sizeof b, "%s  %d", MACRO_NAME[k], m->macro[k]);
        c_text(c, x0 + 32, y, b, TEXT);
        c_text_r(c, x0 + w - 16, y, "range   now", FAINT);
        y += 19;
        for (i = 0; i < s->nmaps && y < y0 + h - 150; i++) {
            const host_mapping_t *p = &s->maps[i];
            if (p->ctl != k)
                continue;
            c_text(c, x0 + 32, y, p->target, p->role ? DIM : FAINT);
            snprintf(b, sizeof b, "%+d..%+d %s", p->min, p->max, p->curve);
            c_text_r(c, x0 + 296, y, b, FAINT);
            if (p->role) {                       /* the parameter now (moved by this mapping, or others, or at home) */
                snprintf(b, sizeof b, "%.2f", p->norm);
                c_text_r(c, x0 + 342, y, b, p->offset != 0 ? TEXT : DIM);
                meter_bar(c, x0 + 350, y, 50, p->norm, p->offset != 0 ? col : c_mix(CARD, col, 120));
            }
            y += 18;
        }
        y += 6;
    }
    label(c, x0 + 16, y, "RULES");
    y += 20;
    for (i = 0; i < s->nrules && y < y0 + h - 70; i++) {
        const host_rule_t *r = &s->rules[i];
        char acts[160] = "";
        if (r->a == r->b)
            snprintf(b, sizeof b, "%s > %.2f", host_macro_name((uint32_t)r->a), r->ta);
        else
            snprintf(b, sizeof b, "%s > %.2f & %s > %.2f", host_macro_name((uint32_t)r->a), r->ta,
                     host_macro_name((uint32_t)r->b), r->tb);
        c_text(c, x0 + 32, y, b, r->strength > 0 ? TEXT : DIM);
        snprintf(b, sizeof b, "%.2f", r->strength);
        c_text_r(c, x0 + 342, y, b, r->strength > 0 ? TEXT : DIM);
        meter_bar(c, x0 + 350, y, 50, r->strength, ACCENT);
        for (k = 0; k < r->nact; k++)
            snprintf(acts + strlen(acts), sizeof acts - strlen(acts), "%s%s", k ? ", " : "", r->act[k]);
        snprintf(b, sizeof b, "%.44s", acts);
        c_text(c, x0 + 40, y + 17, b, FAINT);
        y += 38;
    }
    {
        const host_energy_t *e = &s->energy;
        char lay[48] = "";
        for (k = 0; k < HOST_NTRK; k++)
            if (e->layers >> k & 1)
                snprintf(lay + strlen(lay), sizeof lay - strlen(lay), "%s%s", lay[0] ? " " : "", m->role[k]);
        label(c, x0 + 16, y, "ENERGY ARRANGEMENT");
        y += 20;
        if (e->nbands)
            snprintf(b, sizeof b, "band %d/%d at %.2f%s%s", e->band + 1, e->nbands, e->pos / 1000.0,
                     e->sel != e->band ? " (next: " : "", e->sel != e->band ? "on the bar)" : "");
        else
            snprintf(b, sizeof b, "no ENERGY table in this scene: all play");
        c_text(c, x0 + 32, y, b, TEXT);
        c_text(c, x0 + 32, y + 18, lay, DIM);
        if (e->fill)
            c_text_r(c, x0 + w - 16, y + 18, "FILL", LAV);
        snprintf(b, sizeof b, "%d of 48 slots moving", s->nslots);
        c_text_r(c, x0 + w - 16, y, b, FAINT);
    }
}

/* ------------------------------------------------ mouse --- */
enum { DRAG_NONE, DRAG_MACRO, DRAG_LEVEL };
static int drag, drag_k, drag_y, drag_acc, mouse_x, mouse_y;
static float wheel_acc;

static int level_at(int k, int x)
{
    int v = (x - strip_x(k) - 3) * 127 / 100;
    return v < 0 ? 0 : v > 127 ? 127 : v;
}
static void mouse_down(int x, int y, int button, const studio_t *m)
{
    int i, left = button == SDL_BUTTON_LEFT && !(SDL_GetModState() & KMOD_CTRL);
    if (m->browse >= 0) {                        /* the World browser has the mouse */
        int row[STUDIO_WORLDS + 1], first, n = browser_rows(m, row, &first), r;
        if (in(x, y, BRX + BRW - 220, BRY + BRH - 46, 96, 32))
            keys_send(OP_WORLD, WA_CONFIRM, 0, 0);
        else if (!in(x, y, BRX, BRY, BRW, BRH) || in(x, y, BRX + BRW - 114, BRY + BRH - 46, 96, 32))
            keys_send(OP_WORLD, WA_CANCEL, 0, 0);
        else
            for (r = first; r < n && r < first + BROWS; r++)
                if (row[r] >= 0 && in(x, y, BRX + 10, BRY + 39 + (r - first) * 22, BRW - 20, 22))
                    keys_send(OP_WORLD, row[r] == m->browse ? WA_CONFIRM : WA_PICK, row[r], 0);   /* (again: load) */
        return;
    }
    if (in(x, y, WX + 12, WY + 34, ARROW, ARROW) || in(x, y, WX + WW - 12 - ARROW, WY + 34, ARROW, ARROW)) {
        keys_send(OP_WORLD, WA_STEP, x < WX + WW / 2 ? -1 : 1, 0);
        return;
    }
    if (in(x, y, WX + ARROW + 24, WY + 8, WW - 2 * ARROW - 48, 72)) {   /* the title: choose */
        keys_send(OP_WORLD, WA_STEP, 0, 0);
        return;
    }
    for (i = 0; i < 4; i++)
        if (in(x, y, scene_x(i), SY, SW, SH)) {
            keys_send(OP_SCENE, (uint8_t)i, 0, 0);
            return;
        }
    if (in(x, y, VX, SY, VW, SH)) {              /* VAR: the left half back, the right half on */
        keys_send(OP_VAR, WA_STEP, x < VX + VW / 2 ? -1 : 1, 0);
        return;
    }
    for (i = 0; i < 4; i++)
        if ((x - macro_cx(i)) * (x - macro_cx(i)) + (y - MY - 70) * (y - MY - 70) <= (MR + 14) * (MR + 14)) {
            drag = DRAG_MACRO, drag_k = i, drag_y = y, drag_acc = 0;
            return;
        }
    for (i = 0; i < 5; i++)
        if (in(x, y, perf_x(i), PY + 30, PBW, PBH)) {
            if (left)
                keys_mouse_button(PERF[i]);
            else
                keys_latch_button(PERF[i]);
            return;
        }
    for (i = 0; i < HOST_NTRK; i++) {
        int sx = strip_x(i), sy = PY + 30;
        if (in(x, y, sx, sy - 2, TSW, 22)) {
            keys_send(OP_MUTE, (uint8_t)i, -1, 0);
        } else if (in(x, y, sx - 4, sy + 22, 114, 18)) {
            drag = DRAG_LEVEL, drag_k = i;
            keys_send(OP_LEVEL, (uint8_t)i, level_at(i, x), 0);
        } else if (in(x, y, sx, sy + 40, TSW, 22)) {
            keys_send(OP_SELECT, (uint8_t)i, 0, 0);
        }
    }
}
void studio_view_mouse(const void *ev, const sim_snap_t *s)
{
    const SDL_Event *e = ev;
    int i;
    if (s->studio.browse < 0 && kbd_mouse(&KBD, ev))
        return;
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        mouse_x = e->button.x, mouse_y = e->button.y;
        mouse_down(e->button.x, e->button.y, e->button.button, &s->studio);
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            drag = DRAG_NONE;
            keys_mouse_button(-1);
        }
        break;
    case SDL_MOUSEMOTION:
        mouse_x = e->motion.x, mouse_y = e->motion.y;
        if (drag == DRAG_MACRO) {                /* 3 px up = 1 */
            drag_acc += drag_y - e->motion.y;
            drag_y = e->motion.y;
            if (drag_acc / 3) {
                keys_send(OP_MACRO, (uint8_t)drag_k, drag_acc / 3, 1);
                drag_acc %= 3;
            }
        } else if (drag == DRAG_LEVEL) {
            keys_send(OP_LEVEL, (uint8_t)drag_k, level_at(drag_k, e->motion.x), 0);
        }
        break;
    case SDL_MOUSEWHEEL: {
        float dy = e->wheel.preciseY;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            dy = -dy;
        wheel_acc += dy;
        for (i = 0; i < 4; i++)
            if ((mouse_x - macro_cx(i)) * (mouse_x - macro_cx(i)) + (mouse_y - MY - 70) * (mouse_y - MY - 70) <=
                (MR + 14) * (MR + 14) && (int)wheel_acc)
                keys_send(OP_MACRO, (uint8_t)i, 2 * (int)wheel_acc, 1);
        for (i = 0; i < HOST_NTRK; i++)
            if (in(mouse_x, mouse_y, strip_x(i) - 4, PY + 52, 114, 18) && (int)wheel_acc)
                keys_send(OP_LEVEL, (uint8_t)i, 4 * (int)wheel_acc, 1);
        if (in(mouse_x, mouse_y, VX, SY, VW, SH) && (int)wheel_acc)
            keys_send(OP_VAR, WA_STEP, wheel_acc > 0 ? 1 : -1, 0);
        wheel_acc -= (float)(int)wheel_acc;
        break;
    }
    default:
        break;
    }
}
