/* SPDX-License-Identifier: GPL-3.0-only */
/* PLAY MODE (docs/design/play-mode-architecture.md 8, UI spec 2-10): the beginner's screens, controls and lights over
 * a Musical World, and the runtime modes (design 1.5). Main loop only. The keys stay in the audio ISR: Smart Keys,
 * and with FX held SLOOP's punch-in effects (play_layers_init keeps that one layer, H20).
 *
 *   modes     wrt.mode: WM_PLAY (these screens), WM_ADV (SLOOP's UI over the World: wrt.keys_on 0, the macros
 *             frozen where they are), WM_SLOOP (no World). SLOOP -> a World parks the working project first
 *             (world_store.c); LEAVE WORLD puts it back exactly. EDIT held 2 s with nothing else touched: PLAY ->
 *             the ADVANCED dialog (an EDIT tap enters), ADVANCED or SLOOP -> PLAY. The HOME-held menu: PLAY MODE,
 *             LEAVE WORLD (H24)
 *   screens   FIRST (until PLAY is pressed once), HOME, and the overlays that time out back to it: MACRO, CHOOSE
 *             WORLD, SCENES, VARIATION, PULSE, BEAT, LIVE FX / SOUND SHAPE / MOVEMENT, RECORDING, LOOP, SAVE,
 *             ADVANCED. The screen is a few bands; each is redrawn only when what it shows changed (pl_need), and
 *             a knob's detent sends only the bar and the digits it moved
 *   controls  design 8.2: PRESETS = WORLD (PLAY confirms: design D10), SELECT = SCENE (GLO held: the tempo inside
 *             the World's range), ALGORITHM = VARIATION, K1..K4 = COLOR MOTION SPACE ENERGY, 10 units a detent
 *             (FX / ENV / LFO held: their page's four), ARP = PULSE, SEQ = BEAT, REC, EDIT, SCL, GLO, SAVE, HOME,
 *             OCT- / OCT+
 *   lights    design 8.4: PLAY the beat, REC its state, EDIT dim while there is an undo, the keys pressed (on), the
 *             chord's tones and the tonic (dim)
 *   REC       play_rec.c records (UI spec 7): REC arms / takes / closes the take on its bar / toggles overdub, REC
 *             held 1.5 s clears the loop; EDIT tapped: UNDO, EDIT held + OCT+: REDO (+ OCT-: UNDO, SLOOP's pair).
 *             RECORDING (its bars as dots) while the take runs, LOOP n (layers) after it and while overdubbing
 * The fonts lack the spec's glyphs (a triangle, square, disc, heavy line, sparkle, electric arrow, chevron): they are
 * drawn with the canvas. */

enum { PS_FIRST, PS_HOME, PS_MACRO, PS_WORLDS, PS_SCENES, PS_VARS, PS_PULSE, PS_BEAT, PS_PAGE, PS_REC, PS_LOOP,
       PS_SAVE, PS_ADVDLG, PS_COUNT };
enum { PG_LIVEFX, PG_SHAPE, PG_MOVE };             /* PS_PAGE: controls 12..15, 4..7, 8..11 */
enum { PH_NONE, PH_EDIT, PH_CLEAR };               /* a hold's ring: EDIT for ADVANCED, REC to clear the loop */
#define PL_ADV_MS 2000u                            /* EDIT held untouched: the ADVANCED dialog / back to PLAY */
#define PL_RING_MS 500u                            /* .. the ring shows from here */
#define PL_CLEAR0_MS 700u                          /* REC held: the press undone, the ring */
#define PL_CLEAR_MS 1500u                          /* .. the loop cleared */
#define PL_TOAST_MS 1200u
#define PL_GLO2_MS 400u                            /* GLO tapped twice: the World's tempo */
#define PL_ACT_MS 66u                              /* the activity bars: 15 frames a second at most */
static const uint16_t PL_TIMEOUT[PS_COUNT] = {0, 0, 1500, 4000, 2500, 2500, 2500, 2500, 2500, 0, 2500, 6000, 5000};
#define PL_FIRST_WORLD 0x4eee4454u                 /* NEON RAIN (the factory index is by category) */

#define PL_VIO RGB(139, 124, 246)                  /* the spec's accent: cursor, bars, the sparkle */
#define PL_LAV RGB(170, 166, 232)                  /* secondary lines */
#define PL_GRY RGB(178, 182, 194)                  /* values */
#define PL_DIM RGB(52, 54, 70)                     /* tracks, empty dots */
#define PL_ROW RGB(30, 32, 44)                     /* the current row */
#define PL_RED RGB(255, 60, 70)                    /* recording */

static const char *const PL_CTL[WF_NCTL] = {"COLOR", "MOTION", "SPACE", "ENERGY", "SOFT", "SHORT", "BODY", "TAIL",
                                            "DRIFT", "WOBBLE", "PULSE", "RATE", "FILTER", "ECHO", "CRUSH", "FREEZE"};
static const char *const PL_END[4][2] = {{"DARK", "BRIGHT"}, {"STILL", "ALIVE"}, {"CLOSE", "HUGE"},
                                         {"SPARSE", "INTENSE"}};
static const char *const PL_PULSE[WF_NPULSE] = {"OFF", "SLOW", "PULSE", "DRIVE"};
static const char *const PL_BEAT[WF_NBEATS] = {"MINIMAL", "GROOVE", "BUSY", "BREAK"};
static const char *const PL_PAGE[3] = {"LIVE FX", "SOUND SHAPE", "MOVEMENT"};
static const uint8_t PL_PAGE_BTN[3] = {B_FX, B_ENV, B_LFO};
static const uint8_t PL_PAGE_CTL[3] = {12, 4, 8};

static struct {
    uint8_t scr;                 /* PS_*: the screen */
    uint8_t played;              /* PLAY was pressed once: HOME, no longer FIRST (the session keeps it) */
    uint8_t ctl;                 /* MACRO: the control (0..3); PS_PAGE: PG_* */
    uint8_t held;                /* PS_PAGE: its button is held (shown until it is let go) */
    uint8_t hot;                 /* the CONTROLS column turned last (white), 4 = none */
    uint8_t browse;              /* CHOOSE WORLD: the highlighted factory World */
    uint8_t row;                 /* SAVE: the highlighted row */
    uint8_t ask;                 /* SAVE: the row armed (RESET WORLD, DELETE USER WORLD: SAVE again confirms) */
    uint8_t rec_seen, rec_prev;  /* play_rec.c's PR_* as last seen; before the press that became a hold */
    uint8_t hold;                /* PH_* */
    uint8_t edit_eat;            /* EDIT's release is not a tap (it opened the dialog, or came from SLOOP's UI) */
    uint8_t force;               /* every band again */
    uint8_t fbody;               /* another screen: what a band shows is drawn whole (not just what moved) */
    uint8_t tforce;              /* the body's bands moved (another tiling): all of them again */
    uint8_t cpage, ring;         /* as drawn: the CONTROLS band showed a page; the middle band a hold's ring */
    uint32_t t;                  /* the overlay's last touch (its timeout), ms */
    uint32_t loop_t;             /* LOOP shown until */
    uint32_t hold_t0;
    uint32_t glo_t;              /* GLO's last tap */
    uint32_t toast_t;
    char toast[28];
    uint32_t bt0[NB];            /* each button's press time | 1, 0 = not pressed in PLAY */
    uint8_t bused[NB];           /* .. a key, knob or other button meanwhile: no tap, no 2 s EDIT */
    uint32_t act_t;              /* the activity bars' last sample */
    uint8_t act[7];              /* .. their heights */
    uint32_t sig[24];            /* each band's signature as drawn */
    uint8_t tiling;              /* the body's bands as drawn: 0 HOME, 1 list, 2 CHOOSE WORLD */
} pl = {.scr = PS_FIRST, .hot = 4};

/* ------------------------------------------------------------------- drawing --- */
/* A band: the canvas covers the screen rectangle (x, y, w, h), the pl_* drawing calls take screen coordinates.
 * pl_need: the band shows something else now (or everything is drawn again: pl.force) */
