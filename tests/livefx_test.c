/* SPDX-License-Identifier: GPL-3.0-only */
/* LIVE FX, SOUND SHAPE and MOVEMENT on the host (Phase 13: macro.c's built-in mappings of controls 4..15 and FREEZE,
 * guard.c guard_live, guard_limits.h's Smart Keys windows; design 9.3, 9.4; UI spec 8, 10): the real firmware through
 * tests/world_render.c's harness (FELUCCA_WORLD 1, wb_check, world_load, world_apply, PLAY, mix_block with the macro
 * overlay), each render and each scenario in its own fork. The Worlds on the command line: the factory ones, then
 * worlds/test/full.world.json (its `controls` replace SOFT and FILTER):
 *   targets   every World at its default scene and variation, each control 4..15 at 0, 0.5 and 1 (the rest at the
 *             defaults), a fresh table each: every effective value inside its descriptor and its slot's range (a base
 *             already past one stays); at home exactly the default table; at 1 something moves (FREEZE: nothing, it
 *             is the punch engine's); ECHO's feedback at most GL_ECHO_DFDBK, and 72 with a big room (SPACE up);
 *             CRUSH: DUST up, the levels down (its gain compensation), DUST at most 64 on a distorted part; FILTER
 *             0 low-pass, 0.5 off, 1 high-pass; the Smart Keys track's envelope and LFO inside guard_limits.h's
 *             windows, MOTION and MOVEMENT at 1 together too; a World's `controls` replacing the built-in ones. The
 *             first World's effective values are printed (the table of docs/guardrails.md)
 *   renders   per factory World with a player on the Smart Keys track (world_render --keys): ECHO and SPACE at 1
 *             through STOP: the tail under -60 dBFS within ECHO_TAIL s (no runaway); CRUSH at 1 within CRUSH_LU of
 *             the dry loudness (BS.1770); FILTER at 0 and at 1; SOUND SHAPE all at 1, SHORT alone, MOVEMENT all at 1
 *             (RATE at 1 and at 0), and everything at 1 with MOTION: clean (peak under -0.5 dBFS, nothing at full
 *             scale, no DC, the tail under -60 dBFS within 6 s)
 *   filter    a FILTER sweep while playing (0 -> 1 -> 0 -> 1, the main loop between blocks): no peak over -0.5 dBFS, no
 *             sample jump of a wrapped integer, the DJ filter's state bounded
 *   freeze    NEON RAIN playing, FX held and let go, FREEZE turned and white keys emulated at random 1000 times: the
 *             punch request always FREEZE's loop for its position (1, 1/2, 1/4 beat) when nothing else holds it,
 *             a held key's effect never taken over, and dry (no request, the engine faded out) within 4 blocks of FX
 *             let go or the knob under 25 %: never stuck. After the last release the output equals a render of the
 *             same bars without any of it, bit for bit
 *   punch     FREEZE and the white keys (emulated as seq.c key_down / key_up write punch): a key wins, FREEZE comes
 *             back after it, nothing in ADVANCED or SLOOP, withdrawn by a mode change or a World unloaded
 *   release   every factory World playing with the four LIVE FX at 1 (FILTER at 0 and at 1 alternately), FX let go
 *             as ui_play.c does: the effective values ramp back (at most 2 steps a block: no snap), all home within
 *             RELEASE_MS, and no sample jump in the 0.5 s after beyond CLICK_X times the larger of the dry render's
 *             largest and the effects' own
 *   --cost    (an -O2 build, tests/run_tests.sh) host instructions a sample over 2 bars, the World at its defaults and
 *             with everything of Phase 13 at 1 (FREEZE engaged): at most GL_CPU_BUDGET; guard.c's estimate beside it
 *   build/host/livefx_test [--cost] WORLD.wblob ... */
#define WORLD_RENDER_LIB 1
#include "world_render.c"
#include <stdarg.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

#define ECHO_TAIL 10.0                           /* s after STOP for LIVE ECHO at 1 with SPACE at 1 (a slow tempo's echo) */
#define CRUSH_LU 3.0
#define RELEASE_MS 400.0
#define CLICK_X 1.25
#define SLEW_MAX 29491                           /* (tests/guard_sweep.c: a wrapped integer, not music) */

static int fails;
static void check(int ok, const char *fmt, ...)
{
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    printf("livefx: %-100s %s\n", b, ok ? "ok" : "FAIL");
    fails += !ok;
}

