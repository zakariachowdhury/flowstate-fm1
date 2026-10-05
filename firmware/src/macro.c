/* SPDX-License-Identifier: GPL-3.0-only */
/* The performance macros (PLAY MODE, docs/design/play-mode-architecture.md 5, decision D1): COLOR (dark - bright),
 * MOTION (still - alive), SPACE (close - huge) and ENERGY (sparse - intense), four knobs with fixed meanings whose
 * hidden mappings each World defines (its MAPS, CURVES and RULES sections), and the slots of the later controls
 * (SOUND SHAPE, MOVEMENT, LIVE FX: ids 4..15, Phase 13 adds their built-in mappings).
 *
 * Main loop. A control is a position 0..1000 with a home (500 for the four macros) where it moves nothing: the
 * World's DEFAULTS set them on load. On a change macro_eval turns the positions into a target table:
 *   - every mapping's offset: from home toward its min (below) or max (above), through its curve (lin exp log s
 *     late, or a CURVES LUT9), so that 100 % is the largest value the World's author chose, never "every
 *     parameter at its maximum";
 *   - its targets: a track parameter (one or more tracks), an engine role (@BRIGHT, @RESO ...) resolved through the
 *     role table for the engine each track runs, ~bright / ~shape (the per-part vmod offsets, H3), a global;
 *   - offsets summed per target (a slot), then the cross-macro rules: each action adds its amount times the rule's
 *     strength, 0 at the thresholds and 1 when both controls are at the end. Gain compensation is part of this
 *     data (design 5.6): the World's level mappings on ENERGY and SPACE and the rules' level cuts;
 *   - a target has a slot only while it moves (a non-zero offset, or still ramping home): at home it costs nothing;
 *   - each slot's range: its descriptor, the hard limits of guard_limits.h (feedback < 1, resonance, level) and
 *     the World's GUARD caps and ranges (guard.c guard_range); then the guard's rules over the whole table
 *     (guard_sound: the sound combinations, the distorted tracks); its smoothing: the slowest class of the mappings
 *     on it (stepped wins, an enum always steps).
 * The table goes to the audio ISR through a double buffer and a publication count (world_rt.h ovb, ov_pub): the
 * main loop writes the table the ISR does not use, and only after the ISR has taken the one before.
 *
 * Audio ISR. world_fx_pre (fx.c mix_block, H4, before events_block) moves each slot's offset toward its target, one
 * pole per smoothing class (fast 10 ms, medium 60 ms, slow 250 ms, stepped), saves the parameter's base value and
 * writes clamp(base + offset); world_fx_post (H5, after djf_process) writes every base value back. So for the whole
 * block, sequencer included (gate, glide), the engines and effects read the effective values, while the main loop
 * (UI, editor, autosave, the stage builder) only ever sees the authored ones. A commit inside the block (world.c,
 * a scene on the bar) takes the overlay out, writes the new bases and puts it back in. Range rule: a slot never
 * goes past its range, and never moves a base that already is past it further away (a cap never changes the
 * authored sound). While the CPU guard holds (guard.c), the costly slots ramp back to their base.
 *
 *   macro_load(...)          world.c: the loaded World's mappings (and its DEFAULTS positions, keep = 0)
 *   macro_set(c, pos), macro_pos(c)    main loop: a control
 *   macro_service()          main loop, every pass: macro_eval when something changed, at most every MC_BLOCKS
 *   macro_eval()             main loop: the target table, published (1: the ISR has not taken the last one yet)
 *   ov_reset()               IRQs off (world.c, a World switch or unload): no overlay until the next table
 *   world_fx_pre / _post     audio ISR (H4, H5)
 *   wgl_add(p, from)         world.c world_commit (audio ISR, a commit while playing): base *p glides from from
 *   ov_effective(...)        (guard.c) what a slot makes of a base value (the ISR's rule; the host's inspector) */