enum { PB_BRAND, PB_B1, PB_B2, PB_B3, PB_B4, PB_B5, PB_B6, PB_B7, PB_C0 };   /* PB_C0 + 3 k + 0..2: column k */
static int32_t pl_ox;
static uint32_t pl_cx, pl_cy;
static int pl_need(uint32_t id, uint32_t sig)
{
    if (!pl.force && !(pl.tforce && id >= PB_B1 && id <= PB_B7) && pl.sig[id] == sig)
        return 0;
    pl.sig[id] = sig;
    return 1;
}
static int pl_full(void) { return pl.force || pl.tforce || pl.fbody; }   /* (a body band whole, not what moved) */
static void pl_canvas(int32_t x, int32_t y, int32_t w, int32_t h)
{
    cv_begin((uint32_t)w, (uint32_t)h, C_BLACK);
    pl_ox = x;
    cv_oy = -y;
    pl_cx = (uint32_t)x;
    pl_cy = (uint32_t)y;
}
static void pl_band(int32_t y, int32_t h) { pl_canvas(0, y, 240, h); }   /* a whole-width band */
static void pl_end(void)
{
    cv_blit(pl_cx, pl_cy);
    cv_oy = 0;
    pl_ox = 0;
}
static void pl_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t c) { cv_rect(x - pl_ox, y, w, h, c); }
static void pl_pset(int32_t x, int32_t y, uint16_t c) { cv_pset(x - pl_ox, y, c); }
#ifndef PL_TEXT_HOOK
#define PL_TEXT_HOOK(s)                            /* (tests/ui_play_test.c: every text drawn) */
#endif
static int32_t pl_text(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    PL_TEXT_HOOK(s);
    return cv_text(x - pl_ox, y, f, s, c) + pl_ox;
}
static void pl_text_c(int32_t cx, int32_t y, const felucca_font_t *f, const char *s, uint16_t c)
{
    pl_text(cx - text_w(f, s) / 2, y, f, s, c);
}
static void pl_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r)
                pl_pset(cx + x, cy + y, c);
}
static void pl_seg(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint16_t c) { cv_line(x0 - pl_ox, y0, x1 - pl_ox, y1, c); }
/* the glyphs the fonts lack */
static void pl_play_icon(int32_t x, int32_t y, uint16_t c)  /* a triangle, 10 x 11 */
{
    int32_t i;
    for (i = 0; i < 10; i++)
        pl_rect(x + i, y + i / 2, 1, 11 - 2 * (i / 2), c);
}
static void pl_chevron(int32_t x, int32_t y, uint16_t c)    /* a chevron, 5 x 9 */
{
    int32_t i;
    for (i = 0; i < 5; i++) {
        pl_rect(x + i, y + i, 2, 1, c);
        pl_rect(x + i, y + 8 - i, 2, 1, c);
    }
}
static void pl_sparkle(int32_t cx, int32_t cy, int32_t r, uint16_t c)   /* four points, curved in (an astroid) */
{
    int32_t x, y;
    for (y = -r; y <= r; y++)
        for (x = -r; x <= r; x++) {
            int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, d = r - ax - ay;
            if (d >= 0 && 4 * ax * ay <= d * d)    /* sqrt|x| + sqrt|y| <= sqrt r */
                pl_pset(cx + x, cy + y, c);
        }
}
static void pl_bolt(int32_t x, int32_t y, uint16_t c)       /* the electric arrow: a zigzag with a head, 16 x 10 */
{
    int32_t k;
    for (k = 0; k < 2; k++) {
        pl_seg(x, y + 6 + k, x + 5, y + 1 + k, c);
        pl_seg(x + 5, y + 1 + k, x + 9, y + 8 + k, c);
        pl_seg(x + 9, y + 8 + k, x + 14, y + 3 + k, c);
    }
    pl_seg(x + 10, y + 3, x + 15, y + 2, c);
    pl_seg(x + 15, y + 2, x + 14, y + 7, c);
}
static void pl_wave(int32_t cy, uint16_t c)         /* FIRST's waveform line: ~ ~ ~~ ~~~ ~~ ~ */
{
    static const uint8_t G[6] = {1, 1, 2, 3, 2, 1};
    int32_t x = 120 - (10 * 12 + 5 * 9) / 2, g, i;
    for (g = 0; g < 6; g++) {
        for (i = 0; i < 12 * G[g]; i++) {
            int32_t y = cy - ((SINE[((uint32_t)i * 1024u / 12u) & 1023u] * 3) >> 15);
            pl_rect(x + i, y, 1, 2, c);
        }
        x += 12 * G[g] + 9;
    }
}
static void pl_ring(int32_t cx, int32_t cy, int32_t ratio, uint16_t c)   /* a ring filling clockwise, 0..1000 */
{
    int32_t a, rr, end = ratio * 1024 / 1000;
    for (a = 0; a < 1024; a += 3) {
        int32_t co = SINE[((uint32_t)a + 768u) & 1023u], si = SINE[(uint32_t)a & 1023u];
        for (rr = 16; rr <= 21; rr++)
            pl_pset(cx + ((si * rr) >> 15), cy + ((co * rr) >> 15), a <= end ? c : PL_DIM);
    }
}
static void pl_title_case(char *d, const char *s, uint32_t n)   /* "NEON RAIN" -> "Neon Rain" (the World list) */
{
    uint32_t i;
    int up = 1;
    for (i = 0; i + 1u < n && s[i]; i++) {
        char ch = s[i];
        d[i] = !up && ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
        up = ch == ' ';
    }
    d[i] = 0;
}

/* ------------------------------------------------------------------- state --- */
static const char *pl_err(int rc)                  /* "CRC", "BUSY": a World error's name (WE_NAMES) */
{
    static char b[12];
    const char *s = WE_NAMES;
    int k = 0;
    for (; *s && k < rc; s++)
        k += *s == ' ';
    for (k = 0; *s && *s != ' ' && k < 11; s++)
        b[k++] = *s;
    b[k] = 0;
    return b;
}
static void pl_toast(const char *s)
{
    str_cpy(pl.toast, s, sizeof pl.toast);
    pl.toast_t = fm1_ms | 1u;
}
static uint32_t pl_rest(void)                      /* the screen the overlays go back to */
{
    if (prec.st == PR_ARMED || prec.st == PR_TAKE)
        return PS_REC;
    if (prec.st == PR_OVERDUB)
        return PS_LOOP;
    return pl.played ? PS_HOME : PS_FIRST;
}
static void pl_screen(uint32_t s)
{
    if (pl.scr != s)
        pl.fbody = 1, pl.ask = 0;
    pl.scr = (uint8_t)s;
    pl.t = fm1_ms;
}
static uint32_t pl_keys_trk(void) { return wrt.keys_trk % NPART; }
/* MY WORLDS (Phase 14): the user World slots' ids (0: empty or damaged) and names, read once (world_store.c wus_scan)
 * and kept by SAVE and DELETE. CHOOSE WORLD's rows: the factory Worlds, then (with any) a MY WORLDS row, then the user
 * Worlds by slot */
enum { WU_OK, WU_STOP, WU_FULL, WU_BIG, WU_FAIL, WU_NONE };   /* world_store.c wuser_save / wuser_delete */
static struct {
    uint32_t id;
    char name[WF_NAME_LEN];
} wus[WF_USER_SLOTS];
static uint8_t wus_ok;
static uint32_t wus_gen;                           /* counts the list's changes (the simulator's Studio follows) */
static void wus_scan(void);
static int wuser_read(uint32_t k, const uint8_t **b, uint32_t *n);
static int wuser_save(int over);
static int wuser_delete(void);
static int wus_slot(uint32_t id)                   /* the user slot holding World id, -1: none */
{
    int k;
    if (!wus_ok)
        wus_scan();
    for (k = 0; k < WF_USER_SLOTS; k++)
        if (wus[k].id && wus[k].id == id)
            return k;
    return -1;
}
static uint32_t pl_nrows(void)
{
    uint32_t n = 0, k;
    if (!wus_ok)
        wus_scan();
    for (k = 0; k < WF_USER_SLOTS; k++)
        n += wus[k].id != 0u;
    return WORLD_NFACTORY + (n ? n + 1u : 0u);
}
static int pl_row_slot(uint32_t r)                 /* row r's user slot, -1: a factory World or the MY WORLDS row */
{
    uint32_t k, j = WORLD_NFACTORY;
    for (k = 0; k < WF_USER_SLOTS; k++)
        if (wus[k].id && ++j == r)
            return (int)k;
    return -1;
}
static uint32_t pl_world_index(void)               /* the row of the World playing, pl_nrows(): none */
{
    int i = wrt.loaded ? world_factory_find(wrt.id) : -1, k = wrt.loaded ? wus_slot(wrt.id) : -1;
    uint32_t r, n = pl_nrows();
    if (i >= 0)
        return (uint32_t)i;
    for (r = WORLD_NFACTORY + 1u; k >= 0 && r < n; r++)
        if (pl_row_slot(r) == k)
            return r;
    return n;
}
static uint32_t pl_pulse(void)                     /* the keys track's arp as a PULSE; WF_NPULSE: another one */
{
    const int16_t *p = trk[pl_keys_trk()].p;
    uint32_t i;
    if (!p[P_AMODE])
        return 0;
    for (i = 1; i < WF_NPULSE; i++)
        if (p[P_AMODE] == WB_PULSE[i][0] && p[P_ARATE] == WB_PULSE[i][1] && p[P_AOCT] == WB_PULSE[i][2] &&
            p[P_AGATE] == WB_PULSE[i][3])
            return i;
    return WF_NPULSE;
}
static void pl_pulse_set(uint32_t i)               /* design 9.2: the keys track's arp group (PULSE owns it) */
{
    int16_t *p = trk[pl_keys_trk()].p;
    uint32_t k;
    if (i >= WF_NPULSE)
        return;
    for (k = 0; k < 4u; k++)
        p[P_AMODE + k] = WB_PULSE[i][k];
    p[P_AHOLD] = 0;
}
static void pl_cancel_pending(void)                /* a scene / variation asked for: forgotten (the one playing stays) */
{
    fm1_irq_off();
    if (wst.st == WST_READY)
        wst.st = WST_FREE;
    fm1_irq_on();
}

/* ---- the runtime modes */
static void wpark_save(void);                      /* world_store.c: the SLOOP project parked, and its extras */
static void wpark_restore(void);
static void play_layers_init(void)                 /* H20: in PLAY only the FX layer routes keys in the ISR */
{
    uint32_t l;
    layers_init();
    for (l = LY_FX + 1u; l < LY_COUNT; l++)
        ly_bit[l] = 0;
}
static void pl_ui_reset(void)                      /* SLOOP's UI state that must not follow into another mode */
{
    uint32_t b;
    ui.menu = 0;
    wleave_ask = 0;
    ui.layer = LY_PLAY;
    ui.hold_kind = 0;
    ui.confirm = 0;
    song.seq_mode = 0;
    punch.hold = 0;
    for (b = WF_CTL_LIVE; b < WF_NCTL; b++)
        macro_set(b, MC_HOME[b]);                  /* (LIVE FX is momentary: home across a mode change; FREEZE goes) */
    for (b = 0; b < NB; b++) {
        pl.bt0[b] = 0;                             /* (buttons held across the change: no tap, no hold) */
        pl.bused[b] = 1;
    }
    pl.hold = PH_NONE;
}
static void pl_enter(void)                         /* the PLAY screens from now on */
{
    pl_ui_reset();
    wrt.mode = WM_PLAY;
    wrt.keys_on = 1;
    play_layers_init();
    pl.edit_eat = 1;
    pl.hot = 4;
    pl.force = 1;
    fm1_irq_off();
    prec_sync();                                   /* (ADVANCED may have changed the loop: LOOP or EMPTY for it) */
    fm1_irq_on();
    pl.rec_seen = prec.st;
    pl.scr = (uint8_t)pl_rest();
    pl.t = fm1_ms;
}
static void play_adv_enter(void)                   /* PLAY -> ADVANCED: SLOOP's UI over the World (design 8.3) */
{
    prec_adv();                                    /* (an overdub ends, a take closes on its bar) */
    pl_ui_reset();
    wrt.mode = WM_ADV;
    wrt.keys_on = 0;                               /* the keys: SLOOP's kb_map; the macros stay where they are */
    world_immediate(0);                            /* (scenes on their bar until SAVE + key 5 says at once) */
    layers_init();
    song.sel = wrt.keys_trk;
    go_home();
    lcd_fill(0, 0, 240, 240, C_BLACK);             /* (PLAY's bands cover the screen; SLOOP's pages expect it clear) */
    ui.force = 1;
}
static void play_adv_exit(void)                    /* ADVANCED -> PLAY: the edits kept, the tempo in the World's range */
{
    uint32_t lo, hi;
    int drop = world_capture();                    /* (design 8.3, 10.2: they become the working World's overrides) */
    world_tempo(&lo, &hi);
    song.g[G_BPM] = (int16_t)clamp(song.g[G_BPM], (int32_t)lo, (int32_t)hi);
    song.rec = 0;                                  /* (SLOOP's recording, an arm, a free take: over) */
    rec_wait = 0;
    ft_on = 0;
    pl_enter();
    if (drop)
        pl_toast("TOO MANY EDITS");
    else if (world_dirty())
        pl_toast("EDITS KEPT \xB7 SAVE TO KEEP");
}
static int play_unsaved(void)                      /* LEAVE WORLD asks first: edits or a loop the World does not hold */
{
    if (wrt.mode == WM_ADV)
        world_capture();
    return wrt.active && world_dirty();
}
/* a World from SLOOP or in a World session: SLOOP's project parked first; now while stopped, on the next bar while a
 * World plays, after a stop while SLOOP plays (world.c world_switch). 0, or WE_* */