static const uint8_t *wb[8];
static uint32_t wn[8], nw, nfactory;
static uint16_t defpos[WF_NCTL];
static void load(uint32_t w)
{
    uint32_t c;
    blob = wb[w];
    blob_n = wn[w];
    if (world_load(wb[w], wn[w])) {
        printf("livefx: world_load failed\n");
        exit(2);
    }
    for (c = 0; c < WF_NCTL; c++)
        defpos[c] = (uint16_t)macro_pos(c);
}
static uint32_t def_scene(void) { return defaults()[0]; }
static uint32_t def_var(void) { return defaults()[1]; }
static void ctl_reset(void)
{
    uint32_t c;
    for (c = 0; c < WF_NCTL; c++)
        ctl_want[c] = -1;
}
static void pos_defaults(void)
{
    uint32_t c;
    for (c = 0; c < WF_NCTL; c++)
        macro_set(c, defpos[c]);
}
static const ov_tab_t *table_now(void)           /* a fresh table at the positions now (no target ramping home) */
{
    ov_reset();
    mac.dirty = 1;
    macro_eval();
    return &ovb[ov_live ^ 1u];
}
static int pid_of(const ov_slot_t *o)
{
    return o->kind == OV_P ? (int)(o->ptr - trk[o->part].p) : o->kind == OV_G ? (int)(o->ptr - song.g) : -1;
}
static const param_desc_t *desc_of(const ov_slot_t *o)
{
    int id = pid_of(o);
    if (o->kind == OV_G)
        return &GP[id];
    return o->part == TRK_DRUM || id < P_E0 ? &TP[id] : &ENGINES[trk[o->part].engine % NENGINES]->edit[id - P_E0];
}
static int eff_of(const ov_slot_t *o) { return o->kind >= OV_VCUT ? o->tgt : ov_effective(o, *o->ptr, o->tgt); }
static const ov_slot_t *find(const ov_tab_t *t, const int16_t *ptr)
{
    uint32_t i;
    for (i = 0; i < t->n; i++)
        if (t->s[i].kind < OV_VCUT && t->s[i].ptr == ptr)
            return &t->s[i];
    return 0;
}
static int eff_param(const ov_tab_t *t, int16_t *ptr)   /* the effective value of a parameter in table t */
{
    const ov_slot_t *s = find(t, ptr);
    return s ? eff_of(s) : *ptr;
}
static const int16_t KWIN[][3] = {{P_ATK, 0, GL_KATK_MAX}, {P_DEC, GL_KDEC_MIN, 127}, {P_REL, GL_KREL_MIN, GL_KREL_MAX},
                                  {P_LD_PIT, -GL_KLDPIT_MAX, GL_KLDPIT_MAX}, {P_LD_FLT, -GL_KLDFLT_MAX, GL_KLDFLT_MAX},
                                  {P_LD_AMP, 0, GL_KLDAMP_MAX}, {P_LRATE, GL_KLRATE_MIN, GL_KLRATE_MAX}};
/* every slot of table t: inside its descriptor, its range or its base; the keys track inside its windows */
static uint32_t table_bad(const ov_tab_t *t, char *why, size_t n)
{
    uint32_t i, k, bad = 0;
    for (i = 0; i < t->n; i++) {
        const ov_slot_t *o = &t->s[i];
        int e = eff_of(o), b, lo, hi;
        if (o->kind >= OV_VCUT) {
            bad += e < o->lo * 256 || e > o->hi * 256;
            continue;
        }
        b = *o->ptr;
        lo = o->lo < b ? o->lo : b;
        hi = o->hi > b ? o->hi : b;
        if (e < desc_of(o)->min || e > desc_of(o)->max || e < lo || e > hi) {
            if (!bad)
                snprintf(why, n, " slot %u/%u/%d: %d not in %d..%d", o->kind, o->part, pid_of(o), e, lo, hi);
            bad++;
        }
        if (o->kind == OV_P && o->part == wrt.keys_trk)
            for (k = 0; k < sizeof KWIN / sizeof KWIN[0]; k++)
                if (KWIN[k][0] == pid_of(o) && ((e < KWIN[k][1] && e < b) || (e > KWIN[k][2] && e > b))) {
                    if (!bad)
                        snprintf(why, n, " keys %s: %d outside %d..%d (base %d)", desc_of(o)->label, e, KWIN[k][1],
                                 KWIN[k][2], b);
                    bad++;
                }
    }
    return bad;
}
static int same_table(const ov_tab_t *a, const ov_slot_t *bs, uint32_t bn)
{
    uint32_t i;
    if (a->n != bn)
        return 0;
    for (i = 0; i < bn; i++)
        if (a->s[i].ptr != bs[i].ptr || a->s[i].kind != bs[i].kind || a->s[i].part != bs[i].part ||
            a->s[i].tgt != bs[i].tgt)
            return 0;
    return 1;
}

/* ================================================================ targets === */
static const char *const CN[WF_NCTL] = {"COLOR", "MOTION", "SPACE", "ENERGY", "SOFT", "SHORT", "BODY", "TAIL", "DRIFT",
                                        "WOBBLE", "PULSE", "RATE", "FILTER", "ECHO", "CRUSH", "FREEZE"};
