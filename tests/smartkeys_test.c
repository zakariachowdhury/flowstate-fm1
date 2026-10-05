/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the harmony and Smart Keys (PLAY MODE, Phase 6: firmware/src/harmony.c, smartkeys.c, the H2 pitch
 * reference count in voice.c and the hooks H7 / H8 / H9 / H11 / H16 in seq.c), built as tests/world_test.c is
 * (FELUCCA_WORLD=1 on tests/hostsim.c, -fsanitize=address,undefined) and run by tests/run_tests.sh:
 *   build/host/smartkeys_test [WORLD.wblob ...]   (the factory Worlds are built in, those FACTORY_WORLDS names
 *                                                 when set (tests/world_pick.h); the test Worlds as arguments)
 *
 *  1. maps: every key of the 27, every chord of every progression of every World (the factory Worlds, the test
 *     Worlds, and variants of each with white: safe, black: chord+9 / chord, every avoid policy, the full scale as
 *     the melody scale, narrow and wide ranges), every OCT -3..+3: the note equals an independent model of design
 *     4.1 (written here from the blob, with sets instead of the firmware's arithmetic); white keys in the melody
 *     scale, black keys on the chord (its safe 9th with chord+9), inside the range, the pitch class kept by the
 *     fold; before the fold white and black keys ascend without duplicates and the C4 key is the tonic; the
 *     factory Worlds hold all 27 keys inside their range at OCT 0; chord names; the key LEDs;
 *  2. the clock: every scene of every factory World played through its progression (and on): the chord at each
 *     block is the progression's at the clock (exactly at the boundary block), the key chord anticipates by a
 *     32nd, the black keys follow it and the white keys never move; stopped, chord 0;
 *  3. held notes: keys held across several chord changes keep their voices and pitches; their release ends
 *     exactly what sounded (MIDI out pairs too), no gate and no count left, the voices die out; also across an
 *     OCT change and a scene change on the bar;
 *  4. same pitch (R1): two keys on one pitch, and a held key under a sequencer note of the same pitch: releasing
 *     one keeps the other sounding (POLY and LEGATO); with wrt.refcount = 0, SLOOP's behaviour (the first
 *     release cuts it); the counts stay exact through slides, ratchets, the arp (PULSE), an engine switch and a
 *     panic;
 *  5. octaves: the lowest and highest keys at every OCT, folding at the range edges, a narrow range clamps;
 *  6. polyphony: all 27 keys at once respect max_poly and the 8-voice budget; random mashing; nothing left;
 *  7. MIDI in: every note 0..127 as a virtual key, on the keys track's channel and on another channel, note-off
 *     after a chord change, the polyphony cap, a repeated note-on, other parts raw;
 *  8. SLOOP: with no World active the keys and MIDI take SLOOP's paths (kb_map, raw), Smart Keys are off;
 *  9. a fuzz of everything at once (keys, MIDI, OCT, scenes on the bar, STOP / PLAY) over every factory World:
 *     every key note in scale or chord, the voice budget, then nothing sounding and no count left. */
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
#include "world_pick.h"                         /* (FACTORY_WORLDS: tests/run_tests.sh's choice) */
static void arrangement_apply(uint32_t s) { (void)s; }
static uint32_t arrangement_ready(void) { return 0; }
static void song_backup(void) {}
static void song_restore(void) {}

static int t_fails;
static uint32_t t_checks;
#define CHECK(c, ...) do { t_checks++; if (!(c)) { if (t_fails++ < 60) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                        printf(__VA_ARGS__); printf("\n"); } } } while (0)

static uint32_t t_rs = 0x5eed1234u;
static uint32_t t_rnd(void)
{
    t_rs = t_rs * 1664525u + 1013904223u;
    return t_rs >> 8;
}

static const char *const NN[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const char *t_nn(uint32_t n)
{
    static char b[8][8];
    static int i;
    i = (i + 1) & 7;
    snprintf(b[i], 8, "%s%d", NN[n % 12u], (int)(n / 12u) - 1);
    return b[i];
}
static int t_black(uint32_t k) { uint32_t pc = (53u + k) % 12u; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

/* ------------------------------------------------------------------ the instrument --- */
static void t_reset(void)                        /* power-on: SLOOP's defaults, nothing sounding, no World */
{
    memset(trk, 0, sizeof trk);
    memset(&song, 0, sizeof song);
    memset(&wrt, 0, sizeof wrt);
    memset(&wst, 0, sizeof wst);
    memset(hprog, 0, sizeof hprog);
    memset(skm, 0, sizeof skm);
    memset(sk_midi, 0, sizeof sk_midi);
    memset(vref, 0, sizeof vref);
    memset(vlive, 0, sizeof vlive);
    memset(kb_kind, 0, sizeof kb_kind);
    memset(kb_n, 0, sizeof kb_n);
    memset(roll, 0, sizeof roll);
    hcur = skcur = 0;
    harm.ci = 255;
    harm.ci_key = 0;
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
    usb.config = 1;                              /* (MIDI out queues the key events) */
}

static void t_block(void)                        /* one audio block, as the ISR runs it */
{
    int32_t o[2 * CTL];
    mix_block(o, CTL);
    mo_r = mo_w;                                 /* (nobody drains MIDI out on the host) */
}
static void t_events(void)                       /* one block of events only (no DSP): the clock tests */
{
    events_block(CTL);
    mo_r = mo_w;
}
static void t_blocks(uint32_t n) { while (n--) t_block(); }
static uint32_t t_bar_blocks(void) { return (uint32_t)((uint64_t)4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL) + 1u; }

static void t_play(void)
{
    transport_req = 1;
    t_block();
}
static void t_stop(void)
{
    transport_req = 2;
    t_block();
}

static uint32_t t_gated(uint32_t t, int note)    /* gated voices of part t (on note, or any: -1) */
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[t].v[i].active && trk[t].v[i].gate && trk[t].v[i].stage != 4u && (note < 0 || trk[t].v[i].note == (uint32_t)note);
    return n;
}
static uint32_t t_active(uint32_t t)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[t].v[i].active;
    return n;
}
static uint32_t t_counts(void)                   /* pitch counts left (vref + vlive) */
{
    uint32_t t, n, c = 0;
    for (t = 0; t < NPART; t++)
        for (n = 0; n < 128u; n++)
            c += vref[t][n] + vlive[t][n];
    return c;
}
static uint32_t t_kcounts(void)                  /* .. on the keys track (the World's parts may hold notes) */
{
    uint32_t n, c = 0;
    for (n = 0; n < 128u; n++)
        c += vref[wrt.keys_trk][n] + vlive[wrt.keys_trk][n];
    return c;
}
static uint32_t t_busy(void)
{
    uint32_t p, i, n = 0;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            n += trk[p].v[i].active && trk[p].v[i].stage != 4u;
    return n;
}
static void t_mute_others(int on)                /* the World's own parts silent (voice-budget isolation) */
{
    uint32_t t;
    for (t = 0; t < NTRK; t++)
        if (t != wrt.keys_trk)
            trk[t].p[P_MUTE] = (int16_t)on;
}
static void t_keys(uint32_t mask)                /* the keys down now, applied by the next block */
{
    fm1_in.notes = mask & ((1u << 27) - 1u);
}
static void t_midi(uint32_t ch, uint32_t on, uint32_t note, uint32_t vel)
{
    midi_in_q[mi_w % MQ] = (on ? 0x09u : 0x08u) | ((on ? 0x90u : 0x80u) | ch) << 8 | (note & 127u) << 16 | (vel & 127u) << 24;
    mi_w++;
}

/* ------------------------------------------------------------------ the Worlds --- */
typedef struct {
    char name[40];
    uint8_t *b;
    uint32_t n;
    int factory;
} tworld_t;
static tworld_t worlds[64];
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
static void t_crc(uint8_t *b)
{
    uint32_t L = wb_u16(b + 6), c = wb_crc(b, L);
    b[12] = (uint8_t)c;
    b[13] = (uint8_t)(c >> 8);
    b[14] = (uint8_t)(c >> 16);
    b[15] = (uint8_t)(c >> 24);
}