static int play_world(const uint8_t *b, uint32_t n)
{
    int rc;
    if (wrt.mode != WM_SLOOP) {
        return world_switch(b, n);
    }
    song.rec = 0;
    rec_wait = 0;
    ft_on = 0;
    srec = 0;
    wpark_save();                                  /* (arrangement_enabled too: no song mode in a World, H26) */
    rc = world_switch(b, n);
    if (rc) {
        wpark_restore();
        return rc;
    }
    pl_enter();
    return 0;
}
static uint32_t wss_world(uint32_t *uslot);        /* world_store.c: the session's World (the last one played) */
static void wplay_capture(wplay_t *s);
/* World id (a user World: the one in slot uslot - 1, MY WORLDS): 0; not there: NEON RAIN, 1; no World at all: 2 */
static int pl_find(uint32_t id, uint32_t uslot, const uint8_t **b, uint32_t *n)
{
    int i = uslot ? -1 : world_factory_find(id);
    if (uslot && !wuser_read(uslot - 1u, b, n) && wb_u32(*b + 8) == id)
        return 0;
    if (i >= 0)
        return world_factory((uint32_t)i, b, n) ? 2 : 0;
    i = world_factory_find(PL_FIRST_WORLD);
    return world_factory(i < 0 ? 0u : (uint32_t)i, b, n) ? 2 : 1;
}
static int play_from_sloop(void)                   /* SLOOP -> PLAY: the session's World, else NEON RAIN */
{
    const uint8_t *b;
    uint32_t n, us, id = wss_world(&us);
    if (pl_find(id, us, &b, &n) > 1)
        return WE_STATE;
    return play_world(b, n);
}
static void play_leave(void)                       /* a World session -> SLOOP: the parked project back, exactly */
{
    wplay_t s;
    if (wrt.mode == WM_SLOOP)
        return;
    wplay_capture(&s);                             /* (the World and its state: PLAY MODE comes back to it) */
    if (song.playing || transport_req == 1u)
        transport_req = 2;
    fm1_irq_off();
    wreq.sw = 0;                                   /* (a World switch or a scene waiting for its bar: dropped) */
    if (wst.st == WST_READY)
        wst.st = WST_FREE;
    fm1_irq_on();
    world_unload();
    panic_req = (uint8_t)((1u << NTRK) - 1u);
    wpark_restore();
    pl_ui_reset();
    wrt.mode = WM_SLOOP;
    layers_init();
    go_home();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.force = 1;
}
static void play_menu(uint32_t item)               /* H24: PLAY MODE (0), LEAVE WORLD (1) */
{
    int rc;
    if (item) {
        play_leave();
        return;
    }
    if (wrt.mode == WM_ADV) {
        play_adv_exit();
        return;
    }
    if (wrt.mode == WM_SLOOP && (rc = play_from_sloop()) != 0)
        ui_say("WORLD ERROR ", pl_err(rc));
}

/* ---- what the controls ask for */
static void pl_scene(int32_t s)                    /* SELECT: a scene, at once while stopped, on the next bar while playing */
{
    uint32_t ps = WF_NONE, pv = WF_NONE, cur, var;
    int rc;
    if (!wrt.active)
        return;
    cur = world_pending(&ps, &pv) && ps != WF_NONE ? ps : wrt.scene;
    var = ps != WF_NONE ? pv : wrt.var;
    pl_screen(PS_SCENES);
    cur = (uint32_t)clamp((int32_t)cur + s, 0, WF_NSCENE - 1);
    if (cur == wrt.scene && var == wrt.var)
        pl_cancel_pending();                       /* back to the scene playing: nothing changes */
    else if ((rc = world_request(cur, var)) != 0 && rc != WE_BUSY)   /* (busy: a World switch waits for its bar) */
        ui_say("WORLD ERROR ", pl_err(rc));
}
static void pl_var(int32_t s)                      /* ALGORITHM: a variation, the same way */
{
    uint32_t ps = WF_NONE, pv = WF_NONE, n = world_nvar(), scene, cur;
    int rc;
    if (!wrt.active || !n)
        return;
    cur = world_pending(&ps, &pv) && ps != WF_NONE ? pv : wrt.var;
    scene = ps != WF_NONE ? ps : wrt.scene;
    pl_screen(PS_VARS);
    cur = (uint32_t)(((int32_t)cur + s % (int32_t)n + (int32_t)n) % (int32_t)n);
    if (scene == wrt.scene && cur == wrt.var)
        pl_cancel_pending();
    else if ((rc = world_request(scene, cur)) != 0 && rc != WE_BUSY)
        ui_say("WORLD ERROR ", pl_err(rc));
}
/* CHOOSE WORLD + PLAY (design D10): row i's World (a factory one, or MY WORLDS'); stopped: loaded and started,
 * playing: from the next bar. The World playing chosen again: the list closes (reset: loaded again, its defaults; a
 * user World as last saved) */
static void pl_world_go(uint32_t i, int reset)
{
    const uint8_t *b;
    uint32_t n, stopped = !song.playing && !transport_req;
    char m[24];
    int rc, k = pl_row_slot(i);
    pl_screen(pl_rest());
    if (i >= WORLD_NFACTORY && k < 0)
        return;
    if (!reset && wrt.active && (k < 0 ? WORLD_INDEX[i].id : wus[k].id) == wrt.id && !wreq.sw)
        return;                                    /* (a switch waiting for its bar: this one replaces it) */
    if ((rc = k < 0 ? world_factory(i, &b, &n) : wuser_read((uint32_t)k, &b, &n)) != 0 || (rc = play_world(b, n)) != 0) {
        ui_say("WORLD ERROR ", pl_err(rc));
        return;
    }
    pl.played = 1;
    pl_screen(PS_HOME);
    if (stopped) {
        if (!reset)
            transport_req = 1;                     /* (chosen while stopped: it starts) */
        return;
    }
    {
        char cat[WF_CAT_LEN];
        uint32_t bpm;
        str_cpy(m, "NEXT: ", sizeof m);            /* (the old World plays on until the bar) */
        if (k >= 0)
            str_cpy(m + 6, wus[k].name, sizeof m - 6u);
        else if (world_factory_info(i, m + 6, cat, &bpm))
            m[0] = 0;
    }
    pl_toast(m);
}
static void pl_list_step(int32_t s)                /* PULSE / BEAT: the next (or previous) one */
{
    int32_t d = s > 0 ? 1 : -1;
    pl.t = fm1_ms;
    if (pl.scr == PS_PULSE) {
        uint32_t c = pl_pulse();
        pl_pulse_set((uint32_t)(((int32_t)(c < WF_NPULSE ? c : 0u) + d + WF_NPULSE) % WF_NPULSE));
    } else {
        world_beat((uint32_t)(((int32_t)wrt.beat + d + WF_NBEATS) % WF_NBEATS));
    }
}
static void pl_tempo(int32_t s)                    /* GLO + SELECT: the tempo, inside the World's range */
{
    uint32_t lo, hi;
    world_tempo(&lo, &hi);
    song.g[G_BPM] = (int16_t)clamp(song.g[G_BPM] + s, (int32_t)lo, (int32_t)hi);
}

/* ---- REC (play_rec.c records; design 9.1, UI spec 7) */
static void pl_rec_press(void)
{
    prec_rec();
    pl.rec_seen = prec.st;
    if (prec.st == PR_LOOP) {                      /* (overdub off: LOOP a moment) */
        pl.loop_t = fm1_ms + 2500u;
        pl_screen(PS_LOOP);
    } else {
        pl_screen(pl_rest());
    }
}
static void pl_undo(int redo)                      /* EDIT: UNDO; EDIT + OCT+: REDO */
{
    if (!prec_undo(redo)) {
        pl_toast(redo ? "NOTHING TO REDO" : "NOTHING TO UNDO");
        return;
    }
    pl_toast(redo ? "REDONE" : "UNDONE");
    pl.rec_seen = prec.st;
    if (prec.st == PR_LOOP)
        pl.loop_t = fm1_ms + 2500u;
    pl_screen(prec.st == PR_LOOP ? PS_LOOP : pl_rest());
}
static void pl_rec_service(void)                   /* every pass: what the audio ISR did (a take closed on its bar) */
{
    uint32_t st = prec.st;
    if (st == pl.rec_seen)
        return;
    if (st == PR_LOOP && pl.rec_seen == PR_TAKE) {
        pl.loop_t = fm1_ms + 2500u;                /* the take loops: LOOP 1 a moment */
        pl_screen(PS_LOOP);
    } else if (pl.scr == PS_REC || pl.scr == PS_LOOP || pl.scr == PS_HOME || pl.scr == PS_FIRST) {
        pl_screen(pl_rest());
    }
    pl.rec_seen = (uint8_t)st;
}