static void sc_targets(void)
{
    static ov_slot_t t0[OV_MAX];
    static const uint16_t X[3] = {0, 500, 1000};
    uint32_t w, c, k, i, n0, bad = 0, home_bad = 0, still = 0, echo_bad = 0, crush_bad = 0, filt_bad = 0;
    char why[160] = "";
    for (w = 0; w < nw; w++) {
        const ov_tab_t *t;
        int full = w >= nfactory;
        load(w);
        if (world_apply(def_scene(), def_var())) {
            check(0, "world_apply");
            return;
        }
        t = table_now();
        n0 = t->n;
        memcpy(t0, t->s, sizeof t0);
        for (c = 4; c < WF_NCTL; c++) {
            for (k = 0; k < 3; k++) {
                pos_defaults();
                macro_set(c, X[k]);
                t = table_now();
                bad += table_bad(t, why, sizeof why) != 0;
                if (X[k] == MC_HOME[c] && defpos[c] == MC_HOME[c])
                    home_bad += !same_table(t, t0, n0);
                if (X[k] == 1000 && c != WF_CTL_FREEZE && same_table(t, t0, n0))
                    still++;
                if (c == WF_CTL_FREEZE)
                    home_bad += !same_table(t, t0, n0);    /* (FREEZE moves no parameter) */
                if (c == WF_CTL_ECHO && X[k]) {
                    int b = song.g[G_DFDBK], e = eff_param(t, &song.g[G_DFDBK]);
                    echo_bad += e > (b > GL_ECHO_DFDBK ? b : GL_ECHO_DFDBK) || e < b ||
                                eff_param(t, &song.g[G_DMIX]) < song.g[G_DMIX];
                }
                if (c == WF_CTL_CRUSH && X[k] == 1000) {
                    crush_bad += eff_param(t, &song.g[G_DUST]) < song.g[G_DUST];
                    for (i = 0; i < NPART; i++)
                        crush_bad += eff_param(t, &trk[i].p[P_LEVEL]) > trk[i].p[P_LEVEL];
                    crush_bad += eff_param(t, &song.g[G_DRLVL]) > song.g[G_DRLVL];
                }
                if (c == 12 && !full) {
                    int b = song.g[G_FILT], e = eff_param(t, &song.g[G_FILT]);
                    filt_bad += X[k] == 0 ? !(e < b) : X[k] == 500 ? find(t, &song.g[G_FILT]) != 0 : !(e > b);
                }
            }
        }
        /* SPACE and ECHO at 1: a big room shortens LIVE ECHO (guard_live) */
        pos_defaults();
        macro_set(2, 1000);
        macro_set(WF_CTL_ECHO, 1000);
        t = table_now();
        {
            int r = eff_param(t, &song.g[G_RSIZE]), b = song.g[G_DFDBK], e = eff_param(t, &song.g[G_DFDBK]);
            echo_bad += r >= 104 + WF_COMBO_RAMP && e > (b > 72 ? b : 72);
            if (w < nfactory)
                printf("livefx: %-14s ECHO 1 + SPACE 1: room %d, feedback %d -> %d (%.2f a repeat), mix %d -> %d\n",
                       world_name(), r, b, e, e * 230 / 32768.0, song.g[G_DMIX], eff_param(t, &song.g[G_DMIX]));
        }
        /* MOTION and MOVEMENT at 1 together, SOUND SHAPE at 1: the keys track inside its windows */
        pos_defaults();
        for (c = 4; c < 12; c++)
            macro_set(c, 1000);
        macro_set(1, 1000);
        bad += table_bad(table_now(), why, sizeof why) != 0;
        macro_set(11, 0);
        bad += table_bad(table_now(), why, sizeof why) != 0;
        /* CRUSH on a distorted part: DUST at most 64 (guard_live, whatever the World's combinations) */
        pos_defaults();
        trk[0].p[P_DIST] = 100;
        macro_set(WF_CTL_CRUSH, 1000);
        t = table_now();
        crush_bad += eff_param(t, &song.g[G_DUST]) > (song.g[G_DUST] > 64 ? song.g[G_DUST] : 64);
        trk[0].p[P_DIST] = 0;
        if (full) {                              /* full.world's controls: SOFT keys.atk +60 exp, FILTER fast */
            const ov_slot_t *s;
            pos_defaults();
            macro_set(4, 1000);
            macro_set(12, 0);
            t = table_now();
            s = find(t, &trk[wrt.keys_trk].p[P_ATK]);
            check(s && s->tgt == 60 * 256 && (s = find(t, &song.g[G_FILT])) && s->cls == 0,
                  "a World's controls replace the built-in ones (full: SOFT keys.atk +60, FILTER smooth fast)");
        }
    }
    check(!bad, "every World, each control 4..15 at 0 / 0.5 / 1: inside descriptors, ranges, the keys windows%s", why);
    check(!home_bad, "at home (and FREEZE anywhere) the table is exactly the default one");
    check(!still, "every control but FREEZE moves something at 1 (%u did not)", still);
    check(!echo_bad, "ECHO: mix up, feedback at most %d (with a big room %d)", GL_ECHO_DFDBK, 72);
    check(!crush_bad, "CRUSH: DUST up, the levels down (gain compensation); DUST <= 64 on a distorted part");
    check(!filt_bad, "FILTER: 0 low-pass, 0.5 off (no slot), 1 high-pass");
    /* the first World's values: what each control does (docs/guardrails.md) */
    load(0);
    world_apply(def_scene(), def_var());
    printf("livefx: %s, the effective values at 0 / 0.5 / 1 (the base first):\n", world_name());
    for (c = 4; c < WF_NCTL - 1u; c++) {
        const ov_tab_t *tt;
        char line[480];
        int v[3][OV_MAX];
        int16_t *ptr[OV_MAX];
        uint32_t np = 0, m, j;
        pos_defaults();
        macro_set(c, 1000);
        tt = table_now();
        for (i = 0; i < tt->n && np < OV_MAX; i++)
            if (tt->s[i].kind < OV_VCUT && eff_of(&tt->s[i]) != *tt->s[i].ptr)
                ptr[np++] = tt->s[i].ptr;
        for (k = 0; k < 3; k++) {
            pos_defaults();
            macro_set(c, X[k]);
            tt = table_now();
            for (m = 0; m < np; m++)
                v[k][m] = eff_param(tt, ptr[m]);
        }
        j = (uint32_t)snprintf(line, sizeof line, "  %-6s", CN[c]);
        for (m = 0; m < np && j < sizeof line - 48u; m++) {
            uint32_t tk = NTRK;
            int id;
            const char *lab;
            for (i = 0; i < NTRK; i++)
                if (ptr[m] >= trk[i].p && ptr[m] < trk[i].p + P_COUNT)
                    tk = i;
            id = tk < NTRK ? (int)(ptr[m] - trk[tk].p) : (int)(ptr[m] - song.g);
            lab = tk == NTRK ? GP[id].label : id >= P_E0 && tk < NPART ? ENGINES[trk[tk].engine % NENGINES]->edit[id - P_E0].label
                                                                       : TP[id].label;
            j += (uint32_t)snprintf(line + j, sizeof line - j, "  %s%s %d: %d/%d/%d",
                                    tk == NTRK ? "g." : tk == wrt.keys_trk ? "keys." : tk == TRK_DRUM ? "drums." : "part.",
                                    lab, *ptr[m], v[0][m], v[1][m], v[2][m]);
        }
        puts(line);
    }
}