static int t_start(const tworld_t *w)            /* power-on, then the World (its default scene) */
{
    int rc;
    t_reset();
    rc = world_start(w->b, w->n);
    CHECK(rc == WE_OK, "%s: world_start %d", w->name, rc);
    t_block();                                   /* (the switch's panic, harm_block) */
    return rc;
}

/* ---------------------------------------------- the reference model (design 4.1) --- */
static const uint16_t RQ[WF_NQUAL] = WF_QUAL_MASK;
typedef struct {
    uint32_t root, scale, mel, white, black, tonic, lo, hi, poly, avoid, kt, mode;
} wref_t;
static wref_t wr;

static void ref_world(void)                      /* the loaded World as the blob states it */
{
    const uint8_t *meta = wctx.b + wctx.off[WF_S_META] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN;
    const uint8_t *k = wctx.b + wctx.off[WF_S_KEYS];
    const uint8_t *g = wctx.have >> WF_S_GUARD & 1u ? wctx.b + wctx.off[WF_S_GUARD] : 0;
    wr.root = meta[3];
    wr.scale = meta[4];
    wr.kt = k[0];
    wr.mode = k[1];
    wr.mel = (k[2] | k[3] << 8) & 0xFFFu;
    wr.white = k[4];
    wr.black = k[5];
    wr.tonic = k[6];
    while (wr.tonic % 12u != wr.root)            /* (worldc makes it the root: then this does nothing) */
        wr.tonic--;
    wr.lo = g && g[2 * wr.kt] != 255 ? g[2 * wr.kt] : 48u;
    wr.hi = g && g[2 * wr.kt] != 255 ? g[2 * wr.kt + 1] : 84u;
    if (wr.hi < wr.lo + 11u)                     /* (a range under an octave: widened upward to one) */
        wr.hi = wr.lo + 11u > 127u ? 127u : wr.lo + 11u;
    if (wr.lo + 11u > wr.hi)
        wr.lo = wr.hi - 11u;
    wr.poly = g && g[WF_G_POLY] != 255 ? g[WF_G_POLY] : 4u;
    wr.avoid = g && g[WF_G_AVOID] != 255 ? g[WF_G_AVOID] : 0u;
}
static const uint8_t *ref_prog(uint32_t p)
{
    const uint8_t *r = wctx.b + wctx.off[WF_S_PROGS];
    while (p--)
        r += 1u + 2u * r[0];
    return r;
}
static uint32_t ref_beats(const uint8_t *pr)
{
    uint32_t k, b = 0;
    for (k = 0; k < pr[0]; k++)
        b += pr[2u + 2u * k];
    return b;
}
static uint32_t ref_chord_at(const uint8_t *pr, uint32_t beat)   /* the chord index at a beat of the progression */
{
    uint32_t k, at = beat % ref_beats(pr);
    for (k = 0; k < pr[0]; k++) {
        if (at < pr[2u + 2u * k])
            return k;
        at -= pr[2u + 2u * k];
    }
    return 0;
}
typedef struct {
    uint32_t pc, q, ct, safe, sc, bs, wm;        /* absolute pitch-class sets; wm relative to the tonic */
} rchord_t;
static rchord_t ref_chord(const uint8_t *pr, uint32_t ci)
{
    rchord_t c;
    uint32_t i, x;
    memset(&c, 0, sizeof c);
    c.pc = (wr.root + (pr[1u + 2u * ci] >> 4)) % 12u;
    c.q = pr[1u + 2u * ci] & 15u;
    for (i = 0; i < 12; i++) {
        if (RQ[c.q] >> i & 1u)
            c.ct |= 1u << ((c.pc + i) % 12u);
        if (SCALE_MASK[wr.scale] >> i & 1u)
            c.sc |= 1u << ((wr.root + i) % 12u);
    }
    for (x = 0; x < 12; x++) {
        int in_ct = c.ct >> x & 1u, below = c.ct >> ((x + 11u) % 12u) & 1u, above = c.ct >> ((x + 1u) % 12u) & 1u;
        int avoid = !in_ct && (wr.avoid == 0 ? below : wr.avoid == 2 ? (below || above) : 0);
        if (in_ct || ((c.sc >> x & 1u) && !avoid))
            c.safe |= 1u << x;
    }
    c.bs = c.ct;
    if (wr.black == 1 && (c.sc & c.safe) >> ((c.pc + 2u) % 12u) & 1u)
        c.bs |= 1u << ((c.pc + 2u) % 12u);
    c.wm = wr.mel;
    if (wr.white == 1) {
        uint32_t s = 0;
        for (x = 0; x < 12; x++)
            if ((wr.mel >> x & 1u) && (c.safe >> ((wr.tonic + x) % 12u) & 1u))
                s |= 1u << x;
        if (s)
            c.wm = s;
    }
    return c;
}
/* the 27 keys before OCT and the fold: white keys walk the melody notes from the lowest at or above the tonic (the
 * C4 key), black keys take the lowest chord tone above their left white key and the black key before */
static void ref_raw(const rchord_t *c, int32_t raw[27])
{
    int32_t mel[200], n, nm = 0, c4 = -1, lastw = -1000, lastb = -1000;
    uint32_t k, w = 0;
    for (n = -60; n < 260 && nm < 200; n++)
        if (c->wm >> (uint32_t)((n - (int32_t)wr.tonic + 1200) % 12) & 1u) {
            if (c4 < 0 && n >= (int32_t)wr.tonic)
                c4 = nm;
            mel[nm++] = n;
        }
    for (k = 0; k < 27; k++) {
        if (t_black(k)) {
            int32_t lo = lastw > lastb ? lastw : lastb;
            for (n = lo + 1; !(c->bs >> (uint32_t)((n + 1200) % 12) & 1u); n++)
                ;
            raw[k] = lastb = n;
        } else {
            raw[k] = lastw = mel[c4 + (int32_t)w - 4];
            w++;
        }
    }
}
static uint32_t ref_fold(int32_t n, uint32_t lo, uint32_t hi)   /* the representative of n's pitch class in range */
{
    int32_t m, best = -1;
    if (n < (int32_t)lo || n > (int32_t)hi)
        for (m = (int32_t)lo; m <= (int32_t)hi; m++)
            if ((m - n) % 12 == 0 && (best < 0 || (n > (int32_t)hi ? m > best : 0)))
                best = m;
    if (best >= 0)
        n = best;
    return (uint32_t)(n < 0 ? 0 : n > 127 ? 127 : n);
}
/* MIDI note v as a virtual key: the device's keys as they are; outside, the formula (a black key: the lowest chord
 * tone above its left white key) */
static uint32_t ref_vnote(const rchord_t *c, uint32_t v)
{
    int32_t raw[27], mel[400], n, nm = 0, c4 = -1, d = 0, x;
    uint32_t pc = v % 12u, wv = v;
    if (v >= 53 && v <= 79) {
        ref_raw(c, raw);
        return ref_fold(raw[v - 53], wr.lo, wr.hi);
    }
    if (pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10)
        wv = v - 1;
    for (x = 60; x < (int32_t)wv; x++)           /* white keys from C4 */
        d += !(x % 12 == 1 || x % 12 == 3 || x % 12 == 6 || x % 12 == 8 || x % 12 == 10);
    for (x = (int32_t)wv; x < 60; x++)
        d -= !(x % 12 == 1 || x % 12 == 3 || x % 12 == 6 || x % 12 == 8 || x % 12 == 10);
    for (n = -120; n < 300 && nm < 400; n++)
        if (c->wm >> (uint32_t)((n - (int32_t)wr.tonic + 1200) % 12) & 1u) {
            if (c4 < 0 && n >= (int32_t)wr.tonic)
                c4 = nm;
            mel[nm++] = n;
        }
    n = mel[c4 + d];
    if (wv != v)
        for (n++; !(c->bs >> (uint32_t)((n + 1200) % 12) & 1u); n++)
            ;
    return ref_fold(n, wr.lo, wr.hi);
}

/* ------------------------------------------------------------------ 1. the maps --- */
static uint32_t maps_checked, maps_folded_factory;

