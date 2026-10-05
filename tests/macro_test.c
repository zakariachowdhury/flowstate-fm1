/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the performance macros and ENERGY as arrangement (PLAY MODE, Phase 7: firmware/src/macro.c,
 * arrange.c, guard_limits.h and the hooks H1, H3, H4, H5, H13, H14, H16), built as tests/smartkeys_test.c is
 * (FELUCCA_WORLD=1 on tests/hostsim.c, -fsanitize=address,undefined) and run by tests/run_tests.sh:
 *   build/host/macro_test [WORLD.wblob ...]      (the factory Worlds are built in; the test Worlds as arguments)
 *   build/host/macro_test --cost                 (an -O2 build without sanitizers: the overlay's instructions)
 *
 *  1. inert: with no World, and after world_unload, the overlay, the vmod offsets and the ENERGY mutes do nothing:
 *     p[] and g[] unchanged by mix_block, no slot, wvm 0, wrt.mute 0;
 *  2. the model: for every World, every position of COLOR MOTION SPACE ENERGY on the grid 0, .25, .5, .75, 1 (625
 *     combinations) and the World's default scene, the target table equals an independent model of design 5.2-5.4
 *     written here from the blob with doubles (curves, homes, roles from the TRACKS engines, sums per target,
 *     rules and their strength, the descriptor and guard_limits.h ranges, the World's GUARD caps and ranges and its
 *     distorted-track count; the sound combinations are tests/guard_test.c's and are off here): the same slots,
 *     each target within a
 *     few 1/256 of a step, each effective parameter value the model's (+-1 where the model sits on a rounding
 *     edge); no effective value outside its descriptor or past a hard limit (delay feedback and reverb gain < 1 as
 *     fx.c computes them, level 120, resonance 110, vmod 64), none at its descriptor's maximum unless the World
 *     put the base there; and after world_fx_post every base value is back, bit for bit;
 *  3. curves: every built-in curve and every CURVES LUT at 33 points against the model;
 *  4. rules: each rule is silent at its thresholds, acts just above them, and in full with both controls at 100 %
 *     (the model); the factory Worlds' three cross-macro rules (SPACE+ENERGY, COLOR+ENERGY, MOTION+ENERGY) only cut;
 *  5. smoothing: a step of every macro to its other end ramps: no slot's offset moves more in a block than its
 *     class's one-pole step (at least 1/256), never past its target, the effective value at most that step + 1 per
 *     block, a fast slot settles within 200 blocks, a slow one within 5000; a World load starts at the targets;
 *  6. the real mix: every World played 4 bars with the macros moving every few blocks and a scene committed on a
 *     bar inside the overlay's window: after every mix_block each track's p[] and the globals are the staged base
 *     values exactly (the overlay always restores, a commit included);
 *  7. ENERGY bands: stopped, a band applies at once; the hysteresis holds a knob resting on an edge (+-15 per
 *     mille wiggle: no change) and moves at +-25; playing, a change begins only on a beat, its lanes and masks
 *     land on the beat (the bar with density_change bar), its layers on a bar (every second with mute_change 2),
 *     two changes at least min_band_bars apart; the Smart Keys track is never muted; what the sequencer plays obeys
 *     the band (drum hits only on allowed lanes and density steps, a masked synth NOTE rests, a muted layer starts
 *     nothing); the fill plays on the phrase's last bar;
 *  8. ~bright reaches the engines (H3): a part held dry is brighter at COLOR 100 % than at home;
 *  9. engines and reloads: another engine on a track re-resolves its roles; a hot reload keeps the positions; more
 *     targets than slots: the first 48 apply and restore, the rest are counted. */
#define FELUCCA_WORLD 1
#define FELUCCA_ARRANGER 1                       /* (events_block's World hook, world_block, is in that branch) */
#include <stdint.h>
static void arrangement_apply(uint32_t s);       /* arranger.c's song mode: never reached in a World */
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i)       /* (project.c's PROJ_HOST part asks for it) */
{
    static const uint8_t E[NPART] = {0, 1, 3};
    return i < NPART ? E[i] : 0u;
}
#include "../firmware/src/project.c"
#include "../firmware/src/world.c"
static void arrangement_apply(uint32_t s) { (void)s; }
static uint32_t arrangement_ready(void) { return 0; }
static void song_backup(void) {}
static void song_restore(void) {}
#ifdef __APPLE__
#include <libproc.h>
#endif

static int t_fails;
static uint32_t t_checks;
#define CHECK(c, ...) do { t_checks++; if (!(c)) { if (t_fails++ < 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                        printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t t_rs = 0x7a11c0deu;
static uint32_t t_rnd(void)
{
    t_rs = t_rs * 1664525u + 1013904223u;
    return t_rs >> 8;
}

/* ------------------------------------------------------------------ the instrument --- */
static void t_reset(void)                        /* power-on: SLOOP's defaults, nothing sounding, no World */
{
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    memset(&wrt, 0, sizeof wrt);
    memset(&wst, 0, sizeof wst);
    memset(hprog, 0, sizeof hprog);
    memset(skm, 0, sizeof skm);
    memset(vref, 0, sizeof vref);
    memset(vlive, 0, sizeof vlive);
    memset(roll, 0, sizeof roll);
    memset(ovb, 0, sizeof ovb);
    memset(ov_cur, 0, sizeof ov_cur);
    memset(&mac, 0, sizeof mac);
    memset(&arr, 0, sizeof arr);
    memset(wvm_cut, 0, sizeof wvm_cut);
    memset(wvm_shape, 0, sizeof wvm_shape);
    ov_pub = ov_seen = ov_live = 0;
    ov_blk = 0;
    ov_in = 0;
    hcur = skcur = 0;
    harm.ci = 255;
    sk_mode_req = 255;
    kb_prev = 0;
    fm1_in.notes = fm1_in.buttons = 0;
    transport_req = panic_req = 0;
    clk_beat = clk_pos = 0;
    mi_r = mi_w = mo_r = mo_w = 0;
    rec_wait = 0;
    ft_on = 0;
    host_tracks_init();
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
}
static void t_block(void)
{
    int32_t o[2 * CTL];
    mix_block(o, CTL);
    mo_r = mo_w;
}
static void t_blocks(uint32_t n) { while (n--) t_block(); }
static void t_events(void)                       /* one block of the ISR without the DSP: overlay, events, overlay out */
{
    world_fx_pre();
    events_block(CTL);
    world_fx_post();
    mo_r = mo_w;
}

typedef struct {
    char name[40];
    uint8_t *b;
    uint32_t n;
    int factory;
} tworld_t;
static tworld_t worlds[32];
static uint32_t nworlds;