/* ================================================================ renders === */
static result_t rend(uint32_t w, const char *ctl, double tail)
{
    result_t r;
    load(w);
    ctl_reset();
    if (ctl && ctl_parse(ctl))
        exit(2);
    keys_phrase = 1;
    tail_s = tail;
    peak_max = -0.5;
    r = render_one(def_scene(), def_var(), -1, NULL);
    tail_s = 6;
    return r;
}
static void sc_renders(void)
{
    static const char *const SHAPE[] = {"SOFT=1,SHORT=1,BODY=1,TAIL=1", "SHORT=1", "DRIFT=1,WOBBLE=1,PULSE=1,RATE=1",
                                        "DRIFT=1,WOBBLE=1,PULSE=1,RATE=0",
                                        "MOTION=1,SOFT=1,SHORT=1,BODY=1,TAIL=1,DRIFT=1,WOBBLE=1,PULSE=1,RATE=1",
                                        "FILTER=0", "FILTER=1"};
    uint32_t w, k, bad = 0;
    double worst_lu = 0, worst_peak = -100, worst_tail = -200, echo_tail = -200;
    char why[300] = "";
    for (w = 0; w < nfactory; w++) {
        result_t d = rend(w, NULL, 6), e = rend(w, "ECHO=1,SPACE=1", ECHO_TAIL), c = rend(w, "CRUSH=1", 6);
        printf("livefx: %-14s dry %.2f LUFS; CRUSH 1 %.2f LUFS (%+.2f LU), peak %.2f; ECHO 1 + SPACE 1: peak %.2f, "
               "%.1f dBFS %.0f s after STOP\n", world_name(), d.lufs, c.lufs, c.lufs - d.lufs, c.peak, e.peak, e.tail_db,
               ECHO_TAIL);
        check(!e.fails && e.tail_db < -60, "%s: ECHO and SPACE at 1: clean, the tail under -60 dBFS within %.0f s%s",
              world_name(), ECHO_TAIL, e.why);
        check(!c.fails && fabs(c.lufs - d.lufs) <= CRUSH_LU, "%s: CRUSH at 1: clean, within %.0f LU of dry (%+.2f)%s",
              world_name(), CRUSH_LU, c.lufs - d.lufs, c.why);
        check(!d.fails, "%s: the dry render clean%s", world_name(), d.why);
        worst_lu = fabs(c.lufs - d.lufs) > worst_lu ? fabs(c.lufs - d.lufs) : worst_lu;
        echo_tail = e.tail_db > echo_tail ? e.tail_db : echo_tail;
        for (k = 0; k < sizeof SHAPE / sizeof SHAPE[0]; k++) {
            result_t r = rend(w, SHAPE[k], 6);
            if (r.fails && bad++ < 4)
                snprintf(why + strlen(why), sizeof why - strlen(why), " [%s %s:%s]", world_name(), SHAPE[k], r.why);
            worst_peak = r.peak > worst_peak ? r.peak : worst_peak;
            worst_tail = r.tail_db > worst_tail ? r.tail_db : worst_tail;
        }
    }
    printf("livefx: SOUND SHAPE / MOVEMENT / FILTER extremes: worst peak %.2f dBFS, slowest tail %.1f dBFS 6 s after "
           "STOP; CRUSH at most %.2f LU from dry; ECHO + SPACE tails at most %.1f dBFS\n", worst_peak, worst_tail,
           worst_lu, echo_tail);
    check(!bad, "SOUND SHAPE, MOVEMENT (with MOTION) and FILTER extremes on every factory World: clean%s", why);
}