static void stage_prog(uint32_t p)               /* progression p of the loaded World, committed (as a scene would) */
{
    ws_keys(p);
    harm_commit();
    sk_commit(0);
}

static void maps_world(const tworld_t *w, const char *variant)
{
    uint32_t p, ci, k, np;
    int oct;
    if (world_load(w->b, w->n) != WE_OK || world_apply(0, 0) != WE_OK) {
        CHECK(0, "%s %s: does not load", w->name, variant);
        return;
    }
    ref_world();
    np = wctx.cnt[WF_S_PROGS];
    CHECK(skm[skcur].tonic == wr.tonic && skm[skcur].g.lo == wr.lo && skm[skcur].g.hi == wr.hi && skm[skcur].g.poly == wr.poly,
          "%s %s: tonic / range / poly", w->name, variant);
    for (p = 0; p < np; p++) {
        const uint8_t *pr = ref_prog(p);
        stage_prog(p);
        CHECK(skm[skcur].n == pr[0] && hprog[hcur].n == pr[0], "%s %s prog %u: %u chords", w->name, variant, p, pr[0]);
        for (ci = 0; ci < pr[0]; ci++) {
            rchord_t c = ref_chord(pr, ci);
            int32_t raw[27];
            uint32_t base[27];
            ref_raw(&c, raw);
            harm.ci = harm.ci_key = (uint8_t)ci;
            CHECK(hprog[hcur].ct[ci] == c.ct && hprog[hcur].safe[ci] == c.safe,
                  "%s %s prog %u chord %u: masks %03x/%03x, want %03x/%03x", w->name, variant, p, ci,
                  hprog[hcur].ct[ci], hprog[hcur].safe[ci], c.ct, c.safe);
            for (k = 0; k < 27; k++) {           /* before the fold: ascending, no duplicates, the C4 key */
                if (k && !t_black(k)) {
                    uint32_t j = k - 1;
                    while (t_black(j))
                        j--;
                    CHECK(raw[k] > raw[j], "%s %s: white keys ascend (%u, %u)", w->name, variant, j, k);
                }
                if (t_black(k)) {
                    uint32_t j;
                    CHECK(raw[k] > raw[k - 1], "%s %s: black key %u above its left white", w->name, variant, k);
                    for (j = k - 1; j < 27 && j > 0 && !t_black(j); j--)
                        ;
                    if (j > 0 && j < k && t_black(j))
                        CHECK(raw[k] > raw[j], "%s %s: black keys ascend (%u, %u)", w->name, variant, j, k);
                }
            }
            CHECK(wr.white == 1 || raw[7] == (int32_t)wr.tonic, "%s %s: the C4 key is the tonic", w->name, variant);
            for (oct = -3; oct <= 3; oct++) {
                song.octave = (int8_t)oct;
                for (k = 0; k < 27; k++) {
                    uint32_t got = sk_note(k), want = ref_fold(raw[k] + 12 * oct, wr.lo, wr.hi);
                    uint32_t rel = (got + 12u - wr.tonic % 12u) % 12u;
                    maps_checked++;
                    CHECK(got == want, "%s %s prog %u chord %u OCT %d key %u: %s, want %s", w->name, variant, p, ci, oct,
                          k, t_nn(got), t_nn(want));
                    CHECK(got >= wr.lo && got <= wr.hi, "%s %s: key %u %s outside the range", w->name, variant, k, t_nn(got));
                    CHECK((int32_t)(got % 12u) == (raw[k] + 1200) % 12,
                          "%s %s: the fold keeps the pitch class", w->name, variant);
                    if (t_black(k))
                        CHECK(c.bs >> (got % 12u) & 1u, "%s %s prog %u chord %u: black key %u %s off the chord",
                              w->name, variant, p, ci, k, t_nn(got));
                    else
                        CHECK(c.wm >> rel & 1u, "%s %s: white key %u %s off the melody scale",
                              w->name, variant, k, t_nn(got));
                    if (!oct)
                        base[k] = got;
                }
            }
            song.octave = 0;
            for (k = 0; k < 27; k++)             /* every white key a tone of the World scale (and so in key) */
                CHECK(c.sc >> (base[k] % 12u) & 1u || (t_black(k) && c.ct >> (base[k] % 12u) & 1u),
                      "%s %s: key %u %s neither in the scale nor on the chord", w->name, variant, k, t_nn(base[k]));
            if (w->factory && !strcmp(variant, "as authored"))
                for (k = 0; k < 27; k++)
                    maps_folded_factory += base[k] != (uint32_t)raw[k];
            {                                    /* the key LEDs: white keys on the chord, and the C4 key */
                uint32_t led = sk_chord_keys(), want = 1u << 7;
                for (k = 0; k < 27; k++)
                    if (!t_black(k) && c.ct >> (base[k] % 12u) & 1u)
                        want |= 1u << k;
                CHECK(led == want, "%s %s: key LEDs %07x, want %07x", w->name, variant, led, want);
            }
        }
    }
}

static uint8_t *t_variant(const tworld_t *w, int white, int black, int avoid, int fullscale, int lo, int hi, uint32_t *n)
{
    wb_ctx_t c;
    uint8_t *b = malloc(w->n);
    memcpy(b, w->b, w->n);
    *n = w->n;
    if (wb_check(b, w->n, &c) != WE_OK)
        return b;
    if (white >= 0)
        b[c.off[WF_S_KEYS] + 4] = (uint8_t)white;
    if (black >= 0)
        b[c.off[WF_S_KEYS] + 5] = (uint8_t)black;
    if (fullscale) {
        uint32_t m = SCALE_MASK[c.scale];
        b[c.off[WF_S_KEYS] + 2] = (uint8_t)m;
        b[c.off[WF_S_KEYS] + 3] = (uint8_t)(m >> 8);
    }
    if ((avoid >= 0 || lo >= 0) && c.have >> WF_S_GUARD & 1u) {
        if (avoid >= 0)
            b[c.off[WF_S_GUARD] + WF_G_AVOID] = (uint8_t)avoid;
        if (lo >= 0) {
            b[c.off[WF_S_GUARD] + 2 * c.keys] = (uint8_t)lo;
            b[c.off[WF_S_GUARD] + 2 * c.keys + 1] = (uint8_t)hi;
        }
    }
    t_crc(b);
    CHECK(wb_check(b, *n, &c) == WE_OK, "%s: the variant is a valid blob", w->name);
    return b;
}

static void test_maps(void)
{
    static const struct { const char *name; int white, black, avoid, full, lo, hi; } V[] = {
        {"as authored", -1, -1, -1, 0, -1, -1},
        {"black chord", -1, 0, -1, 0, -1, -1},
        {"black chord+9", -1, 1, -1, 0, -1, -1},
        {"white safe", 1, -1, -1, 0, -1, -1},
        {"full scale, white safe", 1, 1, -1, 1, -1, -1},
        {"full scale, white scale", 0, 1, -1, 1, -1, -1},
        {"full scale, safe, avoid none", 1, 1, 1, 1, -1, -1},
        {"full scale, safe, avoid strict", 1, 1, 2, 1, -1, -1},
        {"avoid strict, chord+9", -1, 1, 2, 0, -1, -1},
        {"range C4..B4", -1, -1, -1, 0, 60, 71},
        {"range C4..F4 (under an octave: widened)", -1, -1, -1, 0, 60, 65},
        {"range 0..127", -1, -1, -1, 0, 0, 127},
        {"range C3..C5", -1, -1, -1, 0, 48, 72},
    };
    uint32_t i, v;
    for (i = 0; i < nworlds; i++)
        for (v = 0; v < sizeof V / sizeof V[0]; v++) {
            tworld_t x = worlds[i];
            x.b = t_variant(&worlds[i], V[v].white, V[v].black, V[v].avoid, V[v].full, V[v].lo, V[v].hi, &x.n);
            t_reset();
            maps_world(&x, V[v].name);
            free(x.b);
        }
    CHECK(!maps_folded_factory, "factory Worlds: %u keys outside their range at OCT 0", maps_folded_factory);
    printf("maps: %u Worlds x 13 variants, every progression, chord, key and OCT -3..+3: %u notes as the model; the "
           "factory Worlds hold all 27 keys in range\n", nworlds, maps_checked);
}