#define MC_BLOCKS 22u                    /* macro_service evaluates at most once in 22 audio blocks (a main-loop pass) */
#define MC_ENERGY 3u                     /* the ENERGY control: also the arrangement's position (arrange.c) */
static const uint16_t MC_HOME[WF_NCTL] = WF_CTL_HOME;
#define MC_ROLE GU_ROLE                  /* per engine (ENGINES order) the EDIT slot of each role (guard.c) */
#define MC_NROLE GU_NROLE
#define MC_RESO WF_EROLE_RESO
/* one pole per smoothing class at the block rate (1378 Hz), Q16: fast 10 ms, medium 60 ms, slow 250 ms; stepped */
static const int32_t OV_K[4] = {4581, 787, 190, 65536};

static struct {
    uint16_t pos[WF_NCTL];               /* the controls, 0..1000 */
    const uint8_t *maps, *curves, *rules;   /* the loaded World's sections (wb_check checked them) */
    uint8_t nmaps, ncurves, nrules;
    uint8_t dirty;                       /* a position, the World or an engine changed: evaluate */
    uint8_t snap;                        /* the next table starts at its targets (a World loaded) */
    uint8_t eng[NPART];                  /* the engines the last table resolved the roles for */
    uint8_t n, over;                     /* slots in the last table; targets that found no slot (> OV_MAX) */
    uint32_t blk;                        /* ov_blk at the last evaluation */
} mac;
static uint8_t ov_in;                    /* ISR: the overlay is in p[] / g[] (between H4 and H5) */

/* ------------------------------------------------------------- controls --- */
/* the loaded World's MAPS, CURVES and RULES (their counts; 0 = none) and DEFAULTS def: the positions from it (keep: a
 * hot reload of the same World keeps the player's) */
static void macro_load(const uint8_t *maps, uint32_t nmaps, const uint8_t *curves, uint32_t ncurves, const uint8_t *rules,
                       uint32_t nrules, const uint8_t *def, int keep)
{
    uint32_t i;
    mac.maps = maps;
    mac.nmaps = (uint8_t)nmaps;
    mac.curves = curves;
    mac.ncurves = (uint8_t)ncurves;
    mac.rules = rules;
    mac.nrules = (uint8_t)nrules;
    if (!keep) {
        for (i = 0; i < WF_NCTL; i++)
            mac.pos[i] = MC_HOME[i];
        for (i = 0; i < 4u; i++) {                       /* DEFAULTS: ctl[4] at 2, shape[4] at 8, move[4] at 12 */
            mac.pos[i] = (uint16_t)(def[2u + i] * 4u);
            mac.pos[4u + i] = (uint16_t)(def[8u + i] * 4u);
            mac.pos[8u + i] = (uint16_t)(def[12u + i] * 4u);
        }
        mac.snap = 1;
        arr.req = mac.pos[MC_ENERGY];
    }
    mac.dirty = 1;
}

static void macro_set(uint32_t c, int32_t v)
{
    if (c >= WF_NCTL)
        return;
    v = clamp(v, 0, 1000);
    if (mac.pos[c] != v) {
        mac.pos[c] = (uint16_t)v;
        mac.dirty = 1;
    }
    if (c == MC_ENERGY)
        arr.req = (uint16_t)v;                           /* (the band follows in the ISR: arr_block) */
}
static uint32_t macro_pos(uint32_t c) { return c < WF_NCTL ? mac.pos[c] : 0u; }

/* --------------------------------------------------------------- curves --- */
static uint32_t mc_isqrt(uint32_t x)     /* floor(sqrt(x)) */
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    for (; b; b >>= 2)
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
    return r;
}

