/* SPDX-License-Identifier: GPL-3.0-only */
/* The computer keyboard, what the user holds, and the virtual keyboard, for both views (main thread).
 *
 * Held: the computer keyboard, the mouse and right-click latches are three sources; the firmware thread is
 * told what changes in their union (OP_KEY / OP_BUTTON), and it holds a short tap for one main-loop pass.
 * Keys are mapped by scancode (by position, whatever the layout); their labels follow the layout.
 *
 *   keys      Z S X D C F V B H N J M , L . ; / ' Q W 3 E 4 R T 6 Y   F3..G5 (white: Z X C V B N M , . / Q W E R T Y)
 *   buttons   U I O P  FX SCL ENV LFO   7 8 9 0  EDIT GLO HOME SAVE   [ ]  ARP SEQ   Space PLAY   Return REC
 *             - =  OCT- OCT+  (held while the key is down)
 *   encoders  < >  PRESETS   v ^  ALGORITHM   shift < >  SELECT   shift v ^  MASTER   (key repeat keeps turning)
 *             shift Z X / C V / B N / M ,: in STUDIO the macros COLOR MOTION SPACE ENERGY, in ADVANCED KNOB 1..4
 *   Option    Space PLAY / STOP   1..4 scene A..D (next bar)   5..8 mute track 1..4   < > DJ filter -/+4
 *             0 filter off   v ^ tempo -/+1   W / shift W choose a World   Return load it   I the inspector
 *   Tab       STUDIO / ADVANCED    Esc  closes the inspector, cancels a World being chosen, else quits */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sim.h"

const char *const MACRO_NAME[4] = {"COLOR", "MOTION", "SPACE", "ENERGY"};
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
#define MACRO_STEP 2                             /* a key press moves a macro by 2 (of 100) */

/* ------------------------------------------------ names --- */
static char key_names[SDL_NUM_SCANCODES][8];
static const char *sc_name(SDL_Scancode sc) { return key_names[sc][0] ? key_names[sc] : "?"; }
const char *keys_name_note(int k) { return k >= 0 && k < SIM_KEYS ? sc_name(KEY_SC[k]) : "?"; }
const char *keys_name_button(int b) { return b >= 0 && b < HOST_NB ? sc_name(BTN_SC[b]) : "?"; }
void keys_name_encoder(uint32_t r, char *b, int n)
{
    snprintf(b, (size_t)n, "%s%s %s", ENC_SC[r].shift ? "sh " : "", sc_name(ENC_SC[r].dn), sc_name(ENC_SC[r].up));
}
void keys_init(int have_video)                   /* the labels of the keys on this keyboard's layout */
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

void keys_print_mapping(void)
{
    static const char *const ENC[SIM_NENC] = {"SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2", "KNOB 3",
                                              "KNOB 4", "MASTER"};
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
        printf("            %-10s %s%s / %s%s%s%s\n", ENC[i], ENC_SC[i].shift ? "shift+" : "", sc_name(ENC_SC[i].dn),
               ENC_SC[i].shift ? "shift+" : "", sc_name(ENC_SC[i].up), i >= HOST_EN_K1 && i <= HOST_EN_K4 ? "   STUDIO: " : "",
               i >= HOST_EN_K1 && i <= HOST_EN_K4 ? MACRO_NAME[i - HOST_EN_K1] : "");
    printf("  Option    Space PLAY / STOP    1..4 scene A..D (on the next bar)    5..8 mute track 1..4\n"
           "            < / > DJ filter -/+4    0 filter off    v / ^ tempo -/+1 BPM\n"
           "            W / shift+W choose a World (the current one plays on)    Return load it    I inspector\n"
           "  Tab       STUDIO / ADVANCED    Esc  close the inspector / cancel choosing / quit\n"
           "  mouse     click / drag the keys; hold a button; wheel or drag a knob or a slider (up = more);\n"
           "            right-click (or ctrl-click) latches a button or key\n");
    fflush(stdout);
}

