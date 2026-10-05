/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 panel view: the LCD at x2, the 14 buttons with their LEDs, the 7 encoders and the MASTER pot,
 * the 27 keys, a legend and two status lines; and the computer keyboard and mouse, turned into commands
 * for the firmware thread (sim.h). Everything drawn comes from a snapshot; nothing here touches the
 * firmware. Keys are mapped by scancode (by position, whatever the layout); their labels follow the layout.
 *
 *   keys      Z S X D C F V B H N J M , L . ; / ' Q W 3 E 4 R T 6 Y   F3..G5 (white: Z X C V B N M , . / Q W E R T Y)
 *   buttons   U I O P  FX SCL ENV LFO   7 8 9 0  EDIT GLO HOME SAVE   [ ]  ARP SEQ   Space PLAY   Return REC
 *             - =  OCT- OCT+  (held while the key is down)
 *   encoders  < >  PRESETS   v ^  ALGORITHM   shift < >  SELECT   shift Z X / C V / B N / M ,  KNOB 1..4
 *             shift v ^  MASTER                (key repeat keeps turning)
 *   commands  (Option)  Space PLAY / STOP   1..4 scene A..D (next bar)   5..8 mute track 1..4
 *             < > DJ filter -/+ 4   0 filter off   v ^ tempo -/+ 1
 * KNOB 1..4 stand in for COLOR, MOTION, SPACE and ENERGY until the macros exist (Phase 7). */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sim.h"