static void test_names(void)                     /* chord names (the Studio shows them) */
{
    static const struct { const char *world; uint32_t scene; const char *names[6]; } N[] = {
        {"NEON RAIN", 1, {"Dmadd9", "Bbmaj7", "Gm7", "C"}},
        {"NEON RAIN", 2, {"Bbmaj7", "Csus4", "C", "Dmadd9", "F"}},
        {"MIDNIGHT DRIVE", 1, {"Am", "Em", "F", "G"}},
        {"FROZEN LAKE", 2, {"Amaj7", "Bsus4", "C#m7", "Eadd9"}},
        {"DUSTY CAFE", 2, {"Gm7", "C7", "Fmaj7", "Dm7"}},
    };
    uint32_t i, j, ci, done = 0;
    for (i = 0; i < sizeof N / sizeof N[0]; i++)
        for (j = 0; j < nworlds; j++) {
            if (!worlds[j].factory)
                continue;
            t_reset();
            if (world_start(worlds[j].b, worlds[j].n) || strcmp(world_name(), N[i].world))
                continue;
            world_apply(N[i].scene, 0);
            for (ci = 0; ci < 6 && N[i].names[ci]; ci++) {
                char nm[8];
                harm.ci = (uint8_t)ci;
                harm_chord_name(nm);
                CHECK(!strcmp(nm, N[i].names[ci]), "%s scene %c chord %u: \"%s\", want \"%s\"", N[i].world, 'A' + N[i].scene,
                      ci, nm, N[i].names[ci]);
                done++;
            }
        }
    {
        char nm[8] = "x";
        t_reset();
        harm_chord_name(nm);
        CHECK(!nm[0], "no World: no chord name");
    }
    CHECK(done >= 20, "chord names: only %u checked", done);
    printf("chord names: %u as written (Dmadd9, Bbmaj7, Csus4, C#m7, ...), none without a World\n", done);
}

/* ------------------------------------------------------------------ 2. the clock --- */
static void test_clock(void)
{
    uint32_t i, s, k, blocks = 0, changes = 0;
    for (i = 0; i < nworlds; i++) {
        if (!worlds[i].factory)
            continue;
        for (s = 0; s < WF_NSCENE; s++) {
            const uint8_t *pr;
            uint32_t beats, n, white0[27], prev_ci = 255, adv;
            t_start(&worlds[i]);
            ref_world();
            CHECK(world_apply(s, 0) == WE_OK, "%s: scene %c", worlds[i].name, 'A' + s);
            pr = ref_prog(wctx.b[wctx.scene[s] + 12]);
            beats = ref_beats(pr);
            t_events();
            CHECK(harm.ci == 0 && harm.ci_key == 0, "%s %c: stopped, chord 0", worlds[i].name, 'A' + s);
            for (k = 0; k < 27; k++)
                white0[k] = sk_note(k);
            adv = CTL * (uint32_t)song.g[G_BPM];
            transport_req = 1;
            n = (uint32_t)((uint64_t)(beats + 5u) * BEAT_U / adv);
            while (n--) {
                uint32_t beat, pos, ci, ck;
                rchord_t c;
                int32_t raw[27];
                t_events();
                blocks++;
                beat = clk_beat;                 /* (the clock before this block: harm_block read it) */
                pos = clk_pos;
                if (pos >= adv)
                    pos -= adv;
                else
                    beat--, pos += BEAT_U - adv;
                ci = ref_chord_at(pr, beat);
                ck = pos + BEAT_U / 8u >= BEAT_U ? ref_chord_at(pr, beat + 1u) : ci;
                CHECK(harm.ci == ci, "%s %c beat %u +%u: chord %u, want %u", worlds[i].name, 'A' + s, beat, pos, harm.ci, ci);
                CHECK(harm.ci_key == ck, "%s %c beat %u +%u: key chord %u, want %u", worlds[i].name, 'A' + s, beat, pos,
                      harm.ci_key, ck);
                if (ci != prev_ci && prev_ci != 255) {
                    changes++;
                    CHECK(pos < adv, "%s %c: the chord changed inside beat %u (+%u)", worlds[i].name, 'A' + s, beat, pos);
                }
                prev_ci = ci;
                c = ref_chord(pr, ck);
                ref_raw(&c, raw);
                for (k = 0; k < 27; k++) {
                    uint32_t got = sk_note(k);
                    CHECK(got == ref_fold(raw[k], wr.lo, wr.hi), "%s %c beat %u: key %u %s", worlds[i].name, 'A' + s, beat, k,
                          t_nn(got));
                    if (!t_black(k))
                        CHECK(got == white0[k], "%s %c: white key %u moved", worlds[i].name, 'A' + s, k);
                }
            }
            transport_req = 2;
            t_events();
            t_events();
            CHECK(harm.ci == 0 && harm.ci_key == 0, "%s %c: stopped again, chord 0", worlds[i].name, 'A' + s);
        }
    }
    CHECK(changes > 40, "clock: only %u chord changes seen", changes);
    printf("clock: every scene of every factory World through its progression: %u blocks, %u chord changes, each on "
           "its beat's first block; keys a 32nd early; black keys follow, white keys never move\n", blocks, changes);
}

/* ------------------------------------------------------------------ 3. held notes --- */
static uint32_t held_notes[27];

static void held_world(const tworld_t *w)
{
    uint32_t kt, k, keys[4], nk, i, changes = 0, prev, bars = 0, held_mask = 0, vnote[NVOICE], vheld[NVOICE];
    t_start(w);
    ref_world();
    kt = wrt.keys_trk;
    t_mute_others(1);
    t_play();
    /* hold a black and a white key (or more, up to max_poly), at the second beat */
    nk = wr.poly < 3u ? wr.poly : 3u;
    keys[0] = 3;                                 /* G#3 (black) */
    keys[1] = 11;                                /* E4 (white) */
    keys[2] = 20;                                /* C#5 (black) */
    while (clk_beat < 1u)
        t_block();
    for (i = 0; i < nk; i++)
        held_mask |= 1u << keys[i];
    t_keys(held_mask);
    t_block();
    for (i = 0; i < nk; i++) {
        k = keys[i];
        CHECK(kb_kind[k] == KS_NOTE && kb_n[k] == 1, "%s: key %u sounds", w->name, k);
        held_notes[k] = kb_nt[k][0];
        CHECK(held_notes[k] == sk_note(k), "%s: key %u plays %s, sk_note %s", w->name, k, t_nn(held_notes[k]), t_nn(sk_note(k)));
    }
    for (i = 0; i < NVOICE; i++) {               /* the voices the keys started, and their notes */
        vnote[i] = trk[kt].v[i].note;
        vheld[i] = trk[kt].v[i].active && trk[kt].v[i].gate;
    }
    CHECK(trk[kt].p[P_VOICE] != V_POLY || t_gated(kt, -1) == nk, "%s: %u voices for %u keys", w->name, t_gated(kt, -1), nk);
    /* across at least 3 chord changes (and the progression's end: it loops) */
    prev = harm.ci;
    while (changes < 3u && bars < 40u) {
        t_block();
        if (harm.ci != prev) {
            changes++;
            prev = harm.ci;
            for (i = 0; i < nk; i++) {           /* each key: its note, still held once (vref, vlive) */
                uint32_t n;
                k = keys[i];
                n = held_notes[k];
                CHECK(kb_nt[k][0] == n, "%s: key %u's note changed", w->name, k);
                CHECK(vref[kt][n] == 1 && vlive[kt][n] == 1, "%s: key %u's %s counted %u/%u after a chord change", w->name,
                      k, t_nn(n), vref[kt][n], vlive[kt][n]);
                CHECK(trk[kt].p[P_VOICE] == V_POLY || trk[kt].nmono == nk, "%s: the mono stack holds %u of %u keys",
                      w->name, trk[kt].nmono, nk);
            }
            for (i = 0; i < NVOICE; i++) {       /* no voice re-pitched, none sounding a note no key holds */
                const voice_t *v = &trk[kt].v[i];
                uint32_t j, ok = 0;
                if (!v->active || !v->gate)
                    continue;
                for (j = 0; j < nk; j++)
                    ok |= v->note == held_notes[keys[j]];
                CHECK(ok, "%s: voice %u sounds %s, which no held key plays", w->name, i, t_nn(v->note));
                CHECK(!vheld[i] || v->note == vnote[i], "%s: voice %u re-pitched %s -> %s", w->name, i, t_nn(vnote[i]),
                      t_nn(v->note));
            }
        }
        if (clk_pos < CTL * (uint32_t)song.g[G_BPM] && !(clk_beat & 3u))
            bars++;
    }
    CHECK(changes >= 3u, "%s: only %u chord changes", w->name, changes);
    /* a key pressed now plays the new chord; the held ones stay */
    t_keys(held_mask | 1u << 25);
    t_block();
    if (kb_n[25]) {
        uint32_t n25 = kb_nt[25][0];
        CHECK(n25 == sk_note(25) || harm.ci_key != harm.ci, "%s: a new key plays the chord now", w->name);
        CHECK(hprog[hcur].ct[harm.ci_key] >> (n25 % 12u) & 1u || skm[skcur].bs[harm.ci_key] >> (n25 % 12u) & 1u,
              "%s: key 25 %s off the chord", w->name, t_nn(n25));
    }
    /* release: exactly what sounded */
    {
        uint32_t mo0 = mo_w, j, offs = 0;
        t_keys(0);
        t_block();
        for (j = mo0; j != mo_w; j++) {          /* (MIDI out: one note-off per held note, the notes that sounded) */
            uint32_t pkt = midi_out_q[j % MQ], nt = pkt >> 16 & 127u;
            int ok = 0;
            if ((pkt >> 8 & 0xF0u) != 0x80u)
                continue;
            offs++;
            for (i = 0; i < nk; i++)
                ok |= nt == held_notes[keys[i]];
            ok |= kb_n[25] == 0 || nt == kb_nt[25][0];
            CHECK(ok, "%s: a note-off for %s, which no key sounded", w->name, t_nn(nt));
        }
        (void)offs;
    }
    CHECK(t_gated(kt, -1) == 0, "%s: %u voices still gated after the release", w->name, t_gated(kt, -1));
    CHECK(t_kcounts() == 0, "%s: %u counts left", w->name, t_kcounts());
    t_blocks(5 * 44100 / CTL);                   /* the release tails */
    CHECK(t_active(kt) == 0, "%s: %u voices still sounding 5 s after the release", w->name, t_active(kt));
    t_stop();
}