void keys_legend(int studio, char l[5][192])
{
    uint32_t i;
    int k;
    char e[8][24];
    for (i = 0; i < SIM_NENC; i++)
        keys_name_encoder(i, e[i], sizeof e[i]);
    k = snprintf(l[0], 192, "KEYS  white");
    for (i = 0; i < 16u; i++)
        k += snprintf(l[0] + k, 192 - (size_t)k, " %s", sc_name(KEY_SC[WHITE_N[i]]));
    k += snprintf(l[0] + k, 192 - (size_t)k, "   black");
    for (i = 0; i < 11u; i++)
        k += snprintf(l[0] + k, 192 - (size_t)k, " %s", sc_name(KEY_SC[BLACK_N[i]]));
    snprintf(l[0] + k, 192 - (size_t)k, "   (F3..G5)");
    if (studio) {
        snprintf(l[1], 192, "MACROS  %s COLOR   %s MOTION   %s SPACE   %s ENERGY      TAB advanced   ESC quit",
                 e[HOST_EN_K1], e[HOST_EN_K2], e[HOST_EN_K3], e[HOST_EN_K4]);
        snprintf(l[2], 192, "OPTION +  SPC play/stop   1..4 scene   5..8 mute   W world   RET load   < > filter   "
                 "v ^ tempo   I inspect");
        l[3][0] = l[4][0] = 0;
        return;
    }
    k = snprintf(l[1], 192, "HOLD ");
    for (i = 0; i < HOST_NB; i++)
        k += snprintf(l[1] + k, 192 - (size_t)k, " %s %s ", sc_name(BTN_SC[i]), host_button_name(i));
    snprintf(l[2], 192, "TURN  %s PRESETS  %s ALGO  %s SELECT  %s K1  %s K2  %s K3  %s K4", e[HOST_EN_PRESET],
             e[HOST_EN_ALGO], e[HOST_EN_SELECT], e[HOST_EN_K1], e[HOST_EN_K2], e[HOST_EN_K3], e[HOST_EN_K4]);
    snprintf(l[3], 192, "OPTION +  SPC play/stop   1..4 scene A..D (next bar)   5..8 mute track 1..4   < > filter   "
             "0 filter off   v ^ tempo");
    snprintf(l[4], 192, "MOUSE click / drag keys, hold buttons; wheel or drag a knob; right-click (ctrl-click) "
             "latches   TAB studio   ESC quit");
}

/* ------------------------------------------------ what is held -> commands --- */
static uint32_t kb_notes, ms_notes, latch_notes, sent_notes;   /* key bits 0 (F3) .. 26 (G5) */
static uint32_t kb_btns, ms_btns, latch_btns, sent_btns;       /* label bits */
static int32_t enc_turns[SIM_NENC];
static double enc_flash[SIM_NENC];