static const uint8_t WHITE_N[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const uint8_t BLACK_N[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
static const SDL_Scancode KEY_SC[SIM_KEYS] = {   /* F3 .. G5: Z row / A row, then Q row / number row */
    SDL_SCANCODE_Z, SDL_SCANCODE_S, SDL_SCANCODE_X, SDL_SCANCODE_D, SDL_SCANCODE_C, SDL_SCANCODE_F,
    SDL_SCANCODE_V, SDL_SCANCODE_B, SDL_SCANCODE_H, SDL_SCANCODE_N, SDL_SCANCODE_J, SDL_SCANCODE_M,
    SDL_SCANCODE_COMMA, SDL_SCANCODE_L, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SEMICOLON, SDL_SCANCODE_SLASH,
    SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_3, SDL_SCANCODE_E, SDL_SCANCODE_4,
    SDL_SCANCODE_R, SDL_SCANCODE_T, SDL_SCANCODE_6, SDL_SCANCODE_Y};
static const SDL_Scancode BTN_SC[HOST_NB] = {    /* HOST_B_FX .. HOST_B_OCTUP */
    SDL_SCANCODE_U, SDL_SCANCODE_I, SDL_SCANCODE_O, SDL_SCANCODE_P,
    SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_SPACE, SDL_SCANCODE_RETURN,
    SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS};
static const struct {
    SDL_Scancode dn, up;
    uint8_t shift;
} ENC_SC[SIM_NENC] = {
    [HOST_EN_SELECT] = {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, 1},
    [HOST_EN_ALGO] = {SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, 0},
    [HOST_EN_PRESET] = {SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, 0},
    [HOST_EN_K1] = {SDL_SCANCODE_Z, SDL_SCANCODE_X, 1},
    [HOST_EN_K2] = {SDL_SCANCODE_C, SDL_SCANCODE_V, 1},
    [HOST_EN_K3] = {SDL_SCANCODE_B, SDL_SCANCODE_N, 1},
    [HOST_EN_K4] = {SDL_SCANCODE_M, SDL_SCANCODE_COMMA, 1},
    [SIM_MASTER] = {SDL_SCANCODE_DOWN, SDL_SCANCODE_UP, 1},
};
static const char *const ENC_LABEL[SIM_NENC] = {"SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2",
                                                "KNOB 3", "KNOB 4", "MASTER"};
static const char *const MACRO[4] = {"COLOR", "MOTION", "SPACE", "ENERGY"};   /* (Phase 7) */

/* ------------------------------------------------ key names --- */
static char key_names[SDL_NUM_SCANCODES][8];
static const char *sc_name(SDL_Scancode sc) { return key_names[sc][0] ? key_names[sc] : "?"; }
void panel_init(int have_video)                  /* the labels of the keys on this keyboard's layout */
{
    uint32_t sc;
    for (sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
        const char *s;
        uint32_t k = 0;
        switch (sc) {
        case SDL_SCANCODE_SPACE: s = "SPC"; break;
        case SDL_SCANCODE_RETURN: s = "RET"; break;
        case SDL_SCANCODE_LEFT: s = "<"; break;
        case SDL_SCANCODE_RIGHT: s = ">"; break;
        case SDL_SCANCODE_UP: s = "^"; break;
        case SDL_SCANCODE_DOWN: s = "v"; break;
        default:
            s = have_video ? SDL_GetKeyName(SDL_GetKeyFromScancode((SDL_Scancode)sc)) : "";
            if (!*s)
                s = SDL_GetScancodeName((SDL_Scancode)sc);                  /* US positions */
            break;
        }
        while (*s && k + 1u < sizeof key_names[0]) {   /* UTF-8 -> Latin-1 (the firmware font) */
            uint8_t c = (uint8_t)*s++;
            if (c < 0x80u)
                key_names[sc][k++] = (char)c;
            else if ((c & 0xE0u) == 0xC0u && *s) {
                uint32_t cp = (c & 0x1Fu) << 6 | ((uint8_t)*s++ & 0x3Fu);
                key_names[sc][k++] = (char)(cp < 256u ? cp : '?');
            } else {
                while ((*s & 0xC0) == 0x80)
                    s++;
                key_names[sc][k++] = '?';
            }
        }
        key_names[sc][k] = 0;
    }
}
static void key_label(uint32_t n, char *b)       /* key 0 = F3 */
{
    static const char *const NM[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    snprintf(b, 6, "%s%u", NM[(5u + n) % 12u], 3u + (5u + n) / 12u);
}

void panel_print_mapping(void)
{
    uint32_t i;
    char b[8];
    printf("Computer keyboard (by key position; names as this layout labels them):\n  keys      ");
    for (i = 0; i < SIM_KEYS; i++) {
        key_label(i, b);
        printf("%s=%s%s", b, sc_name(KEY_SC[i]), i == 26u ? "\n" : i == 13u ? "\n            " : "  ");
    }
    printf("  buttons   ");
    for (i = 0; i < HOST_NB; i++)
        printf("%s=%s%s", host_button_name(i), sc_name(BTN_SC[i]), i == 6u ? "\n            " : i + 1u == HOST_NB ? "\n" : "  ");
    printf("            (held while the key is down; a tap shorter than one main-loop pass still counts)\n");
    printf("  encoders  (- / +, key repeat keeps turning)\n");
    for (i = 0; i < SIM_NENC; i++)
        printf("            %-10s %s%s / %s%s%s%s\n", ENC_LABEL[i], ENC_SC[i].shift ? "shift+" : "", sc_name(ENC_SC[i].dn),
               ENC_SC[i].shift ? "shift+" : "", sc_name(ENC_SC[i].up), i >= HOST_EN_K1 && i <= HOST_EN_K4 ? "   for now: " : "",
               i >= HOST_EN_K1 && i <= HOST_EN_K4 ? MACRO[i - HOST_EN_K1] : "");
    printf("  commands  Option+Space PLAY / STOP    Option+1..4 scene A..D (SAVE + key: on the next bar)\n"
           "            Option+5..8 mute track 1..4  Option+< / > DJ filter -/+4, Option+0 filter off\n"
           "            Option+v / ^ tempo -/+1 BPM  Esc quit\n"
           "  mouse     click / drag the keys; hold a button; wheel or drag a knob (up = clockwise);\n"
           "            right-click (or ctrl-click) latches a button or key\n");
    fflush(stdout);
}

/* ------------------------------------------------ input sources -> commands --- */
static uint32_t kb_notes, ms_notes, latch_notes, sent_notes;   /* key bits 0 (F3) .. 26 (G5) */
static uint32_t kb_btns, ms_btns, latch_btns, sent_btns;       /* label bits */
static int32_t enc_turns[SIM_NENC];              /* drawn pointer per encoder */
static double enc_flash[SIM_NENC];

static void send(uint8_t op, uint8_t a, int32_t v, uint8_t rel)
{
    sim_cmd_t c = {op, a, rel, 0, v};
    engine_send(&c);
}
static void sync_inputs(void)                    /* what is held, from every source, as changes */
{
    uint32_t n = kb_notes | ms_notes | latch_notes, b = kb_btns | ms_btns | latch_btns, d, i;
    for (d = n ^ sent_notes, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            send(OP_KEY, (uint8_t)i, (int32_t)(n >> i & 1u), 0);
    for (d = b ^ sent_btns, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            send(OP_BUTTON, (uint8_t)i, (int32_t)(b >> i & 1u), 0);
    sent_notes = n;
    sent_btns = b;
}
static void turn(uint32_t role, int32_t s)
{
    send(OP_TURN, (uint8_t)role, s, 0);
    enc_turns[role] += s;
    enc_flash[role] = sim_now() + 0.16;
}
void panel_focus_lost(void)                      /* no stuck keys after Cmd-Tab */
{
    kb_notes = kb_btns = 0;
    sync_inputs();
}

static int command(SDL_Scancode sc, int repeat)  /* Option + key */
{
    if (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT)
        send(OP_GLOBAL, HOST_G_FILTER, sc == SDL_SCANCODE_RIGHT ? 4 : -4, 1);
    else if (sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_UP)
        send(OP_GLOBAL, HOST_G_BPM, sc == SDL_SCANCODE_UP ? 1 : -1, 1);
    else if (repeat)
        return sc == SDL_SCANCODE_SPACE || (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0);
    else if (sc == SDL_SCANCODE_SPACE)
        send(OP_PLAYSTOP, 0, 0, 0);
    else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_4)
        send(OP_SCENE, (uint8_t)(sc - SDL_SCANCODE_1), 0, 0);
    else if (sc >= SDL_SCANCODE_5 && sc <= SDL_SCANCODE_8)
        send(OP_MUTE, (uint8_t)(sc - SDL_SCANCODE_5), -1, 0);
    else if (sc == SDL_SCANCODE_0)
        send(OP_GLOBAL, HOST_G_FILTER, 0, 0);
    else
        return 0;
    return 1;
}

void panel_key(const SDL_KeyboardEvent *k, int down, int *quit)
{
    SDL_Scancode sc = k->keysym.scancode;
    uint32_t shift = (k->keysym.mod & KMOD_SHIFT) != 0, i;
    if (down && sc == SDL_SCANCODE_ESCAPE) {
        *quit = 1;
        return;
    }
    if (down && (k->keysym.mod & KMOD_ALT)) {    /* Option: the commands, nothing else */
        command(sc, k->repeat);
        return;
    }
    if (down)
        for (i = 0; i < SIM_NENC; i++)
            if (ENC_SC[i].shift == shift && (sc == ENC_SC[i].dn || sc == ENC_SC[i].up)) {
                turn(i, sc == ENC_SC[i].up ? 1 : -1);
                return;
            }
    if (k->repeat)
        return;
    for (i = 0; i < SIM_KEYS; i++)
        if (sc == KEY_SC[i]) {
            kb_notes = down ? kb_notes | 1u << i : kb_notes & ~(1u << i);
            sync_inputs();
            return;
        }
    if (sc == SDL_SCANCODE_KP_ENTER)
        sc = SDL_SCANCODE_RETURN;
    for (i = 0; i < HOST_NB; i++)
        if (sc == BTN_SC[i]) {
            kb_btns = down ? kb_btns | 1u << i : kb_btns & ~(1u << i);
            sync_inputs();
            return;
        }
}

/* ------------------------------------------------ layout --- */
#define LCD_X 28
#define LCD_Y 28
#define PX0 540                                  /* the right column */
#define BTN_W 101
#define BTN_H 44
#define BTN_Y0 312
#define KB_Y 548
#define KB_H 160
#define KB_WW 58
#define KB_X0 36
#define KB_BW 34
#define KB_BH 100
#define LG_Y 724
#define ST_Y 826

static void btn_rect(uint32_t b, int *x, int *y)  /* rows: FX SCL ENV LFO / EDIT GLO HOME SAVE / ARP SEQ PLAY REC / OCT- OCT+ */
{
    *x = PX0 + (int)(b % 4u) * (BTN_W + 12);
    *y = BTN_Y0 + (int)(b / 4u) * (BTN_H + 12);
}
static void enc_pos(uint32_t r, int *cx, int *cy, int *rad)
{
    switch (r) {
    case HOST_EN_PRESET: *cx = PX0 + 70, *cy = 120, *rad = 30; break;
    case HOST_EN_SELECT: *cx = PX0 + 220, *cy = 120, *rad = 30; break;
    case HOST_EN_ALGO: *cx = PX0 + 370, *cy = 120, *rad = 30; break;
    case SIM_MASTER: *cx = PX0 + 2 * (BTN_W + 12) + 22, *cy = BTN_Y0 + 3 * (BTN_H + 12) + 22, *rad = 18; break;
    default: *cx = PX0 + 55 + 110 * (int)(r - HOST_EN_K1), *cy = 236, *rad = 24; break;
    }
}
static int black_x(uint32_t n)                   /* left edge of black key n */
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (WHITE_N[i] + 1u == n)
            break;
    return KB_X0 + (int)(i + 1u) * KB_WW - KB_BW / 2;
}

enum { HIT_NONE, HIT_KEY, HIT_BTN, HIT_ENC };
static int hit(int x, int y, uint32_t *idx)
{
    uint32_t i;
    for (i = 0; i < 11u; i++) {
        int bx = black_x(BLACK_N[i]);
        if (x >= bx && x < bx + KB_BW && y >= KB_Y && y < KB_Y + KB_BH) {
            *idx = BLACK_N[i];
            return HIT_KEY;
        }
    }
    if (y >= KB_Y && y < KB_Y + KB_H && x >= KB_X0 && x < KB_X0 + 16 * KB_WW) {
        *idx = WHITE_N[(x - KB_X0) / KB_WW];
        return HIT_KEY;
    }
    for (i = 0; i < HOST_NB; i++) {
        int bx, by;
        btn_rect(i, &bx, &by);
        if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) {
            *idx = i;
            return HIT_BTN;
        }
    }
    for (i = 0; i < SIM_NENC; i++) {
        int cx, cy, r;
        enc_pos(i, &cx, &cy, &r);
        if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= (r + 8) * (r + 8)) {
            *idx = i;
            return HIT_ENC;
        }
    }
    return HIT_NONE;
}

/* ------------------------------------------------ mouse --- */
static int ms_note = -1, drag_role = -1, drag_y, drag_acc, mouse_x, mouse_y;
static float wheel_acc;
void panel_mouse(const void *ev)
{
    const SDL_Event *e = ev;
    uint32_t idx;
    int h;
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        mouse_x = e->button.x, mouse_y = e->button.y;
        h = hit(e->button.x, e->button.y, &idx);
        if (e->button.button == SDL_BUTTON_LEFT && !(SDL_GetModState() & KMOD_CTRL)) {
            if (h == HIT_KEY)
                ms_note = (int)idx, ms_notes = 1u << idx;
            else if (h == HIT_BTN)
                ms_btns = 1u << idx;
            else if (h == HIT_ENC)
                drag_role = (int)idx, drag_y = e->button.y, drag_acc = 0;
        } else if (e->button.button == SDL_BUTTON_RIGHT || e->button.button == SDL_BUTTON_LEFT) {
            if (h == HIT_KEY)                    /* right-click / ctrl-click: latch */
                latch_notes ^= 1u << idx;
            else if (h == HIT_BTN)
                latch_btns ^= 1u << idx;
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            ms_note = drag_role = -1;
            ms_notes = ms_btns = 0;
        }
        break;
    case SDL_MOUSEMOTION:
        mouse_x = e->motion.x, mouse_y = e->motion.y;
        if (ms_note >= 0 && hit(e->motion.x, e->motion.y, &idx) == HIT_KEY && (int)idx != ms_note)
            ms_note = (int)idx, ms_notes = 1u << idx;   /* glissando */
        if (drag_role >= 0) {
            drag_acc += drag_y - e->motion.y;
            drag_y = e->motion.y;
            for (; drag_acc >= 6; drag_acc -= 6)
                turn((uint32_t)drag_role, 1);
            for (; drag_acc <= -6; drag_acc += 6)
                turn((uint32_t)drag_role, -1);
        }
        break;
    case SDL_MOUSEWHEEL: {
        float dy = e->wheel.preciseY;
        if (e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
            dy = -dy;                            /* up / away = clockwise, natural scrolling or not */
        if (hit(mouse_x, mouse_y, &idx) != HIT_ENC) {
            wheel_acc = 0;
            break;
        }
        for (wheel_acc += dy; wheel_acc >= 1.0f; wheel_acc -= 1.0f)
            turn(idx, 1);
        for (; wheel_acc <= -1.0f; wheel_acc += 1.0f)
            turn(idx, -1);
        break;
    }
    default:
        break;
    }
    sync_inputs();
}

/* ------------------------------------------------ drawing --- */
#define COL_BG RGBX(23, 24, 27)
#define COL_TEXT RGBX(216, 216, 220)
#define COL_DIM RGBX(118, 122, 132)
#define COL_LED_ON RGBX(255, 176, 46)
#define COL_LED_DIM RGBX(120, 84, 28)
#define COL_LED_OFF RGBX(52, 54, 60)
#define COL_LATCH RGBX(80, 200, 255)

static void draw_encoder(canvas_t *c, const sim_snap_t *s, uint32_t r, double now)
{
    int cx, cy, rad;
    float a;
    uint32_t ring = r >= HOST_EN_K1 && r <= HOST_EN_K4 ? 0xFF000000u | host_knob_rgb(r - HOST_EN_K1) : RGBX(150, 156, 168);
    char b[32];
    enc_pos(r, &cx, &cy, &rad);
    a = (float)enc_turns[r] * (float)(M_PI / 10.0) - (float)(M_PI / 2.0);   /* 20 detents a turn, drawn */
    if (r == SIM_MASTER)
        a = (float)(s->master_adc / 1023.0 * 1.5 * M_PI - 1.25 * M_PI);
    if (enc_flash[r] > now || drag_role == (int)r)
        c_ring(c, (float)cx, (float)cy, 0.0f, (float)rad + 5.0f, c_mix(COL_BG, ring, 70));
    c_ring(c, (float)cx, (float)cy, 0.0f, (float)rad, c_mix(COL_BG, ring, 120));
    c_ring(c, (float)cx, (float)cy, 0.0f, (float)rad - 4.0f, RGBX(44, 47, 54));
    c_line(c, (float)cx + cosf(a) * rad * 0.25f, (float)cy + sinf(a) * rad * 0.25f,
           (float)cx + cosf(a) * (rad - 8), (float)cy + sinf(a) * (rad - 8), 3.0f, RGBX(240, 240, 240));
    if (r == SIM_MASTER) {                       /* the MASTER pot: label and level to its right */
        c_text(c, cx + rad + 12, cy - 18, "MASTER", COL_TEXT);
        snprintf(b, sizeof b, "%d%%  sh %s/%s", s->master_adc * 100 / 1023, sc_name(ENC_SC[SIM_MASTER].dn),
                 sc_name(ENC_SC[SIM_MASTER].up));
        c_text(c, cx + rad + 12, cy + 2, b, COL_DIM);
        return;
    }
    c_text_c(c, cx, cy + rad + 6, ENC_LABEL[r], COL_TEXT);
    snprintf(b, sizeof b, "%s%s %s", ENC_SC[r].shift ? "sh " : "", sc_name(ENC_SC[r].dn), sc_name(ENC_SC[r].up));
    c_text_c(c, cx, cy + rad + 24, b, COL_DIM);
}

static void draw_button(canvas_t *c, const sim_snap_t *s, uint32_t b)
{
    int x, y, led = s->led_btn[b];
    uint32_t held = s->buttons >> b & 1u, face = held ? RGBX(70, 75, 86) : RGBX(44, 47, 54);
    const char *hint = sc_name(BTN_SC[b]);
    btn_rect(b, &x, &y);
    if (led == 2)
        face = c_mix(face, COL_LED_ON, 60);
    if (latch_btns >> b & 1u)
        c_rrect(c, x - 3, y - 3, BTN_W + 6, BTN_H + 6, 9.0f, COL_LATCH);
    else if (held)
        c_rrect(c, x - 2, y - 2, BTN_W + 4, BTN_H + 4, 8.0f, RGBX(230, 230, 230));
    c_rrect(c, x, y, BTN_W, BTN_H, 7.0f, face);
    if (led == 2)
        c_ring(c, (float)x + 15.0f, (float)y + BTN_H / 2.0f, 0.0f, 9.0f, c_mix(face, COL_LED_ON, 90));
    c_ring(c, (float)x + 15.0f, (float)y + BTN_H / 2.0f, 0.0f, 5.5f, led == 2 ? COL_LED_ON : led ? COL_LED_DIM : COL_LED_OFF);
    c_text(c, x + 28, y + BTN_H / 2 - 8, host_button_name(b), COL_TEXT);
    c_text(c, x + BTN_W - 8 - c_text_w(hint), y + BTN_H / 2 - 8, hint, COL_DIM);
}

static void draw_keys(canvas_t *c, const sim_snap_t *s)
{
    uint32_t i;
    char b[8];
    c_rrect(c, KB_X0 - 12, KB_Y - 12, 16 * KB_WW + 24, KB_H + 24, 10.0f, RGBX(14, 14, 16));
    for (i = 0; i < 16u; i++) {
        uint32_t n = WHITE_N[i], down = s->notes >> n & 1u;
        int x = KB_X0 + (int)i * KB_WW, led = s->led_key[n];
        uint32_t face = down ? RGBX(176, 180, 190) : RGBX(233, 233, 230);
        if (led)
            face = c_mix(face, COL_LED_ON, led == 2 ? 200 : 80);
        c_rrect(c, x + 1, KB_Y, KB_WW - 2, KB_H, 5.0f, face);
        if (latch_notes >> n & 1u)
            c_rect(c, x + 6, KB_Y + KB_H - 6, KB_WW - 12, 3, COL_LATCH);
        key_label(n, b);
        c_text_c(c, x + KB_WW / 2, KB_Y + KB_H - 26, b, RGBX(90, 92, 98));
        c_text_c(c, x + KB_WW / 2, KB_Y + KB_H - 48, sc_name(KEY_SC[n]), RGBX(150, 150, 156));
    }
    for (i = 0; i < 11u; i++) {
        uint32_t n = BLACK_N[i], down = s->notes >> n & 1u;
        int x = black_x(n), led = s->led_key[n];
        uint32_t face = down ? RGBX(74, 78, 88) : RGBX(30, 31, 35);
        if (led)
            face = c_mix(face, COL_LED_ON, led == 2 ? 210 : 90);
        c_rrect(c, x, KB_Y - 2, KB_BW, KB_BH, 4.0f, face);
        if (latch_notes >> n & 1u)
            c_rect(c, x + 5, KB_Y + KB_BH - 9, KB_BW - 10, 3, COL_LATCH);
        c_text_c(c, x + KB_BW / 2, KB_Y + KB_BH - 30, sc_name(KEY_SC[n]), RGBX(170, 170, 176));
    }
}

static void legend(char l[5][192])
{
    uint32_t i;
    int k;
    k = snprintf(l[0], 192, "KEYS  white");
    for (i = 0; i < 16u; i++)
        k += snprintf(l[0] + k, 192 - (size_t)k, " %s", sc_name(KEY_SC[WHITE_N[i]]));
    k += snprintf(l[0] + k, 192 - (size_t)k, "   black");
    for (i = 0; i < 11u; i++)
        k += snprintf(l[0] + k, 192 - (size_t)k, " %s", sc_name(KEY_SC[BLACK_N[i]]));
    snprintf(l[0] + k, 192 - (size_t)k, "   (F3..G5)");
    k = snprintf(l[1], 192, "HOLD ");
    for (i = 0; i < HOST_NB; i++)
        k += snprintf(l[1] + k, 192 - (size_t)k, " %s %s ", sc_name(BTN_SC[i]), host_button_name(i));
    snprintf(l[2], 192, "TURN  %s/%s PRESETS  %s/%s ALGO  sh %s/%s SELECT  sh %s/%s K1  sh %s/%s K2  sh %s/%s K3  "
             "sh %s/%s K4   (K1..K4 = the 4 macros, later)",
             sc_name(ENC_SC[HOST_EN_PRESET].dn), sc_name(ENC_SC[HOST_EN_PRESET].up), sc_name(ENC_SC[HOST_EN_ALGO].dn),
             sc_name(ENC_SC[HOST_EN_ALGO].up), sc_name(ENC_SC[HOST_EN_SELECT].dn), sc_name(ENC_SC[HOST_EN_SELECT].up),
             sc_name(ENC_SC[HOST_EN_K1].dn), sc_name(ENC_SC[HOST_EN_K1].up), sc_name(ENC_SC[HOST_EN_K2].dn),
             sc_name(ENC_SC[HOST_EN_K2].up), sc_name(ENC_SC[HOST_EN_K3].dn), sc_name(ENC_SC[HOST_EN_K3].up),
             sc_name(ENC_SC[HOST_EN_K4].dn), sc_name(ENC_SC[HOST_EN_K4].up));
    snprintf(l[3], 192, "OPTION +  SPC play/stop   1..4 scene A..D (next bar)   5..8 mute track 1..4   < > filter   "
             "0 filter off   v ^ tempo");
    snprintf(l[4], 192, "MOUSE click / drag keys, hold buttons; wheel or drag a knob; right-click (ctrl-click) "
             "latches a button or key   ESC quit");
}

void panel_draw(canvas_t *c, const sim_snap_t *s, const char *status1, const char *status2)
{
    static char lg[5][192];
    double now = sim_now();
    uint32_t i;
    if (!lg[0][0])
        legend(lg);
    c_fill(c, COL_BG);
    c_rrect(c, LCD_X - 10, LCD_Y - 10, 500, 500, 12.0f, RGBX(10, 10, 12));
    c_lcd(c, LCD_X, LCD_Y, 2, s->lcd);
    c_text_l(c, PX0, 18, host_version(), COL_TEXT);
    c_text(c, PX0 + 2, 58, "flowstate-sim: the shared core in real time", COL_DIM);
    for (i = 0; i < SIM_NENC; i++)
        draw_encoder(c, s, i, now);
    for (i = 0; i < HOST_NB; i++)
        draw_button(c, s, i);
    draw_keys(c, s);
    for (i = 0; i < 5u; i++)
        c_text(c, KB_X0 - 8, LG_Y + 19 * (int)i, lg[i], i >= 3u ? COL_DIM : RGBX(170, 172, 180));
    c_rect(c, 0, ST_Y - 6, c->w, c->h - ST_Y + 6, RGBX(14, 15, 18));
    c_text(c, 10, ST_Y, status1, RGBX(232, 232, 236));
    c_text(c, 10, ST_Y + 20, status2, RGBX(150, 200, 160));
}
