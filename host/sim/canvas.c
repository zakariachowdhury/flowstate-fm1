/* SPDX-License-Identifier: GPL-3.0-only */
/* Software drawing into an ARGB8888 canvas (the window, or a BMP without one): anti-aliased rounded
 * rectangles (only their corners cost a square root), rings, arcs and lines, the LCD scaled up, and text in
 * the firmware's own font (host_text). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "sim.h"

uint32_t c_mix(uint32_t a, uint32_t b, uint32_t t)   /* t 0..255: a -> b */
{
    uint32_t r = ((a >> 16 & 255u) * (255u - t) + (b >> 16 & 255u) * t) / 255u;
    uint32_t g = ((a >> 8 & 255u) * (255u - t) + (b >> 8 & 255u) * t) / 255u;
    uint32_t c = ((a & 255u) * (255u - t) + (b & 255u) * t) / 255u;
    return RGBX(r, g, c);
}
uint32_t c_565(uint16_t c) { return RGBX((c >> 11) * 255u / 31u, (c >> 5 & 63u) * 255u / 63u, (c & 31u) * 255u / 31u); }

static void blend(canvas_t *c, int x, int y, uint32_t col, uint32_t t)
{
    if ((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h && t)
        c->px[y * c->w + x] = t >= 255u ? col : c_mix(c->px[y * c->w + x], col, t);
}
static uint32_t cover(float v) { return v <= 0.0f ? 0u : v >= 1.0f ? 255u : (uint32_t)(v * 255.0f); }

void c_fill(canvas_t *c, uint32_t col)
{
    int i;
    for (i = 0; i < c->w * c->h; i++)
        c->px[i] = col;
}
void c_copy(canvas_t *d, const canvas_t *s) { memcpy(d->px, s->px, (size_t)s->w * (size_t)s->h * 4u); }
void c_rect(canvas_t *c, int x, int y, int w, int h, uint32_t col)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++)
            blend(c, i, j, col, 255u);
}
void c_rrect(canvas_t *c, int x, int y, int w, int h, float r, uint32_t col)
{
    int i, j, ri = (int)ceilf(r);
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) {
            float px = (float)i + 0.5f, py = (float)j + 0.5f;
            if ((j >= y + ri && j < y + h - ri) || (i >= x + ri && i < x + w - ri)) {
                blend(c, i, j, col, 255u);       /* (away from the corners: inside) */
                continue;
            }
            float cx = px < x + r ? x + r : px > x + w - r ? x + w - r : px;
            float cy = py < y + r ? y + r : py > y + h - r ? y + h - r : py;
            float d = sqrtf((px - cx) * (px - cx) + (py - cy) * (py - cy));
            blend(c, i, j, col, cover(r - d + 0.5f));
        }
}
void c_frame(canvas_t *c, int x, int y, int w, int h, float r, float t, uint32_t col, uint32_t fill)
{
    c_rrect(c, x, y, w, h, r, col);
    c_rrect(c, x + (int)t, y + (int)t, w - 2 * (int)t, h - 2 * (int)t, r - t > 1.0f ? r - t : 1.0f, fill);
}
/* an arc of radius r and width t from angle a0 to a1 (radians, clockwise from 3 o'clock), round ends */
void c_arc(canvas_t *c, float cx, float cy, float r, float t, float a0, float a1, uint32_t col)
{
    int i, j;
    float span = a1 - a0, h = t * 0.5f;
    if (span <= 0.0f)
        return;
    for (j = (int)(cy - r - t); j <= (int)(cy + r + t); j++)
        for (i = (int)(cx - r - t); i <= (int)(cx + r + t); i++) {
            float dx = (float)i + 0.5f - cx, dy = (float)j + 0.5f - cy, d = sqrtf(dx * dx + dy * dy), a;
            if (fabsf(d - r) > h + 1.0f)
                continue;
            a = atan2f(dy, dx) - a0;
            while (a < 0.0f)
                a += (float)(2.0 * M_PI);
            while (a >= (float)(2.0 * M_PI))
                a -= (float)(2.0 * M_PI);
            if (a <= span)
                blend(c, i, j, col, cover(h - fabsf(d - r) + 0.5f));
        }
    c_ring(c, cx + r * cosf(a0), cy + r * sinf(a0), 0.0f, h, col);
    c_ring(c, cx + r * cosf(a1), cy + r * sinf(a1), 0.0f, h, col);
}
void c_ring(canvas_t *c, float cx, float cy, float r0, float r1, uint32_t col)   /* r0 = 0: a disc */
{
    int i, j;
    for (j = (int)(cy - r1 - 1); j <= (int)(cy + r1 + 1); j++)
        for (i = (int)(cx - r1 - 1); i <= (int)(cx + r1 + 1); i++) {
            float dx = (float)i + 0.5f - cx, dy = (float)j + 0.5f - cy, d = sqrtf(dx * dx + dy * dy);
            float a = r1 - d + 0.5f, b = r0 > 0.0f ? d - r0 + 0.5f : 1.0f;
            blend(c, i, j, col, cover(a < b ? a : b));
        }
}
void c_line(canvas_t *c, float x0, float y0, float x1, float y1, float w, uint32_t col)   /* a capsule */
{
    int i, j;
    float minx = (x0 < x1 ? x0 : x1) - w, maxx = (x0 > x1 ? x0 : x1) + w;
    float miny = (y0 < y1 ? y0 : y1) - w, maxy = (y0 > y1 ? y0 : y1) + w;
    float vx = x1 - x0, vy = y1 - y0, ll = vx * vx + vy * vy;
    for (j = (int)miny; j <= (int)maxy; j++)
        for (i = (int)minx; i <= (int)maxx; i++) {
            float px = (float)i + 0.5f - x0, py = (float)j + 0.5f - y0;
            float t = ll > 0.0f ? (px * vx + py * vy) / ll : 0.0f, dx, dy;
            t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
            dx = px - t * vx, dy = py - t * vy;
            blend(c, i, j, col, cover(w * 0.5f - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}
int c_text(canvas_t *c, int x, int y, const char *s, uint32_t col)
{
    return host_text(c->px, c->w, c->w, c->h, x, y, 0, s, col & 0xFFFFFFu);
}
int c_text_l(canvas_t *c, int x, int y, const char *s, uint32_t col)
{
    return host_text(c->px, c->w, c->w, c->h, x, y, 1, s, col & 0xFFFFFFu);
}
int c_text_w(const char *s) { return host_text_width(0, s); }
int c_text_lw(const char *s) { return host_text_width(1, s); }
void c_text_c(canvas_t *c, int cx, int y, const char *s, uint32_t col) { c_text(c, cx - c_text_w(s) / 2, y, s, col); }
void c_text_r(canvas_t *c, int rx, int y, const char *s, uint32_t col) { c_text(c, rx - c_text_w(s), y, s, col); }
int c_text_sp(canvas_t *c, int x, int y, const char *s, int extra, uint32_t col)
{
    char one[2] = {0, 0};
    for (; *s; s++) {
        one[0] = *s;
        x = c_text(c, x, y, one, col) + extra;
    }
    return x - extra;
}

void c_lcd(canvas_t *c, int x0, int y0, int scale, const uint16_t *lcd)
{
    int x, y, i, j;
    for (y = 0; y < 240; y++)
        for (x = 0; x < 240; x++) {
            uint32_t col = c_565(lcd[y * 240 + x]);
            for (j = 0; j < scale; j++)
                for (i = 0; i < scale; i++)
                    if ((unsigned)(x0 + scale * x + i) < (unsigned)c->w && (unsigned)(y0 + scale * y + j) < (unsigned)c->h)
                        c->px[(y0 + scale * y + j) * c->w + x0 + scale * x + i] = col;
        }
}

int c_save_bmp(const canvas_t *c, const char *path)   /* 24-bit, bottom-up */
{
    FILE *f = fopen(path, "wb");
    uint32_t row = ((uint32_t)c->w * 3u + 3u) & ~3u, size = 54u + row * (uint32_t)c->h, i;
    uint8_t h[54] = {'B', 'M'}, pad[3] = {0, 0, 0};
    int x, y;
    if (!f)
        return -1;
    for (i = 0; i < 4; i++) {
        h[2 + i] = (uint8_t)(size >> 8 * i);
        h[18 + i] = (uint8_t)((uint32_t)c->w >> 8 * i);
        h[22 + i] = (uint8_t)((uint32_t)c->h >> 8 * i);
        h[34 + i] = (uint8_t)((size - 54u) >> 8 * i);
    }
    h[10] = 54;
    h[14] = 40;
    h[26] = 1;
    h[28] = 24;
    fwrite(h, 1, sizeof h, f);
    for (y = c->h - 1; y >= 0; y--) {
        for (x = 0; x < c->w; x++) {
            uint32_t p = c->px[y * c->w + x];
            uint8_t bgr[3] = {(uint8_t)p, (uint8_t)(p >> 8), (uint8_t)(p >> 16)};
            fwrite(bgr, 1, 3, f);
        }
        fwrite(pad, 1, row - (uint32_t)c->w * 3u, f);
    }
    return fclose(f) ? -1 : 0;
}