/* ======================================================== playing, by block === */
static int32_t lastl, lastr, pk, jump;           /* (since the last meter_reset) */
static uint32_t nfull, nblk;
static int16_t *cap;
static uint32_t ncap, capn;
static void meter_reset(void) { pk = jump = 0; nfull = 0; }
static void dblk(void)
{
    int32_t o[2 * CTL];
    uint32_t i;
    mix_block(o, CTL);
    for (i = 0; i < CTL; i++) {
        int32_t l = o[2 * i] > 32767 ? 32767 : o[2 * i] < -32768 ? -32768 : o[2 * i];
        int32_t r = o[2 * i + 1] > 32767 ? 32767 : o[2 * i + 1] < -32768 ? -32768 : o[2 * i + 1];
        int32_t a = abs(l - lastl) > abs(r - lastr) ? abs(l - lastl) : abs(r - lastr);
        jump = a > jump ? a : jump;
        pk = abs(l) > pk ? abs(l) : abs(r) > pk ? abs(r) : pk;
        nfull += (abs(l) >= 32767) + (abs(r) >= 32767);
        lastl = l;
        lastr = r;
        if (cap && ncap + 2u <= capn) {
            cap[ncap++] = (int16_t)l;
            cap[ncap++] = (int16_t)r;
        }
    }
    nblk++;
    macro_service();                             /* (the main loop between blocks: FREEZE, the tables) */
}
static void dblocks(uint32_t n)
{
    while (n--)
        dblk();
}
static uint32_t bar_blocks(void) { return bar_frames() / CTL; }
static void play_setup(uint32_t w)               /* the World at its defaults, a player's loop, PLAY MODE, PLAY */
{
    band_t b;
    load(w);
    ctl_reset();
    keys_phrase = 1;
    if (scene_go(def_scene(), def_var(), -1, &b))
        exit(2);
    wrt.mode = WM_PLAY;
    transport_req = 1;
    lastl = lastr = 0;
    meter_reset();
}
static void fx_release(void)                     /* FX let go, as ui_play.c does */
{
    uint32_t k;
    for (k = WF_CTL_LIVE; k < WF_NCTL; k++)
        macro_set(k, MC_HOME[k]);
    if (punch.req >= 0 && !punch.keybit)
        punch.req = -1;
    punch.hold = 0;
}

static void sc_filter(void)
{
    uint32_t i, n, k, bad = 0;
    play_setup(0);
    dblocks(bar_blocks());
    meter_reset();
    punch.hold = 1;
    n = 4u * bar_blocks();
    for (i = 0; i < n; i++) {                    /* 0 -> 1 -> 0 -> 1, a turn a bar */
        uint32_t ph = i * 4000u / n, x = ph % 2000u < 1000u ? ph % 1000u : 1000u - ph % 1000u;
        macro_set(12, (int32_t)x);
        dblk();
        bad += abs(djf.l1) > (1 << 24) || abs(djf.l2) > (1 << 24) || abs(djf.r1) > (1 << 24) || abs(djf.r2) > (1 << 24);
    }
    k = nfull;
    printf("livefx: FILTER swept 0 -> 1 -> 0 -> 1 over 4 bars: peak %.2f dBFS, largest jump %.3f of full scale\n",
           dbfs(pk), jump / 32768.0);
    check(!bad && !k && dbfs(pk) < -0.5 && jump < SLEW_MAX, "FILTER sweep: peak under -0.5 dBFS, no wrap, the filter "
                                                          "stable");
}