static void test_held(void)
{
    uint32_t i, kt;
    for (i = 0; i < nworlds; i++)
        if (worlds[i].factory)
            held_world(&worlds[i]);
    /* across an OCT change and a scene change on the bar (world_request, playing): the held note stays */
    for (i = 0; i < nworlds; i++) {
        uint32_t n, s, t0;
        if (!worlds[i].factory)
            continue;
        t_start(&worlds[i]);
        kt = wrt.keys_trk;
        t_mute_others(1);
        t_play();
        t_keys(1u << 9);                         /* D4 key */
        t_block();
        n = kb_nt[9][0];
        song.octave = 2;
        s = (wrt.scene + 1u) % WF_NSCENE;
        CHECK(world_request(s, 0) == WE_OK, "%s: scene request while playing", worlds[i].name);
        for (t0 = 0; t0 < 9u * t_bar_blocks() && wrt.scene != s; t0++)   /* (its transition: up to 8 bars) */
            t_block();
        world_service();
        t_mute_others(1);
        CHECK(wrt.scene == s, "%s: the scene changed on the bar", worlds[i].name);
        CHECK(kb_nt[9][0] == n && vref[kt][n] == 1 && t_gated(kt, -1) == t_gated(kt, (int)n),
              "%s: the held key's %s through the scene change", worlds[i].name, t_nn(n));
        t_keys(0);
        t_block();
        CHECK(t_gated(kt, -1) == 0 && t_kcounts() == 0, "%s: released after OCT + scene change", worlds[i].name);
        song.octave = 0;
        t_stop();
    }
    printf("held notes: keys held over 3+ chord changes, an OCT change and a scene change keep their pitch and voice; "
           "the release ends exactly them (MIDI out too); no gate, count or voice left\n");
}

/* ------------------------------------------------------------------ 4. same pitch --- */
static const tworld_t *t_find(const char *name)
{
    uint32_t i;
    for (i = 0; i < nworlds; i++)
        if (!strcmp(worlds[i].name, name))
            return &worlds[i];
    return &worlds[0];
}

static int t_pair(uint32_t *a, uint32_t *b)      /* a black and a white key that play the same note now */
{
    uint32_t i, j;
    for (i = 0; i < 27; i++)
        for (j = 0; j < 27; j++)
            if (t_black(i) && !t_black(j) && sk_note(i) == sk_note(j)) {
                *a = i;
                *b = j;
                return 1;
            }
    return 0;
}

static void same_two_keys(int refcount, uint32_t voice)
{
    uint32_t a = 0, b = 0, kt, n;
    t_start(t_find("NEON RAIN"));
    kt = wrt.keys_trk;
    wrt.refcount = (uint8_t)refcount;
    trk[kt].p[P_VOICE] = (int16_t)voice;
    t_mute_others(1);
    CHECK(t_pair(&a, &b), "a black and a white key on one note");
    n = sk_note(a);
    t_keys(1u << a);
    t_block();
    t_keys(1u << a | 1u << b);
    t_block();
    CHECK(kb_nt[a][0] == n && kb_nt[b][0] == n, "two keys on %s", t_nn(n));
    CHECK(t_gated(kt, (int)n) == 1, "one shared voice for %s (%u)", t_nn(n), t_gated(kt, (int)n));
    if (refcount)
        CHECK(vref[kt][n] == 2 && vlive[kt][n] == 2, "counted twice (%u, %u)", vref[kt][n], vlive[kt][n]);
    t_keys(1u << b);                             /* the first key up */
    t_block();
    if (refcount)
        CHECK(t_gated(kt, (int)n) == 1, "voice mode %u: the other key keeps %s sounding", voice, t_nn(n));
    else
        CHECK(t_gated(kt, (int)n) == 0, "refcount off (SLOOP): the first release cuts %s", t_nn(n));
    t_keys(0);
    t_block();
    CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "voice mode %u, refcount %d: all released", voice, refcount);
}

static void same_key_and_seq(int refcount)
{
    uint32_t kt, n, k, i, cut = 0, seq_on = 0, ci, safe = 0xFFF;
    track_t *t;
    t_start(t_find("MIDNIGHT DRIVE"));
    kt = wrt.keys_trk;
    t = &trk[kt];
    wrt.refcount = (uint8_t)refcount;
    t_mute_others(1);
    for (ci = 0; ci < hprog[hcur].n; ci++)       /* a note safe over every chord: the loop plays it as written (an */
        safe &= hprog[hcur].safe[ci];            /* avoid note would follow the chord, guard.c H12) */
    for (k = 9; k < 27u && !(safe >> (sk_note(k) % 12u) & 1u); k++)
        ;
    CHECK(k < 27u, "a key safe over every chord");
    n = sk_note(k % 27u);
    for (i = 0; i < NSTEP; i++) {                /* the keys loop: that note on every 4th step, a short gate */
        memset(&t->step[i], 0, sizeof t->step[i]);
        t->step[i].time = i % 4u ? ST_REST : ST_NOTE;
        t->step[i].n = i % 4u ? 0 : 1;
        t->step[i].note[0] = (uint8_t)n;
        t->step[i].vel = 100;
    }
    t->p[P_SGATE] = 30;
    t_keys(1u << k);
    t_block();
    t_play();
    for (i = 0; i < t_bar_blocks(); i++) {
        t_block();
        seq_on |= t->seq_n != 0;
        cut += t_gated(kt, (int)n) == 0;
    }
    CHECK(seq_on, "the sequencer played %s", t_nn(n));
    if (refcount)
        CHECK(!cut, "refcount: the held key's %s cut by the sequencer's gate for %u blocks", t_nn(n), cut);
    else
        CHECK(cut, "refcount off (SLOOP): the sequencer's gate cuts the held key's note");
    t_keys(0);
    t_stop();
    t_block();
    CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "refcount %d: key and sequencer released", refcount);
}

