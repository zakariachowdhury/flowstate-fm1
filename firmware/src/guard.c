/* SPDX-License-Identifier: GPL-3.0-only */
/* The Musical Guardrail Engine (PLAY MODE, docs/design/play-mode-architecture.md 6, docs/guardrails.md): what a
 * player's actions may do to the music, between them and SLOOP's core. The rules read the World's GUARD section
 * (world_fmt.h: 32 fixed bytes, each WF_NONE = the firmware default, then target ranges and sound combinations);
 * guard_limits.h's hard limits hold underneath, whatever a World says. Everything here is inert with no World.
 *
 *   notes        the keys track's range (an octave at least) and polyphony (smartkeys.c, H7-H9: gnote_t); loop
 *                follow (H12: a user-loop note that is an avoid note over the sounding chord plays the nearest chord
 *                tone); the record guard play_rec.c calls (a note in key as it sounded, else a safe one, in range;
 *                no duplicate in a step; a length inside the loop; at most GUARD max_notes a step; the quantise
 *                strength)
 *   sound        each macro slot's range (macro.c mc_slot): the descriptor and the hard limits, then the World's
 *                soft caps (max_level, max_reso, max_dfdbk, max_rsize, max_dist, max_dust, GRAIN DENS) and its target
 *                ranges; then over the whole table (macro_eval): how many tracks may distort, and the sound
 *                combinations (WF_COMBO_DEFAULT, or the World's). The ISR keeps every slot inside its range
 *                (ov_effective); fx.c and voice.c clamp the stability-critical values where they are read (H6)
 *   CPU          max_unison (voice.c trk_nvoice), GRAIN DENS and the distorted tracks (above); at run time a load,
 *                the device's measured cpu_q8 or this file's estimate from what sounds (the host has no timer: the
 *                estimate makes the guard testable), against GUARD's ceiling: over it for GL_CPU_HOLD DMA halves,
 *                the costly slots (DIST, DUST, GRAIN DENS, SLICER depth) go back to their base and a UNISON part
 *                plays 2 voices, until the load stays under the release level for 2 s. Voice shedding (audio.c)
 *                stays underneath
 *   arrangement  the timing rules act where they apply: world.c (a scene or variation commits on the next bar),
 *                arrange.c (ENERGY layers on the bar or every second bar, lanes and masks on the beat or the bar,
 *                min_band_bars between changes, fills on the last bar of a phrase); they read GUARD through
 *                guard_byte
 *
 *   guard_load(g, nranges)       world.c, with the World's mappings: its GUARD section (0: none, the defaults)
 *   guard_byte(g, off)           a fixed field, or its default
 *   guard_note_stage, guard_fold, guard_admit         smartkeys.c (main loop / ISR)
 *   guard_follows(t), guard_loop_note(n)              seq.c, audio ISR (H12)
 *   guard_rec_note, _dup, _room, _len, _quant         play_rec.c (PLAY REC), audio ISR
 *   guard_range(kind, t, id, e, &lo, &hi)             macro.c mc_slot (main loop)
 *   guard_sound(nt)                                   macro.c macro_eval (main loop)
 *   guard_live(nt, echo, crush)                       macro.c macro_eval: LIVE FX's combinations (WF_COMBO_LIVE)
 *   ov_effective(s, b, c)        what a slot makes of a base value (the ISR, the host's inspector)
 *   guard_costly(s)              audio ISR: a slot the CPU guard holds
 *   guard_cpu_block()            audio ISR (macro.c world_fx_pre), every block: the load and the hold
 *   guard_cpu_est()              the estimate: host instructions a sample of what sounds now */