/* ================================================================= freeze === */
static int32_t want_loop(void)
{
    uint32_t x = macro_pos(WF_CTL_FREEZE);
    return x < WF_FREEZE_ON ? -1 : PX_LOOP4 + (int32_t)(x >= 750u ? 2u : x >= 500u ? 1u : 0u);
}
#define FZ_WARM 1u                               /* bars */
#define FZ_TAILB 2u                              /* bars after the last release; the last one compared */
static uint32_t fz_lcg = 7;
static uint32_t fz_rnd(uint32_t n)
{
    fz_lcg = fz_lcg * 1103515245u + 12345u;
    return (fz_lcg >> 8) % n;
}
static void fz_child(int stress, int fd, uint32_t stress_blocks)
{
    uint32_t ev = 0, stuck = 0, taken = 0, wrong = 0, dry_wait = 0, worst_dry = 0, key = 0, i;
    uint32_t engaged = 0, loops[3] = {0, 0, 0};
    play_setup(0);
    dblocks(FZ_WARM * bar_blocks());
    for (i = 0; i < stress_blocks;) {
        uint32_t run = 1u + fz_rnd(40), j;
        if (stress) {
            uint32_t a = fz_rnd(10);
            ev++;
            if (a < 4)                           /* FREEZE turned: often low, sometimes 0 */
                macro_set(WF_CTL_FREEZE, fz_rnd(3) ? (int32_t)fz_rnd(1001) : (int32_t)fz_rnd(250) * (int32_t)fz_rnd(2));
            else if (a < 7) {                    /* FX pressed or let go */
                if (punch.hold)
                    fx_release();
                else
                    punch.hold = 1;
            } else if (punch.hold && !key) {     /* a white key down (seq.c key_down while FX is held) */
                key = 1u << fz_rnd(16);
                punch.req = (int8_t)fz_rnd(PUNCH_NFX);
                punch.keybit = key;
            } else if (key) {                    /* .. and up (key_up) */
                if (punch.keybit == key) {
                    punch.keybit = 0;
                    punch.req = -1;
                }
                key = 0;
            }
        }
        for (j = 0; j < run && i < stress_blocks; j++, i++) {
            int8_t kreq = punch.req;
            dblk();
            if (!stress)
                continue;
            if (key) {                           /* a held key's effect is its own */
                taken += punch.req != kreq;
                dry_wait = 0;
            } else if (punch.hold && want_loop() >= 0) {
                wrong += punch.req != want_loop();
                engaged++;
                if (punch.cur >= PX_LOOP4 && punch.cur <= PX_LOOP16)
                    loops[punch.cur - PX_LOOP4]++;
                dry_wait = 0;
            } else {                             /* FX let go or under 25 %: dry within 4 blocks */
                dry_wait++;
                if (punch.req < 0 && punch.cur < 0 && punch.g == 0) {
                    worst_dry = dry_wait > worst_dry ? dry_wait : worst_dry;
                    dry_wait = 0;
                } else if (dry_wait > 4u) {
                    stuck++;
                    dry_wait = 0;
                }
            }
        }
    }
    if (stress) {
        if (key) {
            punch.keybit = 0;
            punch.req = -1;
        }
        fx_release();
    }
    dblocks((FZ_TAILB - 1u) * bar_blocks());
    capn = 2u * CTL * bar_blocks();
    cap = malloc(capn * sizeof *cap);
    ncap = 0;
    dblocks(bar_blocks());
    if (stress)
        printf("livefx: FREEZE: %u events over %.1f s (%u blocks engaged: 1 beat %u, 1/2 %u, 1/4 %u): %u stuck, %u wrong "
               "loops, %u keys taken over; dry again within %u blocks\n", ev, stress_blocks * (double)CTL / FS, engaged,
               loops[0], loops[1], loops[2], stuck, wrong, taken, worst_dry);
    if (stress)
        check(ev >= 1000 && !stuck && !wrong && !taken && worst_dry <= 4u && loops[0] && loops[1] && loops[2],
              "FREEZE 1000 times at random while playing: never stuck, the right loop, keys untouched, dry in 4 blocks");
    if (write(fd, &ncap, sizeof ncap) != (ssize_t)sizeof ncap ||
        write(fd, cap, ncap * sizeof *cap) != (ssize_t)(ncap * sizeof *cap))
        _exit(2);
    fflush(stdout);
    _exit(fails != 0);
}
static int16_t *fz_run(int stress, uint32_t blocks, uint32_t *n)
{
    int fd[2], st;
    pid_t pid;
    int16_t *b;
    fflush(stdout);
    if (pipe(fd) || (pid = fork()) < 0)
        exit(2);
    if (!pid) {
        close(fd[0]);
        fz_child(stress, fd[1], blocks);
    }
    close(fd[1]);
    if (read(fd[0], n, sizeof *n) != (ssize_t)sizeof *n)
        *n = 0;
    b = malloc(*n * sizeof *b + 1);
    {
        size_t got = 0, want = *n * sizeof *b;
        ssize_t r;
        while (got < want && (r = read(fd[0], (char *)b + got, want - got)) > 0)
            got += (size_t)r;
        if (got != want)
            *n = 0;
    }
    close(fd[0]);
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
    return b;
}
static void sc_freeze(void)
{
    uint32_t blocks = 1000u * 21u, na, nb, i, diff = 0;
    int16_t *a, *b;
    a = fz_run(0, blocks, &na);
    b = fz_run(1, blocks, &nb);
    for (i = 0; i < na && i < nb; i++)
        diff += a[i] != b[i];
    check(na && na == nb && !diff, "after the last release: the output equals the render without FREEZE, bit for bit "
                                   "(%u samples, %u differ)", na, diff);
    free(a);
    free(b);
}