static uint8_t *t_read(const char *path, uint32_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    long len;
    if (!f) {
        printf("cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len)
        exit(1);
    fclose(f);
    *n = (uint32_t)len;
    return b;
}
static uint8_t t_ncombos;                        /* the World's sound combinations (--cost puts them back) */
static int t_start(const tworld_t *w)            /* power-on, then the World (its default scene) */
{
    int rc;
    t_reset();
    rc = world_start(w->b, w->n);
    CHECK(rc == WE_OK, "%s: world_start %d", w->name, rc);
    t_ncombos = gu.ncombos;
    gu.ncombos = 0;                              /* (the sound combinations: tests/guard_test.c against its model) */
    return rc;
}
/* the positions now into the ISR's table (the last publication taken first); snap: at the targets */
static void t_publish(int snap)
{
    if (ov_pub != ov_seen) {                     /* (the ISR takes a table at its next block) */
        world_fx_pre();
        world_fx_post();
    }
    mac.snap = (uint8_t)snap;
    mac.dirty = 1;
    CHECK(macro_eval() == 0, "macro_eval refused: the last table not taken");
    world_fx_pre();                              /* (taken) */
    world_fx_post();
}

/* ---------------------------------------------- the reference model (design 5.2 - 5.4) --- */
static const uint16_t M_HOME[WF_NCTL] = WF_CTL_HOME;
static const uint8_t M_ROLE[9][WF_NEROLES] = WF_ENG_ROLE;
enum { MK_P, MK_G, MK_VC, MK_VS };               /* as OV_P OV_G OV_VCUT OV_VSHP, written again */
typedef struct {
    int kind, t, id;
    double off;                                  /* steps */
    int nsrc;                                    /* contributions */
    double tol;                                  /* the integer pipeline's error bound: per contribution |amount| x
                                                  * 4/4096 (u and the curve in Q12) + 2/256 (the Q8 floors) */
    int lo, hi, dmin, dmax;                      /* the range (descriptor and limits), the descriptor */
    uint32_t ctls;                               /* controls with a positive max moving it (the saturation check) */
    double raw;                                  /* off before the span's clamp */
    int cls;                                     /* smoothing: the slowest of its mappings' (stepped wins), -1 none */
    int enm;                                     /* an F_ENUM parameter (WHEEL ROTR): always stepped */
} mslot_t;
static mslot_t ms[160];
static int nms;
static const uint8_t *M_b;                       /* the World's blob and its sections */
static const uint8_t *M_maps, *M_curves, *M_rules, *M_tracks, *M_guard;
static uint32_t M_nmaps, M_ncurves, M_nrules, M_nranges;

static void m_world(void)                        /* the loaded World's sections, from its blob (wctx: wb_check's) */
{
    M_b = wctx.b;
    M_maps = wctx.have >> WF_S_MAPS & 1u ? wctx.b + wctx.off[WF_S_MAPS] : 0;
    M_nmaps = wctx.have >> WF_S_MAPS & 1u ? wctx.cnt[WF_S_MAPS] : 0;
    M_curves = wctx.have >> WF_S_CURVES & 1u ? wctx.b + wctx.off[WF_S_CURVES] : 0;
    M_ncurves = wctx.have >> WF_S_CURVES & 1u ? wctx.cnt[WF_S_CURVES] : 0;
    M_rules = wctx.have >> WF_S_RULES & 1u ? wctx.b + wctx.off[WF_S_RULES] : 0;
    M_nrules = wctx.have >> WF_S_RULES & 1u ? wctx.cnt[WF_S_RULES] : 0;
    M_tracks = wctx.b + wctx.off[WF_S_TRACKS];
    M_guard = wctx.have >> WF_S_GUARD & 1u ? wctx.b + wctx.off[WF_S_GUARD] : 0;
    M_nranges = M_guard ? wctx.cnt[WF_S_GUARD] : 0;
}
static uint32_t m_engine(uint32_t t);
static int m_gbyte(uint32_t off)                 /* a GUARD fixed field, or the firmware default (design 6.1) */
{
    static const uint8_t D[] = WF_GUARD_DEFAULT;
    return M_guard && M_guard[off] != 255 ? M_guard[off] : D[off - WF_G_POLY];
}
/* the World's GUARD on a slot's range (design 6.1, written again from the format): the soft caps (max_level on any
 * track's level, max_dist, max_reso on the RESO role, GRAIN DENS, max_dfdbk, max_rsize, max_dust), then every target
 * range naming it (a parameter on a track in its mask, a role resolved on the engine, a global, ~bright / ~shape) */
static void m_guard_range(int kind, int t, int id, int *lo, int *hi)
{
    int cap = 1 << 20, e = t >= 0 && t < NPART ? (int)m_engine((uint32_t)t) : -1;
    uint32_t i;
    if (kind == MK_G && id == G_DFDBK)
        cap = m_gbyte(WF_G_MAXDFDBK);
    else if (kind == MK_G && id == G_RSIZE)
        cap = m_gbyte(WF_G_MAXRSIZE);
    else if (kind == MK_G && id == G_DUST)
        cap = m_gbyte(WF_G_MAXDUST);
    else if (kind == MK_P && id == P_LEVEL)
        cap = m_gbyte(WF_G_MAXLEVEL);
    else if (kind == MK_P && id == P_DIST)
        cap = m_gbyte(WF_G_MAXDIST);
    else if (kind == MK_P && e >= 0 && id >= P_E0 && M_ROLE[e][1] == id - P_E0)
        cap = m_gbyte(WF_G_MAXRESO);
    else if (kind == MK_P && e == 8 && id == P_E3)
        cap = m_gbyte(WF_G_GRAINDENS);
    if (*hi > cap)
        *hi = cap;
    for (i = 0; i < M_nranges; i++) {
        const uint8_t *r = M_guard + WF_GUARD_FIX + 4 * i;
        int rk = r[0] >> 5, m = r[0] & 31, hit;
        if (kind == MK_G)
            hit = rk == WF_K_GLOBAL && r[1] == id;
        else if (t < 0 || !(m >> t & 1))
            hit = 0;
        else if (kind == MK_VC || kind == MK_VS)
            hit = rk == (kind == MK_VC ? WF_K_BRIGHT : WF_K_SHAPE);
        else if (rk == WF_K_PARAM)
            hit = r[1] == id;
        else
            hit = rk == WF_K_ROLE && e >= 0 && r[1] < 8 && M_ROLE[e][r[1]] != WF_NONE && P_E0 + M_ROLE[e][r[1]] == id;
        if (hit) {
            if ((int8_t)r[2] > *lo)
                *lo = (int8_t)r[2];
            if ((int8_t)r[3] < *hi)
                *hi = (int8_t)r[3];
        }
    }
    if (*hi < *lo)
        *hi = *lo;
}
static uint32_t m_engine(uint32_t t)             /* track t's engine, as the TRACKS section says */
{
    const uint8_t *r = M_tracks;
    while (t--)
        r += WF_TRACK_HDR + WF_PAIR * r[4];
    return r[1];
}
static double m_curve(uint32_t c, double u)      /* u 0..1 */
{
    switch (c) {
    case 0: return u;
    case 1: return u * u;
    case 2: return sqrt(u);
    case 3: return u * u * (3 - 2 * u);
    case 4: return u <= 0.5 ? 0 : 2 * (u - 0.5);
    default: {
        const uint8_t *l = M_curves + WF_CURVE_LEN * (c - WF_CURVE_CUSTOM);
        double x = u * 8;
        int i = (int)floor(x);
        if (i >= 8)
            return 1;
        return (l[i] + (l[i + 1] - l[i]) * (x - i)) / 255.0;
    }
    }
}
static double m_offset(const uint8_t *m, const uint16_t *pos)   /* steps */
{
    double x = pos[m[0]], h = M_HOME[m[0]];
    if (x < h)
        return (int8_t)m[4] * m_curve(m[3] & WF_CURVE_MASK, (h - x) / h);
    if (x > h)
        return (int8_t)m[5] * m_curve(m[3] & WF_CURVE_MASK, (x - h) / (1000 - h));
    return 0;
}
static double m_strength(const uint8_t *r, const uint16_t *pos)
{
    double ta = r[1] * 4.0, tb = r[2] * 4.0, sa, sb;
    sa = (pos[r[0] >> 4] - ta) / (1000 - ta);
    sb = (pos[r[0] & 15] - tb) / (1000 - tb);
    sa = sa < sb ? sa : sb;
    return sa < 0 ? 0 : sa > 1 ? 1 : sa;
}
static mslot_t *m_slot(int kind, int t, int id)
{
    const param_desc_t *d;
    mslot_t *s;
    int i;
    for (i = 0; i < nms; i++)
        if (ms[i].kind == kind && ms[i].t == t && ms[i].id == id)
            return &ms[i];
    s = &ms[nms++];
    memset(s, 0, sizeof *s);
    s->kind = kind, s->t = t, s->id = id;
    s->cls = -1;
    if (kind == MK_VC || kind == MK_VS) {
        s->lo = s->dmin = -GL_VMOD_MAX;
        s->hi = s->dmax = GL_VMOD_MAX;
        m_guard_range(kind, t, id, &s->lo, &s->hi);
        return s;
    }
    d = kind == MK_G ? &GP[id] : t == TRK_DRUM || id < P_E0 ? &TP[id] : &ENGINES[m_engine((uint32_t)t)]->edit[id - P_E0];
    s->lo = s->dmin = d->min;
    s->hi = s->dmax = d->max;
    s->enm = d->fmt == F_ENUM;
    if (kind == MK_G && id == G_DFDBK && s->hi > GL_DFDBK_MAX)
        s->hi = GL_DFDBK_MAX;
    if (kind == MK_G && id == G_RSIZE && s->hi > GL_RSIZE_MAX)
        s->hi = GL_RSIZE_MAX;
    if (kind == MK_P && id == P_LEVEL && s->hi > GL_LEVEL_MAX)
        s->hi = GL_LEVEL_MAX;
    if (kind == MK_P && t < NPART && id >= P_E0 && M_ROLE[m_engine((uint32_t)t)][1] == id - P_E0 && s->hi > GL_RESO_MAX)
        s->hi = GL_RESO_MAX;
    m_guard_range(kind, t, id, &s->lo, &s->hi);
    return s;
}
static void m_cls(mslot_t *s, int cls)         /* a mapping's class (-1: a rule's action, none) */
{
    if (cls >= 0)
        s->cls = s->cls == 3 || cls == 3 ? 3 : cls > s->cls ? cls : s->cls;
}
/* a MAPS / RULES target with offset off (steps), ctl: the control of a mapping with a positive max (or -1) */
static int m_cur_cls = -1;
static double m_cur_lim;                         /* |the contribution's end amount| (the tolerance) */
static void m_add(uint32_t tg, uint32_t id, double off, int ctl)
{
    int kind = tg >> 5, t;
    uint32_t m = tg & WF_TMASK;
    mslot_t *s;
    if (kind == WF_K_GLOBAL) {
        s = m_slot(MK_G, -1, (int)id);
        s->off += off, s->nsrc++, s->tol += m_cur_lim * 4.0 / 4096 + 2.0 / 256;
        m_cls(s, m_cur_cls);
        if (ctl >= 0)
            s->ctls |= 1u << ctl;
        return;
    }
    for (t = 0; t < NTRK; t++) {
        if (!(m >> t & 1u))
            continue;
        if (kind == WF_K_PARAM)
            s = m_slot(MK_P, t, (int)id);
        else if (t >= NPART)
            continue;
        else if (kind == WF_K_ROLE) {
            uint32_t r = M_ROLE[m_engine((uint32_t)t)][id];
            if (r == WF_NONE)
                continue;
            s = m_slot(MK_P, t, P_E0 + (int)r);
        } else {
            s = m_slot(kind == WF_K_BRIGHT ? MK_VC : MK_VS, t, 0);
        }
        s->off += off, s->nsrc++, s->tol += m_cur_lim * 4.0 / 4096 + 2.0 / 256;
        m_cls(s, m_cur_cls);
        if (ctl >= 0)
            s->ctls |= 1u << ctl;
    }
}
static int m_effective(const mslot_t *s, int b, double off, int *edge);
static void m_eval(const uint16_t *pos)          /* the model's slots at these positions */
{
    const uint8_t *p;
    uint32_t i, k;
    nms = 0;
    for (i = 0, p = M_maps; i < M_nmaps; i++, p += WF_MAP_LEN) {
        m_cur_cls = p[3] >> 6;
        m_cur_lim = abs((int8_t)p[4]) > abs((int8_t)p[5]) ? abs((int8_t)p[4]) : abs((int8_t)p[5]);
        m_add(p[1], p[2], m_offset(p, pos), (int8_t)p[5] > 0 ? p[0] : -1);
    }
    m_cur_cls = -1;
    m_cur_lim = 128;                             /* (a rule's strength: the same Q12 truncation) */
    for (i = 0, p = M_rules; i < M_nrules; i++, p += WF_RULE_HDR + WF_ACT_LEN * p[3])
        for (k = 0; k < p[3]; k++)
            m_add(p[WF_RULE_HDR + WF_ACT_LEN * k], p[WF_RULE_HDR + WF_ACT_LEN * k + 1],
                  (int8_t)p[WF_RULE_HDR + WF_ACT_LEN * k + 2] * m_strength(p, pos), -1);
    for (i = 0; i < (uint32_t)nms; i++) {        /* the target never further than the range's span */
        double span = ms[i].kind >= MK_VC ? GL_VMOD_MAX : ms[i].hi - ms[i].lo;
        ms[i].raw = ms[i].off;
        ms[i].off = ms[i].off < -span ? -span : ms[i].off > span ? span : ms[i].off;
        ms[i].cls = ms[i].enm ? 3 : ms[i].cls < 0 ? WF_CLASS_DEFAULT : ms[i].cls;
    }
    if (nms > OV_MAX)                            /* (the firmware keeps the first 48 it makes, as made here) */
        nms = OV_MAX;
    {   /* GUARD max_dist_tracks: a clean track a macro would distort beyond them stays clean; vmod in its range */
        int n = 0, t, mx = m_gbyte(WF_G_DISTTRK), edge;
        for (t = 0; t < NPART; t++)
            n += trk[t].p[P_DIST] > 0;
        for (i = 0; i < (uint32_t)nms; i++) {
            mslot_t *s = &ms[i];
            if (s->kind == MK_P && s->t < NPART && s->id == P_DIST && trk[s->t].p[P_DIST] <= 0 &&
                m_effective(s, trk[s->t].p[P_DIST], s->off, &edge) > 0) {
                if (n < mx)
                    n++;
                else
                    s->hi = trk[s->t].p[P_DIST];
            }
            if (s->kind >= MK_VC)
                s->off = s->off < s->lo ? s->lo : s->off > s->hi ? s->hi : s->off;
        }
    }
}
static int m_kind(uint32_t ov) { return ov == OV_P ? MK_P : ov == OV_G ? MK_G : ov == OV_VCUT ? MK_VC : MK_VS; }
static void fw_id(const ov_slot_t *s, int *t, int *id)
{
    *t = s->kind == OV_G ? -1 : s->part;
    *id = s->kind == OV_P ? (int)(s->ptr - trk[s->part].p) : s->kind == OV_G ? (int)(s->ptr - song.g) : 0;
}
static mslot_t *m_find(const ov_slot_t *s)
{
    int t, id, i;
    fw_id(s, &t, &id);
    for (i = 0; i < nms; i++)
        if (ms[i].kind == m_kind(s->kind) && ms[i].t == t && ms[i].id == id)
            return &ms[i];
    return 0;
}
/* the model's effective value of base b with offset off (steps); *edge: off sits near a rounding edge */
static int m_effective(const mslot_t *s, int b, double off, int *edge)
{
    double v = b + off, f = v - floor(v);
    int e = (int)floor(v + 0.5);
    *edge = fabs(f - 0.5) < 0.06;
    if (e > s->hi && e > b)
        e = b > s->hi ? b : s->hi;
    else if (e < s->lo && e < b)
        e = b < s->lo ? b : s->lo;
    return e;
}

/* ---------------------------------------------------------------- 1. inert --- */
static void test_inert(void)
{
    int16_t p0[NTRK][P_COUNT], g0[G_COUNT];
    uint32_t i, t, n = 0;
    for (int pass = 0; pass < 2; pass++) {
        t_reset();
        if (pass) {                              /* a World loaded, played, then left: SLOOP again */
            CHECK(world_start(worlds[0].b, worlds[0].n) == WE_OK, "world_start");
            macro_set(0, 1000);
            macro_set(3, 0);
            t_publish(1);
            transport_req = 1;
            t_blocks(500);
            transport_req = 2;
            t_blocks(10);
            world_unload();
            for (t = 0; t < NTRK; t++)
                trk[t].p[P_MUTE] = 0;
        }
        for (t = 0; t < NPART; t++)              /* something sounding: a held chord on every part */
            for (i = 0; i < 3; i++)
                trk_note_on(&trk[t], 48 + 4 * i + t, 100);
        transport_req = 1;
        for (t = 0; t < NTRK; t++)
            memcpy(p0[t], trk[t].p, sizeof p0[t]);
        memcpy(g0, song.g, sizeof g0);
        for (i = 0; i < 300; i++) {
            uint32_t blk = ov_blk;
            t_block();
            for (t = 0; t < NTRK; t++)
                n += memcmp(p0[t], trk[t].p, sizeof p0[t]) != 0;
            n += memcmp(g0, song.g, sizeof g0) != 0;
            n += ov_blk != blk || ov_in || wrt.mute;
            for (t = 0; t < NPART; t++)
                n += wvm_cut[t] != 0 || wvm_shape[t] != 0;
        }
        CHECK(!n, "inert %s: %u blocks with a parameter, a slot, a vmod offset or a mute moved", pass ? "after world_unload" : "SLOOP", n);
        CHECK(!ovb[ov_live].n || !pass, "world_unload left %u slots", ovb[ov_live].n);
    }
    printf("inert: no World, and after world_unload: mix_block leaves p[] / g[] alone, no slot, no vmod, no ENERGY mute\n");
}

/* ---------------------------------------------------------- 2. the model grid --- */
static uint32_t grid_slots, grid_eff, grid_edge, grid_sat, clamps, idle;
static void check_table(const tworld_t *w, const uint16_t *pos)
{
    const ov_tab_t *tb = &ovb[ov_live];
    int16_t p0[NTRK][P_COUNT], g0[G_COUNT], pe[NTRK][P_COUNT], ge[G_COUNT];
    int32_t vc[NPART], vs[NPART];
    uint32_t i, t;
    char at[64];
    snprintf(at, sizeof at, "%s at %.2f %.2f %.2f %.2f", w->name, pos[0] / 1e3, pos[1] / 1e3, pos[2] / 1e3, pos[3] / 1e3);
    m_eval(pos);
    CHECK(mac.over == 0 || !w->factory, "%s: %u targets found no slot (more than %u)", at, mac.over, OV_MAX);
    for (i = 0; i < (uint32_t)nms; i++) {        /* every target the model moves has its slot (idle ones need none) */
        uint32_t j, found = 0;
        for (j = 0; j < tb->n && !found; j++) {
            int t2, id;
            fw_id(&tb->s[j], &t2, &id);
            found = m_kind(tb->s[j].kind) == ms[i].kind && t2 == ms[i].t && id == ms[i].id;
        }
        CHECK(found || fabs(ms[i].off) <= ms[i].tol, "%s: the model moves kind %d track %d id %d by %.3f: no slot", at,
              ms[i].kind, ms[i].t, ms[i].id, ms[i].off);
        idle += !found;
    }
    /* the ISR's pass: bases saved, effective values written, bases back */
    for (t = 0; t < NTRK; t++)
        memcpy(p0[t], trk[t].p, sizeof p0[t]);
    memcpy(g0, song.g, sizeof g0);
    world_fx_pre();
    for (t = 0; t < NTRK; t++)
        memcpy(pe[t], trk[t].p, sizeof pe[t]);
    memcpy(ge, song.g, sizeof ge);
    memcpy(vc, wvm_cut, sizeof vc);
    memcpy(vs, wvm_shape, sizeof vs);
    world_fx_post();
    for (t = 0; t < NTRK; t++)
        CHECK(!memcmp(p0[t], trk[t].p, sizeof p0[t]), "%s: track %u's p[] not restored", at, t + 1);
    CHECK(!memcmp(g0, song.g, sizeof g0), "%s: g[] not restored", at);
    for (i = 0; i < tb->n; i++) {
        const ov_slot_t *s = &tb->s[i];
        const mslot_t *m = m_find(s);
        double tol = m ? m->tol + 1.0 / 256 : 1;
        int t2, id, edge, want, got, base;
        fw_id(s, &t2, &id);
        grid_slots++;
        if (!m) {
            CHECK(0, "%s: slot %u (kind %u, track %d, id %d) is not in the model", at, i, s->kind, t2, id);
            continue;
        }
        CHECK(fabs(s->tgt / 256.0 - m->off) <= tol, "%s: slot kind %u track %d id %d: target %.3f, the model %.3f", at,
              s->kind, t2, id, s->tgt / 256.0, m->off);
        CHECK(s->cls == m->cls, "%s: slot kind %u track %d id %d: smoothing %u, the model %d", at, s->kind, t2, id, s->cls,
              m->cls);
        CHECK(ov_cur[ov_live][i] == s->tgt, "%s: snapped slot %u not at its target", at, i);
        if (s->kind >= OV_VCUT) {
            int32_t v = (s->kind == OV_VCUT ? vc : vs)[s->part];
            CHECK(v == s->tgt, "%s: vmod part %u = %d, the target %d", at, s->part, v, s->tgt);
            CHECK(v >= -(GL_VMOD_MAX << 8) && v <= GL_VMOD_MAX << 8, "%s: vmod %d past +-%d", at, v, GL_VMOD_MAX);
            continue;
        }
        base = s->kind == OV_G ? g0[id] : p0[t2][id];
        got = s->kind == OV_G ? ge[id] : pe[t2][id];
        want = m_effective(m, base, m->off, &edge);
        grid_eff++;
        if (edge || fabs(s->tgt / 256.0 - m->off) > 1.0 / 256) {
            grid_edge++;
            CHECK(abs(got - want) <= 1, "%s: %s%d.%d: effective %d, the model %d (base %d %+.3f)", at,
                  s->kind == OV_G ? "g" : "t", t2, id, got, want, base, m->off);
        } else {
            CHECK(got == want, "%s: %s%d.%d: effective %d, the model %d (base %d %+.3f)", at, s->kind == OV_G ? "g" : "t",
                  t2, id, got, want, base, m->off);
        }
        /* never outside the descriptor, never past a hard limit the base was not already past */
        CHECK(got >= m->dmin && got <= m->dmax, "%s: %d outside %d..%d", at, got, m->dmin, m->dmax);
        if (s->kind == OV_G && id == G_DFDBK)
            CHECK(got * 230 < 32768 && got <= GL_DFDBK_MAX, "%s: delay feedback %d: coefficient %.3f", at, got, got * 230 / 32768.0);
        if (s->kind == OV_G && id == G_RSIZE)
            CHECK(25000 + got * 50 < 32768 && got <= GL_RSIZE_MAX, "%s: reverb size %d: comb gain %.3f", at, got,
                  (25000 + got * 50) / 32768.0);
        if (s->kind == OV_P && id == P_LEVEL)
            CHECK(got <= GL_LEVEL_MAX || got <= base, "%s: track %d level %d over %d", at, t2 + 1, got, GL_LEVEL_MAX);
        if (s->kind == OV_P && t2 < NPART && id >= P_E0 && M_ROLE[m_engine((uint32_t)t2)][1] == id - P_E0)
            CHECK(got <= GL_RESO_MAX || got <= base, "%s: track %d resonance %d over %d", at, t2 + 1, got, GL_RESO_MAX);
        clamps += got != (int)floor(base + m->off + 0.5) && !edge;
        if (got == m->dmax && base < m->dmax && m->ctls && w->factory) {   /* 100 % is never all-max (the factory data) */
            grid_sat++;
            CHECK(0, "%s: %s%d.%d at its maximum %d (base %d)", at, s->kind == OV_G ? "g" : "t", t2, id, got, base);
        }
    }
}
static void test_grid(void)
{
    static const uint16_t G[5] = {0, 250, 500, 750, 1000};
    uint32_t wi, a, b, c, d, combos = 0, maxslots = 0;
    for (wi = 0; wi < nworlds; wi++) {
        uint16_t pos[WF_NCTL];
        if (t_start(&worlds[wi]))
            continue;
        m_world();
        memcpy(pos, mac.pos, sizeof pos);
        for (a = 0; a < 5; a++)
            for (b = 0; b < 5; b++)
                for (c = 0; c < 5; c++)
                    for (d = 0; d < 5; d++) {
                        pos[0] = G[a], pos[1] = G[b], pos[2] = G[c], pos[3] = G[d];
                        macro_set(0, G[a]);
                        macro_set(1, G[b]);
                        macro_set(2, G[c]);
                        macro_set(3, G[d]);
                        t_publish(1);
                        check_table(&worlds[wi], pos);
                        combos++;
                        if (ovb[ov_live].n > maxslots)
                            maxslots = ovb[ov_live].n;
                    }
        printf("grid: %-14s %u slots of %u, %u mappings, %u rules\n", worlds[wi].name, ovb[ov_live].n, OV_MAX, M_nmaps,
               M_nrules);
    }
    printf("grid: %u combinations (0 .25 .5 .75 1 per macro) x every World: %u slots as the model, %u effective values "
           "(%u on a rounding edge, +-1; %u clamped), none outside its range or past a limit, %u all-max in the factory "
           "Worlds; %u idle targets with no slot; bases restored\n", combos, grid_slots, grid_eff, grid_edge, clamps, grid_sat,
           idle);
    CHECK(clamps, "no effective value was clamped: the test World extreme was not given?");
}

/* --------------------------------------------------------------- 3. curves --- */
static void test_curves(void)
{
    uint32_t wi, c, k, n = 0;
    for (wi = 0; wi < nworlds; wi++) {
        t_reset();
        CHECK(world_load(worlds[wi].b, worlds[wi].n) == WE_OK, "world_load");
        m_world();
        for (c = 0; c < WF_CURVE_CUSTOM + M_ncurves; c++) {
            if (c >= WF_NBUILTIN_CURVES && c < WF_CURVE_CUSTOM)
                continue;
            for (k = 0; k <= 32; k++) {
                int32_t u = (int32_t)(k * 4096u / 32u), y = mc_curve(c, u);
                double want = m_curve(c, u / 4096.0) * 4096;
                CHECK(fabs(y - want) <= 2.0 + (c == 2 && k < 2 ? 2 : 0), "%s: curve %u at %.3f: %d, the model %.1f",
                      worlds[wi].name, c, u / 4096.0, y, want);
                CHECK(k || !y, "curve %u not 0 at 0", c);
                CHECK(k < 32 || y == 4096, "curve %u not 1 at 1 (%d)", c, y);
                n++;
            }
        }
    }
    printf("curves: lin exp log s late and every World's LUT9, 33 points each: %u values as the model (within 2/4096)\n", n);
}

/* ---------------------------------------------------------------- 4. rules --- */
static double m_slot_off(int kind, int t, int id)   /* the model's offset of a target now (0 if none) */
{
    int i;
    for (i = 0; i < nms; i++)
        if (ms[i].kind == kind && ms[i].t == t && ms[i].id == id)
            return ms[i].off;
    return 0;
}
static void test_rules(void)
{
    uint32_t wi, r, k, q, fired = 0, owner = 0, acts = 0;
    for (wi = 0; wi < nworlds; wi++) {
        const uint8_t *p;
        int got_se = 0, got_ce = 0, got_me = 0;
        if (t_start(&worlds[wi]))
            continue;
        m_world();
        for (r = 0, p = M_rules; r < M_nrules; r++, p += WF_RULE_HDR + WF_ACT_LEN * p[3]) {
            uint32_t a = p[0] >> 4, b = p[0] & 15u, ta = 4u * p[1], tb = 4u * p[2], c;
            int all_cut = 1;
            for (k = 0; k < 4; k++) {            /* at the thresholds, just above, both at 100 %, one halfway */
                uint16_t pos[WF_NCTL];
                for (c = 0; c < WF_NCTL; c++)
                    macro_set(c, M_HOME[c]);
                macro_set(a, k == 0 ? (int32_t)ta : k == 1 ? (int32_t)ta + 8 : 1000);
                macro_set(b, k == 0 ? (int32_t)tb : k == 1 ? (int32_t)tb + 8 : k == 2 ? 1000 : (int32_t)(tb + 1000) / 2);
                t_publish(1);
                memcpy(pos, mac.pos, sizeof pos);
                CHECK((mc_strength(p) == 0) == (k == 0), "%s rule %u: strength %d at step %u", worlds[wi].name, r,
                      mc_strength(p), k);
                if (k == 3 && a != b)            /* the lesser of the two: halfway */
                    CHECK(abs(mc_strength(p) - 2048) <= 8, "%s rule %u: strength %d with one control halfway",
                          worlds[wi].name, r, mc_strength(p));
                CHECK(fabs(mc_strength(p) / 4096.0 - m_strength(p, pos)) < 2e-3, "%s rule %u: strength %.4f, the model %.4f",
                      worlds[wi].name, r, mc_strength(p) / 4096.0, m_strength(p, pos));
                check_table(&worlds[wi], pos);   /* (the firmware's whole table as the model, every rule in it) */
                for (q = 0; q < p[3]; q++)
                    all_cut &= (int8_t)p[WF_RULE_HDR + WF_ACT_LEN * q + 2] < 0;
                {   /* the firmware with and without rule r: each slot moves by what r's actions add there (the model's
                     * resolution, before the span), times the strength */
                    static uint8_t buf[512];
                    static ov_slot_t with_s[OV_MAX];
                    static mslot_t mw[160];
                    int nmw, kk;
                    uint32_t nw = ovb[ov_live].n, len = 0, i2, j;
                    const uint8_t *rp, *keep_r = M_rules;
                    uint32_t keep_n = M_nrules;
                    memcpy(with_s, ovb[ov_live].s, sizeof with_s);
                    m_eval(pos);
                    nmw = nms;
                    memcpy(mw, ms, sizeof mw);
                    for (i2 = 0, rp = keep_r; i2 < keep_n; i2++, rp += WF_RULE_HDR + WF_ACT_LEN * rp[3])
                        if (i2 != r) {
                            memcpy(buf + len, rp, WF_RULE_HDR + WF_ACT_LEN * rp[3]);
                            len += WF_RULE_HDR + WF_ACT_LEN * rp[3];
                        }
                    M_rules = buf, M_nrules = keep_n - 1u;
                    m_eval(pos);
                    mac.rules = buf, mac.nrules = (uint8_t)(keep_n - 1u);
                    t_publish(1);
                    for (i2 = 0; i2 < ovb[ov_live].n; i2++) {
                        const ov_slot_t *so = &ovb[ov_live].s[i2];
                        const mslot_t *m = m_find(so);
                        double span = so->kind >= OV_VCUT ? GL_VMOD_MAX : so->hi - so->lo, want, got;
                        for (j = 0; j < nw && !(with_s[j].kind == so->kind && with_s[j].ptr == so->ptr &&
                                                with_s[j].part == so->part); j++)
                            ;
                        if (j == nw || !m)
                            continue;
                        for (kk = 0; kk < nmw && !(mw[kk].kind == m->kind && mw[kk].t == m->t && mw[kk].id == m->id); kk++)
                            ;
                        want = (kk < nmw ? mw[kk].raw : 0) - m->raw;
                        got = (with_s[j].tgt - so->tgt) / 256.0;
                        if (fabs(with_s[j].tgt / 256.0) < span - 1e-9 && fabs(so->tgt / 256.0) < span - 1e-9)
                            CHECK(fabs(got - want) <= 128 * 4.0 / 4096 + 3.0 / 256, "%s rule %u: slot kind %u moved %.3f, "
                                  "its actions %.3f", worlds[wi].name, r, so->kind, got, want);
                        if (k == 0)
                            CHECK(with_s[j].tgt == so->tgt, "%s rule %u acts at its thresholds", worlds[wi].name, r);
                        acts += fabs(want) > 0;
                    }
                    M_rules = keep_r, M_nrules = keep_n;
                    mac.rules = keep_r, mac.nrules = (uint8_t)keep_n;
                    m_eval(pos);
                }
            }
            fired++;
            if (b != a && (b == 3 || a == 3) && all_cut) {   /* the owner's three: X + ENERGY, cuts only */
                uint32_t x = a == 3 ? b : a;
                got_se |= x == 2, got_ce |= x == 0, got_me |= x == 1;
            }
        }
        if (worlds[wi].factory) {
            CHECK(got_se && got_ce && got_me, "%s: the cross-macro rules SPACE+ENERGY %d, COLOR+ENERGY %d, MOTION+ENERGY %d "
                  "(each cutting only)", worlds[wi].name, got_se, got_ce, got_me);
            owner += got_se + got_ce + got_me;
        }
    }
    printf("rules: %u rules silent at their thresholds, acting just above them and in full at 100 %% (%u action checks "
           "against the model); the factory Worlds' %u cross-macro rules (SPACE+ENERGY, COLOR+ENERGY, MOTION+ENERGY) "
           "only cut\n", fired, acts, owner);
}

/* ------------------------------------------------------------- 5. smoothing --- */
/* the classes' one-pole coefficients from their time constants (design 5.5: 10, 60, 250 ms at the block rate) */
static int32_t k_of(uint32_t cls)
{
    static const double TAU[3] = {0.010, 0.060, 0.250};
    return cls >= 3 ? 65536 : (int32_t)(65536 * (1 - exp(-(double)CTL / FS / TAU[cls])) * 1.01 + 1);
}
static void test_smoothing(void)
{
    uint32_t wi, blk, i, ramps = 0, homes = 0, worst_fast = 0, worst_slow = 0;
    for (i = 0; i < 3; i++)                      /* the firmware's coefficients are the time constants' */
        CHECK(OV_K[i] <= k_of(i) && OV_K[i] * 1.03 >= k_of(i) - 1, "class %u: k %d, its time constant gives %d", i, OV_K[i],
              k_of(i));
    for (wi = 0; wi < nworlds; wi++) {
        const ov_tab_t *tb;
        int32_t prev[OV_MAX];
        int16_t pv[OV_MAX];
        uint32_t done[OV_MAX], n, moved = 0;
        if (t_start(&worlds[wi]))
            continue;
        world_fx_pre();                          /* (the load's table taken: the first block) */
        world_fx_post();
        /* a World starts at its targets: the first block already has them */
        tb = &ovb[ov_live];                      /* (at home, as most defaults are, a target needs no slot) */
        for (i = 0; i < tb->n; i++)
            CHECK(ov_cur[ov_live][i] == tb->s[i].tgt, "%s: slot %u not at its target after the load", worlds[wi].name, i);
        for (i = 0; i < WF_NCTL; i++)            /* every control to its other end */
            macro_set(i, mac.pos[i] >= 500 ? 0 : 1000);
        mac.dirty = 1;
        CHECK(macro_eval() == 0, "eval");
        world_fx_pre();                          /* (the take, and the first step) */
        world_fx_post();
        tb = &ovb[ov_live];
        n = tb->n;
        for (i = 0; i < n; i++) {
            prev[i] = ov_cur[ov_live][i];
            done[i] = prev[i] == tb->s[i].tgt ? 1 : 0;
            pv[i] = 0;
        }
        for (blk = 2; blk <= 6000; blk++) {
            world_fx_pre();
            for (i = 0; i < n; i++) {
                const ov_slot_t *s = &tb->s[i];
                int32_t c = ov_cur[ov_live][i], d = s->tgt - prev[i], lim, v;
                lim = s->cls >= WF_CLASS_STEPPED ? abs(d) : abs((d * k_of(s->cls)) >> 16);
                lim = lim < 1 ? 1 : lim;
                CHECK(abs(c - prev[i]) <= lim, "%s: slot %u moved %d in a block (its class allows %d)", worlds[wi].name, i,
                      c - prev[i], lim);
                CHECK((d >= 0 && c >= prev[i] && c <= s->tgt) || (d <= 0 && c <= prev[i] && c >= s->tgt),
                      "%s: slot %u passed its target", worlds[wi].name, i);
                if (s->kind < OV_VCUT) {
                    v = *s->ptr;                 /* (inside the window: the effective value) */
                    if (blk > 2)
                        CHECK(abs(v - pv[i]) <= lim / 256 + 1, "%s: slot %u's value jumped %d", worlds[wi].name, i, v - pv[i]);
                    pv[i] = (int16_t)v;
                }
                moved += c != prev[i];
                if (!done[i] && c == s->tgt) {
                    done[i] = blk;
                    if (s->cls == 0 && blk > worst_fast)
                        worst_fast = blk;
                    if (s->cls == 2 && blk > worst_slow)
                        worst_slow = blk;
                    CHECK(s->cls != 0 || blk <= 200, "%s: a fast slot took %u blocks", worlds[wi].name, blk);
                    CHECK(s->cls != 1 || blk <= 1200, "%s: a medium slot took %u blocks", worlds[wi].name, blk);
                    CHECK(s->cls != 2 || blk <= 5000, "%s: a slow slot took %u blocks", worlds[wi].name, blk);
                }
                prev[i] = c;
            }
            world_fx_post();
        }
        for (i = 0; i < n; i++) {
            CHECK(done[i], "%s: slot %u never reached its target", worlds[wi].name, i);
            ramps += done[i] > 2;
        }
        CHECK(moved || !mac.nmaps, "%s: nothing ramped", worlds[wi].name);
        {   /* and home again: a target at home keeps its slot until its offset is back to 0, so nothing jumps */
            int16_t *ptr[OV_MAX];
            int32_t prevv[OV_MAX], base[OV_MAX];
            uint32_t np = 0, k, moving = 0;
            world_fx_pre();
            for (i = 0; i < tb->n; i++)
                if (tb->s[i].kind < OV_VCUT && tb->s[i].cls != WF_CLASS_STEPPED) {   /* (an enum steps) */
                    ptr[np] = tb->s[i].ptr;
                    prevv[np++] = *tb->s[i].ptr;
                }
            world_fx_post();
            for (k = 0; k < np; k++)
                base[k] = *ptr[k];
            for (i = 0; i < WF_NCTL; i++)
                macro_set(i, M_HOME[i]);
            mac.dirty = 1;
            CHECK(macro_eval() == 0, "eval home");
            for (blk = 0; blk < 6000; blk++) {
                world_fx_pre();
                for (k = 0; k < np; k++) {       /* (at most the fastest class's step on what is left, + rounding) */
                    int32_t v = *ptr[k];
                    CHECK(abs(v - prevv[k]) <= abs(prevv[k] - base[k]) * 0.072 + 1.5,
                          "%s: a target jumped %d on its way home (%d left)", worlds[wi].name, v - prevv[k],
                          prevv[k] - base[k]);
                    prevv[k] = v;
                }
                world_fx_post();
                if (!(blk % 22) && ov_pub == ov_seen) {   /* (the main loop's passes: idle targets drop out) */
                    mac.dirty = 1;
                    macro_eval();
                }
            }
            m_world();                           /* at home only what the model still moves keeps a slot (a rule */
            m_eval(mac.pos);                     /* whose threshold lies below home acts there) */
            for (k = 0; k < (uint32_t)nms; k++)
                moving += fabs(ms[k].off) > ms[k].tol;
            CHECK(ovb[ov_live].n == moving, "%s: %u slots left at home, the model moves %u targets", worlds[wi].name,
                  ovb[ov_live].n, moving);
            homes += np;
        }
    }
    printf("smoothing: every control stepped to its other end: %u slots ramped, each block within its class's step, "
           "never past the target; and home again: %u targets ramped back, none jumped, then only what still moves kept "
           "a slot; settled: fast "
           "within %u blocks, slow within %u (1378 a second); a load snaps\n", ramps, homes,
           worst_fast, worst_slow);
    CHECK(worst_fast && worst_slow, "no fast or no slow slot ramped (the test World full has both)");
}

/* --------------------------------------------- 6. the real mix restores the bases --- */
/* p[0..n) are the bases want, except values a commit glides (macro.c wgl, Phase 11): on their way to want */
static int bases_ok(int16_t *p, const int16_t *want, uint32_t n)
{
    uint32_t i, k;
    for (i = 0; i < n; i++)
        if (p[i] != want[i]) {
            for (k = 0; k < wgl_n && (wgl[k].p != &p[i] || wgl[k].at != p[i] || wgl[k].to != want[i]); k++)
                ;
            if (k == wgl_n)
                return 0;
        }
    return 1;
}
static void test_mix(void)
{
    uint32_t wi, b, t, bad = 0, commits = 0, blocks = 0;
    for (wi = 0; wi < nworlds; wi++) {
        int16_t base[NTRK][P_COUNT], gbase[G_COUNT];
        uint32_t asked = 0;
        if (t_start(&worlds[wi]))
            continue;
        t_block();
        for (t = 0; t < NTRK; t++)
            memcpy(base[t], trk[t].p, sizeof base[t]);
        memcpy(gbase, song.g, sizeof gbase);
        transport_req = 1;
        for (b = 0; b < 6u * (4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL); b++) {   /* (a 4-bar transition) */
            if (!(b % 7))                       /* a hand on the knobs */
                macro_set(t_rnd() % 4u, (int32_t)(t_rnd() % 1001u));
            if (!(b % 22))
                macro_service();
            if (!asked && clk_beat >= 5) {      /* a scene on the next bar: committed inside the overlay's window */
                CHECK(world_request((wrt.scene + 1u) % 4u, wrt.var) == WE_OK, "world_request");
                asked = 1;
            }
            t_block();
            blocks++;
            if (wst.st == WST_APPLIED) {         /* (committed in this block: the staged values are the new bases) */
                for (t = 0; t < NTRK; t++)
                    memcpy(base[t], wst.p[t], sizeof base[t]);
                for (t = 0; t < sizeof WB_GWHITE; t++)
                    gbase[WB_GWHITE[t]] = wst.g[WB_GWHITE[t]];
                wst.st = WST_FREE;
                commits++;
            }
            for (t = 0; t < NTRK; t++)
                bad += !bases_ok(trk[t].p, base[t], P_COUNT);
            bad += !bases_ok(song.g, gbase, G_COUNT);
            bad += ov_in != 0;
        }
        transport_req = 2;
        t_blocks(4);
    }
    CHECK(!bad, "%u blocks left an effective value in p[] / g[]", bad);
    CHECK(commits >= nworlds, "%u scene commits for %u Worlds", commits, nworlds);
    for (wi = 0; wi < nworlds; wi++) {          /* inside the window, a commit's block: the overlay back in on the new bases */
        uint32_t in = 0, done = 0;
        if (t_start(&worlds[wi]))
            continue;
        macro_set(0, 900), macro_set(2, 100);
        t_publish(1);
        transport_req = 1;
        t_block();
        CHECK(world_request((wrt.scene + 1u) % 4u, wrt.var) == WE_OK, "world_request");
        for (b = 0; b < 6u * (4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL) && !done; b++) {
            const ov_tab_t *tb;
            uint32_t i;
            world_fx_pre();
            events_block(CTL);                   /* (mix_block's first half: the commit lands here, on the bar) */
            tb = &ovb[ov_live];
            done = wst.st == WST_APPLIED;
            in = 0;
            for (i = 0; i < tb->n; i++)          /* (a slot at offset 0 is not written: its base is its value) */
                if (tb->s[i].kind < OV_VCUT && ov_cur[ov_live][i])
                    in += *tb->s[i].ptr == ov_effective(&tb->s[i], ov_saved[i], ov_cur[ov_live][i]) && ov_in ? 0u : 1u;
            CHECK(!done || (ov_in && !in), "%s: after the commit the overlay is not in (%u slots)", worlds[wi].name, in);
            world_fx_post();
            if (done)
                for (t = 0; t < NTRK; t++)
                    CHECK(bases_ok(trk[t].p, wst.p[t], P_COUNT), "%s: track %u after the commit's block",
                          worlds[wi].name, t + 1);
        }
        CHECK(done, "%s: no commit", worlds[wi].name);
    }
    printf("mix: %u blocks of every World playing, the macros moving, %u scenes committed on the bar inside the overlay: "
           "after each block p[] and g[] are the staged bases exactly\n", blocks, commits);
}

/* ------------------------------------------------------------ 7. ENERGY bands --- */
static const uint8_t *e_rec(uint32_t i)          /* the World's ENERGY table i, from the blob */
{
    const uint8_t *p = wctx.b + wctx.off[WF_S_ENERGY];
    while (i--)
        p += WF_ENERGY_HDR + WF_BAND_LEN * p[2];
    return p;
}
static uint32_t e_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t g_byte(uint32_t o, uint32_t dflt)
{
    return wctx.have >> WF_S_GUARD & 1u && wctx.b[wctx.off[WF_S_GUARD] + o] != WF_NONE ? wctx.b[wctx.off[WF_S_GUARD] + o] : dflt;
}
static void test_energy(void)
{
    uint32_t wi, s, k, i, nb = 0, beats = 0, bars = 0, hits = 0, rests = 0, fills = 0, wig = 0;
    for (wi = 0; wi < nworlds; wi++) {
        for (s = 0; s < WF_NSCENE; s++) {
            const uint8_t *sc, *e;
            uint32_t n, mute_bars, dens_bar, min_bars, prev_db, prev_mb, prev_tgt, chg_bar = 0, nchg = 0, bar_no = 0;
            uint32_t last_beat;
            int32_t bias;
            if (t_start(&worlds[wi]))
                continue;
            sc = wctx.b + wctx.scene[s];
            if (sc[13] == WF_NONE)
                continue;
            e = e_rec(sc[13]);
            n = e[2];
            bias = (int8_t)wctx.b[wctx.var[wrt.var] + WF_LABEL_LEN];
            mute_bars = g_byte(WF_G_MUTECHG, 1);
            dens_bar = g_byte(WF_G_DENSCHG, 0);
            min_bars = g_byte(WF_G_BANDBARS, 1);
            CHECK(world_apply(s, wrt.var) == WE_OK, "world_apply");
            /* stopped: each band at once (its start + 30), the masks the band's, the keys track never muted */
            for (k = 0; k < n; k++) {
                const uint8_t *bd = e + WF_ENERGY_HDR + WF_BAND_LEN * k;
                macro_set(3, clamp(4 * bd[0] + 30 - 4 * bias, 0, 1000));
                t_events();
                if (arr_pos() < 4u * bd[0] + 20u)
                    continue;                    /* (the bias keeps it below: another band) */
                CHECK(arr.mb == k && arr.db == k, "%s %c: band %u not applied while stopped (%u %u)", worlds[wi].name,
                      'A' + s, k, arr.mb, arr.db);
                CHECK(wrt.mute == (~bd[1] & 15u & ~(1u << wrt.keys_trk)), "%s %c band %u: mute %x", worlds[wi].name, 'A' + s,
                      k, wrt.mute);
                CHECK(arr.lanes == e_u16(bd + 2) && arr.dens == e_u16(bd + 4), "%s: band masks", worlds[wi].name);
                nb++;
            }
            /* hysteresis: resting on each edge (+-15) never changes the band; +-25 does */
            for (k = 1; k < n; k++) {
                uint32_t edge = 4u * e[WF_ENERGY_HDR + WF_BAND_LEN * k] - 4 * bias, from;
                if (edge < 30u || edge > 970u)
                    continue;
                macro_set(3, (int32_t)edge - 40);
                t_events();
                from = arr.sel;
                for (i = 0; i < 200; i++) {
                    macro_set(3, (int32_t)edge - 15 + (int32_t)(t_rnd() % 31u));
                    t_events();
                    CHECK(arr.sel == from, "%s %c: the band flapped at its edge (%u -> %u at %u)", worlds[wi].name, 'A' + s,
                          from, arr.sel, arr_pos());
                    wig++;
                }
                macro_set(3, (int32_t)edge + 25);
                t_events();
                CHECK(arr.sel == k, "%s %c: +25 past the edge of band %u: band %u", worlds[wi].name, 'A' + s, k, arr.sel);
                macro_set(3, (int32_t)edge - 15);
                t_events();
                CHECK(arr.sel == k, "%s %c: hysteresis down", worlds[wi].name, 'A' + s);
                macro_set(3, (int32_t)edge - 25);
                t_events();
                CHECK(arr.sel == k - 1u, "%s %c: -25 below the edge: band %u", worlds[wi].name, 'A' + s, arr.sel);
            }
            /* playing: random hands on ENERGY for 16 bars; changes only on their beats and bars */
            macro_set(3, 0);
            t_events();
            transport_req = 1;
            t_events();
            prev_db = arr.db, prev_mb = arr.mb, prev_tgt = arr.tgt;
            last_beat = arr.beat;                /* (the clock as the last block's arr_block saw it) */
            for (i = 0; bar_no < 16u && i < 200000u; i++) {
                uint32_t b0 = clk_beat, newbeat, newbar, idx, t;
                if (!(t_rnd() % 300u))
                    macro_set(3, (int32_t)(t_rnd() % 1001u));
                drums.hits = 0;
                t_events();
                newbeat = b0 != last_beat;       /* (this block began on a beat: events_block advances the clock at
                                                  * its end, so the block's own beat is the one before it ran) */
                newbar = newbeat && !(b0 & 3u);
                if (newbar)
                    bar_no++;
                if (arr.tgt != prev_tgt) {
                    CHECK(newbeat, "%s %c: a change began between beats", worlds[wi].name, 'A' + s);
                    if (nchg)
                        CHECK(bar_no - chg_bar >= min_bars, "%s %c: two changes %u bars apart (min %u)", worlds[wi].name,
                              'A' + s, bar_no - chg_bar, min_bars);
                    chg_bar = bar_no;
                    nchg++;
                }
                if (arr.db != prev_db) {
                    CHECK(newbeat && (!dens_bar || newbar), "%s %c: the masks changed off their beat / bar", worlds[wi].name,
                          'A' + s);
                    beats++;
                }
                if (arr.mb != prev_mb) {
                    CHECK(newbar && !((b0 >> 2) % mute_bars), "%s %c: the layers changed off their bar",
                          worlds[wi].name, 'A' + s);
                    bars++;
                }
                CHECK(!(wrt.mute >> wrt.keys_trk & 1u), "%s: the Smart Keys track muted", worlds[wi].name);
                /* what played obeys band db: drum hits on its lanes and density, a masked NOTE rests */
                idx = TDRUM->seq_idx;
                if (drums.hits) {
                    const uint8_t *bd = e + WF_ENERGY_HDR + WF_BAND_LEN * arr.db;
                    uint32_t ok = e_u16(bd + 2);
                    if (!(e_u16(bd + 4) >> (idx & 15u) & 1u))
                        ok &= ~e_u16(e);
                    CHECK(!(drums.hits & ~ok), "%s %c band %u step %u: lanes %04x hit, allowed %04x", worlds[wi].name,
                          'A' + s, arr.db, idx, drums.hits, ok);
                    CHECK(!(wrt.mute >> TRK_DRUM & 1u), "%s: a muted drum track hit", worlds[wi].name);
                    hits++;
                }
                for (t = 0; t < NPART; t++) {
                    const step_t *st = &trk[t].step[trk[t].seq_idx % NSTEP];
                    const uint8_t *bd = e + WF_ENERGY_HDR + WF_BAND_LEN * arr.db;
                    if (t == wrt.keys_trk || trk[t].seq_abs == SEQ_NONE || !newbeat)
                        continue;
                    if (st->time == ST_NOTE && !(e_u16(bd + 6 + 2 * t) >> (trk[t].seq_idx & 15u) & 1u)) {
                        CHECK(!trk[t].seq_n, "%s %c: track %u's masked NOTE at step %u sounded", worlds[wi].name, 'A' + s,
                              t + 1, trk[t].seq_idx);
                        rests++;
                    }
                }
                fills += arr.fill_now && newbar;
                last_beat = b0;
                prev_db = arr.db, prev_mb = arr.mb, prev_tgt = arr.tgt;
            }
            transport_req = 2;
            t_events(), t_events();
        }
    }
    CHECK(nb > 10 && beats > 10 && bars > 10, "too few band changes seen (%u %u %u)", nb, beats, bars);
    t_start(&worlds[0]);                         /* a band without the Smart Keys track (worldc refuses one): kept */
    arr.et.n = 1;
    arr.et.flags[0] = 0;
    arr_layers(0);
    CHECK(wrt.mute == (15u & ~(1u << wrt.keys_trk)), "a band without layers muted %x (the keys track %u)", wrt.mute,
          wrt.keys_trk);
    printf("energy: %u bands applied at once while stopped; %u edge wiggles (+-15) never flapped, +-25 moved; playing: %u "
           "mask changes each on its beat (or bar), %u layer changes each on its bar, min_band_bars kept; %u blocks of "
           "drum hits all on allowed lanes and density steps, %u masked NOTEs rested, %u fill bars\n", nb, wig, beats, bars,
           hits, rests, fills);
}

/* -------------------------------------------- 8. the vmod offsets reach the engines (H3) --- */
static uint32_t t_active_part(uint32_t part)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[part].v[i].active;
    return n;
}
static double brightness(uint32_t part, int32_t color)   /* a held chord on part at COLOR color: rms(x[n] - x[n-1]) / rms(x) */
{
    int32_t o[2 * CTL], prev = 0;
    double d = 0, e = 0;
    uint32_t i, b;
    macro_set(0, color);
    t_publish(1);
    for (i = 0; i < 3; i++)
        trk_note_on(&trk[part], 57 + 4 * i, 100);
    for (b = 0; b < 400; b++) {
        mix_block(o, CTL);
        for (i = 0; b >= 100 && i < CTL; i++) {
            d += ((double)o[2 * i] - prev) * ((double)o[2 * i] - prev);
            e += (double)o[2 * i] * o[2 * i];
            prev = o[2 * i];
        }
        if (b < 100)
            prev = o[2 * CTL - 2];
    }
    for (i = 0; i < 3; i++)
        trk_note_off(&trk[part], 57 + 4 * i);
    for (b = 0; b < 2000 && t_active_part(part); b++)
        t_block();
    return e > 0 ? sqrt(d / e) : 0;
}
static void test_vmod(void)
{
    uint32_t wi, i, n = 0;
    for (wi = 0; wi < nworlds; wi++) {
        const ov_tab_t *tb;
        uint32_t part = WF_NONE, t;
        if (t_start(&worlds[wi]))
            continue;
        t_blocks(4);                             /* (the World switch's panic, the engine fades) */
        macro_set(0, 1000);
        t_publish(1);
        tb = &ovb[ov_live];
        for (i = 0; i < tb->n && part == WF_NONE; i++)   /* a part COLOR brightens through ~bright */
            if (tb->s[i].kind == OV_VCUT && tb->s[i].tgt > 4 * 256 && tb->s[i].part != wrt.keys_trk)
                part = tb->s[i].part;
        if (part == WF_NONE)
            continue;
        for (t = 0; t < NTRK; t++)               /* (that part alone and dry, nothing sequenced) */
            trk[t].p[P_MUTE] = t != part;
        trk[part].p[P_CHOR] = trk[part].p[P_DLY] = trk[part].p[P_REV] = 0;
        trk[part].p[P_ATK] = trk[part].p[P_REL] = 0;   /* (a steady tone: no slow attack, no tail into the next) */
        trk[part].p[P_SUS] = 127;
        trk[part].p[P_ED_FLT] = trk[part].p[P_LD_FLT] = trk[part].p[P_LD_SHP] = trk[part].p[P_LD_AMP] = 0;
        for (t = 0; t < NTRK; t++)
            steps_clear(&trk[t]);
        {   /* home against 100 % (a nearly closed filter is no measure: the engines' fixed-point filters turn gritty
             * there, so the dark end is not compared) */
            double home = brightness(part, 500), bright = brightness(part, 1000);
            CHECK(bright > home * 1.15, "%s: part %u: COLOR 1 is not brighter than its home (%.4f, %.4f)", worlds[wi].name,
                  part + 1, bright, home);
            printf("vmod: %-14s part %u held, dry: brightness (rms of the slope / rms) %.4f at home, %.4f at COLOR 1\n",
                   worlds[wi].name, part + 1, home, bright);
            n++;
        }
    }
    CHECK(n >= 3, "only %u Worlds brighten a part through ~bright", n);
}

/* ---------------------------------------------- 9. engines, reloads, overflow --- */
static void test_engines(void)
{
    uint32_t wi, t, i, tried = 0;
    for (wi = 0; wi < nworlds; wi++) {
        if (t_start(&worlds[wi]))
            continue;
        for (i = 0; i < 4; i++)                  /* (every target moving: every role has its slot) */
            macro_set(i, 1000);
        t_publish(1);
        for (t = 0; t < NPART; t++) {          /* another engine (ADV_WORLD, later): the roles follow it */
            const ov_tab_t *tb;
            uint32_t e = (trk[t].eng_req + 1u) % 9u, found = 0;
            const uint8_t *p = mac.maps;
            for (i = 0; i < mac.nmaps; i++, p += WF_MAP_LEN)
                found |= (p[1] >> 5) == WF_K_ROLE && (p[1] >> t & 1u);
            if (!found)
                continue;
            trk[t].eng_req = (uint8_t)e;
            ov_blk += MC_BLOCKS;
            if (ov_pub != ov_seen)
                world_fx_pre(), world_fx_post();
            macro_service();
            CHECK(mac.eng[t] == e, "%s: track %u's engine change not seen", worlds[wi].name, t + 1);
            world_fx_pre();
            world_fx_post();
            tb = &ovb[ov_live];
            for (i = 0; i < tb->n; i++)
                if (tb->s[i].kind == OV_P && tb->s[i].part == t && tb->s[i].ptr - trk[t].p >= P_E0) {
                    uint32_t slot = (uint32_t)(tb->s[i].ptr - trk[t].p) - P_E0, r, is = 0;
                    const uint8_t *q = mac.maps;
                    uint32_t j;
                    for (r = 0; r < WF_NEROLES; r++)
                        is |= M_ROLE[e][r] == slot;
                    for (j = 0; j < mac.nmaps; j++, q += WF_MAP_LEN)   /* (or a mapping names it directly) */
                        is |= (q[1] >> 5) == WF_K_PARAM && q[2] == P_E0 + slot;
                    CHECK(is, "%s: track %u, engine %u: slot E%u is none of its roles", worlds[wi].name, t + 1, e, slot);
                }
            tried++;
        }
        /* a hot reload of the same World keeps the positions */
        macro_set(0, 123);
        macro_set(3, 877);
        CHECK(world_hot_reload(worlds[wi].b, worlds[wi].n) == WE_OK, "hot reload");
        CHECK(mac.pos[0] == 123 && mac.pos[3] == 877 && mac.dirty, "%s: the hot reload lost the positions",
              worlds[wi].name);
    }
    printf("engines: %u tracks given another engine: their roles re-resolved; a hot reload keeps the positions\n", tried);
    {   /* more targets than slots: the first 48 kept, the rest counted, nothing written past the tables */
        static const uint8_t PIDS[24] = {P_LEVEL, P_ATK, P_DEC, P_SUS, P_REL, P_ED_FLT, P_ED_PIT, P_ED_SHP, P_LRATE,
                                         P_LPHASE, P_LFADE, P_LD_PIT, P_LD_FLT, P_LD_SHP, P_LD_AMP, P_SGATE, P_DIST,
                                         P_CHOR, P_DLY, P_REV, P_GLIDE, P_PAN, P_DETUNE, P_SLDEPTH};
        uint8_t maps[24 * WF_MAP_LEN];
        int16_t p0[NTRK][P_COUNT];
        for (i = 0; i < 24; i++) {
            uint8_t *m = maps + WF_MAP_LEN * i;
            m[0] = 0, m[1] = WF_K_PARAM << 5 | WF_TMASK_SYNTH, m[2] = PIDS[i], m[3] = WF_CLASS_DEFAULT << 6;
            m[4] = (uint8_t)-10, m[5] = 10;
        }
        t_start(&worlds[0]);
        macro_load(maps, 24, 0, 0, 0, 0, wctx.b + wctx.off[WF_S_DEFAULTS], 1);
        macro_set(0, 1000);
        t_publish(1);
        CHECK(ovb[ov_live].n == OV_MAX && mac.over == 3 * 24 - OV_MAX, "overflow: %u slots, %u over", ovb[ov_live].n,
              mac.over);
        for (t = 0; t < NTRK; t++)
            memcpy(p0[t], trk[t].p, sizeof p0[t]);
        transport_req = 1;
        t_blocks(50);
        for (t = 0; t < NTRK; t++)
            CHECK(!memcmp(p0[t], trk[t].p, sizeof p0[t]), "overflow: track %u not restored", t + 1);
        printf("overflow: 72 targets for 48 slots: the first 48 applied and restored, %u counted as over\n", mac.over);
    }
}

/* ------------------------------------------------------------------ --cost --- */
static uint64_t instr_now(void)
{
#ifdef __APPLE__
    struct rusage_info_v4 ri;
    if (!proc_pid_rusage(getpid(), RUSAGE_INFO_V4, (rusage_info_t *)&ri))
        return ri.ri_instructions;
#endif
    return 0;
}
static int cost(void)
{
    uint32_t wi, i, N = 20000;
    if (!instr_now()) {
        printf("cost: no instruction counter on this host\n");
        return 0;
    }
    for (wi = 0; wi < nworlds; wi++) {
        uint64_t c0, ramp, settled, eval, home;
        uint32_t n;
        if (t_start(&worlds[wi]))
            continue;
        gu.ncombos = t_ncombos;                  /* (the guard whole: its ranges and combinations, guard.c) */
        t_blocks(4);
        c0 = instr_now();                        /* at the World's defaults (mostly home: offsets 0) */
        for (i = 0; i < N; i++)
            world_fx_pre(), world_fx_post();
        home = instr_now() - c0;
        for (i = 0; i < 4; i++)
            macro_set(i, 900);
        t_publish(0);
        n = ovb[ov_live].n;
        c0 = instr_now();                        /* every slot ramping: a new target every 400 blocks */
        for (i = 0; i < N; i++) {
            if (!(i % 400)) {
                macro_set(i / 400 % 4, i / 400 % 2 ? 100 : 900);
                mac.dirty = 1;
                macro_eval();
            }
            world_fx_pre();
            world_fx_post();
        }
        ramp = instr_now() - c0;
        for (i = 0; i < 6000; i++)               /* (settling) */
            world_fx_pre(), world_fx_post();
        c0 = instr_now();
        for (i = 0; i < N; i++)
            world_fx_pre(), world_fx_post();
        settled = instr_now() - c0;
        c0 = instr_now();
        for (i = 0; i < 1000; i++) {
            mac.dirty = 1;
            if (ov_pub != ov_seen)
                world_fx_pre(), world_fx_post();
            macro_eval();
        }
        eval = instr_now() - c0;
        printf("cost: %-14s %2u slots: the overlay %.1f instructions / sample at the defaults, %.1f with every slot "
               "ramping, %.1f settled away from home; macro_eval %.0f instructions (main loop)\n", worlds[wi].name, n,
               (double)home / ((double)N * CTL), (double)ramp / ((double)N * CTL), (double)settled / ((double)N * CTL),
               (double)eval / 1000.0);
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint32_t i;
    int do_cost = 0;
    for (i = 0; i < WORLD_NFACTORY && nworlds < 32; i++) {
        const uint8_t *b;
        uint32_t n;
        world_factory(i, &b, &n);
        worlds[nworlds].b = malloc(n);
        memcpy(worlds[nworlds].b, b, n);
        worlds[nworlds].n = n;
        worlds[nworlds].factory = 1;
        t_reset();
        world_load(b, n);
        snprintf(worlds[nworlds].name, sizeof worlds[nworlds].name, "%s", world_name());
        nworlds++;
    }
    for (i = 1; i < (uint32_t)argc && nworlds < 32; i++) {
        if (!strcmp(argv[i], "--cost")) {
            do_cost = 1;
            continue;
        }
        worlds[nworlds].b = t_read(argv[i], &worlds[nworlds].n);
        snprintf(worlds[nworlds].name, sizeof worlds[nworlds].name, "%s",
                 strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i]);
        nworlds++;
    }
    if (do_cost)
        return cost();
    {
        void (*const T[])(void) = {test_inert, test_curves, test_grid, test_rules, test_smoothing, test_mix, test_energy,
                                   test_vmod, test_engines};
        for (i = 0; i < sizeof T / sizeof T[0]; i++) {
            clock_t c0 = clock();
            T[i]();
            if (getenv("MT_TIME"))
                printf("  (%.2f s)\n", (double)(clock() - c0) / CLOCKS_PER_SEC);
        }
    }
    printf("macro test: %u checks, %s\n", t_checks, t_fails ? "FAILED" : "all passed");
    return t_fails ? 1 : 0;
}