/* ------------------------------------------------------------------- input --- */
#define PL_BT(b) (1u << panel.btn[b])
/* SAVE on the SAVE list's row (design 10.3, Phase 14): SAVE AS USER WORLD, SAVE (a factory World: as SAVE AS), RESET
 * WORLD (the World again: a factory one without the edits, a user World as last saved; on the bar while playing),
 * DELETE USER WORLD. RESET and DELETE ask first: the row armed, SAVE again confirms (a knob or another screen: not) */
static void pl_save_row(void)
{
    static const char *const MSG[] = {"SAVED: ", "STOP TO SAVE", "MY WORLDS FULL", "WORLD TOO BIG", "SAVE FAILED",
                                      "NOT A USER WORLD"};
    uint32_t r = pl.row;
    char m[28];
    int rc;
    if (r >= 2u && pl.ask != r) {
        pl.ask = (uint8_t)r;
        pl.t = fm1_ms;
        return;
    }
    pl_screen(pl_rest());
    if (r == 2u) {
        if ((r = pl_world_index()) < pl_nrows()) {
            pl_world_go(r, 1);
            pl_toast("WORLD RESET");
        }
        return;
    }
    rc = r == 3u ? wuser_delete() : wuser_save(r == 1u);
    str_cpy(m, rc == WU_OK && r == 3u ? "DELETED" : MSG[rc], sizeof m);
    if (rc == WU_OK && r < 3u)
        str_cpy(m + 7, world_name(), sizeof m - 7u);
    pl_toast(m);
}
static int pl_tap(uint32_t b, uint32_t now)       /* button b let go: a tap (short, nothing else touched) */
{
    return pl.bt0[b] && !pl.bused[b] && now - (pl.bt0[b] & ~1u) < TAP_MS;
}
static void play_input(void)
{
    uint32_t rel = 0, pr = fm1_input_edges(&rel), notes = fm1_input_note_edges(), now = fm1_ms, down = fm1_in.buttons;
    uint32_t home, b, k, moved = 0, page;
    int32_t es[NE];
    if (pr || notes)
        ui_input_ms = now;
    home = btn_hold(&ui.home_t0, B_HOME, fm1_ticks(), 1);
    if (home == BT_HOLD) {                         /* HOME held: the menu (SETTINGS, LEAVE WORLD), or out of it */
        if (ui.menu) {
            menu_close();
        } else {
            ui.menu = 1;
            ui.menu_sel = 0;
            ui.force = 1;
            pl.hold = PH_NONE;
        }
    }
    if (ui.menu) {
        punch.hold = 0;
        if (!ui.home_t0)
            menu_input(pr);
        if (wrt.mode == WM_PLAY)
            play_layers_init();                    /* (the calibration sets every layer again) */
        return;
    }
    for (k = 0; k < NE; k++)
        moved |= (es[k] = panel_enc(k)) != 0;
    for (b = 0; b < NB; b++) {                     /* a press; anything else while a button is held: it was used */
        uint32_t bit = PL_BT(b);
        if (pr & bit) {
            pl.bt0[b] = now | 1u;
            pl.bused[b] = (down & ~bit & ~PL_BT(B_HOME)) != 0u;
            if (b == B_EDIT)
                pl.edit_eat = 0;                   /* (a new hold) */
        } else if ((down & bit) && (notes || moved || (pr & ~bit) || (down & ~bit))) {
            pl.bused[b] = 1;
        }
    }
    if (pl.scr == PS_ADVDLG) {                     /* the dialog: an EDIT tap enters, any other button cancels */
        if (pr & ~PL_BT(B_EDIT)) {
            pl_screen(pl_rest());
            pr = 0;
        } else if ((rel & PL_BT(B_EDIT)) && !pl.edit_eat && pl_tap(B_EDIT, now)) {
            play_adv_enter();
            return;
        }
        for (k = 0; k < NE; k++)
            es[k] = 0;
    }
    /* EDIT: tapped, UNDO; held 2 s with nothing else, the ADVANCED dialog (a ring from 0.5 s) */
    if (rel & PL_BT(B_EDIT)) {
        if (!pl.edit_eat && pl_tap(B_EDIT, now))
            pl_undo(0);
        if (pl.hold == PH_EDIT)
            pl.hold = PH_NONE;
        pl.edit_eat = 0;
        pl.bt0[B_EDIT] = 0;
    } else if ((down & PL_BT(B_EDIT)) && pl.bt0[B_EDIT] && !pl.edit_eat) {
        uint32_t held = now - (pl.bt0[B_EDIT] & ~1u);
        if (pl.bused[B_EDIT]) {
            if (pl.hold == PH_EDIT)
                pl.hold = PH_NONE;
        } else if (held >= PL_ADV_MS) {
            pl.hold = PH_NONE;
            pl.edit_eat = 1;
            pl_screen(PS_ADVDLG);
        } else if (held >= PL_RING_MS && pl.hold == PH_NONE) {
            pl.hold = PH_EDIT;
            pl.hold_t0 = (pl.bt0[B_EDIT] & ~1u) + PL_RING_MS;
            if (pl.scr != PS_HOME && pl.scr != PS_FIRST && pl.scr != PS_REC && pl.scr != PS_LOOP)
                pl_screen(pl_rest());
        }
    }
    /* REC: acts on the press; held with a loop, the press is undone and a ring clears it (an undo brings it back).
     * Armed, a keys note or PLAY starts the take and the transport (play_rec.c) */
    if (pr & PL_BT(B_REC)) {
        pl.rec_prev = prec.st;
        pl_rec_press();
    } else if ((down & PL_BT(B_REC)) && pl.bt0[B_REC] && (pl.rec_prev == PR_LOOP || pl.rec_prev == PR_OVERDUB)) {
        uint32_t held = now - (pl.bt0[B_REC] & ~1u);
        if (held >= PL_CLEAR_MS && pl.hold == PH_CLEAR) {
            pl.hold = PH_NONE;
            prec_clear();
            pl.rec_seen = prec.st;
            pl.rec_prev = PR_EMPTY;
            pl.bt0[B_REC] = 0;
            pl_screen(pl_rest());
            pl_toast("LOOP CLEARED");
        } else if (held >= PL_CLEAR0_MS && pl.hold == PH_NONE) {
            if (prec.st != pl.rec_prev)
                prec_rec();                        /* (a hold, not a press: overdub as it was) */
            pl.rec_seen = prec.st;
            pl.hold = PH_CLEAR;
            pl.hold_t0 = (pl.bt0[B_REC] & ~1u) + PL_CLEAR0_MS;
            pl_screen(prec.st == PR_OVERDUB ? PS_LOOP : PS_HOME);
        }
    }
    if ((rel & PL_BT(B_REC)) && pl.hold == PH_CLEAR)
        pl.hold = PH_NONE;                         /* let go before the end: nothing */
    if ((pr & PL_BT(B_PLAY)) && prec.st == PR_ARMED && !song.playing) {
        pl.played = 1;                             /* (PLAY armed: the take starts with the transport) */
        pl_screen(PS_REC);
    }
    /* FX / ENV / LFO: their page while held (the knobs are its four); tapped, it stays a moment */
    for (page = 0; page < 3u; page++) {
        uint32_t bb = PL_PAGE_BTN[page], bit = PL_BT(bb);
        if (pr & bit) {
            pl.ctl = (uint8_t)page;
            pl.held = 1;
            pl.hot = 4;
            pl_screen(PS_PAGE);
        }
        if (rel & bit) {
            if (page == PG_LIVEFX) {               /* LIVE FX is momentary: its targets home (the clean way back: */
                for (k = WF_CTL_LIVE; k < WF_NCTL; k++)    /* the overlay ramps them, the punch engine fades) */
                    macro_set(k, MC_HOME[k]);
                if (punch.req >= 0 && !(fm1_in.notes & punch.keybit))
                    punch.req = -1;
            }
            if (pl.scr == PS_PAGE && pl.ctl == page) {
                pl.held = 0;
                if (!pl_tap(bb, now))
                    pl_screen(pl_rest());
                else
                    pl.t = now;
            }
        }
    }
    punch.hold = (uint8_t)((down & PL_BT(B_FX)) != 0u);
    /* the buttons pressed */
    if (pr & PL_BT(B_PLAY)) {
        if (pl.scr == PS_WORLDS) {
            pl_world_go(pl.browse, 0);
        } else {
            if (!pl.played) {
                pl.played = 1;
                pl_screen(PS_HOME);
            }
            transport_req = song.playing ? 2 : 1;
        }
    }
    if (pr & PL_BT(B_ARP)) {
        if (pl.scr == PS_PULSE)
            pl_list_step(1);
        else
            pl_screen(PS_PULSE);
    }
    if (pr & PL_BT(B_SEQ)) {
        if (pl.scr == PS_BEAT)
            pl_list_step(1);
        else
            pl_screen(PS_BEAT);
    }
    if (pr & PL_BT(B_SCL))
        pl_toast("KEYS: SMART MELODY");            /* (the only KEYS mode until the later ones exist) */
    if (pr & PL_BT(B_GLO)) {
        if (pl.glo_t && now - pl.glo_t < PL_GLO2_MS) {   /* tapped twice: the World's tempo */
            char m[12];
            song.g[G_BPM] = (int16_t)world_bpm();
            fmt_int(m, song.g[G_BPM]);
            str_cpy(m + str_len(m), " BPM", sizeof m - str_len(m));
            pl_toast(m);
            pl.glo_t = 0;
        } else {
            pl.glo_t = now;
        }
    }
    if (pr & PL_BT(B_SAVE)) {
        if (pl.scr != PS_SAVE) {
            pl.row = 0;
            pl_screen(PS_SAVE);
        } else {
            pl_save_row();
        }
    }
    if (pr & (PL_BT(B_OCTDN) | PL_BT(B_OCTUP))) {
        uint32_t both = PL_BT(B_OCTDN) | PL_BT(B_OCTUP);
        if (down & PL_BT(B_EDIT))
            pl_undo((pr & PL_BT(B_OCTUP)) != 0u);  /* EDIT held: + OCT+ REDO, + OCT- UNDO (SLOOP's pair) */
        else if ((down & both) == both)
            song.octave = 0;
        else if (pr & PL_BT(B_OCTDN))
            song.octave = (int8_t)(song.octave > -3 ? song.octave - 1 : -3);
        else
            song.octave = (int8_t)(song.octave < 3 ? song.octave + 1 : 3);
    }
    if (home == BT_TAP) {
        pl.hold = PH_NONE;
        pl_screen(pl_rest());                      /* HOME: back (a list closes, CHOOSE WORLD forgets) */
    }
    /* the knobs */
    if (es[EN_PRESET]) {
        if (pl.scr == PS_SAVE) {
            pl.row = (uint8_t)clamp((int32_t)pl.row + es[EN_PRESET], 0, 3);
            pl.ask = 0;
            pl.t = now;
        } else {
            uint32_t n = pl_nrows();
            int32_t b;
            if (pl.scr != PS_WORLDS) {
                uint32_t i = pl_world_index();
                pl.browse = (uint8_t)(i < n ? i : 0u);
            }
            pl_screen(PS_WORLDS);
            b = clamp((int32_t)pl.browse + es[EN_PRESET], 0, (int32_t)n - 1);
            if (b == WORLD_NFACTORY && n > WORLD_NFACTORY)   /* (the MY WORLDS row: over it) */
                b = clamp(b + (es[EN_PRESET] > 0 ? 1 : -1), 0, (int32_t)n - 1);
            pl.browse = (uint8_t)b;
        }
    }
    if (es[EN_SELECT]) {
        if (down & PL_BT(B_GLO))
            pl_tempo(es[EN_SELECT]);
        else
            pl_scene(es[EN_SELECT]);
    }
    if (es[EN_ALGO])
        pl_var(es[EN_ALGO]);
    for (k = 0; k < 4u; k++) {
        int32_t s = es[EN_K1 + k];
        uint32_t c;
        if (!s)
            continue;
        if (pl.scr == PS_PULSE || pl.scr == PS_BEAT) {
            pl_list_step(s);
            continue;
        }
        if (pl.scr == PS_SAVE)
            continue;
        if (pl.scr == PS_PAGE) {
            c = PL_PAGE_CTL[pl.ctl] + k;
            pl.t = now;
        } else {
            c = k;
            if (pl.ctl != k)
                pl.fbody = 1;                      /* (another control: its names and ends) */
            pl.ctl = (uint8_t)k;
            pl_screen(PS_MACRO);
        }
        pl.hot = (uint8_t)k;
        macro_set(c, (int32_t)macro_pos(c) + 10 * accel(EN_K1 + k, s, 100));
    }
}