void keys_send(uint8_t op, uint8_t a, int32_t v, uint8_t rel)
{
    sim_cmd_t c;
    memset(&c, 0, sizeof c);
    c.op = op;
    c.a = a;
    c.v = v;
    c.rel = rel;
    engine_send(&c);
}
static void sync_inputs(void)                    /* what is held, from every source, as changes */
{
    uint32_t n = kb_notes | ms_notes | latch_notes, b = kb_btns | ms_btns | latch_btns, d, i;
    for (d = n ^ sent_notes, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            keys_send(OP_KEY, (uint8_t)i, (int32_t)(n >> i & 1u), 0);
    for (d = b ^ sent_btns, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            keys_send(OP_BUTTON, (uint8_t)i, (int32_t)(b >> i & 1u), 0);
    sent_notes = n;
    sent_btns = b;
}
void keys_mouse_note(int k)
{
    ms_notes = k >= 0 && k < SIM_KEYS ? 1u << k : 0;
    sync_inputs();
}
void keys_mouse_button(int b)
{
    ms_btns = b >= 0 && b < HOST_NB ? 1u << b : 0;
    sync_inputs();
}
void keys_latch_note(int k)
{
    if (k >= 0 && k < SIM_KEYS)
        latch_notes ^= 1u << k;
    sync_inputs();
}
void keys_latch_button(int b)
{
    if (b >= 0 && b < HOST_NB)
        latch_btns ^= 1u << b;
    sync_inputs();
}
int keys_latched_note(int k) { return (int)(latch_notes >> k & 1u); }
int keys_latched_button(int b) { return (int)(latch_btns >> b & 1u); }
void keys_focus_lost(void)                       /* no stuck keys after Cmd-Tab: holds and latches go */
{
    kb_notes = kb_btns = ms_notes = ms_btns = latch_notes = latch_btns = 0;
    sync_inputs();
}
void keys_turn(uint32_t role, int32_t s)
{
    keys_send(OP_TURN, (uint8_t)role, s, 0);
    enc_turns[role] += s;
    enc_flash[role] = sim_now() + 0.16;
}
int32_t keys_turns(uint32_t role) { return enc_turns[role]; }
double keys_flash(uint32_t role) { return enc_flash[role]; }

/* Option + key: the commands; KA_INSPECT for the inspector, else KA_NONE */
static int command(SDL_Scancode sc, int shift, int repeat)
{
    if (sc == SDL_SCANCODE_LEFT || sc == SDL_SCANCODE_RIGHT)
        keys_send(OP_GLOBAL, HOST_G_FILTER, sc == SDL_SCANCODE_RIGHT ? 4 : -4, 1);
    else if (sc == SDL_SCANCODE_DOWN || sc == SDL_SCANCODE_UP)
        keys_send(OP_GLOBAL, HOST_G_BPM, sc == SDL_SCANCODE_UP ? 1 : -1, 1);
    else if (sc == SDL_SCANCODE_W)
        keys_send(OP_WORLD, WA_STEP, shift ? -1 : 1, 0);
    else if (repeat)
        return KA_NONE;
    else if (sc == SDL_SCANCODE_SPACE)
        keys_send(OP_PLAYSTOP, 0, 0, 0);
    else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_4)
        keys_send(OP_SCENE, (uint8_t)(sc - SDL_SCANCODE_1), 0, 0);
    else if (sc >= SDL_SCANCODE_5 && sc <= SDL_SCANCODE_8)
        keys_send(OP_MUTE, (uint8_t)(sc - SDL_SCANCODE_5), -1, 0);
    else if (sc == SDL_SCANCODE_0)
        keys_send(OP_GLOBAL, HOST_G_FILTER, 0, 0);
    else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER)
        keys_send(OP_WORLD, WA_CONFIRM, 0, 0);
    else if (sc == SDL_SCANCODE_I)
        return KA_INSPECT;
    return KA_NONE;
}

int keys_event(const SDL_KeyboardEvent *k, int down, int studio)
{
    SDL_Scancode sc = k->keysym.scancode;
    uint32_t shift = (k->keysym.mod & KMOD_SHIFT) != 0, i;
    if (down && sc == SDL_SCANCODE_ESCAPE)
        return KA_QUIT;                          /* (main.c: the inspector, a World being chosen first) */
    if (down && sc == SDL_SCANCODE_TAB && !k->repeat)
        return KA_VIEW;
    if (down && (k->keysym.mod & KMOD_ALT))      /* Option: the commands, nothing else */
        return command(sc, (int)shift, k->repeat);
    if (down)
        for (i = 0; i < SIM_NENC; i++)
            if (ENC_SC[i].shift == shift && (sc == ENC_SC[i].dn || sc == ENC_SC[i].up)) {
                int32_t s = sc == ENC_SC[i].up ? 1 : -1;
                if (studio && i >= HOST_EN_K1 && i <= HOST_EN_K4) {   /* STUDIO: the macros */
                    keys_send(OP_MACRO, (uint8_t)(i - HOST_EN_K1), s * MACRO_STEP, 1);
                    enc_flash[i] = sim_now() + 0.16;
                } else {
                    keys_turn(i, s);
                }
                return KA_NONE;
            }
    if (k->repeat)
        return KA_NONE;
    for (i = 0; i < SIM_KEYS; i++)
        if (sc == KEY_SC[i]) {
            kb_notes = down ? kb_notes | 1u << i : kb_notes & ~(1u << i);
            sync_inputs();
            return KA_NONE;
        }
    if (sc == SDL_SCANCODE_KP_ENTER)
        sc = SDL_SCANCODE_RETURN;
    for (i = 0; i < HOST_NB; i++)
        if (sc == BTN_SC[i]) {
            kb_btns = down ? kb_btns | 1u << i : kb_btns & ~(1u << i);
            sync_inputs();
            return KA_NONE;
        }
    return KA_NONE;
}