static void same_seq_paths(void)                 /* slides, ratchets with a live note, arp, engine switch, panic */
{
    uint32_t kt, i, n;
    track_t *t;
    t_start(t_find("DUSTY CAFE"));
    kt = wrt.keys_trk;
    t = &trk[kt];
    t_mute_others(1);
    n = sk_note(9);
    for (i = 0; i < NSTEP; i++) {                /* a slide chain on one note, a held-over chord, then rests */
        memset(&t->step[i], 0, sizeof t->step[i]);
        t->step[i].time = ST_REST;
    }
    for (i = 0; i < 6; i++) {
        t->step[i].time = ST_NOTE;
        t->step[i].n = i < 4 ? 1 : 2;
        t->step[i].note[0] = (uint8_t)n;
        t->step[i].note[1] = (uint8_t)(n + 4u);
        t->step[i].flags = SF_SLIDE;
        t->step[i].vel = 100;
    }
    t->step[8].time = ST_NOTE;                   /* a ratchet x4 on the note a key holds */
    t->step[8].n = 1;
    t->step[8].note[0] = (uint8_t)n;
    t->step[8].rat = 3;
    t_play();
    for (i = 0; i < t_bar_blocks() / 2u; i++) {
        if (clk_beat == 1u && !fm1_in.notes)
            t_keys(1u << 9);                     /* (the key on the ratchet's note, from step 4) */
        t_block();
    }
    t_keys(0);
    t_block();
    t_stop();
    t_block();
    CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "slides and ratchets: %u gated, %u counts left", t_gated(kt, -1),
          t_counts());
    /* PULSE: one key held, the arp plays it and the chord's tones above it */
    t_start(t_find("DUSTY CAFE"));
    t = &trk[wrt.keys_trk];
    t_mute_others(1);
    t->p[P_AMODE] = 1;
    t->p[P_ARATE] = 3;
    t_play();
    t_keys(1u << 11);
    t_block();
    {
        uint32_t held = t->held[0], seen = 0, bad = 0;
        for (i = 0; i < 2u * t_bar_blocks(); i++) {
            t_block();
            if (t->arp_note) {
                uint32_t a = t->arp_note;
                seen |= 1u << (a - held < 32u ? a - held : 31u);
                bad += a != held && (!(harm.ct >> (a % 12u) & 1u) || a < held || a > skm[skcur].g.hi);
            }
        }
        CHECK(t->nheld == 1 && held == kb_nt[11][0], "PULSE: one key held");
        CHECK(!bad, "PULSE: %u arp notes neither the held note nor a chord tone above it", bad);
        CHECK(seen & 1u && (seen & ~1u), "PULSE: the arp plays the held note and chord tones above it (%x)", seen);
    }
    t_keys(0);
    t_blocks(t_bar_blocks() / 4u);
    t_stop();
    t_block();
    CHECK(t_gated(wrt.keys_trk, -1) == 0 && t_counts() == 0, "PULSE: released, no count left");
    /* a PULSE key over a sequencer note of the same pitch: the key's release ends no count of the sequencer's (the
     * arp's notes end with the arp: input_off releases only what input_on started, vlive) */
    t_start(t_find("DUSTY CAFE"));
    t = &trk[wrt.keys_trk];
    kt = wrt.keys_trk;
    t_mute_others(1);
    n = sk_note(9);
    for (i = 0; i < NSTEP; i++) {                /* the keys loop holds that note (a NOTE, then ties) */
        memset(&t->step[i], 0, sizeof t->step[i]);
        t->step[i].time = i ? ST_TIE : ST_NOTE;
    }
    t->step[0].n = 1;
    t->step[0].note[0] = (uint8_t)n;
    t->step[0].vel = 100;
    t->p[P_AMODE] = 1;
    t->p[P_ARATE] = 2;
    t_play();
    t_blocks(20);
    CHECK(t->seq_n == 1 && t->seq_notes[0] == n && vref[kt][n] == 1, "the loop holds %s", t_nn(n));
    t_keys(1u << 9);
    t_blocks(t_bar_blocks() / 2u);
    CHECK(t->nheld == 1 && vlive[kt][n] == 0, "PULSE: the key went to the arp");
    t_keys(0);
    t_blocks(t_bar_blocks() / 4u);
    CHECK(t->seq_n == 1 && vref[kt][n] == 1 && t_gated(kt, (int)n) == 1,
          "PULSE key released over the loop's %s: the loop's note cut (count %u, gated %u)", t_nn(n), vref[kt][n],
          t_gated(kt, (int)n));
    t_stop();
    t_block();
    CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "PULSE over the loop: released, no count left");
    /* an engine switch while keys are held (notes queued over the fade), and a panic with keys down */
    t_start(t_find("FROZEN LAKE"));
    kt = wrt.keys_trk;
    t = &trk[kt];
    t_mute_others(1);
    t_keys(1u << 9);
    t_block();
    host_preset_req(t, (t->engine + 1u) % NENGINES, 0);   /* (the UI's preset load: eng_req and its parameters) */
    t_keys(1u << 9 | 1u << 11);
    events_block(CTL);                           /* (the new key arrives during the fade) */
    t_keys(1u << 11);
    t_blocks(10);
    CHECK(t->engine == t->eng_req, "the engine switched");
    CHECK(t_gated(kt, (int)kb_nt[11][0]) == 1, "the key pressed during the fade sounds on the new engine");
    t_keys(0);
    t_block();
    CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "engine switch: released, no count left");
    t_keys(1u << 9 | 1u << 12);
    t_block();
    panic_req = 15;
    t_block();
    CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "panic: everything released, the counts cleared");
    t_keys(1u << 12);                            /* (a release after the panic: nothing to end) */
    t_block();
    t_keys(1u << 12 | 1u << 16);                 /* a new press works and ends */
    t_block();
    CHECK(t_gated(kt, (int)kb_nt[16][0]) == 1, "a key after the panic sounds");
    t_keys(0);
    t_block();
    CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "after the panic: released, no count left");
}

static void test_same_pitch(void)
{
    same_two_keys(1, V_POLY);
    same_two_keys(1, V_LEGATO);
    same_two_keys(1, V_MONO);
    same_two_keys(0, V_POLY);
    same_key_and_seq(1);
    same_key_and_seq(0);
    same_seq_paths();
    printf("same pitch: two keys and key + sequencer share one voice, the last release ends it (POLY, LEGATO, MONO); "
           "refcount off: SLOOP's cut; slides, ratchets, PULSE, engine switch, panic: no count left\n");
}

/* ------------------------------------------------------------------ 5. octaves --- */
static void test_octaves(void)
{
    uint32_t i, k, edges = 0;
    int oct;
    for (i = 0; i < nworlds; i++) {
        uint32_t kt, n0[27];
        if (!worlds[i].factory)
            continue;
        t_start(&worlds[i]);
        ref_world();
        kt = wrt.keys_trk;
        t_mute_others(1);
        for (k = 0; k < 27; k++)
            n0[k] = sk_note(k);
        for (oct = -3; oct <= 3; oct++) {        /* the lowest and highest keys pressed at every OCT */
            song.octave = (int8_t)oct;
            for (k = 0; k < 27; k += 26) {
                uint32_t n, want = (uint32_t)((int32_t)n0[k] + 12 * oct);
                t_keys(1u << k);
                t_block();
                n = kb_nt[k][0];
                CHECK(n >= wr.lo && n <= wr.hi && n % 12u == n0[k] % 12u, "%s OCT %d key %u: %s", worlds[i].name, oct, k, t_nn(n));
                if (want >= wr.lo && want <= wr.hi)
                    CHECK(n == want, "%s OCT %d key %u: %s, want %s (in range)", worlds[i].name, oct, k, t_nn(n), t_nn(want));
                else {
                    edges++;
                    CHECK(n != want && (want > wr.hi ? n + 12u > wr.hi : n < wr.lo + 12u),
                          "%s OCT %d key %u: %s folds to the nearest octave in range", worlds[i].name, oct, k, t_nn(n));
                }
                CHECK(t_gated(kt, (int)n) == 1, "%s OCT %d: key %u sounds", worlds[i].name, oct, k);
                t_keys(0);
                t_block();
                CHECK(t_gated(kt, -1) == 0, "%s OCT %d: key %u released", worlds[i].name, oct, k);
            }
        }
        song.octave = 0;
    }
    CHECK(edges > 4, "octaves: only %u folds at the edges", edges);
    printf("octaves: the lowest and highest keys at OCT -3..+3 in every factory World: in range, in key, %u folds at the "
           "edges, each to the nearest octave inside\n", edges);
}