static const uint8_t GU_DEF[] = WF_GUARD_DEFAULT;
static const uint8_t GU_COMBO_DEF[] = WF_COMBO_DEFAULT;
static const uint8_t GU_COMBO_LIVE[] = WF_COMBO_LIVE;   /* while ECHO (the first two) and CRUSH (the third) are up */
static const uint8_t GU_ROLE[][WF_NEROLES] = WF_ENG_ROLE;   /* per engine (ENGINES order) the EDIT slot of each role */
#define GU_NROLE (sizeof GU_ROLE / sizeof GU_ROLE[0])
_Static_assert(sizeof GU_DEF == WF_G_RESERVED - WF_G_POLY, "a default per GUARD field");
_Static_assert(sizeof GU_COMBO_DEF % WF_COMBO_LEN == 0 && sizeof GU_COMBO_DEF / WF_COMBO_LEN <= WF_MAX_COMBOS,
               "WF_COMBO_DEFAULT: whole records");
_Static_assert(sizeof GU_COMBO_LIVE == 3 * WF_COMBO_LEN, "WF_COMBO_LIVE: ECHO's two, CRUSH's one (its feedback cap is "
               "GL_ECHO_DFDBK: tools/worldc.py and tests/livefx_test.c check)");
#define GU_RANGE_LO 48u                  /* a synth track's range when GUARD sets none: C3..C6 (design 6.1) */
#define GU_RANGE_HI 84u
#define GU_NOVAL (-0x8000)               /* a combination's condition that names nothing */
#define GU_HALF 8u                       /* audio blocks in a DMA half (audio.c: HALF_FRAMES / CTL) */

static struct {
    const uint8_t *fix;                  /* the World's GUARD fixed bytes; 0: none (every field its default) */
    const uint8_t *ranges, *combos;      /* its target ranges; its combinations, or the firmware's */
    uint8_t nranges, ncombos;
} gu;

/* GUARD bytes g (0: none): field off (WF_G_POLY .. WF_G_COMBOS) as set, or its firmware default */
static uint32_t guard_byte(const uint8_t *g, uint32_t off)
{
    return g && g[off] != WF_NONE ? g[off] : GU_DEF[off - WF_G_POLY];
}

/* the loaded World's GUARD section (wb_check checked it; 0: the World has none) with its nranges range records */
static void guard_load(const uint8_t *g, uint32_t nranges)
{
    uint32_t nc = guard_byte(g, WF_G_COMBOS);
    gu.fix = g;
    gu.ranges = g ? g + WF_GUARD_FIX : 0;
    gu.nranges = (uint8_t)(g ? nranges : 0u);
    if (nc == WF_NONE) {
        gu.combos = GU_COMBO_DEF;
        gu.ncombos = (uint8_t)(sizeof GU_COMBO_DEF / WF_COMBO_LEN);
    } else {
        gu.combos = g + WF_GUARD_FIX + WF_GRANGE_LEN * nranges;
        gu.ncombos = (uint8_t)nc;
    }
    wg.unison = (uint8_t)guard_byte(g, WF_G_UNISON);
    wg.ceil = (uint8_t)guard_byte(g, WF_G_CPU);
}

/* ================================================================== notes === */
typedef struct {
    uint8_t lo, hi;                      /* the range (MIDI), an octave at least */
    uint8_t poly;                        /* max_poly */
} gnote_t;

/* main loop (smartkeys.c sk_stage): the note rules of synth track t from GUARD bytes g. The range is widened to an
 * octave when it is under one (upward, or down from 127), so that every pitch class has a place in it and a fold
 * never leaves the key */
static void guard_note_stage(gnote_t *n, const uint8_t *g, uint32_t t)
{
    int set = g && g[WF_G_RANGE + 2u * t] != WF_NONE;
    n->lo = (uint8_t)(set ? g[WF_G_RANGE + 2u * t] : GU_RANGE_LO);
    n->hi = (uint8_t)(set ? g[WF_G_RANGE + 2u * t + 1u] : GU_RANGE_HI);
    if (n->hi < n->lo + 11u)
        n->hi = (uint8_t)(n->lo + 11u > 127u ? 127u : n->lo + 11u);
    if (n->lo > n->hi - 11u)
        n->lo = (uint8_t)(n->hi - 11u);
    n->poly = (uint8_t)guard_byte(g, WF_G_POLY);
}

