/* SPDX-License-Identifier: GPL-3.0-only */
/* ADVANCED: the FM-1 itself. The LCD at x2, the 14 buttons with their LEDs, the 7 encoders and the MASTER
 * pot, the 27 keys, a legend and two status lines; everything drawn comes from a snapshot, everything done
 * goes to the firmware thread as commands (keys.c). Advanced Mode on the device is Phase 14; here it means
 * the raw panel: every SLOOP control, with nothing in between. */
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sim.h"

static const char *const ENC_LABEL[SIM_NENC] = {"SELECT", "ALGORITHM", "PRESETS", "KNOB 1", "KNOB 2",
                                                "KNOB 3", "KNOB 4", "MASTER"};

/* ------------------------------------------------ layout --- */
#define LCD_X 28
#define LCD_Y 28
#define PX0 540                                  /* the right column */
#define BTN_W 101
#define BTN_H 44
#define BTN_Y0 312
#define LG_Y 724
#define ST_Y 826
static const kbd_t KBD = {36, 548, 58, 160, 34, 100};

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
enum { HIT_NONE, HIT_BTN, HIT_ENC };
static int hit(int x, int y, uint32_t *idx)
{
    uint32_t i;
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
static int drag_role = -1, drag_y, drag_acc, mouse_x, mouse_y;
static float wheel_acc;
void panel_mouse(const void *ev)
{
    const SDL_Event *e = ev;
    uint32_t idx;
    int h;
    if (kbd_mouse(&KBD, ev))
        return;
    switch (e->type) {
    case SDL_MOUSEBUTTONDOWN:
        mouse_x = e->button.x, mouse_y = e->button.y;
        h = hit(e->button.x, e->button.y, &idx);
        if (e->button.button == SDL_BUTTON_LEFT && !(SDL_GetModState() & KMOD_CTRL)) {
            if (h == HIT_BTN)
                keys_mouse_button((int)idx);
            else if (h == HIT_ENC)
                drag_role = (int)idx, drag_y = e->button.y, drag_acc = 0;
        } else if (h == HIT_BTN) {
            keys_latch_button((int)idx);         /* right-click / ctrl-click: latch */
        }
        break;
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            drag_role = -1;
            keys_mouse_button(-1);
        }
        break;
    case SDL_MOUSEMOTION:
        mouse_x = e->motion.x, mouse_y = e->motion.y;
        if (drag_role >= 0) {
            drag_acc += drag_y - e->motion.y;
            drag_y = e->motion.y;
            for (; drag_acc >= 6; drag_acc -= 6)
                keys_turn((uint32_t)drag_role, 1);
            for (; drag_acc <= -6; drag_acc += 6)
                keys_turn((uint32_t)drag_role, -1);
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
            keys_turn(idx, 1);
        for (; wheel_acc <= -1.0f; wheel_acc += 1.0f)
            keys_turn(idx, -1);
        break;
    }
    default:
        break;
    }
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
    a = (float)keys_turns(r) * (float)(M_PI / 10.0) - (float)(M_PI / 2.0);   /* 20 detents a turn, drawn */
    if (r == SIM_MASTER)
        a = (float)(s->master_adc / 1023.0 * 1.5 * M_PI - 1.25 * M_PI);
    if (keys_flash(r) > now || drag_role == (int)r)
        c_ring(c, (float)cx, (float)cy, 0.0f, (float)rad + 5.0f, c_mix(COL_BG, ring, 70));
    c_ring(c, (float)cx, (float)cy, 0.0f, (float)rad, c_mix(COL_BG, ring, 120));
    c_ring(c, (float)cx, (float)cy, 0.0f, (float)rad - 4.0f, RGBX(44, 47, 54));
    c_line(c, (float)cx + cosf(a) * rad * 0.25f, (float)cy + sinf(a) * rad * 0.25f,
           (float)cx + cosf(a) * (rad - 8), (float)cy + sinf(a) * (rad - 8), 3.0f, RGBX(240, 240, 240));
    keys_name_encoder(r, b, sizeof b);
    if (r == SIM_MASTER) {                       /* the MASTER pot: label and level to its right */
        char m[40];
        c_text(c, cx + rad + 12, cy - 18, "MASTER", COL_TEXT);
        snprintf(m, sizeof m, "%d%%  %s", s->master_adc * 100 / 1023, b);
        c_text(c, cx + rad + 12, cy + 2, m, COL_DIM);
        return;
    }
    c_text_c(c, cx, cy + rad + 6, ENC_LABEL[r], COL_TEXT);
    c_text_c(c, cx, cy + rad + 24, b, COL_DIM);
}

static void draw_button(canvas_t *c, const sim_snap_t *s, uint32_t b)
{
    int x, y, led = s->led_btn[b];
    uint32_t held = s->buttons >> b & 1u, face = held ? RGBX(70, 75, 86) : RGBX(44, 47, 54);
    const char *hint = keys_name_button((int)b);
    btn_rect(b, &x, &y);
    if (led == 2)
        face = c_mix(face, COL_LED_ON, 60);
    if (keys_latched_button((int)b))
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

void panel_draw(canvas_t *c, const sim_snap_t *s, const char *status1, const char *status2)
{
    static char lg[5][192];
    double now = sim_now();
    uint32_t i;
    if (!lg[0][0])
        keys_legend(0, lg);
    c_fill(c, COL_BG);
    c_rrect(c, LCD_X - 10, LCD_Y - 10, 500, 500, 12.0f, RGBX(10, 10, 12));
    c_lcd(c, LCD_X, LCD_Y, 2, s->lcd);
    c_text_l(c, PX0, 18, host_version(), COL_TEXT);
    c_text(c, PX0 + 2, 58, "ADVANCED: the FM-1 itself, every control", COL_DIM);
    for (i = 0; i < SIM_NENC; i++)
        draw_encoder(c, s, i, now);
    for (i = 0; i < HOST_NB; i++)
        draw_button(c, s, i);
    kbd_draw(c, &KBD, s, COL_LED_ON, COL_LED_DIM);
    for (i = 0; i < 5u; i++)
        c_text(c, KBD.x - 8, LG_Y + 19 * (int)i, lg[i], i >= 3u ? COL_DIM : RGBX(170, 172, 180));
    c_rect(c, 0, ST_Y - 6, c->w, c->h - ST_Y + 6, RGBX(14, 15, 18));
    c_text(c, 10, ST_Y, status1, RGBX(232, 232, 236));
    c_text(c, 10, ST_Y + 20, status2, RGBX(150, 200, 160));
}