/* SLOOP's UI (SLOOP, ADVANCED): EDIT held 2 s with nothing else touched -> PLAY (design 1.5, 8.3) */
static int pl_edit_watch(void)
{
    static uint32_t t0, turns;
    static uint8_t used;
    uint32_t eb = 1u << panel.btn[B_EDIT];
    if (!(fm1_in.buttons & eb)) {
        t0 = 0;
        return 0;
    }
    if (!t0) {
        t0 = fm1_ms | 1u;
        turns = panel_turns;
        used = (fm1_in.buttons & ~eb) != 0u;
    }
    if ((fm1_in.buttons & ~eb) || fm1_in.notes || panel_turns != turns || ui.menu || ui.hold_kind)
        used = 1;
    if (used || fm1_ms - (t0 & ~1u) < PL_ADV_MS)
        return 0;
    used = 1;                                      /* (once per hold) */
    if (wrt.mode == WM_ADV)
        play_adv_exit();
    else
        play_from_sloop();
    return wrt.mode == WM_PLAY;
}
static int play_input_hook(void)                   /* H19 (ui_input.c): 1 = PLAY took the panel */
{
    if (wrt.mode == WM_PLAY) {
        play_input();
        return 1;
    }
    return pl_edit_watch();
}

/* H17 (ui_layers.c): SAVE + key in a World session (ADVANCED): keys 1..4 its scenes (the edits captured first), key 5
 * toggles world_immediate (Phase 14: a scene or variation commits at the next block instead of its bar line), keys 6..8
 * (the section store) and the song refused (no song mode over a World's scenes in v1, H26) */
static int play_scene_key(uint32_t w)
{
    char b[2] = {(char)('A' + (w & 3u)), 0};
    if (w < 4u) {
        uint32_t playing = song.playing || transport_req;
        int rc;
        world_capture();                           /* (the edits so far: every scene keeps them, design 10.2) */
        rc = world_request(w, wrt.var);
        if (rc)
            ui_say("WORLD ERROR ", pl_err(rc));
        else
            ui_say(playing ? "NEXT: " : "SCENE ", b);
    } else if (w == 4u) {                          /* Phase 14: scenes and variations at once / on their bar */
        world_immediate(!wnow);
        ui_say("SCENES: ", wnow ? "AT ONCE" : "ON THEIR BAR");
    } else if (w < 8u) {
        ui_message("SCENES ARE THE WORLD'S");
    } else if (w == 12u || w == 13u || w == 15u) {
        ui_message("NO SONG IN A WORLD");
    }
    return 1;
}

/* ------------------------------------------------------------------- screens --- */
/* 240 x 240 (design 8.5): BRAND 0-19, the body 20-155, CONTROLS 156-239 (four 60 px columns). The body's bands, as
 * the screens tile it: HOME (and the overlays drawn like it) 20/36 56/20 76/56 132/24; the lists 20/36 then 20 px
 * rows from 56; CHOOSE WORLD 20/36, five 24 px rows from 56, its category, the hint (to 239, no CONTROLS) */
#define PL_HINT RGB(112, 114, 140)
static const uint8_t PL_TILING[PS_COUNT] = {0, 0, 0, 2, 1, 0, 1, 1, 0, 0, 0, 1, 0};
static char pl_wname[WORLD_NFACTORY ? WORLD_NFACTORY : 1][WF_NAME_LEN], pl_wcat[WORLD_NFACTORY ? WORLD_NFACTORY : 1][WF_CAT_LEN];
static uint8_t pl_wnames;                          /* the factory Worlds' names read (once: CHOOSE WORLD) */

static uint32_t pl_hash(uint32_t h, const char *s) { return studio_hash(h * 31u + 7u, s); }
static void pl_line(uint32_t id, int32_t y, int32_t h, int32_t ty, const felucca_font_t *f, const char *s, uint16_t c)
{   /* a band with one centred line */
    if (!pl_need(id, pl_hash(c * 3u + (f == &FONT_L), s) + (uint32_t)ty))
        return;
    pl_band(y, h);
    pl_text_c(120, ty, f, s, c);
    pl_end();
}
static void pl_append(char *b, uint32_t n, const char *s) { str_cpy(b + str_len(b), s, n - str_len(b)); }

static void pl_brand(void)
{
    uint32_t r = prec.st, st = pl.toast_t ? 9u : pl.scr == PS_FIRST ? 1u :
                                prec.arm || r == PR_ARMED ? 3u + ((fm1_ms / 250u) & 1u) :
                                r == PR_TAKE || r == PR_OVERDUB ? 2u : song.playing ? 5u : 6u;
    if (!pl_need(PB_BRAND, pl_hash(st, st == 9u ? pl.toast : "")))
        return;
    pl_band(0, 20);
    if (st == 9u) {                                /* a toast (and SLOOP's messages: the update countdown) */
        pl_rect(0, 0, 240, 19, RGB(46, 40, 96));
        pl_text_c(120, 2, &FONT_S, pl.toast, C_WHITE);
    } else {
        pl_text(6, 2, &FONT_S, "FLOWSTATE", PL_GRY);
        if (st == 1u)
            pl_text(234 - text_w(&FONT_S, "READY"), 2, &FONT_S, "READY", PL_GRY);
        else if (st == 2u || st == 3u)
            pl_disc(226, 10, 5, PL_RED);
        else if (st == 4u)
            pl_disc(226, 10, 5, PL_DIM);
        else if (st == 5u)
            pl_play_icon(222, 5, C_WHITE);
        else
            pl_rect(222, 6, 9, 9, PL_GRY);
    }
    pl_rect(0, 19, 240, 1, RGB(22, 24, 32));
    pl_end();
}

/* ---- the CONTROLS band: four columns, label, value (FONT_L), a bar; PS_PAGE: that page's four and K1..K4 */
static void pl_controls(void)
{
    uint32_t k, page = pl.scr == PS_PAGE, base = page ? PL_PAGE_CTL[pl.ctl] : 0u, full = pl.force || page != pl.cpage;
    pl.cpage = (uint8_t)page;
    for (k = 0; k < 4u; k++) {
        uint32_t c = base + k, pos = macro_pos(c), v = (pos + 5u) / 10u, id = PB_C0 + 3u * k;
        int32_t x = 60 * (int32_t)k, cx = x + 30, by = page ? 237 : 226, fill = (int32_t)(pos * 44u / 1000u);
        int hot = pl.hot == k && (pl.scr == PS_MACRO || pl.scr == PS_PAGE);
        char b[8];
        if (pl_need(id, pl_hash(page, PL_CTL[c]))) {
            pl_canvas(x, 156, 60, 22);
            pl_text_c(cx, 160, &FONT_S, PL_CTL[c], C_WHITE);
            pl_end();
        }
        if (page && pl.ctl == PG_LIVEFX && v < 10u) {   /* LIVE FX as the spec writes it: 00 */
            b[0] = '0';
            fmt_int(b + 1, (int32_t)v);
        } else {
            fmt_int(b, (int32_t)v);
        }
        if (pl_need(id + 1u, pl_hash(hot, b))) {
            if (full)
                pl_canvas(x, 178, 60, 40);
            else
                pl_canvas(x + 4, 184, 52, 32);    /* (a detent: the digits only) */
            pl_text_c(cx, 184, &FONT_L, b, hot ? C_WHITE : PL_GRY);
            pl_end();
        }
        if (pl_need(id + 2u, (uint32_t)fill * 3u + page)) {
            if (full)
                pl_canvas(x, 218, 60, 22);
            else
                pl_canvas(x + 8, by, 44, 3);      /* (a detent: the bar only) */
            if (page) {
                char kn[3] = {'K', (char)('1' + k), 0};
                pl_text_c(cx, 219, &FONT_S, kn, PL_HINT);
            }
            pl_rect(x + 8, by, 44, 3, PL_DIM);
            pl_rect(x + 8, by, fill, 3, PL_VIO);
            pl_end();
        }
    }
}

