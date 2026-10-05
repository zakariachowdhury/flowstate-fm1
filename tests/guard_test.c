/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the Musical Guardrail Engine (PLAY MODE, Phase 8: firmware/src/guard.c, the H6 read-site clamps in
 * fx.c and voice.c, H12 in seq.c; docs/guardrails.md), built as tests/macro_test.c is (FELUCCA_WORLD=1 on
 * tests/hostsim.c, -fsanitize=address,undefined) and run by tests/run_tests.sh:
 *   build/host/guard_test [WORLD.wblob ...]      (the factory Worlds are built in; the test Worlds as arguments, one
 *                                                 of them worlds/test/guard.world.json: every guard rule engaged)
 *
 *  1. inert: with no World the guard does nothing: no hold, a UNISON part keeps its voices, no loop follow;
 *  2. H6: every value inside its descriptor reads unchanged (the bus globals, DIST, every engine's EDIT values);
 *     out of it, the mix renders bit for bit as with the nearest limit, the stored value is left as it was, and
 *     with garbage in every feedback, damping, colour, DUST and drive value the tail still falls after the notes;
 *  3. notes: the range (unset, under an octave, at the top), the fold, the polyphony cap, the nearest-note snap,
 *     loop follow (H12) over every chord of every World: a keys loop holding all twelve pitch classes plays only
 *     safe tones and chord tones (and as written with loop_follow off), the record guard's entry points;
 *  4. ranges: the World's soft caps and target ranges on the slots (worlds/test/guard.world.json);
 *  5. combinations: silent at their thresholds, whole WF_COMBO_RAMP past them, the cap only ever lowers a target,
 *     never moves an authored base, judged per track, the default ones on the factory Worlds;
 *  6. distorted tracks: no more than GUARD max_dist_tracks distort through the macros;
 *  7. random writers (design 13, Phase 8): editor-like writes of the bases and random macro positions never put an
 *     effective value past its descriptor, a hard limit or its range (a base already past one stays);
 *  8. CPU: the estimate grows with what sounds; over the ceiling for GL_CPU_HOLD halves the guard holds (costly
 *     slots back to their base, UNISON at 2 voices), and lets go after GL_CPU_RELEASE halves under it; the factory
 *     Worlds never make it hold; GUARD max_unison caps a UNISON part;
 *  9. arrangement timing (design 6.2, as built in world.c / arrange.c): a scene asked for mid-bar commits on the next
 *     bar, never sooner (the ENERGY bands' beat and bar timing: tests/macro_test.c). */
#define FELUCCA_WORLD 1
#define FELUCCA_ARRANGER 1
#include <stdint.h>
static void arrangement_apply(uint32_t s);
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
static uint32_t trk_def_engine(uint32_t i)
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

static int t_fails;
static uint32_t t_checks;
#define CHECK(c, ...) do { t_checks++; if (!(c)) { if (t_fails++ < 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                        printf(__VA_ARGS__); printf("\n"); } } } while (0)
static uint32_t t_rs = 0x9a4d1e37u;
static uint32_t t_rnd(void)
{
    t_rs = t_rs * 1664525u + 1013904223u;
    return t_rs >> 8;
}

/* ------------------------------------------------------------------ the instrument --- */
static void t_reset(void)
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
    memset(&gu, 0, sizeof gu);
    memset(&gcpu, 0, sizeof gcpu);
    memset(&wg, 0, sizeof wg);
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
    {
        uint32_t i;
        for (i = 0; i < NDRUM; i++)
            drums.v[i].active = 0;
    }
}
static void t_block_out(int32_t *o)
{
    mix_block(o, CTL);
    mo_r = mo_w;
}
static void t_block(void)
{
    int32_t o[2 * CTL];
    t_block_out(o);
}
static void t_blocks(uint32_t n) { while (n--) t_block(); }
static uint32_t t_bar_blocks(void) { return (uint32_t)((uint64_t)4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL) + 1u; }

typedef struct {
    char name[40];
    uint8_t *b;
    uint32_t n;
    int factory;
} tworld_t;
static tworld_t worlds[32];
static uint32_t nworlds;
static int guard_world = -1;                     /* worlds/test/guard.world.json, when given */

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
static int t_start(const tworld_t *w)
{
    int rc;
    t_reset();
    rc = world_start(w->b, w->n);
    CHECK(rc == WE_OK, "%s: world_start %d", w->name, rc);
    return rc;
}
/* the macros at a..d (per mille), evaluated on a fresh overlay; the table the ISR would take */
static const ov_tab_t *t_eval(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    ov_reset();
    mac.pos[0] = (uint16_t)a, mac.pos[1] = (uint16_t)b, mac.pos[2] = (uint16_t)c, mac.pos[3] = (uint16_t)d;
    mac.dirty = 1;
    macro_eval();
    return &ovb[ov_live ^ 1u];
}
static const ov_slot_t *t_find(const ov_tab_t *nt, uint32_t kind, uint32_t t, uint32_t pid)
{
    const int16_t *ptr = kind == OV_P ? &trk[t].p[pid] : kind == OV_G ? &song.g[pid] : 0;
    uint32_t i;
    for (i = 0; i < nt->n; i++)
        if (nt->s[i].kind == kind && (ptr ? nt->s[i].ptr == ptr : nt->s[i].part == t))
            return &nt->s[i];
    return 0;
}
static int32_t t_eff(const ov_slot_t *s) { return s->kind >= OV_VCUT ? s->tgt : ov_effective(s, *s->ptr, s->tgt); }
static void t_publish(void)                      /* the positions now into the ISR's table, taken */
{
    if (ov_pub != ov_seen) {
        world_fx_pre();
        world_fx_post();
    }
    mac.dirty = 1;
    macro_eval();
    world_fx_pre();
    world_fx_post();
}

/* ---------------------------------------------------------------- 1. inert --- */
static void test_inert(void)
{
    uint32_t e;
    t_reset();
    trk[0].p[P_VOICE] = V_UNISON;
    for (e = 0; e < NENGINES; e++) {
        uint32_t c = ENGINES[e]->poly;
        trk[0].engine = (uint8_t)e;
        wg.unison = 1;                           /* (a World's cap left behind: no World active, no effect) */
        wg.hold = 1;
        CHECK(trk_nvoice(&trk[0]) == (c && c < NPOLY ? c : NPOLY), "no World: %s UNISON keeps its voices",
              ENGINES[e]->name);
    }
    wg.hold = 0;
    CHECK(!guard_follows(&trk[0]) && !guard_follows(&trk[1]), "no World: no loop follow");
    printf("inert: with no World a UNISON part keeps its voices whatever the guard's state, no loop follows a chord\n");
}

/* ------------------------------------------------------------------- 2. H6 --- */
/* the mix's output over n blocks from now, as a hash (a fork: the state is the same for both renders) */
static uint64_t t_render_hash(uint32_t n, int32_t *peak)
{
    uint64_t h = 1469598103934665603ull;
    int32_t o[2 * CTL], pk = 0;
    uint32_t i;
    while (n--) {
        t_block_out(o);
        for (i = 0; i < 2u * CTL; i++) {
            h = (h ^ (uint32_t)o[i]) * 1099511628211ull;
            pk = abs(o[i]) > pk ? abs(o[i]) : pk;
        }
    }
    if (peak)
        *peak = pk;
    return h;
}
typedef struct {
    uint32_t g;                                  /* 1: a global, 0: a track parameter of track 0 */
    uint32_t id;
    int32_t v;
} t_write_t;
/* the same notes from the same state, once with the writes w as they are (garbage), once with each clamped to its
 * descriptor: the same bytes. And the stored values are still the garbage afterwards */
static int t_same_as_clamped(const t_write_t *w, uint32_t nw, uint32_t blocks)
{
    int fd[2];
    pid_t pid;
    uint64_t hc = 0, hg;
    uint32_t i;
    if (pipe(fd) || (pid = fork()) < 0)
        exit(2);
    if (!pid) {
        for (i = 0; i < nw; i++) {
            const param_desc_t *d = w[i].g ? &GP[w[i].id] : w[i].id >= P_E0 ? &ENGINES[trk[0].engine]->edit[w[i].id - P_E0]
                                                                              : &TP[w[i].id];
            int16_t *p = w[i].g ? &song.g[w[i].id] : &trk[0].p[w[i].id];
            *p = (int16_t)clamp(w[i].v, d->min, d->max);
        }
        hc = t_render_hash(blocks, 0);
        if (write(fd[1], &hc, sizeof hc) != (ssize_t)sizeof hc)
            _exit(2);
        _exit(0);
    }
    for (i = 0; i < nw; i++)
        *(w[i].g ? &song.g[w[i].id] : &trk[0].p[w[i].id]) = (int16_t)w[i].v;
    hg = t_render_hash(blocks, 0);
    if (read(fd[0], &hc, sizeof hc) != (ssize_t)sizeof hc)
        hc = ~hg;
    close(fd[0]);
    close(fd[1]);
    waitpid(pid, 0, 0);
    for (i = 0; i < nw; i++)
        if (*(w[i].g ? &song.g[w[i].id] : &trk[0].p[w[i].id]) != (int16_t)w[i].v)
            return 0;                            /* (the read-site clamp wrote the stored value) */
    return hg == hc;
}
static void t_engine(uint32_t e)                 /* part 1 on engine e, its first preset, polyphonic, nothing sounding */
{
    trk[0].engine = trk[0].eng_req = (uint8_t)e;
    preset_fill(trk[0].p, ENGINES[e], 0);
    trk[0].p[P_VOICE] = V_POLY;
}
static void test_h6(void)
{
    static const uint8_t GB[] = {G_DFDBK, G_RSIZE, G_DCOLOR, G_DMIX, G_RDAMP, G_CDEPTH, G_DUST};
    uint32_t i, e, k, n = 0, same = 0;
    int32_t v;
    CHECK(GP[G_DFDBK].max == GL_DFDBK_MAX && GP[G_RSIZE].max == GL_RSIZE_MAX,
          "the descriptors' maxima are the hard limits: H6 is a no-op for every value the editor can set");
    for (i = 0; i < sizeof GB; i++)              /* in range: read as written */
        for (v = GP[GB[i]].min; v <= GP[GB[i]].max; v++) {
            song.g[GB[i]] = (int16_t)v;
            n++;
            same += gbus(GB[i]) == v && (GB[i] != G_DFDBK || clamp(v, 0, GL_DFDBK_MAX) == v) &&
                    (GB[i] != G_RSIZE || clamp(v, 0, GL_RSIZE_MAX) == v);
        }
    CHECK(same == n, "in range: %u of %u bus values read unchanged", same, n);
    /* out of range: as the limit, bit for bit; the stored garbage left alone */
    for (e = 0; e < NENGINES; e++) {
        t_reset();
        t_engine(e);
        trk_note_on(&trk[0], 48, 110);
        trk_note_on(&trk[0], 55, 100);
        trk[0].p[P_DLY] = 90;
        trk[0].p[P_REV] = 90;
        for (k = 0; k < 8u; k++) {
            t_write_t w[2] = {{0, P_E0 + k, ENGINES[e]->edit[k].max + 300}, {0, P_E0 + (k + 3u) % 8u, ENGINES[e]->edit[(k + 3u) % 8u].min - 200}};
            CHECK(t_same_as_clamped(w, 2, 40), "%s EDIT %u = %d, EDIT %u = %d: renders as the clamped values",
                  ENGINES[e]->name, k, w[0].v, (k + 3u) % 8u, w[1].v);
        }
    }
    t_reset();
    t_engine(0);
    trk_note_on(&trk[0], 48, 110);
    trk[0].p[P_DLY] = trk[0].p[P_REV] = trk[0].p[P_CHOR] = 100;
    {
        t_write_t w[] = {{1, G_DFDBK, 4000}, {1, G_RSIZE, 999}, {1, G_RDAMP, -300}, {1, G_DCOLOR, 777}, {1, G_DMIX, -9},
                         {1, G_CDEPTH, 5000}, {1, G_DUST, 31000}, {0, P_DIST, 20000}};
        CHECK(t_same_as_clamped(w, sizeof w / sizeof w[0], 200), "garbage on every bus value, DUST and DIST: renders as "
              "the clamped values");
        for (i = 0; i < sizeof w / sizeof w[0]; i++)
            w[i].v = -w[i].v;
        CHECK(t_same_as_clamped(w, sizeof w / sizeof w[0], 200), "negative garbage too");
    }
    {   /* garbage left in, the notes released: the echo and the room die away, nothing past full scale */
        int32_t pk;
        t_reset();
        t_engine(0);
        trk[0].p[P_DLY] = trk[0].p[P_REV] = 127;
        song.g[G_DFDBK] = 32767, song.g[G_RSIZE] = 32767, song.g[G_RDAMP] = -32768, song.g[G_DCOLOR] = 32767;
        song.g[G_DUST] = 32767;
        trk[0].p[P_DIST] = 32767;
        trk_note_on(&trk[0], 48, 127);
        trk_note_on(&trk[0], 52, 127);
        int32_t p1;
        t_render_hash(1500, &pk);                /* ~1 s */
        trk_note_off(&trk[0], 48);
        trk_note_off(&trk[0], 52);
        t_render_hash(8000, 0);                  /* (the echo at the descriptor's longest, 0.84 a repeat) */
        t_render_hash(345, &p1);
        t_render_hash(34000, 0);                 /* ~25 s */
        t_render_hash(345, &pk);                 /* the last 0.25 s */
        CHECK(pk < p1 / 2 && pk < 32767, "garbage feedback: the tail falls (%.1f, then %.1f dBFS 25 s after the notes: "
              "the descriptors' longest echo, 0.84 a repeat, and a 0.957 room)", 20 * log10(p1 / 32768.0 + 1e-9),
              20 * log10(pk / 32768.0 + 1e-9));
    }
    printf("H6: %u in-range bus values read unchanged; out of range (every engine's EDIT values, every bus value, DUST, "
           "DIST) the mix is bit for bit the clamped one and the stored values stay; with garbage feedback the tail falls\n", n);
}

/* ---------------------------------------------------------------- 3. notes --- */
static void test_notes(void)
{
    gnote_t g;
    uint8_t gb[WF_GUARD_FIX];
    uint32_t wi, n, ci, checked = 0, moved = 0, kept = 0, i;
    memset(gb, WF_NONE, sizeof gb);
    guard_note_stage(&g, 0, 0);
    CHECK(g.lo == 48 && g.hi == 84 && g.poly == 4, "no GUARD: C3..C6, 4 notes");
    gb[0] = 60, gb[1] = 64;                      /* under an octave: widened upward */
    gb[WF_G_POLY] = 2;
    guard_note_stage(&g, gb, 0);
    CHECK(g.lo == 60 && g.hi == 71 && g.poly == 2, "E4 range of 4: %u..%u, poly %u", g.lo, g.hi, g.poly);
    gb[0] = 124, gb[1] = 126;                    /* at the top: down from 127 */
    guard_note_stage(&g, gb, 0);
    CHECK(g.lo == 116 && g.hi == 127, "at the top: %u..%u", g.lo, g.hi);
    gb[0] = 48, gb[1] = 72;
    guard_note_stage(&g, gb, 0);
    for (i = 0; i < 128u; i++) {
        uint32_t f = guard_fold(&g, (int32_t)i);
        CHECK(f >= 48 && f <= 72 && f % 12u == i % 12u, "fold %u -> %u", i, f);
    }
    for (i = 0; i < 128u; i++)                   /* the snap: the nearest in the mask, a tie below */
        for (n = 1; n < 4096u; n += 37u) {
            uint32_t s = guard_snap(i, n), d = s > i ? s - i : i - s, j, best = 99;
            for (j = 0; j < 128u; j++)
                if (n >> (j % 12u) & 1u && (j > i ? j - i : i - j) < best)
                    best = j > i ? j - i : i - j;
            CHECK(n >> (s % 12u) & 1u && d == best && (s <= i || !(i >= d && n >> ((i - d) % 12u) & 1u)),
                  "snap %u over %03x -> %u", i, n, s);
        }
    /* H12 over every World: a keys loop of all twelve pitch classes plays only safe tones, through every chord */
    for (wi = 0; wi < nworlds; wi++) {
        track_t *t;
        uint32_t kt, blocks, b, bad = 0, off = 0;
        if (t_start(&worlds[wi]))
            continue;
        kt = wrt.keys_trk;
        t = &trk[kt];
        for (i = 0; i < NSTEP; i++) {
            memset(&t->step[i], 0, sizeof t->step[i]);
            t->step[i].time = ST_NOTE;
            t->step[i].n = 1;
            t->step[i].note[0] = (uint8_t)(60u + i % 12u);
            t->step[i].vel = 100;
        }
        t->p[P_SLEN] = 16;
        t->p[P_SDIV] = 2;
        transport_req = 1;
        blocks = 4u * t_bar_blocks() * (hprog[hcur].beats / 4u ? hprog[hcur].beats / 4u : 1u);
        for (b = 0; b < blocks; b++) {
            t_block();
            for (i = 0; i < NVOICE; i++) {
                const voice_t *v = &t->v[i];
                if (!v->active || !v->gate || v->stage == 4u || harm.ci >= hprog[hcur].n)
                    continue;
                checked++;
                bad += !((harm.safe | harm.ct) >> (v->note % 12u) & 1u);
            }
        }
        transport_req = 2;
        t_block();
        CHECK(!bad, "%s: %u keys-loop notes off the safe tones", worlds[wi].name, bad);
        /* each chord: the snap moves only the notes that are not safe, to chord tones */
        for (ci = 0; ci < hprog[hcur].n; ci++) {
            harm.ci = (uint8_t)ci;
            harm.ct = hprog[hcur].ct[ci];
            harm.safe = hprog[hcur].safe[ci];
            for (n = 36; n < 96u; n++) {
                uint32_t m = guard_loop_note(n);
                if (harm.safe >> (n % 12u) & 1u)
                    kept += m == n, off += m != n;
                else
                    moved++, off += !(harm.ct >> (m % 12u) & 1u) || (m > n ? m - n : n - m) > 6u;
            }
        }
        CHECK(!off, "%s: %u notes snapped wrongly", worlds[wi].name, off);
        /* loop_follow off: the loop as written */
        memcpy(gb, wctx.b + wctx.off[WF_S_GUARD], WF_GUARD_FIX);
        gb[WF_G_FOLLOW] = 0;
        gu.fix = gb;
        CHECK(!guard_follows(t), "%s: loop_follow off", worlds[wi].name);
        t_reset();
    }
    CHECK(checked > 1000, "only %u keys-loop notes heard", checked);
    /* the polyphony cap (H9): the keys held on the keys track, plus the MIDI notes held there, under max_poly */
    memset(kb_kind, 0, sizeof kb_kind);
    memset(kb_n, 0, sizeof kb_n);
    wrt.keys_trk = 1;
    for (i = 0; i < 2u; i++)
        kb_kind[3u + i] = KS_NOTE, kb_n[3u + i] = 1, kb_trk[3u + i] = 1;
    kb_kind[9] = KS_NOTE, kb_n[9] = 1, kb_trk[9] = 2;   /* (another track's key: not counted) */
    CHECK(guard_admit(3, 0) && !guard_admit(2, 0) && !guard_admit(3, 1) && guard_admit(4, 1), "the polyphony cap");
    memset(kb_kind, 0, sizeof kb_kind);
    memset(kb_n, 0, sizeof kb_n);
    /* the record guard's entry points (Phase 10) */
    {
        step_t s;
        memset(&s, 0, sizeof s);
        s.time = ST_NOTE;
        s.n = 2;
        s.note[0] = 60, s.note[1] = 64;
        memset(gb, WF_NONE, sizeof gb);
        gu.fix = gb;
        CHECK(guard_rec_dup(&s, 64) && !guard_rec_dup(&s, 65), "duplicates in a step");
        CHECK(guard_rec_room(&s), "notes per step: the default 4");
        gb[WF_G_RECNOTES] = 2;
        CHECK(!guard_rec_room(&s), "max_notes 2: the step is full");
        CHECK(guard_rec_len(0, 16) == 1 && guard_rec_len(40, 16) == 16 && guard_rec_len(5, 16) == 5, "lengths");
        CHECK(guard_rec_quant(100) == 100 * (250 - 188) / 250, "the default quantise 0.75");
        gb[WF_G_QUANT] = 250;
        CHECK(guard_rec_quant(-77) == 0, "quantise 1: on the grid");
        gb[0] = 60, gb[1] = 72;
        guard_note_stage(&g, gb, 0);
        CHECK(guard_rec_note(&g, 30) >= 60 && guard_rec_note(&g, 30) <= 72, "a recorded note in the range");
        gu.fix = 0;
    }
    printf("notes: ranges (unset, under an octave, at the top), the fold, the snap; loop follow over every World: %u "
           "keys-loop notes heard, all safe; %u snapped to a near chord tone, %u safe kept; the record guard\n",
           checked, moved, kept);
}

/* --------------------------------------------------------------- 4. ranges --- */
static void test_ranges(void)
{
    const ov_tab_t *nt;
    const ov_slot_t *s;
    uint32_t t;
    if (guard_world < 0) {
        CHECK(0, "worlds/test/guard.world.json not given");
        return;
    }
    t_start(&worlds[guard_world]);
    gu.ncombos = 0;
    nt = t_eval(1000, 1000, 1000, 1000);
    for (t = 0; t < NPART; t++)
        CHECK((s = t_find(nt, OV_P, t, P_LEVEL)) && s->hi == 110, "track %u level: max_level 110 (%d)", t, s ? s->hi : -1);
    CHECK((s = t_find(nt, OV_G, WF_NONE, G_DFDBK)) && s->hi == 96 && s->lo == 0, "dfdbk: max_dfdbk 96 inside the range 0..100");
    CHECK((s = t_find(nt, OV_G, WF_NONE, G_RSIZE)) && s->hi == 118, "rsize: max_rsize 118");
    CHECK((s = t_find(nt, OV_G, WF_NONE, G_DUST)) && s->hi == 60, "dust: max_dust 60");
    CHECK((s = t_find(nt, OV_P, 2, P_E0 + WF_GRAIN_DENS)) && s->hi == 90, "keys (GRAIN) DENS: grain_dens 90");
    CHECK((s = t_find(nt, OV_P, 0, P_E0 + 5)) && s->hi == 100, "pad (ANALOG) RES: max_reso 100");
    CHECK((s = t_find(nt, OV_P, 1, P_E0 + 6)) && s->lo == 10 && s->hi == 85, "bass (TRIO) RES: the range 10..85");
    CHECK((s = t_find(nt, OV_VCUT, 2, 0)) && s->hi == 30 && t_eff(s) <= 30 * 256, "keys ~bright: the range -20..30");
    nt = t_eval(0, 0, 0, 0);
    CHECK((s = t_find(nt, OV_VCUT, 2, 0)) && t_eff(s) >= -20 * 256, "keys ~bright at COLOR 0: %d", s ? s->tgt : 0);
    for (t = 0; t < nt->n; t++) {
        int32_t v = nt->s[t].kind >= OV_VCUT ? nt->s[t].tgt / 256 : t_eff(&nt->s[t]), b = nt->s[t].kind >= OV_VCUT ? 0 : *nt->s[t].ptr;
        CHECK((v <= nt->s[t].hi || v <= b) && (v >= nt->s[t].lo || v >= b), "slot %u: %d outside %d..%d", t, v,
              nt->s[t].lo, nt->s[t].hi);
    }
    printf("ranges: the guard World's soft caps (level, dfdbk, rsize, dust, GRAIN DENS, resonance) and its target "
           "ranges (a global, an engine parameter, ~bright) on the slots\n");
}

/* --------------------------------------------------------- 5. combinations --- */
static void test_combos(void)
{
    /* rsize over 100 caps dfdbk at 60; pad's DRIVE over 40 caps its RES at 50 (one condition) */
    static const uint8_t C[] = {WF_K_GLOBAL << 5, G_RSIZE, 100, WF_K_GLOBAL << 5, G_RSIZE, 100, WF_K_GLOBAL << 5, G_DFDBK, 60,
                                WF_K_ROLE << 5 | 7, WF_EROLE_DRIVE, 40, WF_K_ROLE << 5 | 7, WF_EROLE_DRIVE, 40,
                                WF_K_ROLE << 5 | 7, WF_EROLE_RESO, 50};
    uint32_t wi, x, n = 0, engaged = 0, worlds_engaged = 0;
    int32_t prev = 0x7FFF;
    if (guard_world < 0)
        return;
    t_start(&worlds[guard_world]);
    gu.combos = C;
    gu.ncombos = 2;
    for (x = 0; x <= 1000; x += 10) {            /* SPACE up: rsize rises past 100, dfdbk comes down to 60 */
        const ov_tab_t *nt = t_eval(500, 500, x, 500);
        const ov_slot_t *rs = t_find(nt, OV_G, WF_NONE, G_RSIZE), *fb = t_find(nt, OV_G, WF_NONE, G_DFDBK);
        int32_t r = rs ? t_eff(rs) : song.g[G_RSIZE], f = fb ? t_eff(fb) : song.g[G_DFDBK], free;
        uint32_t saved = gu.ncombos;
        gu.ncombos = 0;
        nt = t_eval(500, 500, x, 500);
        fb = t_find(nt, OV_G, WF_NONE, G_DFDBK);
        free = fb ? t_eff(fb) : song.g[G_DFDBK];
        gu.ncombos = saved;
        n++;
        CHECK(f <= free, "a cap only lowers: %d over %d", f, free);
        if (r <= 100)
            CHECK(f == free, "SPACE %u: rsize %d at or under 100: dfdbk %d as without (%d)", x, r, f, free);
        else if (r >= 100 + WF_COMBO_RAMP)
            CHECK(f <= 60 || f == song.g[G_DFDBK], "SPACE %u: rsize %d: dfdbk %d, the cap 60", x, r, f);
        if (r > 100) {
            CHECK(f <= prev || free > prev, "SPACE %u: dfdbk %d rose to %d as the cap came in", x, prev, f);
            engaged += f < free;
        }
        prev = f;
    }
    CHECK(engaged > 5, "the dfdbk cap engaged at %u positions", engaged);
    {   /* per track: pad's DRIVE high caps pad's RES only (bass has no DRIVE role: its RES runs free) */
        const ov_tab_t *nt = t_eval(1000, 500, 500, 500);
        const ov_slot_t *pr = t_find(nt, OV_P, 0, P_E0 + 5), *br = t_find(nt, OV_P, 1, P_E0 + 6);
        CHECK(pr && t_eff(pr) <= 50, "pad RES %d: capped at 50 (its DRIVE %d)", pr ? t_eff(pr) : -1, trk[0].p[P_E0 + 6]);
        CHECK(br && t_eff(br) > 50, "bass RES %d: not capped (TRIO has no DRIVE)", br ? t_eff(br) : -1);
    }
    {   /* a base past the cap stays: the authored sound is never moved */
        const ov_tab_t *nt;
        const ov_slot_t *fb;
        song.g[G_DFDBK] = 90;
        nt = t_eval(500, 500, 1000, 500);
        fb = t_find(nt, OV_G, WF_NONE, G_DFDBK);
        CHECK(!fb || t_eff(fb) == 90 || t_eff(fb) < 90, "base 90 past the cap 60: %d (never above the base)", fb ? t_eff(fb) : 90);
        CHECK(!fb || t_eff(fb) >= 60, "base 90: the macro may lower it, the cap never pushes it under 60: %d",
              fb ? t_eff(fb) : 90);
    }
    {   /* a base past the cap that a macro lowers, not under the cap: left where the macro put it (MOTION drives
         * DUST past 20, SPACE lowers dfdbk from its base 90 toward 74; the cap 60 must not pull it back up to 90) */
        static const uint8_t C2[] = {WF_K_GLOBAL << 5, G_DUST, 20, WF_K_GLOBAL << 5, G_DUST, 20, WF_K_GLOBAL << 5, G_DFDBK, 60};
        const ov_tab_t *nt;
        const ov_slot_t *fb;
        int32_t on, off;
        gu.combos = C2;
        gu.ncombos = 1;
        song.g[G_DFDBK] = 90;
        nt = t_eval(500, 1000, 300, 500);
        fb = t_find(nt, OV_G, WF_NONE, G_DFDBK);
        on = fb ? t_eff(fb) : 90;
        gu.ncombos = 0;
        nt = t_eval(500, 1000, 300, 500);
        fb = t_find(nt, OV_G, WF_NONE, G_DFDBK);
        off = fb ? t_eff(fb) : 90;
        CHECK(on == off && off < 90 && off > 60, "base 90, lowered by SPACE to %d: the cap left it at %d", off, on);
        song.g[G_DFDBK] = 60;
    }
    /* the firmware's own combinations on the factory Worlds: at least one engages somewhere on the grid, and never
     * raises a target */
    for (wi = 0; wi < nworlds; wi++) {
        uint32_t a, b, c, d, hit = 0, sc;
        if (!worlds[wi].factory || t_start(&worlds[wi]))
            continue;
        for (sc = 0; sc < WF_NSCENE; sc++)
        for (a = 0, world_apply(sc, 0); a <= 1000; a += 500)
            for (b = 0; b <= 1000; b += 500)
                for (c = 0; c <= 1000; c += 500)
                    for (d = 0; d <= 1000; d += 500) {
                        int32_t on[OV_MAX];
                        const ov_tab_t *nt = t_eval(a, b, c, d);
                        uint32_t i, saved = gu.ncombos;
                        for (i = 0; i < nt->n; i++)
                            on[i] = t_eff(&nt->s[i]);
                        gu.ncombos = 0;
                        nt = t_eval(a, b, c, d);
                        gu.ncombos = (uint8_t)saved;
                        for (i = 0; i < nt->n; i++) {
                            CHECK(on[i] <= t_eff(&nt->s[i]), "%s: a combination raised slot %u", worlds[wi].name, i);
                            hit += on[i] < t_eff(&nt->s[i]);
                        }
                    }
        worlds_engaged += hit != 0;
    }
    CHECK(worlds_engaged >= 2, "the firmware's combinations engaged in %u factory Worlds", worlds_engaged);
    printf("combinations: silent at the threshold, whole %u steps past it, only lowering, per track, never moving a "
           "base past its cap; %u SPACE positions, the firmware's own engaged in %u factory Worlds\n", WF_COMBO_RAMP, n,
           worlds_engaged);
}

/* ------------------------------------------------------- 6. distorted tracks --- */
static void test_dist(void)
{
    uint32_t s, e, t, worst = 0;
    if (guard_world < 0)
        return;
    for (s = 0; s < WF_NSCENE; s++) {
        t_start(&worlds[guard_world]);
        CHECK(world_apply(s, 0) == WE_OK, "scene %u", s);
        for (e = 0; e <= 1000; e += 50) {
            const ov_tab_t *nt = t_eval(500, 500, 500, e);
            uint32_t n = 0, base = 0;
            for (t = 0; t < NPART; t++) {
                const ov_slot_t *sl = t_find(nt, OV_P, t, P_DIST);
                int32_t v = sl ? t_eff(sl) : trk[t].p[P_DIST];
                n += v > 0;
                base += trk[t].p[P_DIST] > 0;
            }
            CHECK(n <= (base > 1u ? base : 1u), "scene %u ENERGY %u: %u tracks distort (max_dist_tracks 1, %u by the "
                  "World)", s, e, n, base);
            worst = n > worst ? n : worst;
        }
    }
    CHECK(worst == 1, "the cap never needed? %u", worst);
    printf("distorted tracks: ENERGY drives three tracks' DIST, GUARD max_dist_tracks 1: one distorts (a base the World "
           "distorts counts first)\n");
}

/* ---------------------------------------------------------- 7. random writers --- */
static void test_writers(void)
{
    uint32_t wi, it, bad = 0, n = 0;
    for (wi = 0; wi < nworlds; wi++) {
        if (t_start(&worlds[wi]))
            continue;
        for (it = 0; it < 400; it++) {
            const ov_tab_t *tb;
            uint32_t i, k;
            int16_t p0[NTRK][P_COUNT], g0[G_COUNT];
            for (k = 0; k < 6; k++) {            /* an editor writes some bases, inside their descriptors */
                uint32_t t = t_rnd() % NTRK, id = t_rnd() % P_COUNT;
                const param_desc_t *d = t == TRK_DRUM ? (id < P_E0 ? &TP[id] : 0) : id >= P_E0 ? &ENGINES[trk[t].eng_req]->edit[id - P_E0]
                                                                                           : &TP[id];
                if (d && !WB_HAS(WB_PSTRUCT, id) && id != P_MUTE && id != P_VOICE)
                    trk[t].p[id] = (int16_t)(d->min + (int32_t)(t_rnd() % (uint32_t)(d->max - d->min + 1)));
                id = WB_GWHITE[t_rnd() % sizeof WB_GWHITE];
                if (id != G_SWING && id != G_DTIME)
                    song.g[id] = (int16_t)(GP[id].min + (int32_t)(t_rnd() % (uint32_t)(GP[id].max - GP[id].min + 1)));
            }
            for (k = 0; k < 4; k++)
                macro_set(k, (int32_t)(t_rnd() % 1001u));
            mac.dirty = 1;
            if (ov_pub != ov_seen) {
                world_fx_pre();
                world_fx_post();
            }
            macro_eval();
            for (k = 0; k < NTRK; k++)
                memcpy(p0[k], trk[k].p, sizeof p0[k]);
            memcpy(g0, song.g, sizeof g0);
            world_fx_pre();                      /* the overlay in: what the engines read now */
            tb = &ovb[ov_live];
            for (i = 0; i < tb->n; i++) {
                const ov_slot_t *s = &tb->s[i];
                int32_t v, b, id;
                const param_desc_t *d;
                if (s->kind >= OV_VCUT) {
                    v = (s->kind == OV_VCUT ? wvm_cut : wvm_shape)[s->part];
                    bad += v < -(GL_VMOD_MAX << 8) || v > GL_VMOD_MAX << 8;
                    continue;
                }
                id = s->kind == OV_G ? (int32_t)(s->ptr - song.g) : (int32_t)(s->ptr - trk[s->part].p);
                b = s->kind == OV_G ? g0[id] : p0[s->part][id];
                v = *s->ptr;
                d = s->kind == OV_G ? &GP[id] : s->part == TRK_DRUM || id < P_E0 ? &TP[id] : &ENGINES[trk[s->part].eng_req]->edit[id - P_E0];
                n++;
                bad += v < d->min || v > d->max;
                bad += (v > s->hi && v > b) || (v < s->lo && v < b);
                bad += s->kind == OV_G && ((id == G_DFDBK && v > GL_DFDBK_MAX) || (id == G_RSIZE && v > GL_RSIZE_MAX));
                bad += s->kind == OV_P && id == P_LEVEL && v > GL_LEVEL_MAX && v > b;
            }
            world_fx_post();
            for (k = 0; k < NTRK; k++)
                bad += memcmp(p0[k], trk[k].p, sizeof p0[k]) != 0;
            bad += memcmp(g0, song.g, sizeof g0) != 0;
        }
    }
    CHECK(!bad, "%u effective values past a descriptor, a limit or a range (or a base not restored)", bad);
    printf("random writers: %u effective values under editor-like writes and random macros, none past its descriptor, "
           "hard limit or range; every base restored\n", n);
}

/* ------------------------------------------------------------------ 8. CPU --- */
static void test_cpu(void)
{
    uint32_t i, e0, e1, wi, held = 0;
    t_reset();
    e0 = guard_cpu_est();
    CHECK(e0 == GU_COST_BASE, "nothing sounding: the base %u", e0);
    t_engine(0);
    trk_note_on(&trk[0], 48, 100);
    e1 = guard_cpu_est();
    trk_note_on(&trk[0], 52, 100);
    CHECK(e1 > e0 && guard_cpu_est() > e1, "more voices, more cost: %u %u %u", e0, e1, guard_cpu_est());
    song.g[G_DUST] = 50;
    trk[0].p[P_DIST] = 50;
    CHECK(guard_cpu_est() >= e1 + GU_COST_DUST + GU_COST_DIST, "DUST and DIST cost");
    if (guard_world >= 0) {
        const ov_slot_t *ds;
        uint32_t k;
        t_start(&worlds[guard_world]);
        macro_set(3, 1000);                      /* ENERGY: pad's DIST up */
        t_publish();
        t_blocks(200);
        ds = t_find(&ovb[ov_live], OV_P, 0, P_DIST);
        CHECK(ds && guard_costly(ds), "pad's DIST slot is costly");
        trk[0].p[P_VOICE] = V_UNISON;
        k = trk_nvoice(&trk[0]);
        CHECK(k == (wg.unison && wg.unison < 8u ? wg.unison : k), "max_unison %u: a UNISON part plays %u", wg.unison, k);
        wg.ceil = 1;                             /* everything is over this ceiling */
        for (i = 0; i < GU_HALF * (GL_CPU_HOLD - 1u); i++)
            t_block();
        CHECK(!wg.hold, "not before %u halves", GL_CPU_HOLD);
        t_blocks(GU_HALF * 2u);
        CHECK(wg.hold, "held after %u halves over the ceiling", GL_CPU_HOLD);
        CHECK(trk_nvoice(&trk[0]) <= 2u, "held: UNISON at 2 voices");
        t_blocks(1000);
        for (k = 0; k < ovb[ov_live].n; k++) {
            const ov_slot_t *s = &ovb[ov_live].s[k];
            if (guard_costly(s))
                CHECK(ov_effective(s, *s->ptr, ov_cur[ov_live][k]) <= *s->ptr, "held: costly slot %u back at its base "
                      "(%d)", k, ov_cur[ov_live][k]);
        }
        wg.ceil = 254;
        song.cpu_q8 = 0;
        t_blocks(GU_HALF * (GL_CPU_RELEASE - 2u));
        CHECK(wg.hold, "still held before %u halves under", GL_CPU_RELEASE);
        t_blocks(GU_HALF * 4u);
        CHECK(!wg.hold, "let go after %u halves under", GL_CPU_RELEASE);
        song.cpu_q8 = 255;                       /* the device's own measure, over the ceiling */
        wg.ceil = 217;
        t_blocks(GU_HALF * (GL_CPU_HOLD + 2u));
        CHECK(wg.hold, "the device's cpu_q8 over the ceiling: held");
        song.cpu_q8 = 0;
    }
    for (wi = 0; wi < nworlds; wi++) {           /* the factory Worlds at their heaviest corner: never held */
        if (!worlds[wi].factory || t_start(&worlds[wi]))
            continue;
        for (i = 0; i < 4; i++)
            macro_set(i, 1000);
        transport_req = 1;
        t_blocks(4u * t_bar_blocks());
        held += wg.hold;
        transport_req = 2;
        t_block();
    }
    CHECK(!held, "%u factory Worlds made the CPU guard hold", held);
    printf("CPU: the estimate grows with voices, DIST and DUST; over the ceiling the guard holds after %u halves "
           "(costly slots to their base, UNISON at 2), lets go after %u under; the device's cpu_q8 counts; max_unison; "
           "the factory Worlds never hold\n", GL_CPU_HOLD, GL_CPU_RELEASE);
}

/* -------------------------------------------------------------- 9. timing --- */
static void test_timing(void)
{
    uint32_t wi, k, bad = 0, n = 0;
    for (wi = 0; wi < nworlds; wi++) {
        if (!worlds[wi].factory)
            continue;
        for (k = 0; k < 6; k++) {
            uint32_t wait = 7u + t_rnd() % (2u * t_bar_blocks()), b, req_beat, q;
            if (t_start(&worlds[wi]))
                continue;
            transport_req = 1;
            t_blocks(wait);
            req_beat = clk_beat;
            CHECK(world_request((wrt.scene + 1u) % WF_NSCENE, 0) == WE_OK, "request");
            q = wst.q ? wst.q : 1u;              /* (the scene's transition: 1, 2 or 4 bars, or the phrase) */
            for (b = 0; b < 9u * t_bar_blocks() && wst.st == WST_READY; b++) {
                uint32_t before = clk_beat;
                t_block();
                if (wst.st == WST_APPLIED) {
                    /* committed in this block: on the first bar line after the request on its transition's grid */
                    n++;
                    bad += !(clk_beat < before || (clk_beat & 3u) == 0u) || before != 4u * q * (req_beat / 4u / q + 1u);
                }
            }
            CHECK(wst.st == WST_APPLIED, "%s: the scene committed", worlds[wi].name);
        }
    }
    CHECK(!bad && n, "%u of %u scene commits not on their boundary", bad, n);
    printf("timing: %u scene requests at random points of a bar, each committed on its boundary (its transition)\n", n);
}

int main(int argc, char **argv)
{
    uint32_t i;
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
        worlds[nworlds].b = t_read(argv[i], &worlds[nworlds].n);
        snprintf(worlds[nworlds].name, sizeof worlds[nworlds].name, "%s",
                 strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i]);
        if (strstr(argv[i], "guard"))
            guard_world = (int)nworlds;
        nworlds++;
    }
    {
        void (*const T[])(void) = {test_inert, test_h6, test_notes, test_ranges, test_combos, test_dist, test_writers,
                                   test_cpu, test_timing};
        for (i = 0; i < sizeof T / sizeof T[0]; i++)
            T[i]();
    }
    printf("guard test: %u checks, %s\n", t_checks, t_fails ? "FAILED" : "all passed");
    return t_fails ? 1 : 0;
}