/* note v folded by octaves into the range */
static uint32_t guard_fold(const gnote_t *g, int32_t v)
{
    while (v > (int32_t)g->hi)
        v -= 12;
    while (v < (int32_t)g->lo)
        v += 12;
    return (uint32_t)clamp(v, 0, 127);
}

/* audio ISR (H9): may one more note sound on the keys track? Its keys held, plus extra (MIDI notes held there),
 * under poly. A key refused stays silent until it is pressed again (its release is a no-op) */
static int guard_admit(uint32_t poly, uint32_t extra)
{
    uint32_t k, n = extra;
    for (k = 0; k < 27u; k++)
        n += kb_kind[k] == KS_NOTE && kb_n[k] && kb_trk[k] == wrt.keys_trk;
    return n < poly;
}

/* the nearest note to n whose pitch class is in mask (a tie: the lower), n itself when it is */
static uint32_t guard_snap(uint32_t n, uint32_t mask)
{
    uint32_t d;
    mask &= 0xFFFu;
    if (!mask || (mask >> (n % 12u) & 1u))
        return n;
    for (d = 1; d < 12u; d++) {
        if (n >= d && (mask >> ((n - d) % 12u) & 1u))
            return n - d;
        if (n + d <= 127u && (mask >> ((n + d) % 12u) & 1u))
            return n + d;
    }
    return n;
}

/* H12, audio ISR: track t is the keys track of a World whose GUARD loop_follow is snap, over a progression */
static int guard_follows(const track_t *t)
{
    return wrt.active && t == &trk[wrt.keys_trk % NPART] && guard_byte(gu.fix, WF_G_FOLLOW) &&
           harm.ci < hprog[hcur].n;
}
/* H12: a loop note over the sounding chord: a safe tone stays, an avoid note (or one outside the scale) moves to the
 * nearest chord tone. The loop itself is never rewritten: each trigger asks again */
static uint32_t guard_loop_note(uint32_t n)
{
    return (harm.safe >> (n % 12u) & 1u) ? n : guard_snap(n, harm.ct);
}

/* ---- the record guard (PLAY REC, design 6.2: play_rec.c, through seq.c rec_note / step_add) */
/* a note to record: in key (the World scale, or a tone of the sounding chord: every Smart Keys note) as it sounded,
 * else the nearest safe tone; in the range */
static uint32_t guard_rec_note(const gnote_t *g, uint32_t n)
{
    if (harm.ci < hprog[hcur].n && !(((uint32_t)hprog[hcur].scale | harm.ct) >> (n % 12u) & 1u))
        n = guard_loop_note(n);
    return guard_fold(g, (int32_t)n);
}
static int guard_rec_dup(const step_t *s, uint32_t n)   /* the step already holds n: recording it again adds nothing */
{
    uint32_t i;
    if (s->time != ST_NOTE)
        return 0;
    for (i = 0; i < s->n; i++)
        if (s->note[i] == n)
            return 1;
    return 0;
}
static int guard_rec_room(const step_t *s)   /* another note fits the step: under GUARD max_notes (and a step's 4) */
{
    uint32_t mx = guard_byte(gu.fix, WF_G_RECNOTES);
    return s->time != ST_NOTE || s->n < (mx < 4u ? mx : 4u);
}
static uint32_t guard_rec_len(uint32_t len, uint32_t max)   /* a recorded note's length: 1 .. max steps (the loop) */
{
    return len < 1u ? 1u : len > max ? max : len;
}
static int32_t guard_rec_quant(int32_t off)   /* an offset from the grid as recorded: GUARD quantize pulls it in */
{
    return off * (int32_t)(WF_UNIT - guard_byte(gu.fix, WF_G_QUANT)) / (int32_t)WF_UNIT;
}