/* ------------------------------------------------------------------ 6. polyphony --- */
static uint32_t t_admitted(void)
{
    uint32_t k, n = 0;
    for (k = 0; k < 27; k++)
        n += kb_kind[k] == KS_NOTE && kb_n[k];
    return n;
}

static void test_poly(void)
{
    uint32_t i, b, worst = 0, worst_busy = 0;
    for (i = 0; i < nworlds; i++) {
        uint32_t kt, poly;
        if (!worlds[i].factory)
            continue;
        t_start(&worlds[i]);
        ref_world();
        kt = wrt.keys_trk;
        poly = wr.poly;
        t_play();
        t_keys((1u << 27) - 1u);                 /* all 27 at once, with the World playing */
        t_block();
        CHECK(t_admitted() == poly, "%s: %u keys sound, max_poly %u", worlds[i].name, t_admitted(), poly);
        CHECK(t_gated(kt, -1) <= poly && t_busy() <= NVOICE, "%s: %u gated, %u busy", worlds[i].name, t_gated(kt, -1), t_busy());
        t_blocks(200);
        t_keys(0);
        t_block();
        CHECK(t_gated(kt, -1) == 0, "%s: after the mash %u gated", worlds[i].name, t_gated(kt, -1));
        for (b = 0; b < 6000; b++) {             /* random mashing: a few keys change every few blocks */
            uint32_t m = fm1_in.notes, a, g;
            if (!(t_rnd() % 6u)) {
                for (a = t_rnd() % 4u; a; a--)
                    m ^= 1u << (t_rnd() % 27u);
                if (!(t_rnd() % 50u))
                    m = t_rnd() & ((1u << 27) - 1u);
                t_keys(m);
            }
            t_block();
            g = t_gated(kt, -1);
            CHECK(t_admitted() <= poly, "%s: %u keys admitted", worlds[i].name, t_admitted());
            CHECK(g <= poly, "%s: %u voices gated on the keys track, max_poly %u", worlds[i].name, g, poly);
            CHECK(t_busy() <= NVOICE, "%s: %u voices busy", worlds[i].name, t_busy());
            worst = g > worst ? g : worst;
            worst_busy = t_busy() > worst_busy ? t_busy() : worst_busy;
        }
        t_keys(0);
        t_block();
        t_stop();
        t_block();
        CHECK(t_gated(kt, -1) == 0 && t_counts() == 0, "%s: mash released (%u gated, %u counts)", worlds[i].name,
              t_gated(kt, -1), t_counts());
        t_blocks(5 * 44100 / CTL);
        CHECK(t_active(kt) == 0, "%s: %u keys-track voices left 5 s after", worlds[i].name, t_active(kt));
    }
    printf("polyphony: all 27 keys at once and 6000 blocks of mashing per factory World: never over max_poly (most %u "
           "gated) or the voice budget (most %u busy); nothing left after\n", worst, worst_busy);
}

/* ------------------------------------------------------------------ 7. MIDI in --- */
static void test_midi(void)
{
    uint32_t i, v, kt, ch, mapped = 0;
    for (i = 0; i < nworlds; i++) {
        if (!worlds[i].factory)
            continue;
        t_start(&worlds[i]);
        ref_world();
        kt = wrt.keys_trk;
        t_mute_others(1);
        for (ch = 0; ch < 2; ch++) {
            uint32_t c = ch ? 6u : kt;           /* the keys track's own channel, and a non-part channel (selected) */
            for (v = 0; v < 128; v++) {
                rchord_t cc = ref_chord(ref_prog(wrt.prog), harm.ci_key);
                uint32_t want = ref_vnote(&cc, v);
                t_midi(c, 1, v, 90);
                t_block();
                CHECK(sk_midi[v] == want + 1u, "%s ch %u: MIDI %u -> %s, want %s", worlds[i].name, c + 1, v,
                      t_nn(sk_midi[v] - 1u), t_nn(want));
                CHECK(t_gated(kt, (int)want) == 1, "%s: MIDI %u sounds %s", worlds[i].name, v, t_nn(want));
                t_midi(c, 0, v, 0);
                t_block();
                CHECK(t_gated(kt, -1) == 0 && t_counts() == 0 && !sk_midi[v], "%s: MIDI %u released", worlds[i].name, v);
                mapped++;
            }
        }
        /* note-off after a chord change: the note that sounded ends */
        t_play();
        {
            uint32_t n0, ci0 = harm.ci, guard = 0;
            t_midi(kt, 1, 66, 100);              /* F#4: a black key, the chord's */
            t_block();
            n0 = sk_midi[66] - 1u;
            while (harm.ci == ci0 && guard++ < 8u * t_bar_blocks())
                t_block();
            CHECK(harm.ci != ci0, "%s: the chord changed", worlds[i].name);
            CHECK(vref[kt][n0] == 1 && sk_midi[66] == n0 + 1u, "%s: MIDI note held over the chord change", worlds[i].name);
            t_midi(kt, 0, 66, 0);
            t_block();
            CHECK(t_gated(kt, -1) == 0 && t_kcounts() == 0, "%s: MIDI note-off after the chord change ends %s", worlds[i].name,
                  t_nn(n0));
        }
        /* the polyphony cap, a repeated note-on, velocity, release of all */
        for (v = 60; v < 60u + wr.poly + 2u; v++)
            t_midi(kt, 1, v, 70);
        t_midi(kt, 1, 60, 110);                  /* (again, without a note-off) */
        t_block();
        {
            uint32_t held = 0, n;
            for (n = 0; n < 128; n++)
                held += sk_midi[n] != 0;
            CHECK(held == wr.poly, "%s: %u MIDI notes held, max_poly %u", worlds[i].name, held, wr.poly);
            CHECK(t_gated(kt, -1) <= wr.poly, "%s: %u gated", worlds[i].name, t_gated(kt, -1));
        }
        for (v = 60; v < 60u + wr.poly + 2u; v++)
            t_midi(kt, 0, v, 0);
        t_block();
        CHECK(t_gated(kt, -1) == 0 && t_kcounts() == 0, "%s: all MIDI released", worlds[i].name);
        t_stop();
        t_blocks(10);
        /* another part's channel: SLOOP's raw notes */
        {
            uint32_t other = (kt + 1u) % NPART;
            trk[other].p[P_MUTE] = 0;
            t_midi(other, 1, 61, 100);
            t_block();
            CHECK(t_gated(other, 61) >= 1, "%s: MIDI on part %u's channel raw", worlds[i].name, other + 1);
            t_midi(other, 0, 61, 0);
            t_block();
            CHECK(t_gated(other, 61) == 0, "%s: MIDI on part %u released", worlds[i].name, other + 1);
        }
        t_stop();
    }
    printf("MIDI in: %u notes as virtual keys (the keys track's channel and a non-part channel), note-off after a "
           "chord change, the polyphony cap, a repeated note-on, other parts raw\n", mapped);
}