/* curve c (MAPS: 0 lin, 1 exp, 2 log, 3 s, 4 late, 8 + k the CURVES record k) at u; both Q12, 0..4096 */
static int32_t mc_curve(uint32_t c, int32_t u)
{
    int32_t u2;
    switch (c) {
    case 0:
        return u;
    case 1:
        return u * u >> 12;                              /* exp: x^2, a slow start */
    case 2:
        return (int32_t)mc_isqrt((uint32_t)u << 12);     /* log: sqrt x, a fast start */
    case 3:
        u2 = u * u >> 12;
        return 3 * u2 - 2 * (u2 * u >> 12);              /* s: smoothstep */
    case 4:
        return u > 2048 ? 2 * (u - 2048) : 0;            /* late: nothing until halfway */
    default:
        if (c >= WF_CURVE_CUSTOM && c - WF_CURVE_CUSTOM < mac.ncurves) {   /* LUT9: y at x = 0, 1/8 .. 1 (0..255) */
            const uint8_t *l = mac.curves + WF_CURVE_LEN * (c - WF_CURVE_CUSTOM);
            int32_t x = 8 * u, i = x >> 12, f = x & 4095;
            return i >= 8 ? 4096 : (l[i] * 4096 + (l[i + 1] - l[i]) * f + 127) / 255;
        }
        return u;
    }
}

/* mapping m's offset now, Q8 steps (design 5.2): from home toward min (below it) or max (above), through the curve */
static int32_t mc_offset(const uint8_t *m)
{
    uint32_t c = m[0] % WF_NCTL, x = mac.pos[c], h = MC_HOME[c];
    int32_t u, lim;
    if (x < h) {
        u = (int32_t)((h - x) * 4096u / h);
        lim = (int8_t)m[4];
    } else if (x > h) {
        u = (int32_t)((x - h) * 4096u / (1000u - h));
        lim = (int8_t)m[5];
    } else {
        return 0;
    }
    return lim * mc_curve(m[3] & WF_CURVE_MASK, u) >> 4;   /* lim * 256 * c / 4096 */
}

/* rule r's strength now, Q12 (design 5.4): the lesser of how far each control is past its threshold, 0 at it, 1 at
 * the end */
static int32_t mc_strength(const uint8_t *r)
{
    uint32_t a = r[0] >> 4, b = r[0] & 15u, ta = 4u * r[1], tb = 4u * r[2];
    int32_t sa, sb;
    if (mac.pos[a] <= ta || mac.pos[b] <= tb)
        return 0;
    sa = (int32_t)((mac.pos[a] - ta) * 4096u / (1000u - ta));
    sb = (int32_t)((mac.pos[b] - tb) * 4096u / (1000u - tb));
    return sa < sb ? sa : sb;
}

/* ---------------------------------------------------------------- slots --- */
/* the slot of (kind, track t / part, id) in table nt. A new one is made when make is 1 (a non-zero offset), or 2 and
 * the target still sounds moved in the live table (it ramps home); its range: the descriptor, guard_limits.h, the
 * World's GUARD (guard.c). Idle targets (offset 0, at home) get no slot: the ISR spends nothing on them */