/* ================================================================== sound === */
/* what slot s makes of base value b with offset c (Q8): clamped to its range, and never past a limit further than
 * the base already is (a cap never changes the authored sound) */
static int32_t ov_effective(const ov_slot_t *s, int32_t b, int32_t c)
{
    int32_t v = b + ((c + 128) >> 8);
    if (v > s->hi && v > b)
        v = b > s->hi ? b : s->hi;
    else if (v < s->lo && v < b)
        v = b < s->lo ? b : s->lo;
    return v;
}

/* does GUARD target (tg, id) name the slot of kind on track t writing parameter pid (track t runs engine e)? */
static int guard_names(uint32_t tg, uint32_t id, uint32_t kind, uint32_t t, uint32_t pid, uint32_t e)
{
    uint32_t k = tg >> 5, m = tg & WF_TMASK;
    if (kind == OV_G)
        return k == WF_K_GLOBAL && id == pid;
    if (t >= NTRK || !(m >> t & 1u))
        return 0;
    if (kind == OV_VCUT || kind == OV_VSHP)
        return k == (kind == OV_VCUT ? WF_K_BRIGHT : WF_K_SHAPE);
    if (k == WF_K_PARAM)
        return id == pid;
    return k == WF_K_ROLE && t < NPART && e < GU_NROLE && id < WF_NEROLES && GU_ROLE[e][id] != WF_NONE &&
           P_E0 + GU_ROLE[e][id] == pid;
}

/* main loop (macro.c mc_slot): a new slot's range *lo..*hi (the descriptor and the hard limits) narrowed by the
 * World's soft caps and target ranges. kind OV_*, t the track (OV_VCUT / OV_VSHP: the part), pid the P_* / G_*
 * parameter, e track t's engine */
static void guard_range(uint32_t kind, uint32_t t, uint32_t pid, uint32_t e, int16_t *lo, int16_t *hi)
{
    const uint8_t *r = gu.ranges;
    int32_t l = *lo, h = *hi, cap = 0x7FFF;
    uint32_t i;
    if (kind == OV_G)
        cap = pid == G_DFDBK ? (int32_t)guard_byte(gu.fix, WF_G_MAXDFDBK)
            : pid == G_RSIZE ? (int32_t)guard_byte(gu.fix, WF_G_MAXRSIZE)
            : pid == G_DUST ? (int32_t)guard_byte(gu.fix, WF_G_MAXDUST) : cap;
    else if (kind == OV_P && pid == P_LEVEL)
        cap = (int32_t)guard_byte(gu.fix, WF_G_MAXLEVEL);
    else if (kind == OV_P && pid == P_DIST)
        cap = (int32_t)guard_byte(gu.fix, WF_G_MAXDIST);
    else if (kind == OV_P && t < NPART && pid >= P_E0 && e < GU_NROLE) {
        if (GU_ROLE[e][WF_EROLE_RESO] == pid - P_E0)
            cap = (int32_t)guard_byte(gu.fix, WF_G_MAXRESO);
        else if (e == WF_ENG_GRAIN && pid - P_E0 == WF_GRAIN_DENS)
            cap = (int32_t)guard_byte(gu.fix, WF_G_GRAINDENS);
    }
    if (h > cap)
        h = cap;
    for (i = 0; i < gu.nranges; i++, r += WF_GRANGE_LEN)
        if (guard_names(r[0], r[1], kind, t, pid, e)) {
            if ((int8_t)r[2] > l)
                l = (int8_t)r[2];
            if ((int8_t)r[3] < h)
                h = (int8_t)r[3];
        }
    *lo = (int16_t)l;
    *hi = (int16_t)(h < l ? l : h);
}

/* ---- over the whole table */
/* target (tg, id) on track t: its slot kind and parameter; 0 when it names nothing there (t outside its tracks, or
 * a role track t's engine lacks) */