/* ------------------------------------------------------------------ 8. SLOOP --- */
static void test_sloop(void)
{
    uint32_t k, kt;
    t_reset();
    CHECK(!sk_on(), "no World: Smart Keys off");
    song.sel = 0;
    trk[0].p[P_QUANT] = 2;                       /* SLOOP's WHITE mode: black keys silent */
    trk[0].p[P_SCALE] = 2;
    for (k = 0; k < 27; k++) {
        uint32_t want = kb_map(&trk[0], k);
        t_keys(1u << k);
        t_block();
        CHECK(want == KB_SILENT ? kb_n[k] == 0 : kb_nt[k][0] == want && kb_trk[k] == 0, "SLOOP key %u", k);
        t_keys(0);
        t_block();
    }
    t_midi(5, 1, 61, 100);                       /* a non-part channel: the selected track, raw */
    t_block();
    CHECK(t_gated(0, 61) == 1 && !sk_midi[61], "SLOOP: MIDI raw");
    t_midi(5, 0, 61, 0);
    t_block();
    CHECK(t_gated(0, -1) == 0, "SLOOP: MIDI released");
    /* a World, then world_unload: SLOOP's paths again */
    t_start(&worlds[0]);
    kt = wrt.keys_trk;
    CHECK(sk_on() && wrt.refcount && wrt.keys_on, "a World: Smart Keys and counts on");
    world_unload();
    CHECK(!sk_on() && !wrt.refcount && !wrt.keys_on, "world_unload: off");
    song.sel = (uint8_t)((kt + 1u) % NPART);
    t_keys(1u << 4);
    t_block();
    CHECK(kb_trk[4] == song.sel && kb_nt[4][0] == kb_map(&trk[song.sel], 4), "after world_unload: kb_map on the selected track");
    t_keys(0);
    t_block();
    /* ADV_WORLD (Phase 14): keys_on = 0, the World still active: SLOOP's kb_map, the counts stay on */
    t_start(&worlds[0]);
    wrt.keys_on = 0;
    song.sel = 0;
    t_keys(1u << 4);
    t_block();
    CHECK(kb_trk[4] == 0 && kb_nt[4][0] == kb_map(&trk[0], 4), "keys_on 0: kb_map on the selected track");
    t_keys(0);
    t_block();
    CHECK(t_counts() == 0, "keys_on 0: counted and released");
    printf("SLOOP: no World, after world_unload and with keys_on 0 the keys and MIDI take SLOOP's paths\n");
}

/* ------------------------------------------------------------------ 9. fuzz --- */
static uint32_t fuzz_blocks = 12000;            /* SK_FUZZ=n blocks per World, SK_SEED=s (a longer search) */
static void test_fuzz(void)
{
    uint32_t i, b, presses = 0, refused = 0, bad = 0, scenes = 0, midis = 0, arps = 0;
    for (i = 0; i < nworlds; i++) {
        uint32_t kt, prev = 0;
        if (!worlds[i].factory)
            continue;
        t_start(&worlds[i]);
        ref_world();
        kt = wrt.keys_trk;
        t_play();
        for (b = 0; b < fuzz_blocks; b++) {
            uint32_t r = t_rnd(), m = fm1_in.notes, k, down, cnt = 0;
            if (r % 5u == 0) {                   /* a key down or up; a player holds a few at a time */
                k = t_rnd() % 27u;
                for (down = m; down; down &= down - 1u)
                    cnt++;
                if (m >> k & 1u)
                    m &= ~(1u << k);
                else if (cnt < 1u + t_rnd() % 5u)
                    m |= 1u << k;
                else
                    m &= m - 1u;
                t_keys(m);
            }
            if (r % 2000u == 5)                  /* PULSE on / off (the keys track's arp, H11) */
                trk[kt].p[P_AMODE] = (int16_t)(t_rnd() % 3u ? 0u : 1u + t_rnd() % 3u);
            if (r % 97u == 1) {
                song.octave = (int8_t)((int)(t_rnd() % 7u) - 3);
            }
            if (r % 41u == 2) {                  /* MIDI: some held notes end, a new one starts */
                uint32_t v = 36u + t_rnd() % 60u, j;
                for (j = 0; j < 128u; j++)
                    if (sk_midi[j] && t_rnd() % 2u)
                        t_midi(t_rnd() & 1u ? kt : 7u, 0, j, 0), midis++;
                if (!sk_midi[v])
                    t_midi(t_rnd() & 1u ? kt : 7u, 1, v, 30u + t_rnd() % 90u), midis++;
            }
            if (r % 1500u == 3) {
                world_request(t_rnd() % WF_NSCENE, t_rnd() % wctx.cnt[WF_S_VARS]);
                scenes++;
            }
            if (r % 4000u == 4)
                transport_req = song.playing ? 2 : 1;
            t_block();
            world_service();
            down = fm1_in.notes & ~prev;
            prev = fm1_in.notes;
            for (k = 0; k < 27; k++)
                if (down >> k & 1u && !kb_n[k])
                    refused++;
                else if (down >> k & 1u && kb_kind[k] == KS_NOTE) {
                    uint32_t n = kb_nt[k][0], ci = harm.ci_key < skm[skcur].n ? harm.ci_key : 0u;
                    uint32_t rel = (n + 12u - skm[skcur].tonic % 12u) % 12u;
                    int ok = t_black(k) ? skm[skcur].bs[ci] >> (n % 12u) & 1u : skm[skcur].wm[ci] >> rel & 1u;
                    presses++;
                    bad += !ok || n < skm[skcur].g.lo || n > skm[skcur].g.hi;
                }
            if (trk[kt].arp_note) {              /* PULSE: the held notes and the chord's tones */
                uint32_t a = trk[kt].arp_note % 12u, allowed = hprog[hcur].scale | harm.ct, c;
                for (c = 0; c < skm[skcur].n; c++)
                    allowed |= skm[skcur].bs[c];
                arps++;
                bad += !(allowed >> a & 1u);
            }
            CHECK(t_busy() <= NVOICE, "%s: %u voices", worlds[i].name, t_busy());
            CHECK(t_admitted() <= wr.poly, "%s: %u keys over max_poly", worlds[i].name, t_admitted());
        }
        trk[kt].p[P_AMODE] = 0;
        t_keys(0);
        for (b = 0; b < 128; b++)
            if (sk_midi[b])
                t_midi(kt, 0, b, 0);
        t_block();
        t_stop();
        t_block();
        world_service();
        {
            uint32_t t, g = 0;
            for (t = 0; t < NPART; t++)
                g += t_gated(t, -1);
            CHECK(g == 0 && t_counts() == 0, "%s: after the fuzz %u gated, %u counts", worlds[i].name, g, t_counts());
        }
        t_blocks(6 * 44100 / CTL);
        CHECK(t_active(kt) == 0, "%s: %u keys-track voices left", worlds[i].name, t_active(kt));
        song.octave = 0;
    }
    CHECK(!bad, "fuzz: %u key notes off the scale / chord / range", bad);
    printf("fuzz: %u blocks per factory World (keys, %u MIDI events, OCT, PULSE, %u scene requests, STOP / PLAY): %u "
           "key notes (%u more refused by max_poly) and %u arp blocks, every note in scale or chord and in range; nothing left "
           "sounding, no count left\n", fuzz_blocks, midis, scenes, presses, refused, arps);
}

int main(int argc, char **argv)
{
    uint32_t i;
    if (getenv("SK_SEED"))
        t_rs = (uint32_t)strtoul(getenv("SK_SEED"), 0, 0);
    if (getenv("SK_FUZZ"))
        fuzz_blocks = (uint32_t)strtoul(getenv("SK_FUZZ"), 0, 0);
    for (i = 0; i < WORLD_NFACTORY && nworlds < 64; i++) {
        const uint8_t *b;
        uint32_t n;
        if (!world_picked(WORLD_INDEX[i].id))
            continue;
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
    for (i = 1; i < (uint32_t)argc && nworlds < 64; i++) {
        worlds[nworlds].b = t_read(argv[i], &worlds[nworlds].n);
        snprintf(worlds[nworlds].name, sizeof worlds[nworlds].name, "%s", strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i]);
        nworlds++;
    }
    test_maps();
    test_names();
    test_clock();
    test_held();
    test_same_pitch();
    test_octaves();
    test_poly();
    test_midi();
    test_sloop();
    test_fuzz();
    printf("smartkeys test: %u checks, %s\n", t_checks, t_fails ? "FAILED" : "all passed");
    return t_fails ? 1 : 0;
}