/* ------------------------------------------------ the virtual keyboard --- */
static int black_x(const kbd_t *g, uint32_t n)   /* left edge of black key n */
{
    uint32_t i;
    for (i = 0; i < 16u; i++)
        if (WHITE_N[i] + 1u == n)
            break;
    return g->x + (int)(i + 1u) * g->ww - g->bw / 2;
}
int kbd_hit(const kbd_t *g, int x, int y)
{
    uint32_t i;
    for (i = 0; i < 11u; i++) {
        int bx = black_x(g, BLACK_N[i]);
        if (x >= bx && x < bx + g->bw && y >= g->y && y < g->y + g->bh)
            return BLACK_N[i];
    }
    if (y >= g->y && y < g->y + g->h && x >= g->x && x < g->x + 16 * g->ww)
        return WHITE_N[(x - g->x) / g->ww];
    return -1;
}
void kbd_draw(canvas_t *c, const kbd_t *g, const sim_snap_t *s, uint32_t led_on, uint32_t led_dim)
{
    uint32_t i;
    char b[8];
    (void)led_dim;
    c_rrect(c, g->x - 12, g->y - 12, 16 * g->ww + 24, g->h + 24, 10.0f, RGBX(10, 10, 12));
    for (i = 0; i < 16u; i++) {
        uint32_t n = WHITE_N[i], down = s->notes >> n & 1u;
        int x = g->x + (int)i * g->ww, led = s->led_key[n];
        uint32_t face = down ? RGBX(176, 180, 190) : RGBX(233, 233, 230);
        if (led)
            face = c_mix(face, led_on, led == 2 ? 200 : 80);
        c_rrect(c, x + 1, g->y, g->ww - 2, g->h, 5.0f, face);
        if (latch_notes >> n & 1u)
            c_rect(c, x + 6, g->y + g->h - 6, g->ww - 12, 3, RGBX(80, 200, 255));
        key_label(n, b);
        c_text_c(c, x + g->ww / 2, g->y + g->h - 26, b, RGBX(90, 92, 98));
        c_text_c(c, x + g->ww / 2, g->y + g->h - 48, sc_name(KEY_SC[n]), RGBX(150, 150, 156));
    }
    for (i = 0; i < 11u; i++) {
        uint32_t n = BLACK_N[i], down = s->notes >> n & 1u;
        int x = black_x(g, n), led = s->led_key[n];
        uint32_t face = down ? RGBX(74, 78, 88) : RGBX(30, 31, 35);
        if (led)
            face = c_mix(face, led_on, led == 2 ? 210 : 90);
        c_rrect(c, x, g->y - 2, g->bw, g->bh, 4.0f, face);
        if (latch_notes >> n & 1u)
            c_rect(c, x + 5, g->y + g->bh - 9, g->bw - 10, 3, RGBX(80, 200, 255));
        c_text_c(c, x + g->bw / 2, g->y + g->bh - 30, sc_name(KEY_SC[n]), RGBX(170, 170, 176));
    }
}
static int kbd_held = -1;                        /* the key the mouse holds (glissando follows it) */
int kbd_mouse(const kbd_t *g, const void *ev)
{
    const SDL_Event *e = ev;
    int k;
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        if ((k = kbd_hit(g, e->button.x, e->button.y)) < 0)
            return 0;
        if (e->button.button == SDL_BUTTON_LEFT && !(SDL_GetModState() & KMOD_CTRL))
            keys_mouse_note(kbd_held = k);
        else
            keys_latch_note(k);                  /* right-click / ctrl-click: latch */
        return 1;
    case SDL_MOUSEMOTION:
        if (kbd_held < 0)
            return 0;
        if ((k = kbd_hit(g, e->motion.x, e->motion.y)) >= 0 && k != kbd_held)
            keys_mouse_note(kbd_held = k);       /* glissando */
        return 1;
    case SDL_MOUSEBUTTONUP:
        if (kbd_held < 0 || e->button.button != SDL_BUTTON_LEFT)
            return 0;
        kbd_held = -1;
        keys_mouse_note(-1);
        return 1;
    default:
        return 0;
    }
}