static int gu_resolve(uint32_t tg, uint32_t id, uint32_t t, uint32_t *kind, uint32_t *pid)
{
    uint32_t k = tg >> 5, e;
    if (k == WF_K_GLOBAL) {
        *kind = OV_G;
        *pid = id;
        return id < G_COUNT;
    }
    if (t >= NTRK || !((tg & WF_TMASK) >> t & 1u))
        return 0;
    *kind = OV_P;
    if (k == WF_K_PARAM) {
        *pid = id;
        return id < P_COUNT;
    }
    if (k != WF_K_ROLE || t >= NPART || id >= WF_NEROLES)
        return 0;
    e = trk[t].eng_req % NENGINES;
    if (e >= GU_NROLE || GU_ROLE[e][id] == WF_NONE)
        return 0;
    *pid = P_E0 + GU_ROLE[e][id];
    return 1;
}
static ov_slot_t *gu_find(ov_tab_t *nt, uint32_t kind, uint32_t t, uint32_t pid)
{
    const int16_t *ptr = kind == OV_P ? &trk[t].p[pid] : &song.g[pid];
    uint32_t i;
    for (i = 0; i < nt->n; i++)
        if (nt->s[i].kind == kind && nt->s[i].ptr == ptr)
            return &nt->s[i];
    return 0;
}
/* the value of (tg, id) on track t as the table makes it: the base (the main loop only ever sees bases) through its
 * slot; for a per-track target that does not name t, its highest over its tracks. GU_NOVAL: nothing */
static int32_t gu_value(ov_tab_t *nt, uint32_t tg, uint32_t id, uint32_t t)
{
    uint32_t kind, pid, u;
    int32_t v = GU_NOVAL, x;
    const ov_slot_t *s;
    if (tg >> 5 != WF_K_GLOBAL && (t >= NTRK || !((tg & WF_TMASK) >> t & 1u))) {
        for (u = 0; u < NTRK; u++)
            if ((tg & WF_TMASK) >> u & 1u && (x = gu_value(nt, tg, id, u)) > v)
                v = x;
        return v;
    }
    if (!gu_resolve(tg, id, t, &kind, &pid))
        return GU_NOVAL;
    x = kind == OV_P ? trk[t].p[pid] : song.g[pid];
    s = gu_find(nt, kind, t, pid);
    return s ? ov_effective(s, x, s->tgt) : x;
}
/* a condition's strength, Q12: 0 at its threshold, 1 WF_COMBO_RAMP steps above it (self: a condition on the capped
 * target itself is whole as soon as it holds) */
static int32_t gu_strength(int32_t v, int32_t over, int self)
{
    if (v == GU_NOVAL || v <= over)
        return 0;
    if (self || v - over >= WF_COMBO_RAMP)
        return 4096;
    return (v - over) * 4096 / WF_COMBO_RAMP;
}
/* combination c on track t (a global target c: any t): its cap, in as far as its conditions hold, on c's slot. A
 * target nothing moves has no slot and needs no cap: its value is the authored one */
static void gu_combo(ov_tab_t *nt, const uint8_t *c, uint32_t t)
{
    uint32_t kind, pid;
    ov_slot_t *s;
    int32_t b, v, st, sb, cap, hi;
    if (!gu_resolve(c[6], c[7], t, &kind, &pid) || !(s = gu_find(nt, kind, t, pid)))
        return;
    st = gu_strength(gu_value(nt, c[0], c[1], t), (int8_t)c[2], c[0] == c[6] && c[1] == c[7]);
    sb = gu_strength(gu_value(nt, c[3], c[4], t), (int8_t)c[5], c[3] == c[6] && c[4] == c[7]);
    if (sb < st)
        st = sb;
    hi = s->hi;
    cap = (int8_t)c[8];
    if (!st || cap >= hi)
        return;
    cap = hi - ((hi - cap) * st >> 12);
    b = *s->ptr;
    v = ov_effective(s, b, s->tgt);
    if (v > cap && v > b)
        s->tgt = ((b > cap ? b : cap) - b) * 256;
}