static ov_slot_t *mc_slot(ov_tab_t *nt, uint32_t kind, uint32_t t, uint32_t id, uint32_t make)
{
    int16_t *ptr = kind == OV_P ? &trk[t].p[id] : kind == OV_G ? &song.g[id] : 0;
    const param_desc_t *d = 0;
    const ov_tab_t *ot = &ovb[ov_live];
    ov_slot_t *s;
    uint32_t i, e = t < NPART ? trk[t].eng_req % NENGINES : 0u;
    for (i = 0; i < nt->n; i++) {
        s = &nt->s[i];
        if (s->kind == kind && (ptr ? s->ptr == ptr : s->part == t))
            return s;
    }
    if (make == 2) {                                     /* (in the live table, away from home: it must ramp back) */
        for (i = 0; i < ot->n && !(ot->s[i].kind == kind && (ptr ? ot->s[i].ptr == ptr : ot->s[i].part == t)); i++)
            ;
        make = i < ot->n && ov_cur[ov_live][i];
    }
    if (!make)
        return 0;
    if (nt->n >= OV_MAX) {
        mac.over++;
        return 0;
    }
    s = &nt->s[nt->n++];
    memset(s, 0, sizeof *s);
    s->ptr = ptr;
    s->kind = (uint8_t)kind;
    s->part = (uint8_t)t;
    s->cls = WF_NONE;
    if (kind >= OV_VCUT) {
        s->lo = -GL_VMOD_MAX;
        s->hi = GL_VMOD_MAX;
        guard_range(kind, t, 0, e, &s->lo, &s->hi);
        return s;
    }
    if (kind == OV_G)
        d = &GP[id];
    else
        d = t == TRK_DRUM || id < P_E0 ? &TP[id] : &ENGINES[e]->edit[id - P_E0];
    s->lo = d->min;
    s->hi = d->max;
    if (d->fmt == F_ENUM)
        s->cls = WF_CLASS_STEPPED;                       /* (WHEEL ROTR: a choice, never in between) */
    if (kind == OV_G && id == G_DFDBK && s->hi > GL_DFDBK_MAX)
        s->hi = GL_DFDBK_MAX;
    if (kind == OV_G && id == G_RSIZE && s->hi > GL_RSIZE_MAX)
        s->hi = GL_RSIZE_MAX;
    if (kind == OV_P && id == P_LEVEL && s->hi > GL_LEVEL_MAX)
        s->hi = GL_LEVEL_MAX;
    if (kind == OV_P && t < NPART && id >= P_E0 && e < MC_NROLE && MC_ROLE[e][MC_RESO] == id - P_E0 && s->hi > GL_RESO_MAX)
        s->hi = GL_RESO_MAX;
    guard_range(kind, t, id, e, &s->lo, &s->hi);         /* the World's soft caps and ranges inside them */
    return s;
}

/* offset off (Q8) into the slot of each target of target byte tg / id (cls == WF_NONE: a rule's action); or, with
 * off == MC_CLASS, only the mapping's smoothing class into the targets that have a slot: the slowest class of every
 * mapping on a target (stepped wins), whether it moves now or not */
#define MC_CLASS 0x7FFFFFFF
static void mc_put(ov_slot_t *s, uint32_t cls, int32_t off)
{
    if (!s)
        return;
    if (off != MC_CLASS)
        s->tgt += off;
    else if (s->cls != WF_CLASS_STEPPED)
        s->cls = (uint8_t)(s->cls == WF_NONE || cls > s->cls ? cls : s->cls);
}
static void mc_add(ov_tab_t *nt, uint32_t tg, uint32_t id, uint32_t cls, int32_t off)
{
    uint32_t kind = tg >> 5, m = tg & WF_TMASK, t, e, make = off == MC_CLASS ? 0u : off ? 1u : 2u;
    if (kind == WF_K_GLOBAL) {
        mc_put(mc_slot(nt, OV_G, WF_NONE, id, make), cls, off);
        return;
    }
    for (t = 0; t < NTRK; t++) {
        if (!(m >> t & 1u))
            continue;
        if (kind == WF_K_PARAM) {
            mc_put(mc_slot(nt, OV_P, t, id, make), cls, off);
        } else if (t >= NPART) {
            continue;                                    /* (roles and vmod: synth tracks only) */
        } else if (kind == WF_K_ROLE) {
            e = trk[t].eng_req % NENGINES;
            if (e < MC_NROLE && id < WF_NEROLES && MC_ROLE[e][id] != WF_NONE)   /* (else this engine has none) */
                mc_put(mc_slot(nt, OV_P, t, P_E0 + MC_ROLE[e][id], make), cls, off);
        } else {
            mc_put(mc_slot(nt, kind == WF_K_BRIGHT ? OV_VCUT : OV_VSHP, t, 0, make), cls, off);
        }
    }
}