/* ---- HOME's lines */
static void pl_sub(char *b, uint32_t n)            /* "CINEMATIC · SCENE B" (· DREAMY: not the first variation) */
{
    char sc[8] = "SCENE A";
    sc[6] = (char)('A' + (wrt.scene & 3u));
    str_cpy(b, world_category(), n);
    pl_append(b, n, " \xB7 ");
    pl_append(b, n, sc);
    if (wrt.var) {
        pl_append(b, n, " \xB7 ");
        pl_append(b, n, world_var_name(wrt.var));
        if (text_w(&FONT_S, b) > 232) {            /* (too wide: the scene's letter alone) */
            str_cpy(b, world_category(), n);
            pl_append(b, n, " \xB7 ");
            pl_append(b, n, sc + 6);
            pl_append(b, n, " \xB7 ");
            pl_append(b, n, world_var_name(wrt.var));
        }
    }
}
static void pl_keys_band(uint32_t id)              /* "KEYS: SMART MELODY" (OCT +1 · 96 BPM when not the default) */
{
    char x[24] = "", b[12];
    const char *keys = pl.scr == PS_PAGE && pl.ctl == PG_LIVEFX && pl.held ? "KEYS: PUNCH FX" : "KEYS: SMART MELODY";
    if (song.octave) {
        str_cpy(x, song.octave > 0 ? "OCT +" : "OCT -", sizeof x);
        fmt_int(b, song.octave > 0 ? song.octave : -song.octave);
        pl_append(x, sizeof x, b);
    }
    if ((uint32_t)song.g[G_BPM] != world_bpm()) {
        if (x[0])
            pl_append(x, sizeof x, " \xB7 ");
        fmt_int(b, song.g[G_BPM]);
        pl_append(x, sizeof x, b);
        pl_append(x, sizeof x, " BPM");
    }
    if (!pl_need(id, pl_hash(pl_hash(1u, keys), x)))
        return;
    pl_band(132, 24);
    if (x[0]) {
        pl_text(6, 136, &FONT_S, keys + 6, C_WHITE);
        pl_text(234 - text_w(&FONT_S, x), 136, &FONT_S, x, PL_LAV);
    } else {
        pl_text_c(120, 136, &FONT_S, keys, C_WHITE);
    }
    pl_end();
}
static uint32_t pl_level(int32_t a)                /* |peak| (Q15) -> 0..40 px: 8 log2, -54 .. +6 dB */
{
    int32_t lg = 0, v;
    if (a < 64)
        return 0;
    while ((a >> lg) > 1)
        lg++;
    v = lg * 8 + (((a << 3) >> lg) & 7);
    return (uint32_t)clamp((v - 48) * 40 / 80, 0, 40);
}
static void pl_activity(void)                      /* the activity bars' heights from the tracks' peaks, <= 15 fps */
{
    static const uint8_t W[7] = {128, 179, 218, 255, 218, 179, 128};   /* a bell, /255 */
    uint32_t lv[NTRK], k, mix = 0, kt = pl_keys_trk(), a = kt == 0u ? 1u : 0u, b = kt == 2u ? 1u : 2u;
    uint32_t src[7];
    if (fm1_ms - pl.act_t < PL_ACT_MS)
        return;
    pl.act_t = fm1_ms;
    for (k = 0; k < NPART; k++) {
        lv[k] = pl_level(trk[k].peak);
        trk[k].peak = 0;
    }
    lv[TRK_DRUM] = pl_level(drums.peak);
    drums.peak = 0;
    for (k = 0; k < NTRK; k++)
        mix = lv[k] > mix ? lv[k] : mix;
    src[0] = src[6] = mix;                         /* the mix outside, the drums, the other parts, the keys in the middle */
    src[1] = src[5] = lv[TRK_DRUM];
    src[2] = lv[a];
    src[4] = lv[b];
    src[3] = lv[kt];
    for (k = 0; k < 7u; k++) {
        uint32_t h = 3u + src[k] * W[k] / 255u;
        pl.act[k] = (uint8_t)(h >= pl.act[k] ? h : pl.act[k] > h + 3u ? pl.act[k] - 3u : h);   /* up at once, down slowly */
    }
}
static void pl_stage(uint32_t id)                  /* HOME's middle: the activity bars, or a hold's ring */
{
    uint32_t sig, k;
    if (pl.hold) {
        uint32_t span = pl.hold == PH_EDIT ? PL_ADV_MS - PL_RING_MS : PL_CLEAR_MS - PL_CLEAR0_MS;
        uint32_t el = fm1_ms - pl.hold_t0, ratio = el >= span ? 1000u : el * 1000u / span;
        if (pl_need(id, 0x7F00u + pl.hold * 64u + ratio / 50u)) {
            if (pl_full() || !pl.ring)
                pl_band(76, 56);
            else
                pl_canvas(96, 80, 48, 48);         /* (the ring alone, in 5 % steps) */
            pl_ring(120, 103, (int32_t)(ratio / 50u * 50u), pl.hold == PH_EDIT ? PL_VIO : PL_RED);
            pl_end();
        }
        pl.ring = 1;
        pl_line(id + 1u, 132, 24, 136, &FONT_S, pl.hold == PH_EDIT ? "HOLD FOR ADVANCED" : "CLEAR LOOP", C_WHITE);
        return;
    }
    pl_activity();
    for (sig = 7u, k = 0; k < 7u; k++)
        sig = sig * 61u + pl.act[k];
    if (pl_need(id, sig) || pl.ring) {
        if (pl_full() || pl.ring)
            pl_band(76, 56);
        else
            pl_canvas(44, 80, 152, 46);
        pl.ring = 0;
        for (k = 0; k < 7u; k++)
            pl_rect(48 + 22 * (int32_t)k, 124 - pl.act[k], 12, pl.act[k], PL_VIO);
        pl_end();
    }
    pl_keys_band(id + 1u);
}