/* main loop (macro.c macro_eval), the target table nt summed and clamped to its spans: the rules over the whole
 * table. 1. CPU: at most GUARD max_dist_tracks synth tracks distort; a track the World leaves clean that a macro
 * would distort beyond them stays clean (its slot's top at its base, 0). 2. a vmod slot's offset inside its GUARD
 * range (the ISR writes vmod offsets without a clamp). 3. the sound combinations, in order, each per track */
static void guard_sound(ov_tab_t *nt)
{
    const uint8_t *c;
    uint32_t i, t, n = 0, maxd = guard_byte(gu.fix, WF_G_DISTTRK);
    for (t = 0; t < NPART; t++)
        n += trk[t].p[P_DIST] > 0;
    for (i = 0; i < nt->n; i++) {
        ov_slot_t *s = &nt->s[i];
        if (s->kind == OV_P && s->part < NPART && s->ptr == &trk[s->part].p[P_DIST] && *s->ptr <= 0 &&
            ov_effective(s, *s->ptr, s->tgt) > 0) {
            if (n < maxd)
                n++;
            else
                s->hi = *s->ptr;
        }
        if (s->kind >= OV_VCUT)
            s->tgt = clamp(s->tgt, s->lo * 256, s->hi * 256);
    }
    for (i = 0, c = gu.combos; i < gu.ncombos; i++, c += WF_COMBO_LEN) {
        if (c[6] >> 5 == WF_K_GLOBAL)
            gu_combo(nt, c, WF_NONE);
        else
            for (t = 0; t < NTRK; t++)
                if ((c[6] & WF_TMASK) >> t & 1u)
                    gu_combo(nt, c, t);
    }
}

/* main loop (macro.c macro_eval), after guard_sound: while LIVE ECHO is up, a big room shortens the echo and its
 * feedback stays at most GL_ECHO_DFDBK; while CRUSH is up, DUST stays moderate on a distorted part. These hold
 * whatever combinations the World's GUARD sets (Phase 13, design 9.3) */
static void guard_live(ov_tab_t *nt, int echo, int crush)
{
    uint32_t i;
    for (i = 0; i < 3u; i++)
        if (i < 2u ? echo : crush)
            gu_combo(nt, GU_COMBO_LIVE + WF_COMBO_LEN * i, WF_NONE);
}

/* ==================================================================== CPU === */
/* host instructions a sample (cc -O2) of what sounds, fitted by least squares over the DMA halves of the factory and
 * test Worlds' sweeps (tests/guard_sweep.c --calib, about 800,000 halves, a residual of 77) and rounded up about 10 %:
 * per sounding voice of each engine (ENGINES order; PHASE and VOICE, which no World plays yet, from
 * tests/cpu_baseline.txt x 1.2), the stages a part or the master switches on, and the rest (buses, master, an
 * idle part, the sequencer, the overlay) */
static const uint8_t GU_VCOST[] = {135, 95, 160, 70, 125, 220, 205, 165, 30};
#define GU_COST_BASE 480u                /* the buses and the master, the parts' fixed work, the sequencer */
#define GU_COST_DRUM 128u                /* a sounding drum voice */
#define GU_COST_DIST 55u                 /* a part's DIST stage */
#define GU_COST_SLICER 40u               /* a part's SLICER */
#define GU_COST_DUST 30u                 /* DUST on the master */
#define GU_COST_DJF 66u                  /* the DJ filter (LIVE FX FILTER; tests/livefx_test.c --cost: 56..60) */
#define GU_COST_PUNCH 96u                /* a punch-in effect (FREEZE's loop, a white key's FX; measured 88) */
#define GU_COST_GRAIN 360u               /* a GRAIN voice at DENS 127 (its grains), scaled by DENS */