/* the target table from the positions now, published to the ISR; 1 when the ISR has not taken the last one yet */
static int macro_eval(void)
{
    ov_tab_t *nt, *ot;
    const uint8_t *p;
    uint32_t i, k;
    if (ov_pub != ov_seen)
        return 1;
    ot = &ovb[ov_live];
    nt = &ovb[ov_live ^ 1u];
    nt->n = 0;
    mac.over = 0;
    for (i = 0, p = mac.maps; i < mac.nmaps; i++, p += WF_MAP_LEN)
        mc_add(nt, p[1], p[2], WF_NONE, mc_offset(p));
    for (i = 0, p = mac.rules; i < mac.nrules; i++, p += WF_RULE_HDR + WF_ACT_LEN * p[3]) {
        int32_t st = mc_strength(p);
        for (k = 0; k < p[3]; k++) {
            const uint8_t *a = p + WF_RULE_HDR + WF_ACT_LEN * k;
            mc_add(nt, a[0], a[1], WF_NONE, (int8_t)a[2] * st >> 4);
        }
    }
    for (i = 0, p = mac.maps; i < mac.nmaps; i++, p += WF_MAP_LEN)   /* the classes, from every mapping */
        mc_add(nt, p[1], p[2], p[3] >> 6, MC_CLASS);
    for (i = 0; i < nt->n; i++) {
        ov_slot_t *s = &nt->s[i];
        int32_t span = s->kind >= OV_VCUT ? GL_VMOD_MAX : s->hi - s->lo;
        if (s->cls == WF_NONE)
            s->cls = WF_CLASS_DEFAULT;
        s->tgt = clamp(s->tgt, -(span << 8), span << 8);
    }
    guard_sound(nt);                                     /* the combinations, the distorted tracks (guard.c) */
    for (i = 0; i < nt->n; i++) {
        ov_slot_t *s = &nt->s[i];
        s->from = WF_NONE;
        for (k = 0; k < ot->n; k++)                      /* the same slot before: its smoothing goes on */
            if (ot->s[k].kind == s->kind && ot->s[k].ptr == s->ptr && ot->s[k].part == s->part) {
                s->from = (uint8_t)k;
                break;
            }
    }
    nt->snap = mac.snap;
    for (i = 0; i < NPART; i++)
        mac.eng[i] = trk[i].eng_req;
    mac.n = nt->n;
    mac.blk = ov_blk;
    mac.dirty = mac.snap = 0;
    RING_PUBLISH();
    ov_pub = (uint8_t)(ov_pub + 1u);                     /* (the ISR takes it at its next block) */
    return 0;
}

/* Commit glides (Phase 11). A commit while playing (world.c: a scene, a variation, a World on its boundary) writes new
 * bases at once; those that click or zip when they jump (levels, pans, sends, DIST, an engine's continuous EDIT values
 * when the engine stays, the bus globals) glide instead: the base starts at its old value and moves 1/16 of the way
 * each block (at least one step), about 50 ms for a jump of 100. The overlay rides on top of the moving base (H4). A
 * base something else writes meanwhile (the editor) ends its glide; the next commit starts over from where they are */
#define WGL_MAX 48
static struct {
    int16_t *p;
    int16_t to, at;
} wgl[WGL_MAX];
static uint8_t wgl_n;
static void wgl_add(int16_t *p, int32_t from)    /* *p is the new base: it glides there from from */
{
    if (*p == from || wgl_n >= WGL_MAX)
        return;                                          /* (no jump; or no slot left: it jumps) */
    wgl[wgl_n].p = p;
    wgl[wgl_n].to = *p;
    wgl[wgl_n++].at = *p = (int16_t)from;
}
static void wgl_block(void)              /* H5, after the bases are back: each glide a step on */
{
    uint32_t i;
    for (i = 0; i < wgl_n;) {
        int32_t at = wgl[i].at, d = wgl[i].to - at;
        if (!d || *wgl[i].p != at) {                     /* there, or another writer took it */
            wgl[i] = wgl[--wgl_n];
            continue;
        }
        at += d / 16 ? d / 16 : d > 0 ? 1 : -1;
        wgl[i].at = (int16_t)at;
        *wgl[i++].p = (int16_t)at;
    }
}