/* ================================================================== punch === */
static void svc(void) { macro_service(); }
static void sc_punch(void)
{
    int ok;
    play_setup(0);
    dblocks(bar_blocks());
    punch.hold = 1;
    macro_set(WF_CTL_FREEZE, 600);
    svc();
    dblocks(4);
    check(punch.req == PX_LOOP8 && punch.cur == PX_LOOP8, "FX held, FREEZE 60 %%: the half-beat loop plays");
    punch.req = PX_REV;                          /* a white key (seq.c key_down) */
    punch.keybit = 1u << 3;
    svc();
    dblocks(4);
    check(punch.req == PX_REV && punch.cur == PX_REV, ".. a white key while FREEZE is up: its effect (REVERSE) wins");
    punch.keybit = 0;                            /* (key_up) */
    punch.req = -1;
    svc();
    dblocks(4);
    check(punch.req == PX_LOOP8 && punch.cur == PX_LOOP8, ".. the key let go: FREEZE's loop again");
    macro_set(WF_CTL_FREEZE, 0);
    svc();
    dblocks(3);
    check(punch.req == -1 && punch.cur == -1 && punch.g == 0, ".. FREEZE turned to 0: dry");
    punch.req = PX_STUT;
    punch.keybit = 1u << 5;
    svc();
    dblocks(3);
    ok = punch.req == PX_STUT && punch.cur == PX_STUT;
    punch.keybit = 0;
    punch.req = -1;
    svc();
    dblocks(3);
    check(ok && punch.cur == -1, "FREEZE at 0: SLOOP's punch FX on the white keys as before (STUTTER, then dry)");
    macro_set(WF_CTL_FREEZE, 1000);
    wrt.mode = WM_ADV;
    svc();
    ok = punch.req == -1;
    wrt.mode = WM_SLOOP;
    svc();
    check(ok && punch.req == -1, "FREEZE up in ADVANCED or SLOOP: nothing (their FX layer is SLOOP's)");
    wrt.mode = WM_PLAY;
    svc();
    ok = punch.req == PX_LOOP16;
    wrt.mode = WM_ADV;                           /* a mode change (ui_play.c pl_ui_reset lets FX go) */
    punch.hold = 0;
    svc();
    check(ok && punch.req == -1, "FREEZE 100 %%: the quarter-beat loop; a mode change withdraws it");
    wrt.mode = WM_PLAY;
    punch.hold = 1;
    svc();
    ok = punch.req == PX_LOOP16;
    world_unload();
    svc();
    check(ok && punch.req == -1, "a World unloaded with FREEZE up: withdrawn");
}

/* ================================================================ release === */
static int eff_live(int16_t *p)                  /* the effective value the ISR writes now (the live table) */
{
    const ov_tab_t *t = &ovb[ov_live];
    uint32_t i;
    for (i = 0; i < t->n; i++)
        if (t->s[i].kind < OV_VCUT && t->s[i].ptr == p)
            return ov_effective(&t->s[i], *p, ov_cur[ov_live][i]);
    return *p;
}
static void sc_release(void)
{
    uint32_t w;
    double worst_ms = 0, worst_x = 0;
    for (w = 0; w < nfactory; w++) {
        int fd[2], st;
        pid_t pid;
        int32_t ref = 0;
        fflush(stdout);
        if (pipe(fd) || (pid = fork()) < 0)
            exit(2);
        if (!pid) {                              /* the dry render's largest jump over the same blocks */
            close(fd[0]);
            play_setup(w);
            dblocks(bar_blocks());
            meter_reset();
            dblocks(bar_blocks() + FS / CTL);
            if (write(fd[1], &jump, sizeof jump) != (ssize_t)sizeof jump)
                _exit(2);
            _exit(0);
        }
        close(fd[1]);
        if (read(fd[0], &ref, sizeof ref) != (ssize_t)sizeof ref)
            ref = 0;
        close(fd[0]);
        waitpid(pid, &st, 0);
        if (!(pid = fork())) {
            int16_t *G[4] = {&song.g[G_FILT], &song.g[G_DMIX], &song.g[G_DFDBK], &song.g[G_DUST]};
            int prev[4], k, snap = 0;
            uint32_t b, home = 0;
            int32_t steady, after;
            play_setup(w);
            dblocks(bar_blocks());
            punch.hold = 1;
            macro_set(12, w & 1u ? 1000 : 0);
            macro_set(WF_CTL_ECHO, 1000);
            macro_set(WF_CTL_CRUSH, 1000);
            macro_set(WF_CTL_FREEZE, 1000);
            dblocks(FS / 2u / CTL);
            meter_reset();
            dblocks(bar_blocks() - FS / 2u / CTL);
            steady = jump;
            for (k = 0; k < 4; k++)
                prev[k] = eff_live(G[k]);
            fx_release();
            meter_reset();
            for (b = 0; b < FS / CTL; b++) {
                int all = 1;
                dblk();
                for (k = 0; k < 4; k++) {
                    int e = eff_live(G[k]);
                    snap += abs(e - prev[k]) > 2;
                    prev[k] = e;
                    all &= e == *G[k];
                }
                all &= !djf.mode && punch.cur < 0 && punch.g == 0;
                if (all && !home)
                    home = b + 1u;
                if (b + 1u == FS / 2u / CTL)
                    after = jump;
            }
            {
                double ms = home * (double)CTL * 1000.0 / FS, x = after / (double)(ref > steady ? ref : steady);
                printf("livefx: %-14s release (FILTER %s): home in %.0f ms; largest jump after %.3f, dry %.3f, the "
                       "effects' own %.3f (%.2fx)\n", world_name(), w & 1u ? "1" : "0", ms, after / 32768.0,
                       ref / 32768.0, steady / 32768.0, x);
                check(home && ms <= RELEASE_MS && !snap && x <= CLICK_X, "%s: FX let go: a ramp (no snap), home in "
                      "%.0f ms, no click (%.2fx)", world_name(), ms, x);
                fflush(stdout);
                _exit(fails != 0);
            }
        }
        waitpid(pid, &st, 0);
        fails += !WIFEXITED(st) || WEXITSTATUS(st);
        (void)worst_ms;
        (void)worst_x;
    }
}