static int gu_moved(const int16_t *p)    /* the overlay (its live table) moves *p up now: the base is what p holds */
{
    const ov_tab_t *t = &ovb[ov_live];
    uint32_t i;
    for (i = 0; i < t->n; i++)
        if (t->s[i].kind < OV_VCUT && t->s[i].ptr == p)
            return ov_cur[ov_live][i] > 0;
    return 0;
}
static uint32_t guard_cpu_est(void)
{
    uint32_t p, i, n = GU_COST_BASE;
    for (p = 0; p < NPART; p++) {
        const track_t *t = &trk[p];
        uint32_t e = t->engine, v = 0, c = e < sizeof GU_VCOST ? GU_VCOST[e] : 190u;
        for (i = 0; i < NVOICE; i++)
            v += t->v[i].active != 0;
        if (e == WF_ENG_GRAIN)
            c += GU_COST_GRAIN * (uint32_t)clamp(t->p[P_E0 + WF_GRAIN_DENS], 0, 127) / 127u;
        n += v * c;
        if (t->p[P_DIST] > 0 && (v || t->tail))
            n += GU_COST_DIST;
        if (t->p[P_SLCR])
            n += GU_COST_SLICER;
    }
    for (i = 0; i < NDRUM; i++)
        n += drums.v[i].active ? GU_COST_DRUM : 0u;
    if (song.g[G_DUST] > 0 || gu_moved(&song.g[G_DUST]))
        n += GU_COST_DUST;
    if (djf.mode)
        n += GU_COST_DJF;
    if (punch.cur >= 0)
        n += GU_COST_PUNCH;
    return n;
}

static struct {
    uint8_t blk, over;                   /* blocks into this half; halves over the ceiling in a row */
    uint16_t under;                      /* halves under the release level in a row, while holding */
    uint16_t load;                       /* the last half's load, 1/256 */
    uint16_t est;                        /* .. the estimate's part of it, host instructions a sample */
} gcpu;

/* audio ISR (world_fx_pre), each block: every DMA half the load, the larger of the measured cpu_q8 (the device)
 * and the estimate; over GUARD's ceiling GL_CPU_HOLD halves in a row: hold; under the release level (1/16 under the
 * ceiling: 80 % for the default 85 %) GL_CPU_RELEASE halves in a row: let go */
static void guard_cpu_block(void)
{
    uint32_t load, rel;
    if (++gcpu.blk < GU_HALF)
        return;
    gcpu.blk = 0;
    gcpu.est = (uint16_t)guard_cpu_est();
    load = gcpu.est * 256u / GL_CPU_FULL;
    if (song.cpu_q8 > load)
        load = song.cpu_q8;
    gcpu.load = (uint16_t)load;
    rel = wg.ceil - wg.ceil / 16u;
    if (load > wg.ceil) {
        gcpu.under = 0;
        if (gcpu.over < 255u)
            gcpu.over++;
        if (gcpu.over >= GL_CPU_HOLD)
            wg.hold = 1;
    } else {
        gcpu.over = 0;
        if (wg.hold && load < rel) {
            if (++gcpu.under >= GL_CPU_RELEASE) {
                wg.hold = 0;
                gcpu.under = 0;
            }
        } else {
            gcpu.under = 0;
        }
    }
}

/* audio ISR: slot s is one the CPU guard holds at its base (a stage that costs: DIST, DUST, GRAIN DENS, SLICER) */
static int guard_costly(const ov_slot_t *s)
{
    uint32_t id;
    if (s->kind == OV_G)
        return s->ptr == &song.g[G_DUST];
    if (s->kind != OV_P || s->part >= NPART)
        return 0;
    id = (uint32_t)(s->ptr - trk[s->part].p);
    return id == P_DIST || id == P_SLDEPTH || (trk[s->part].engine == WF_ENG_GRAIN && id == P_E0 + WF_GRAIN_DENS);
}

static void guard_reset(void)            /* a World loaded or left: no hold carried over */
{
    memset(&gcpu, 0, sizeof gcpu);
    wg.hold = 0;
}