static void macro_service(void)          /* main loop, every pass */
{
    uint32_t t;
    if (!wrt.active || wrt.swp)
        return;                                          /* (a World switch staged: its own table after its commit) */
    for (t = 0; t < NPART; t++)
        if (trk[t].eng_req != mac.eng[t])
            mac.dirty = 1;                               /* (another engine: its roles are other parameters) */
    if (mac.dirty && (mac.snap || ov_blk - mac.blk >= MC_BLOCKS))
        macro_eval();
}

static void ov_reset(void)               /* IRQs off or the ISR: no overlay until the next table, no glide */
{
    uint32_t i;
    ovb[0].n = ovb[1].n = wgl_n = 0;
    ov_seen = ov_pub;
    for (i = 0; i < NPART; i++)
        wvm_cut[i] = wvm_shape[i] = 0;
}

/* ------------------------------------------------------------ audio ISR --- */

static void ov_take(void)                /* the new table: each slot's offset carried from its slot before */
{
    uint32_t nl = ov_live ^ 1u, ol = ov_live, i;
    const ov_tab_t *nt = &ovb[nl];
    for (i = 0; i < NPART; i++)
        wvm_cut[i] = wvm_shape[i] = 0;                   /* (a vmod slot gone: back to 0) */
    for (i = 0; i < nt->n; i++) {
        uint32_t f = nt->s[i].from;
        ov_cur[nl][i] = nt->snap ? nt->s[i].tgt : f < ovb[ol].n ? ov_cur[ol][f] : 0;
    }
    ov_live = (uint8_t)nl;
    ov_seen = ov_pub;
}

/* the overlay in: each slot's base saved, its effective value written; step: one smoothing step first (toward the
 * target; while the CPU guard holds, a costly slot toward its base) */
static void ov_apply(int step)
{
    const ov_tab_t *tb = &ovb[ov_live];
    int32_t *cur = ov_cur[ov_live];
    uint32_t i;
    for (i = 0; i < tb->n; i++) {
        const ov_slot_t *s = &tb->s[i];
        int32_t c = cur[i], tg = s->tgt;
        if (wg.hold && tg > 0 && guard_costly(s))
            tg = 0;
        if (step && c != tg) {
            if (s->cls >= WF_CLASS_STEPPED) {
                c = tg;
            } else {
                int32_t d = tg - c, st = (d * OV_K[s->cls]) >> 16;   /* (|d| <= 2 x 127 << 8: no overflow) */
                c += st ? st : d > 0 ? 1 : -1;
            }
            cur[i] = c;
        }
        if (s->kind >= OV_VCUT) {
            (s->kind == OV_VCUT ? wvm_cut : wvm_shape)[s->part % NPART] = c;
            continue;
        }
        if (!c)
            continue;                                    /* (at home: the base is the value, nothing to write) */
        ov_saved[i] = *s->ptr;
        *s->ptr = (int16_t)ov_effective(s, ov_saved[i], c);
    }
    ov_in = 1;
}

static void ov_restore(void)             /* the overlay out: every base value back (the slots ov_apply wrote) */
{
    const ov_tab_t *tb = &ovb[ov_live];
    const int32_t *cur = ov_cur[ov_live];
    uint32_t i;
    for (i = tb->n; i--;)
        if (tb->s[i].kind < OV_VCUT && cur[i])
            *tb->s[i].ptr = ov_saved[i];
    ov_in = 0;
}

static void world_fx_pre(void)           /* H4: fx.c mix_block, before events_block */
{
    if (!wrt.active)
        return;
    ov_blk++;
    guard_cpu_block();                                   /* the CPU guard's load, every DMA half (guard.c) */
    if (ov_pub != ov_seen)
        ov_take();
    ov_apply(1);
}

static void world_fx_post(void)          /* H5: fx.c mix_block, after djf_process */
{
    if (ov_in)
        ov_restore();
    if (wgl_n)
        wgl_block();                                     /* (the bases a commit moves: a step on, Phase 11) */
}
