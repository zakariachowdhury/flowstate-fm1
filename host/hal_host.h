/* SPDX-License-Identifier: GPL-3.0-only */
/* The FM-1 for the host build (host/core.c): every function and object the firmware takes from hal/ and
 * from its hardware-only files (main.c, lcd.c, felucca.c's flash glue), as emulations. Consolidates the
 * doubles of tests/hostsim.c, tests/ui_pages_test.c, tests/storage_test.c and the build/sim preview.
 *
 * One thread, lockstep (docs/architecture-audit.md §14.2): the audio interrupt is host_audio, the main
 * loop host_ui_frame, both called by the program, never at once. Time is the audio rendered so far:
 * host_t frames give fm1_ticks (TIMER4, 24 MHz) and fm1_ms. The interrupt guards are no-ops.
 * USB and the update path need nothing here: core.c builds usb.c without starting it and leaves the
 * update, editor and console out (FELUCCA_OTA = FELUCCA_CDC = 0).
 *
 * Included once, by host/core.c, before the firmware: the system headers first, then the attribute
 * removal (Mach-O rejects the .pool / .noinit / .ram_text section names, as in the tests). */
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>                    /* (core.c: a World compiled by tools/worldc.py) */
#include "felucca_tables.h"            /* FS, CTL */
#define __attribute__(x)

/* ------------------------------------------------ the real HAL headers, register code renamed away --- */
/* hal/fm1_input.h: the matrix (FM1_KEYMAP, FM1_NCOL), fm1_in, fm1_led[], fm1_led_dim[], fm1_led_key();
 * hal/fm1_adc.h: the channel numbers; hal/fm1_flash.h: the store windows (FL_STORE_OK) and FL_FAR.
 * What reads registers or masks interrupts gets a fm1_hw_ name and is never called (so never emitted);
 * the host versions below take the real names. */
#define fm1_time_init fm1_hw_time_init
#define fm1_ticks fm1_hw_ticks
#define fm1_micros fm1_hw_micros
#define fm1_delay_us fm1_hw_delay_us
#define fm1_delay_ms fm1_hw_delay_ms
#define fm1_input_init fm1_hw_input_init
#define fm1_enc_take fm1_hw_enc_take
#define fm1_input_edges fm1_hw_input_edges
#define fm1_input_note_edges fm1_hw_input_note_edges
#define fm1_adc_init fm1_hw_adc_init
#define fm1_adc_read fm1_hw_adc_read
#define irq_save fm1_hw_irq_save
#define irq_restore fm1_hw_irq_restore
#define fl_jedec_ram fm1_hw_fl_jedec_ram
#define fl_plain_window_init fm1_hw_fl_plain_window_init
#include "fm1_input.h"
#include "fm1_adc.h"
#include "fm1_flash.h"
#undef fm1_time_init
#undef fm1_ticks
#undef fm1_micros
#undef fm1_delay_us
#undef fm1_delay_ms
#undef fm1_input_init
#undef fm1_enc_take
#undef fm1_input_edges
#undef fm1_input_note_edges
#undef fm1_adc_init
#undef fm1_adc_read
#undef irq_save
#undef irq_restore
#undef fl_jedec_ram
#undef fl_plain_window_init

/* ------------------------------------------------ interrupts (fm1_irq.h, fm1_flash.h) --- */
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t f) { (void)f; }

/* ------------------------------------------------ time (fm1_time.h), watchdog (fm1_sys.h) --- */
static uint64_t host_t;                          /* stereo frames rendered since boot */
static uint32_t fm1_ticks(void) { return (uint32_t)(host_t * 24000000u / FS); }   /* wraps as TIMER4 does */
static int host_in_loop;                         /* inside host_ui_frame: a busy wait there renders (core.c) */
static void host_yield(void);
static void host_wait_ms(uint32_t ms);
static void fm1_wdt_feed(void)                   /* the main loop's busy waits (panel_setup) feed it: time moves */
{
    if (host_in_loop)
        host_yield();
}
static void fm1_delay_ms(uint32_t ms) { host_wait_ms(ms); }

/* ------------------------------------------------ input (fm1_input.h) --- */
/* fm1_in as fm1__key fills it: the debounced state and the edges since they were last taken */
static void host_in_set(uint32_t notes, uint32_t buttons)
{
    fm1_in.notes_pressed |= notes & ~fm1_in.notes;
    fm1_in.notes = notes;
    fm1_in.pressed |= buttons & ~fm1_in.buttons;
    fm1_in.released |= fm1_in.buttons & ~buttons;
    fm1_in.buttons = buttons;
}
static int32_t fm1_enc_take(uint32_t e)
{
    int32_t s = fm1_in.enc_steps[e % FM1_NENC];
    fm1_in.enc_steps[e % FM1_NENC] = 0;
    return s;
}
static uint32_t fm1_input_edges(uint32_t *released)
{
    uint32_t p = fm1_in.pressed;
    if (released)
        *released = fm1_in.released;
    fm1_in.pressed = fm1_in.released = 0;
    return p;
}
static uint32_t fm1_input_note_edges(void)
{
    uint32_t p = fm1_in.notes_pressed;
    fm1_in.notes_pressed = 0;
    return p;
}

/* ------------------------------------------------ ADC (fm1_adc.h) --- */
static int32_t host_adc_master = 1023;           /* the MASTER pot, 0..1023 (fully up); -1: no reading */
static int32_t host_adc_batt = 620;              /* battery divider: full */
static int32_t fm1_adc_read(uint32_t ch)
{
    return ch == FM1_ADC_MASTER ? host_adc_master : ch == FM1_ADC_BATT ? host_adc_batt : -1;
}