/* =================================================================== cost === */
#ifdef __APPLE__
static uint64_t instr_now(void)
{
    struct rusage_info_v4 ri;
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return 0;
    return ri.ri_instructions;
}
#else
static uint64_t instr_now(void) { return 0; }
#endif
static double cost_of(uint32_t w, int all, uint32_t *est)
{
    int fd[2];
    pid_t pid;
    double r[2] = {0, 0};
    if (pipe(fd) || (pid = fork()) < 0)
        exit(2);
    if (!pid) {
        uint32_t c, n, i, e = 0;
        int32_t o[2 * CTL];
        uint64_t a, best = ~0ull;
        play_setup(w);
        if (all) {                               /* (LIVEFX_CTL=c: that control alone, to measure its stage) */
            const char *one = getenv("LIVEFX_CTL");
            for (c = one ? (uint32_t)atoi(one) : 4u; c < (one ? (uint32_t)atoi(one) + 1u : WF_NCTL); c++)
                macro_set(c, c == 12 ? 0 : 1000);
            if (!one)
                macro_set(1, 1000);
            punch.hold = 1;
        }
        dblocks(bar_blocks());
        n = 2u * bar_blocks();
        for (i = 0; i < 3u; i++) {               /* (the least of three: the kernel only adds) */
            uint32_t k;
            a = instr_now();
            for (k = 0; k < n; k++) {
                mix_block(o, CTL);
                if (gcpu.est > e)
                    e = gcpu.est;
            }
            a = instr_now() - a;
            best = a < best ? a : best;
            macro_service();
        }
        r[0] = (double)best / ((double)n * CTL);
        r[1] = e;
        if (write(fd[1], r, sizeof r) != (ssize_t)sizeof r)
            _exit(2);
        _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], r, sizeof r) != (ssize_t)sizeof r)
        r[0] = 1e9;
    close(fd[0]);
    waitpid(pid, 0, 0);
    *est = (uint32_t)r[1];
    return r[0];
}
static int cost(void)
{
    uint32_t w, e0, e1, bad = 0;
    for (w = 0; w < nfactory; w++) {
        double c0 = cost_of(w, 0, &e0), c1 = cost_of(w, 1, &e1);
        load(w);
        printf("cost: %-14s %.0f host instructions a sample at the defaults, %.0f with everything of Phase 13 at 1 "
               "(%+.0f); guard.c's estimate %u / %u\n", world_name(), c0, c1, c1 - c0, e0, e1);
        bad += c1 > GL_CPU_BUDGET || (c1 > 0 && e1 < c1 * 0.85);
    }
    if (bad)
        printf("cost: over GL_CPU_BUDGET (%d) or the estimate more than 15 %% under\n", GL_CPU_BUDGET);
    return bad != 0;
}

/* =================================================================== main === */
static void run_sc(const char *name, void (*fn)(void))
{
    int st;
    pid_t pid;
    fflush(stdout);
    if (!(pid = fork())) {
        fn();
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("livefx: scenario %s FAILED%s\n", name, WIFEXITED(st) ? "" : " (crashed)");
        fails++;
    }
}

int main(int argc, char **argv)
{
    int i, do_cost = 0;
    const char *only = getenv("LIVEFX");
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cost")) {
            do_cost = 1;
            continue;
        }
        if (nw >= 8 || !read_all(argv[i]))
            return 2;
        wb[nw] = blob;
        wn[nw] = blob_n;
        nw++;
    }
    host_tracks_init();
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    song.playing = 0;
    transport_req = 0;
    for (nfactory = 0; nfactory < nw; nfactory++) {   /* (the factory Worlds first; full.world after them) */
        load(nfactory);
        if (!strcmp(world_name(), "MIDNIGHT TEST"))
            break;
    }
    if (!nfactory) {
        printf("usage: livefx_test [--cost] FACTORY.wblob ... [full.wblob]\n");
        return 2;
    }
    if (do_cost)
        return cost();
    if (GU_COMBO_LIVE[WF_COMBO_LEN + 8] != GL_ECHO_DFDBK) {
        printf("livefx: WF_COMBO_LIVE's feedback cap is not GL_ECHO_DFDBK\n");
        return 1;
    }
#define SC(n, f) if (!only || !strcmp(only, n)) run_sc(n, f)
    SC("targets", sc_targets);
    SC("punch", sc_punch);
    SC("filter", sc_filter);
    SC("freeze", sc_freeze);
    SC("release", sc_release);
    SC("renders", sc_renders);
    printf(fails ? "LIVE FX TEST FAILED\n" : "livefx test: all checks passed\n");
    return fails != 0;
}