/* ---- the screens */
static void pl_home(void)
{
    char b[48];
    pl_line(PB_B1, 20, 36, 22, &FONT_L, wrt.loaded ? world_name() : "", C_WHITE);
    if (pl.scr == PS_FIRST) {
        pl_line(PB_B2, 56, 20, 58, &FONT_S, world_category(), PL_LAV);
        if (pl.hold) {
            pl_stage(PB_B3);
            return;
        }
        if (pl_need(PB_B3, 0x5151u)) {
            pl_band(76, 56);
            PL_TEXT_HOOK("~ ~ ~~ ~~~ ~~ ~");
            pl_wave(87, PL_VIO);                   /* the waveform line */
            pl_text_c(120, 98, &FONT_L, "PRESS PLAY", C_WHITE);
            pl_end();
        }
        pl_line(PB_B4, 132, 24, 136, &FONT_S, "", C_WHITE);
        return;
    }
    pl_sub(b, sizeof b);
    pl_line(PB_B2, 56, 20, 58, &FONT_S, b, PL_LAV);
    if (pl.scr == PS_PAGE) {
        if (pl.hold) {
            pl_stage(PB_B3);
            return;
        }
        pl_line(PB_B3, 76, 56, 88, &FONT_L, PL_PAGE[pl.ctl], C_WHITE);
        pl_keys_band(PB_B4);
        return;
    }
    pl_stage(PB_B3);
}
static void pl_macro(void)                         /* MACRO: SPACE / CLOSE ━━━━●━━ HUGE / 78 / NEON RAIN · SCENE B */
{
    uint32_t c = pl.ctl & 3u, pos = macro_pos(c);
    int32_t xl = 8 + text_w(&FONT_S, PL_END[c][0]) + 8, xr = 232 - text_w(&FONT_S, PL_END[c][1]) - 8;
    int32_t xk = xl + (xr - xl) * (int32_t)pos / 1000;
    char b[40], sc[8] = "SCENE A";
    pl_line(PB_B1, 20, 36, 22, &FONT_L, PL_CTL[c], C_WHITE);
    if (pl_need(PB_B2, c * 1009u + (uint32_t)xk)) {
        if (pl_full()) {
            pl_band(56, 20);
            pl_text(8, 58, &FONT_S, PL_END[c][0], PL_GRY);
            pl_text(232 - text_w(&FONT_S, PL_END[c][1]), 58, &FONT_S, PL_END[c][1], PL_GRY);
        } else {
            pl_canvas(xl - 5, 59, xr - xl + 11, 12);   /* (a detent: the bar only) */
        }
        pl_rect(xl, 64, xr - xl, 3, PL_DIM);           /* the heavy line, lit up to the knob */
        pl_rect(xl, 64, xk - xl, 3, PL_VIO);
        pl_disc(xk, 65, 4, C_WHITE);
        pl_end();
    }
    fmt_int(b, (int32_t)((pos + 5u) / 10u));
    if (pl_need(PB_B3, pl_hash(c, b))) {
        if (pl_full())
            pl_band(76, 56);
        else
            pl_canvas(72, 88, 96, 32);
        pl_text_c(120, 88, &FONT_L, b, C_WHITE);
        pl_end();
    }
    str_cpy(b, world_name(), sizeof b);
    sc[6] = (char)('A' + (wrt.scene & 3u));
    pl_append(b, sizeof b, " \xB7 ");
    pl_append(b, sizeof b, sc);
    pl_line(PB_B4, 132, 24, 136, &FONT_S, b, PL_LAV);
}
static void pl_row(uint32_t id, int32_t y, const char *mark, const char *name, uint16_t c, int cur, int chevron)
{   /* a list row, 20 px: a letter (SCENES), the name, the current row's background, the chevron (SCENES: the one
     * asked for, until its bar) */
    if (!pl_need(id, pl_hash(pl_hash(c + 3u * (uint32_t)cur + 7u * (uint32_t)chevron, mark), name)))
        return;
    pl_band(y, 20);
    if (cur)
        pl_rect(12, y + 1, 216, 18, PL_ROW);
    if (chevron)
        pl_chevron(mark[0] ? 210 : 82, y + 6, PL_VIO);
    if (mark[0]) {
        pl_text(24, y + 2, &FONT_S, mark, C_WHITE);
        pl_text(64, y + 2, &FONT_S, name, c);
        pl_rect(12, y + 19, 216, 1, RGB(22, 24, 32));
    } else {
        pl_text(96, y + 2, &FONT_S, name, c);
    }
    pl_end();
}
static void pl_scenes(void)                        /* SCENES: NEON RAIN / A INTRO .. D BREAKDOWN / CHANGES NEXT BAR */
{
    uint32_t ps = WF_NONE, pv = WF_NONE, i, blink = (fm1_ms / 250u) & 1u;
    int pend = world_pending(&ps, &pv) && ps != WF_NONE && ps != wrt.scene, ph;
    char m[20] = "CHANGES IN 0 BARS";             /* (when the change lands: its transition, Phase 11) */
    uint32_t n = world_bars_left(&ph);
    if (n <= 1u)
        str_cpy(m, "CHANGES NEXT BAR", sizeof m);
    else if (ph)
        str_cpy(m, "CHANGES NEXT PHRASE", sizeof m);
    else
        m[11] = (char)('0' + n % 10u);
    pl_line(PB_B1, 20, 36, 22, &FONT_L, world_name(), C_WHITE);
    for (i = 0; i < WF_NSCENE; i++) {
        char m[2] = {(char)('A' + i), 0};
        uint16_t c = pend && ps == i ? (blink ? PL_VIO : C_WHITE) : i == wrt.scene ? C_WHITE : PL_GRY;
        pl_row(PB_B2 + i, 56 + 20 * (int32_t)i, m, world_scene_name(i), c, i == wrt.scene, pend && ps == i);
    }
    pl_line(PB_B6, 136, 20, 138, &FONT_S, song.playing ? m : "", PL_GRY);
}
static void pl_list(void)                          /* PULSE, BEAT, SAVE: a title, rows with the chevron, a footer */
{
    static const char *const SAVE_ROW[4] = {"SAVE AS USER WORLD", "SAVE", "RESET WORLD", "DELETE USER WORLD"};
    const char *const *names = pl.scr == PS_PULSE ? PL_PULSE : pl.scr == PS_BEAT ? PL_BEAT : SAVE_ROW;
    uint32_t cur = pl.scr == PS_PULSE ? pl_pulse() : pl.scr == PS_BEAT ? wrt.beat : pl.row, i;
    uint32_t fac = pl.scr == PS_SAVE && wus_slot(wrt.id) < 0;   /* (a factory World: no DELETE) */
    pl_line(PB_B1, 20, 36, 22, &FONT_L, pl.scr == PS_PULSE ? "PULSE" : pl.scr == PS_BEAT ? "BEAT" : "SAVE", C_WHITE);
    for (i = 0; i < 4u; i++)
        pl_row(PB_B2 + i, 56 + 20 * (int32_t)i, "", names[i], i == cur ? PL_VIO : fac && i == 3u ? PL_HINT : C_WHITE, 0,
               i == cur);
    pl_line(PB_B6, 136, 20, 138, &FONT_S, pl.scr == PS_PULSE ? "HOLD KEYS AND LISTEN" : pl.scr == PS_BEAT ?
            "TURN TO CHANGE FEEL" : pl.ask ? "SAVE AGAIN TO CONFIRM" : "SAVE TO CONFIRM", PL_GRY);
}
static void pl_vars(void)                          /* VARIATION 03 / sparkle / DREAMY / SAME WORLD · NEW FEEL */
{
    uint32_t ps = WF_NONE, pv = WF_NONE, v, pend;
    char b[16] = "VARIATION 00";
    pend = world_pending(&ps, &pv) && ps != WF_NONE && pv != wrt.var;
    v = pend ? pv : wrt.var;
    b[10] = (char)('0' + (v + 1u) / 10u % 10u);
    b[11] = (char)('0' + (v + 1u) % 10u);
    pl_line(PB_B1, 20, 36, 22, &FONT_L, b, C_WHITE);
    if (pl_need(PB_B2, 0x5a5au)) {
        pl_band(56, 20);
        pl_sparkle(120, 65, 8, PL_VIO);
        pl_end();
    }
    pl_line(PB_B3, 76, 56, 88, &FONT_L, world_var_name(v), pend && ((fm1_ms / 250u) & 1u) ? PL_VIO : C_WHITE);
    pl_line(PB_B4, 132, 24, 136, &FONT_S, "SAME WORLD \xB7 NEW FEEL", PL_GRY);
}
static void pl_rec_screen(void)                    /* RECORDING... / the bar dots / PLAY SOMETHING / SMART KEYS ACTIVE */
{
    uint32_t done = prec_bars(), i;               /* (a take is 4 bars at most: a dot a bar, filled as they pass) */
    pl_line(PB_B1, 20, 36, 22, &FONT_L, "RECORDING...", C_WHITE);
    if (pl_need(PB_B2, 64u + done)) {
        pl_band(56, 20);
        for (i = 0; i < 4u; i++)
            pl_disc(72 + 32 * (int32_t)i, 66, 6, i < done ? PL_VIO : PL_DIM);
        pl_end();
    }
    pl_line(PB_B3, 76, 56, 96, &FONT_S, "PLAY SOMETHING", C_WHITE);
    pl_line(PB_B4, 132, 24, 136, &FONT_S, "SMART KEYS ACTIVE", PL_LAV);
}
static void pl_loop_screen(void)                   /* LOOP n / its bars / PLAYING + REC / TAP KEYS TO ADD MORE · UNDO READY */
{
    const track_t *t = &trk[pl_keys_trk()];
    uint32_t len = trk_len(t), cnt[8] = {0}, i, sig, now = song.playing ? t->seq_idx % len * 8u / len : 8u;
    char b[12] = "LOOP ";
    for (i = 0; i < len && i < NSTEP; i++)         /* the loop's notes in eighths of it; the eighth playing white */
        if (t->step[i].time == ST_NOTE)
            cnt[i * 8u / len] += t->step[i].n;
    for (sig = now, i = 0; i < 8u; i++)
        sig = sig * 7u + (cnt[i] > 4u ? 4u : cnt[i]);
    fmt_int(b + 5, prec.layers ? prec.layers : 1);
    pl_line(PB_B1, 20, 36, 22, &FONT_L, b, C_WHITE);
    if (pl_need(PB_B2, sig)) {
        pl_band(56, 20);
        for (i = 0; i < 8u; i++) {
            int32_t h = 3 + 4 * (int32_t)(cnt[i] > 4u ? 4u : cnt[i]);
            pl_rect(66 + 11 * (int32_t)i + (i >= 4u ? 12 : 0), 75 - h, 8, h, i == now ? C_WHITE : PL_VIO);
        }
        pl_end();
    }
    if (pl.hold) {
        pl_stage(PB_B3);
        return;
    }
    if (pl_need(PB_B3, prec.st == PR_OVERDUB ? 0x77u : 0x78u)) {
        pl_band(76, 56);
        pl_text_c(120, 82, &FONT_S, prec.st == PR_OVERDUB ? "PLAYING + REC" : "PLAYING", C_WHITE);
        pl_text_c(120, 106, &FONT_S, "TAP KEYS TO ADD MORE", PL_LAV);   /* (the spec's line, wrapped: 240 px) */
        pl_end();
    }
    pl_line(PB_B4, 132, 24, 136, &FONT_S, prec.rc ? "UNDO READY" : "", PL_LAV);
}
static void pl_advdlg(void)                        /* ADVANCED MODE / electric arrow / FULL SLOOP CONTROL / ENTER / CANCEL */
{
    pl_line(PB_B1, 20, 36, 22, &FONT_L, "ADVANCED MODE", C_WHITE);
    if (pl_need(PB_B2, 0xadu)) {
        pl_band(56, 20);
        pl_bolt(112, 60, PL_VIO);
        pl_end();
    }
    pl_line(PB_B3, 76, 56, 96, &FONT_S, "FULL SLOOP CONTROL", C_WHITE);
    pl_line(PB_B4, 132, 24, 136, &FONT_S, "ENTER / CANCEL", PL_LAV);
}
/* CHOOSE WORLD: one knob scrolls, the category below, PLAY loads. The factory Worlds (by category, as built), then the
 * user Worlds under a MY WORLDS row (Phase 14; the knob steps over that row) */