/* ------------------------------------------------ LCD (lcd.c) --- */
/* the panel as a 240 x 240 RGB565 framebuffer (native byte order; lcd_blit gets the canvas byte-swapped,
 * as the SPI wants it). lcd.c clips fills to the screen and sends blits as they are: here both clip,
 * and host_lcd_clipped counts what reached past the edge (the UI test asserts there is none) */
static uint16_t host_fb[240 * 240];
static uint32_t host_lcd_clipped;
static uint64_t host_lcd_px;                     /* pixels sent to the panel (fills and blits): the SPI's work */
static void lcd_init(void) { memset(host_fb, 0, sizeof host_fb); }
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    if (!w || !h || x >= 240u || y >= 240u)
        return;
    if (x + w > 240u || y + h > 240u)
        host_lcd_clipped++;
    w = x + w > 240u ? 240u - x : w;
    h = y + h > 240u ? 240u - y : h;
    host_lcd_px += (uint64_t)w * h;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            host_fb[(y + j) * 240u + x + i] = c;
}
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    if (!w || !h)
        return;
    if (x + w > 240u || y + h > 240u)
        host_lcd_clipped++;
    host_lcd_px += (uint64_t)w * h;
    for (j = 0; j < h && y + j < 240u; j++)
        for (i = 0; i < w && x + i < 240u; i++) {
            uint16_t p = px[j * w + i];
            host_fb[(y + j) * 240u + x + i] = (uint16_t)(p >> 8 | p << 8);
        }
}

/* ------------------------------------------------ audio (fm1_audio.h) --- */
/* audio.c is built as it is; its interrupt handler is never called (host_audio renders through its
 * audio_block), so the DMA reads nothing back */
#define FM1_AUDIO_HALF 0x80u
static void fm1_audio_init(int32_t *buf, uint32_t half_words, void (*isr)(void), uint32_t prio)
{
    (void)buf, (void)half_words, (void)isr, (void)prio;
}
static uint8_t fm1_audio_pending(void) { return 0; }
static void fm1_audio_ack_aux(uint8_t p) { (void)p; }
static uint32_t fm1_audio_free_half(void) { return 0; }
static void fm1_audio_ack_half(void) {}
static void fm1_audio_stop(void) {}

/* ------------------------------------------------ flash: 1 MiB SPI NOR (felucca.c's glue, fm1_flash.h) --- */
/* NOR semantics as tests/storage_test.c: an erase sets a 4 KiB sector to 0xFF, a program can only clear
 * bits and goes one 256-byte page at a time (fl_write splits there). The firmware's own windows apply
 * (FL_STORE_OK: st_erase / st_prog refuse the rest with -8, as felucca.c). Read through XIP as well: the
 * user sample slots (eng_sample.c SMP_USER_XIP) point into the same image. The image is optionally
 * loaded from and written back to a file (core.c host_boot, host_flash_write). */
#define HOST_NOR_SIZE 0x100000u
static uint8_t host_nor[HOST_NOR_SIZE];
static uint32_t host_nor_erases, host_nor_progs;
static uint32_t host_nor_lo = 0xFFFFFFFFu, host_nor_hi; /* the lowest and highest byte erased or programmed (tests) */
static uint8_t flash_ok;                         /* the expected part answered (project.c persist_boot) */
static uint32_t fl_jedec_ram(void) { return 0x856014u; }
static void fl_plain_window_init(void) {}        /* the image is plain: no encrypted window to map */
static int st_read(uint32_t off, void *dst, uint32_t n)
{
    if (off > HOST_NOR_SIZE || n > HOST_NOR_SIZE - off)
        return -1;
    memcpy(dst, host_nor + off, n);
    return 0;
}
static int st_erase(uint32_t off)
{
    if (!FL_STORE_OK(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    memset(host_nor + off, 0xFF, 0x1000u);
    host_nor_erases++;
    host_nor_lo = off < host_nor_lo ? off : host_nor_lo;
    host_nor_hi = off + 0xFFFu > host_nor_hi ? off + 0xFFFu : host_nor_hi;
    return 0;
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    const uint8_t *s = src;
    if (!FL_STORE_OK(off, n))
        return -8;
    if (n) {
        host_nor_lo = off < host_nor_lo ? off : host_nor_lo;
        host_nor_hi = off + n - 1u > host_nor_hi ? off + n - 1u : host_nor_hi;
    }
    while (n) {                                  /* fl_write: page by page, a program only clears bits */
        uint32_t k = 256u - (off & 0xFFu), i;
        k = k > n ? n : k;
        for (i = 0; i < k; i++)
            host_nor[off + i] &= s[i];
        host_nor_progs++;
        off += k;
        s += k;
        n -= k;
    }
    return 0;
}
/* XIP: user sample slot k in the image. eng_sample.c:89 stores a slot's data as a uint32_t offset from
 * SMP_DATA (a pointer difference truncated to 32 bits, exact on the 32-bit device): on a 64-bit host it
 * is exact only when the image lies above SMP_DATA and within 4 GiB of it. host_nor is a static array:
 * Mach-O places __DATA (and its bss) after the __TEXT const data that holds SMP_DATA, and ASLR slides
 * the image as a whole, so that holds. core.c host_boot checks it and aborts if it ever does not. */
#define SMP_USER_XIP(k) ((const uint8_t *)host_nor + SMP_USER_BASE + (k) * SMP_USER_SIZE)