static void pl_worlds(void)
{
    uint32_t n = WORLD_NFACTORY, i, top, cur = pl_world_index(), bpm, rows = pl_nrows();
    if (!pl_wnames) {
        for (i = 0; i < n; i++)
            if (world_factory_info(i, pl_wname[i], pl_wcat[i], &bpm))
                pl_wname[i][0] = pl_wcat[i][0] = 0;
        pl_wnames = 1;
    }
    top = pl.browse < 5u ? 0u : pl.browse - 4u;
    pl_line(PB_B1, 20, 36, 22, &FONT_L, "CHOOSE WORLD", C_WHITE);
    for (i = 0; i < 5u; i++) {
        uint32_t e = top + i, hl = e == pl.browse, play = e == cur, my = e == n && rows > n;
        int k = pl_row_slot(e);
        char nm[WF_NAME_LEN];
        nm[0] = 0;
        if (e < n || k >= 0)
            pl_title_case(nm, k >= 0 ? wus[k].name : pl_wname[e], sizeof nm);
        else if (my)
            str_cpy(nm, "MY WORLDS", sizeof nm);
        if (!pl_need(PB_B2 + i, pl_hash(hl * 2u + play + 4u * my, nm)))
            continue;
        pl_band(56 + 24 * (int32_t)i, 24);
        if (hl && nm[0])
            pl_chevron(26, 63 + 24 * (int32_t)i, PL_VIO);
        pl_text(my ? 24 : 40, 60 + 24 * (int32_t)i, &FONT_S, nm, hl ? PL_VIO : my ? PL_LAV : C_WHITE);
        if (play && nm[0])
            pl_play_icon(214, 62 + 24 * (int32_t)i, hl ? PL_VIO : PL_LAV);   /* the World that plays on */
        pl_end();
    }
    pl_line(PB_B7, 176, 24, 180, &FONT_S, pl.browse < n ? pl_wcat[pl.browse] : rows > n ? "MY WORLDS" : "", PL_LAV);
    pl_line(PB_C0, 200, 40, 214, &FONT_S, "PRESS PLAY", PL_HINT);
}

/* the overlays' timeouts, FIRST -> HOME once the music plays, the toast's end; once a frame */
static void pl_timeouts(void)
{
    uint32_t now = fm1_ms, to, ps, pv;
    if (!pl.played && song.playing) {
        pl.played = 1;
        if (pl.scr == PS_FIRST)
            pl_screen(PS_HOME);
    }
    if (pl.toast_t && now - (pl.toast_t & ~1u) >= PL_TOAST_MS)
        pl.toast_t = 0;
    if (pl.hold == PH_EDIT && !(fm1_in.buttons & PL_BT(B_EDIT)))
        pl.hold = PH_NONE;
    if (pl.hold == PH_CLEAR && !(fm1_in.buttons & PL_BT(B_REC)))
        pl.hold = PH_NONE;
    to = PL_TIMEOUT[pl.scr];
    if ((pl.scr == PS_SCENES || pl.scr == PS_VARS) && world_pending(&ps, &pv))
        pl.t = now;                                /* (on screen until the change lands on its bar) */
    if (pl.scr == PS_PAGE && pl.held)
        to = 0;
    if (pl.scr == PS_LOOP) {
        if (prec.st == PR_OVERDUB)
            to = 0;
        else if (prec.st != PR_LOOP || (int32_t)(now - pl.loop_t) >= 0)
            to = 1;
    }
    if (pl.scr == PS_REC && prec.st != PR_ARMED && prec.st != PR_TAKE)
        to = 1;
    if (pl.scr == PS_FIRST || pl.scr == PS_HOME) {
        if (pl.scr != pl_rest())
            pl_screen(pl_rest());
        return;
    }
    if (to && now - pl.t >= to) {
        pl.hot = 4;
        pl_screen(pl_rest());
    }
}

static void play_draw(void)                        /* H19 (ui_draw.c): PLAY MODE's screen, band by band */
{
    if (ui.menu) {                                 /* (HOME held: SLOOP's menu, drawn as SLOOP draws it) */
        draw_menu();
        ui.force = 0;
        pl.force = 1;
        return;
    }
    if (ui.force) {
        ui.force = 0;
        pl.force = 1;
    }
    if (ui.msg_t) {                                /* SLOOP's messages (the update countdown, ...): a toast */
        pl_toast(ui.msg);
        ui.msg_t = 0;
    }
    pl_timeouts();
    if (PL_TILING[pl.scr] != pl.tiling) {          /* (the bands move: the body again; CHOOSE WORLD's covers CONTROLS) */
        if (PL_TILING[pl.scr] == 2u || pl.tiling == 2u)
            pl.force = 1;
        pl.tforce = 1;
        pl.tiling = PL_TILING[pl.scr];
    }
    pl_brand();
    switch (pl.scr) {
    case PS_MACRO:
        pl_macro();
        break;
    case PS_WORLDS:
        pl_worlds();
        break;
    case PS_SCENES:
        pl_scenes();
        break;
    case PS_VARS:
        pl_vars();
        break;
    case PS_PULSE:
    case PS_BEAT:
    case PS_SAVE:
        pl_list();
        break;
    case PS_REC:
        pl_rec_screen();
        break;
    case PS_LOOP:
        pl_loop_screen();
        break;
    case PS_ADVDLG:
        pl_advdlg();
        break;
    default:                                       /* FIRST, HOME, the pages */
        pl_home();
        break;
    }
    if (pl.scr != PS_WORLDS)
        pl_controls();
    pl.force = pl.fbody = pl.tforce = 0;
}

/* ------------------------------------------------------------------- lights --- */
static void play_leds(void)                        /* H19 (ui_input.c ui_leds): design 8.4 */
{
    uint8_t nl[FM1_NCOL] = {0}, dl[FM1_NCOL] = {0};
    uint32_t down = fm1_in.buttons, k, keys, dim, c, b125 = (fm1_ms / 125u) & 1u, b250 = (fm1_ms / 250u) & 1u;
    uint32_t pulse = pl_pulse(), shape = 0, move = 0, rec = prec.st;
    for (k = 4; k < 8u; k++) {
        shape |= macro_pos(k) != MC_HOME[k];
        move |= macro_pos(k + 4u) != MC_HOME[k + 4u];
    }
    led_put(nl, panel.btn[B_PLAY], play_led());
    led_put(nl, panel.btn[B_REC], pl.hold == PH_CLEAR ? (fm1_ms / 60u) & 1u : prec.arm || rec == PR_ARMED ? b125 :
                                  rec == PR_TAKE ? 1 : rec == PR_OVERDUB ? b250 : 0);
    led_put(dl, panel.btn[B_REC], rec == PR_LOOP);                     /* a loop exists */
    led_put(nl, panel.btn[B_EDIT], (down & PL_BT(B_EDIT)) != 0u);
    led_put(dl, panel.btn[B_EDIT], prec.rc && rec != PR_ARMED && rec != PR_TAKE);   /* an undo is ready */
    led_put(nl, panel.btn[B_ARP], pulse != 0u);                        /* not at their defaults */
    led_put(nl, panel.btn[B_SEQ], wrt.beat != WF_BEAT_GROOVE);
    led_put(nl, panel.btn[B_ENV], (down & PL_BT(B_ENV)) != 0u);
    led_put(dl, panel.btn[B_ENV], (int)shape);                         /* a control off its home */
    led_put(nl, panel.btn[B_LFO], (down & PL_BT(B_LFO)) != 0u);
    led_put(dl, panel.btn[B_LFO], (int)move);
    led_put(nl, panel.btn[B_FX], (down & PL_BT(B_FX)) != 0u);
    led_put(dl, panel.btn[B_SAVE], rec >= PR_LOOP);                    /* a loop not saved (in a user World) */
    led_put(nl, panel.btn[B_OCTDN], song.octave < 0);
    led_put(nl, panel.btn[B_OCTUP], song.octave > 0);
    keys = fm1_in.notes & 0x7FFFFFFu;              /* on: pressed; dim: the chord's tones and the tonic (sk_chord_keys) */
    dim = sk_chord_keys() & ~keys;
    for (k = 0; k < 27u; k++) {
        led_put(nl, 14u + k, (int)((keys >> k) & 1u));
        led_put(dl, 14u + k, (int)((dim >> k) & 1u));
    }
    for (c = 0; c < FM1_NCOL; c++) {
        fm1_led[c] = nl[c];
        fm1_led_dim[c] = dl[c] & (uint8_t)~nl[c];
    }
}

/* ------------------------------------------------------------------- service, boot --- */
static void play_service(void)                     /* main loop, every pass: a World switch staged and finished, the modes */
{
    int rc = world_service();
    if (rc > 0)
        ui_say("WORLD ERROR ", pl_err(rc));
    if (wrt.mode != WM_SLOOP && !wrt.active && !wreq.sw) {
        play_leave();                              /* (no World could start: SLOOP's project again) */
        return;
    }
    if (wrt.mode == WM_PLAY)
        pl_rec_service();
}

/* world_store.c wsession_boot (H22): the session's World, stopped, as it was left (restore 0: its defaults, the
 * FIRST screen). The SLOOP project autosave_resume loaded is parked first */
static void play_boot(const wplay_t *s, int restore)
{
    const uint8_t *b;
    uint32_t n, sc, var, k, lo, hi;
    int missing = pl_find(s->world, s->uslot, &b, &n);   /* (a user World gone or damaged: NEON RAIN) */
    if (missing > 1 || world_load(b, n))
        return;                                    /* (SLOOP, as it booted) */
    wpark_save();
    sc = wctx.b[wctx.off[WF_S_DEFAULTS]];
    var = wctx.b[wctx.off[WF_S_DEFAULTS] + 1u];
    restore = restore && !missing;
    if (restore) {
        if (s->beat < WF_NBEATS)
            wrt.beat = s->beat;
        sc = s->scene < WF_NSCENE ? s->scene : sc;
        var = s->var < world_nvar() ? s->var : var;
    }
    if (world_apply(sc, var)) {
        world_unload();
        wpark_restore();
        return;
    }
    if (restore) {
        for (k = 0; k < WF_CTL_LIVE; k++)          /* (LIVE FX, momentary, stay home) */
            macro_set(k, s->ctl[k] * 4 + (int32_t)(s->ctl_lo >> (2u * k) & 3u));
        mac.snap = mac.dirty = 1;                  /* (at its positions: no ramp) */
        pl_pulse_set(s->pulse);
        world_tempo(&lo, &hi);
        song.g[G_BPM] = (int16_t)clamp(s->bpm, (int32_t)lo, (int32_t)hi);
        song.octave = (int8_t)clamp(s->octave, -3, 3);
    }
    pl.played = restore && s->first;
    pl_enter();
    if (restore && s->mode == WM_ADV)
        play_adv_enter();
    if (missing && s->world)
        pl_toast("WORLD NOT FOUND");
}
