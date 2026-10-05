/* SPDX-License-Identifier: GPL-3.0-only */
/* world_render: a compiled Musical World through the real firmware, to a WAV and a measurement: the audition of
 * the factory Worlds until the host renderer learns Worlds. Built as tests/world_test.c is (FELUCCA_WORLD=1 on
 * tests/hostsim.c); tests/run_tests.sh runs it on every factory World:
 *
 *   build/host/world_render [options] WORLD.wblob
 *     --scene A..D | all   the scene (default: the World's)
 *     --var NAME | all     the variation (default: the World's)
 *     --bars N             bars of 4/4 played (default 8)
 *     --energy X           the ENERGY position 0..1 (default: the World's), plus the variation's bias
 *     --ctl C=X,..         macro positions (COLOR MOTION SPACE ENERGY, or the later controls), 0..1 or 0..100
 *     --raw                no ENERGY arrangement: every pattern of the scene plays, as the Phase 5 firmware did
 *     --keys               a phrase on the Smart Keys track (written as a recorded loop: the player's lead)
 *     --mash               a beginner mashing the 27 keys live (Smart Keys, Phase 6): on most eighth notes one or two
 *                          random keys, each held one to four eighths (--seed N: another player); every key note must
 *                          be in the World's key and range, and nothing may hang after the release
 *     --tail S             seconds after STOP (default 6)
 *     --wav OUT.wav        the audio of a single render
 *     --sequence OUT.wav   scenes A, B, C, D while playing: each asked for in the N-th bar of the one before and
 *                          committed on its transition's bar line (Phase 11), the bar printed
 *     --bands              every scene at the start of each of its ENERGY bands (ORIGINAL)
 *     --macros             each macro / control mapping over every scene x variation (world_stage's bases): the
 *                          effective value at 0 and at 1 against the parameter's range (design 6.4: inside it,
 *                          and 100 % is never the maximum)
 *     --extremes           the default scene and variation with each macro at 0 and at 1 (the others at the
 *                          World's defaults), ENERGY at 0.5, all four at 0 and all at 1: clean (peak under
 *                          -0.5 dBFS, nothing at full scale, the tail under -60 dBFS within the 6 s after STOP), and
 *                          ENERGY's gain compensation: from 0 to 1 more notes and hits, the loudness at most
 *                          EXT_LU_HALF LU over 0.5 and EXT_LU_SPAN LU over 0 (design 5.6); and no corner far quieter
 *                          than the World's defaults (at most EXT_LU_FLOOR under them: dark and sparse, not silent)
 *     --mute T,..          tracks kept silent, 1..4 (gain staging: a part alone)
 *     --check              exit 1 when a check fails
 *
 * A render: wb_check, world_load, the macro positions (macro.c: the World's defaults, --energy, --ctl) and
 * world_apply (stage + commit while stopped: the real world.c, which evaluates the macros' target table and commits
 * the scene's ENERGY band: arrange.c's layers, drum lanes, density and play masks, ratchets, the fill on the last
 * bar of each phrase), PLAY through the transport (transport_req, as the panel asks), N bars of mix_block (the
 * macro overlay in it, H4 / H5), STOP, the tail. Each render runs in a fork of the booted process, so each starts
 * from the same state (no reverb, delay or voice left over from the one before).
 *
 * Measured over the bars played: peak, RMS, loudness (BS.1770: K-weighted, gated, LUFS), DC, samples at and
 * within 0.1 dB of full scale, the longest silence, the limiter (the time it takes more than 1 dB and more than
 * 6 dB, its largest reduction); the most voices sounding (per part and in all), the voices given up and the held
 * notes among them; notes and hits per track (what the sequencer played), their range, in the World's scale, on
 * the sounding chord. After STOP and the tail: voices still sounding, the peak of the last 0.25 s.
 * Checks: heard (RMS above -45 dBFS, no silence over 2 s while playing), peak at most -1 dBFS, no sample at full
 * scale, the limiter over 6 dB at most 5 % of the time (design 12.3), |DC| at most 0.001, no voice left after the
 * tail and its end below -60 dBFS, at most 8 voices and no held note stolen (without --keys: the World alone
 * leaves room for the player), every synth note in the scale, bass in E1..G3, pad / chords / keys in E2..E6,
 * texture in C3..C7; with --keys the lead in C3..C7; with --bands the notes and drum hits never fall as ENERGY
 * rises (the bands only add), and the loudness falls by at most BAND_LU from one band to the next (ENERGY's gain
 * compensation, design 5.6, may take back what a layer adds, not more). */
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

static const char *const ROLE[WF_NROLES] = {"pad", "chords", "bass", "lead", "keys", "texture", "drums"};
static const uint16_t QMASK[WF_NQUAL] = WF_QUAL_MASK;

/* ------------------------------------------------------------- the World --- */
static const uint8_t *blob;
static uint32_t blob_n;
static int energy_emul = 1, keys_phrase = 0, mash = 0;
static uint32_t mash_seed = 1;
static uint32_t mute_mask;                       /* --mute: tracks kept silent (gain staging: one part at a time) */
static double energy_set = -1, tail_s = 6, peak_max = -1.0;
static double ctl_want[WF_NCTL] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   /* < 0: default */
static uint32_t bars = 8;

static const uint8_t *meta(void) { return wctx.b + wctx.off[WF_S_META]; }
static uint32_t w_bpm(void) { return meta()[WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN]; }
static uint32_t w_root(void) { return meta()[WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN + 3]; }
static uint32_t w_scale(void) { return meta()[WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN + 4]; }
static const uint8_t *scene_rec(uint32_t s) { return wctx.b + wctx.scene[s]; }
static const uint8_t *var_rec(uint32_t v) { return wctx.b + wctx.var[v]; }
static const uint8_t *defaults(void) { return wctx.b + wctx.off[WF_S_DEFAULTS]; }
static const uint8_t *prog_rec(uint32_t i)
{
    const uint8_t *p = wctx.b + wctx.off[WF_S_PROGS];
    while (i--)
        p += WF_PROG_HDR + 2u * p[0];
    return p;
}
static uint32_t prog_beats(const uint8_t *p)
{
    uint32_t k, b = 0;
    for (k = 0; k < p[0]; k++)
        b += p[2u + 2u * k];
    return b;
}
static const uint8_t *energy_rec(uint32_t i)     /* table i, or 0 */
{
    const uint8_t *p;
    if (!(wctx.have >> WF_S_ENERGY & 1u) || i >= wctx.cnt[WF_S_ENERGY])
        return 0;
    p = wctx.b + wctx.off[WF_S_ENERGY];
    while (i--)
        p += WF_ENERGY_HDR + WF_BAND_LEN * p[2];
    return p;
}
static uint32_t w_guard_byte(uint32_t off)         /* GUARD fixed byte, WF_NONE when unset */
{
    return wctx.have >> WF_S_GUARD & 1u ? wctx.b[wctx.off[WF_S_GUARD] + off] : WF_NONE;
}
static uint32_t var_find(const char *name)
{
    uint32_t v;
    for (v = 0; v < wctx.cnt[WF_S_VARS]; v++)
        if (!strcmp((const char *)var_rec(v), name))
            return v;
    return WF_NONE;
}

/* the chord sounding at beat b of scene s: root pitch class, tone mask (12 bits, relative to the root) */
static void chord_at(uint32_t s, uint32_t b, uint32_t *pc, uint32_t *mask)
{
    const uint8_t *p = prog_rec(scene_rec(s)[12]);
    uint32_t k, at = b % prog_beats(p);
    for (k = 0; k < p[0]; k++) {
        if (at < p[2u + 2u * k])
            break;
        at -= p[2u + 2u * k];
    }
    *pc = (w_root() + (p[1u + 2u * k] >> 4)) % 12u;
    *mask = QMASK[p[1u + 2u * k] & 15u];
}

/* ------------------------------------------------- the ENERGY band (arrange.c) --- */
typedef struct {
    uint32_t idx, nbands, layers, pos;           /* the band the layers follow, of how many; tracks heard; 0..1000 */
} band_t;
static band_t band_now(void)                     /* what the firmware's arrangement plays now */
{
    band_t b;
    b.nbands = arr.et.n;
    b.idx = arr.mb;
    b.layers = 15u & ~(uint32_t)wrt.mute;
    b.pos = arr_pos();
    return b;
}

/* ------------------------------------------------------ the player's phrase --- */
/* --keys: a 4-bar phrase on the Smart Keys track, as a recorded loop (what Phase 6 + 10 make of a beginner's
 * playing): melody-scale degrees from the World's tonic, quarter and eighth notes, held notes as ties */
static void keys_write(void)
{
    static const int8_t PH[][3] = {              /* step (1/16), melody degree (0 = tonic), length in steps */
        {0, 2, 6}, {6, 3, 2}, {8, 4, 8}, {16, 3, 4}, {20, 2, 4}, {24, 1, 8},
        {36, 0, 4}, {40, 4, 4}, {44, 5, 4}, {48, 4, 6}, {54, 2, 2}, {56, 3, 8}};
    const uint8_t *k = wctx.b + wctx.off[WF_S_KEYS];
    uint32_t mask = wb_u16(k + 2), deg[12], m = 0, i, j;
    track_t *t = &trk[k[0]];
    for (i = 0; i < 12; i++)
        if (mask >> i & 1u)
            deg[m++] = i;
    for (i = 0; i < NSTEP; i++) {
        memset(&t->step[i], 0, sizeof t->step[i]);
        t->step[i].time = ST_REST;
    }
    for (i = 0; i < sizeof PH / sizeof PH[0]; i++) {
        uint32_t at = (uint32_t)PH[i][0] % (uint32_t)t->p[P_SLEN], d = (uint32_t)PH[i][1];
        step_t *s = &t->step[at];
        s->time = ST_NOTE;
        s->n = 1;
        s->note[0] = (uint8_t)(k[6] + 12u * (d / m) + deg[d % m]);
        s->vel = 100;
        for (j = 1; j < (uint32_t)PH[i][2] && at + j < (uint32_t)t->p[P_SLEN]; j++)
            t->step[at + j].time = ST_TIE;
    }
}

/* -------------------------------------------------------------- measuring --- */
typedef struct {
    double peak, rms, lufs, dc[2], lim_pct, lim6_pct, lim_db, silence_s, tail_db;
    uint32_t full, near, steals, held, vmax, vpart[NPART], left_voices;
    uint32_t notes[NTRK], out_scale[NTRK], on_chord[NTRK], lo[NTRK], hi[NTRK], hits_bar10, lanes;
    uint32_t layers, band, nbands, energy_pos;
    uint32_t mash_keys, mash_refused, mash_black, mash_chord, mash_offkey, mash_range, mash_counts, mash_lo, mash_hi;
    int fails;
    char why[256];
} result_t;

/* BS.1770 K-weighting at FS (the coefficients of libebur128) and the gated loudness over 400 ms blocks */
typedef struct { double b[3], a[3], z[2][2]; } biq_t;
static void biq_init(biq_t *f, int hp)
{
    double K, a0;
    memset(f, 0, sizeof *f);
    if (!hp) {
        double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
        double Vh = pow(10.0, G / 20.0), Vb = pow(Vh, 0.4996667741545416);
        K = tan(M_PI * f0 / FS);
        a0 = 1.0 + K / Q + K * K;
        f->b[0] = (Vh + Vb * K / Q + K * K) / a0;
        f->b[1] = 2.0 * (K * K - Vh) / a0;
        f->b[2] = (Vh - Vb * K / Q + K * K) / a0;
        f->a[1] = 2.0 * (K * K - 1.0) / a0;
        f->a[2] = (1.0 - K / Q + K * K) / a0;
    } else {
        double f0 = 38.13547087602444, Q = 0.5003270373238773;
        K = tan(M_PI * f0 / FS);
        a0 = 1.0 + K / Q + K * K;
        f->b[0] = 1.0;
        f->b[1] = -2.0;
        f->b[2] = 1.0;
        f->a[1] = 2.0 * (K * K - 1.0) / a0;
        f->a[2] = (1.0 - K / Q + K * K) / a0;
    }
}
static double biq(biq_t *f, int c, double x)
{
    double y = f->b[0] * x + f->z[c][0];
    f->z[c][0] = f->b[1] * x - f->a[1] * y + f->z[c][1];
    f->z[c][1] = f->b[2] * x - f->a[2] * y;
    return y;
}

#define LBLK (FS / 10u)                          /* 100 ms: the gating blocks are 4 of them (75 % overlap) */
typedef struct {
    biq_t pre, rlb;
    double acc, *e;                              /* mean square per 100 ms */
    uint32_t n, ne, cap;
} loud_t;
static void loud_init(loud_t *l)
{
    memset(l, 0, sizeof *l);
    biq_init(&l->pre, 0);
    biq_init(&l->rlb, 1);
}
static void loud_put(loud_t *l, int32_t L, int32_t R)
{
    double a = biq(&l->rlb, 0, biq(&l->pre, 0, L / 32768.0)), b = biq(&l->rlb, 1, biq(&l->pre, 1, R / 32768.0));
    l->acc += a * a + b * b;
    if (++l->n == LBLK) {
        if (l->ne == l->cap) {
            l->cap = l->cap ? 2 * l->cap : 256;
            l->e = realloc(l->e, l->cap * sizeof *l->e);
        }
        l->e[l->ne++] = l->acc / LBLK;
        l->acc = 0;
        l->n = 0;
    }
}
static double loud_lufs(const loud_t *l)         /* integrated: absolute gate -70, relative gate -10 LU */
{
    double s = 0, g, lu;
    uint32_t i, n = 0;
    for (int pass = 0; pass < 2; pass++) {
        g = pass ? -0.691 + 10 * log10(s / n) - 10 : -70;
        s = 0;
        n = 0;
        for (i = 3; i < l->ne; i++) {
            double z = (l->e[i] + l->e[i - 1] + l->e[i - 2] + l->e[i - 3]) / 4;
            lu = -0.691 + 10 * log10(z > 1e-20 ? z : 1e-20);
            if (lu > g) {
                s += z;
                n++;
            }
        }
        if (!n)
            return -INFINITY;
    }
    return -0.691 + 10 * log10(s / n);
}

static double dbfs(double v) { return v > 0 ? 20.0 * log10(v / 32768.0) : -INFINITY; }

typedef struct {                                 /* what the render loop gathers */
    FILE *wav;
    loud_t loud;
    double sum2, sum[2];
    int32_t peak;
    uint32_t frames, pframes, full, near, quiet, quiet_best, lim, lim6, lim_max, vmax, vpart[NPART];
    uint32_t last_abs[NTRK], scene;
    uint32_t hits;
    result_t *r;
} meter_t;

/* a held note given up for another (voice.c voice_kill or a restart in place): a voice that sounded a note the
 * sequencer still holds is cut, and no other voice of the part sounds that note (a release cut short is not) */
static uint8_t pv_note[NPART][NVOICE], pv_on[NPART][NVOICE];
static void held_before(void)
{
    uint32_t p, i;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++) {
            pv_note[p][i] = trk[p].v[i].note;
            pv_on[p][i] = trk[p].v[i].active && trk[p].v[i].stage != 4u;
        }
}
static uint32_t held_after(void)
{
    uint32_t p, i, j, k, n = 0;
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++) {
            const voice_t *v = &trk[p].v[i];
            int held = 0, still = 0;
            if (!pv_on[p][i] || (v->stage != 4u && v->note == pv_note[p][i]))
                continue;
            for (k = 0; k < trk[p].seq_n; k++)
                held |= trk[p].seq_notes[k] == pv_note[p][i];
            held |= trk[p].mono_note == pv_note[p][i] && trk[p].p[P_VOICE] != V_POLY && v->stage == 4u;
            for (j = 0; j < NVOICE; j++)
                still |= trk[p].v[j].active && trk[p].v[j].stage != 4u && trk[p].v[j].note == pv_note[p][i];
            n += held && !still;
        }
    return n;
}

static uint32_t voices_part(uint32_t p)
{
    uint32_t i, n = 0;
    for (i = 0; i < NVOICE; i++)
        n += trk[p].v[i].active && trk[p].v[i].stage != 4u;
    return n;
}

/* what the sequencer starts in this block: notes per track, in the scale, on the chord, range; drum hits */
static void snoop(meter_t *m, uint32_t beat)
{
    uint32_t t, i, pc, cm, sm = SCALE_MASK[w_scale()];
    chord_at(m->scene, beat, &pc, &cm);
    for (t = 0; t < NTRK; t++) {
        track_t *k = &trk[t];
        if (k->seq_abs == m->last_abs[t] || k->seq_abs == SEQ_NONE)
            continue;
        m->last_abs[t] = k->seq_abs;
        if (trk_silent(k))
            continue;
        if (t == TRK_DRUM) {                     /* (as seq_tick played it: the band's lanes, density, the fill) */
            uint32_t msk = dstep_mask(arr_dstep(&k->dstep[k->seq_idx], k->seq_idx)) & ~arr_dskip(k->seq_idx);
            m->r->lanes |= msk;
            for (; msk; msk &= msk - 1u)
                m->hits++;
            continue;
        }
        if (k->step[k->seq_idx].time != ST_NOTE || !arr_plays(t, k->seq_idx))
            continue;
        for (i = 0; i < k->step[k->seq_idx].n; i++) {
            uint32_t n = k->step[k->seq_idx].note[i], rel = (n + 12u - w_root()) % 12u;
            m->r->notes[t]++;
            m->r->out_scale[t] += !(sm >> rel & 1u);
            m->r->on_chord[t] += (cm >> ((n + 12u - pc) % 12u) & 1u) != 0;
            if (n < m->r->lo[t])
                m->r->lo[t] = n;
            if (n > m->r->hi[t])
                m->r->hi[t] = n;
        }
    }
}

static void put16(FILE *f, uint32_t v) { fputc((int)(v & 255u), f); fputc((int)(v >> 8 & 255u), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFFu); put16(f, v >> 16); }
static void wav_begin(FILE *f)
{
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, FS);
    put32(f, FS * 4u);
    put16(f, 4);
    put16(f, 16);
    fwrite("data\0\0\0\0", 1, 8, f);
}
static void wav_end(FILE *f, uint32_t frames)
{
    fseek(f, 4, SEEK_SET);
    put32(f, 36u + frames * 4u);
    fseek(f, 40, SEEK_SET);
    put32(f, frames * 4u);
    fclose(f);
}

/* --mash: a beginner on the Smart Keys. On each eighth note (the transport's), the keys held long enough go up,
 * then most eighths one or two random keys go down, held one to four eighths. After the block, each key that went
 * down is judged as it sounded (kb_nt): in the World scale, on the chord (harm.ct), inside the range */
static uint32_t mash_rs, mash_e8 = 0xFFFFFFFFu, mash_left[27], mash_prev;
static uint32_t mash_rnd(void)
{
    mash_rs = mash_rs * 1664525u + 1013904223u;
    return mash_rs >> 8;
}
static void mash_before(void)
{
    uint32_t e8 = clk_beat * 2u + (clk_pos >= BEAT_U / 2u), k, m = fm1_in.notes, r, n;
    if (!song.playing || e8 == mash_e8)
        return;
    mash_e8 = e8;
    for (k = 0; k < 27; k++)
        if (m >> k & 1u && !--mash_left[k])
            m &= ~(1u << k);
    r = mash_rnd() % 10u;
    for (n = r < 3 ? 0u : r < 8 ? 1u : 2u; n; n--) {
        k = mash_rnd() % 27u;
        if (m >> k & 1u)
            continue;
        m |= 1u << k;
        mash_left[k] = 1u + mash_rnd() % 4u;
    }
    fm1_in.notes = m;
}
static void mash_after(result_t *r)
{
    uint32_t down = fm1_in.notes & ~mash_prev, k, sm = SCALE_MASK[w_scale()];
    mash_prev = fm1_in.notes;
    for (k = 0; k < 27; k++) {
        uint32_t n, black = (0x54Au >> ((53u + k) % 12u)) & 1u;
        if (!(down >> k & 1u))
            continue;
        if (!kb_n[k]) {
            r->mash_refused++;
            continue;
        }
        n = kb_nt[k][0];
        r->mash_keys++;
        r->mash_black += black;
        r->mash_chord += harm.ct >> (n % 12u) & 1u;
        r->mash_offkey += !(sm >> ((n + 12u - w_root()) % 12u) & 1u);
        r->mash_range += n < skm[skcur].g.lo || n > skm[skcur].g.hi;
        if (n < r->mash_lo)
            r->mash_lo = n;
        if (n > r->mash_hi)
            r->mash_hi = n;
    }
}

/* blocks of audio; play 1: measured (the bars played), 2: the peak only (the end of the tail) */
static void run(meter_t *m, uint32_t frames, int play)
{
    uint32_t f, i, p;
    for (f = 0; f < frames; f += CTL) {
        int32_t o[2 * CTL];
        uint32_t beat = clk_beat;                /* (the steps this block starts are on this beat) */
        held_before();
        if (mash && play == 1)
            mash_before();
        mix_block(o, CTL);
        if (mash && play == 1)
            mash_after(m->r);
        if (play == 1) {
            uint32_t v = 0;
            m->r->held += held_after();
            snoop(m, beat);
            for (p = 0; p < NPART; p++) {
                uint32_t n = voices_part(p);
                v += n;
                if (n > m->vpart[p])
                    m->vpart[p] = n;
            }
            if (v > m->vmax)
                m->vmax = v;
            if (lim_env > LIM_T * 1.122) {         /* more than 1 dB of gain reduction */
                uint32_t gr = (uint32_t)lim_env;
                m->lim += CTL;
                m->lim6 += lim_env > LIM_T * 1.995 ? CTL : 0u;   /* (more than 6 dB) */
                if (gr > m->lim_max)
                    m->lim_max = gr;
            }
        }
        for (i = 0; i < CTL; i++) {
            int32_t l = o[2 * i], r = o[2 * i + 1];
            int32_t sl = l > 32767 ? 32767 : l < -32768 ? -32768 : l, sr = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
            if (m->wav) {
                put16(m->wav, (uint16_t)sl);
                put16(m->wav, (uint16_t)sr);
            }
            m->frames++;
            if (!play)
                continue;
            int32_t al = sl < 0 ? -sl : sl, ar = sr < 0 ? -sr : sr, a = al > ar ? al : ar;
            if (a > m->peak)
                m->peak = a;
            if (play == 2)
                continue;
            m->pframes++;
            m->full += (sl >= 32767 || sl <= -32767) + (sr >= 32767 || sr <= -32767);
            m->near += (al >= 32393) + (ar >= 32393);
            m->sum[0] += sl;
            m->sum[1] += sr;
            m->sum2 += (double)sl * sl + (double)sr * sr;
            m->quiet = a <= 2 ? m->quiet + 1 : 0;
            if (m->quiet > m->quiet_best)
                m->quiet_best = m->quiet;
            loud_put(&m->loud, sl, sr);
        }
    }
}

static uint32_t bar_frames(void) { return (uint32_t)((uint64_t)4u * BEAT_U / w_bpm()); }

static int stopped_play(meter_t *m, uint32_t nbars)   /* PLAY, N bars, measured */
{
    uint32_t t, n = nbars * bar_frames();
    for (t = 0; t < NTRK; t++)
        m->last_abs[t] = SEQ_NONE;
    transport_req = 1;                           /* PLAY, as the panel asks for it */
    run(m, (n + CTL - 1u) / CTL * CTL, 1);
    return 0;
}

/* the macro positions (--ctl, the extremes; energy >= 0: ENERGY there), stage + commit scene s / variation v
 * (stopped: the macros' table and the scene's ENERGY band with it) */
static int scene_go(uint32_t s, uint32_t v, double energy, band_t *b)
{
    uint32_t c, t;
    int rc;
    for (c = 0; c < WF_NCTL; c++)
        if (ctl_want[c] >= 0)
            macro_set(c, (int32_t)(ctl_want[c] * 1000 + 0.5));
    if (energy >= 0)
        macro_set(MC_ENERGY, (int32_t)(energy * 1000 + 0.5));
    rc = world_apply(s, v);
    if (rc)
        return rc;
    if (!energy_emul) {                          /* --raw: no table, every pattern plays */
        arr.et.n = 0;
        arr_masks(0);
        arr_layers(0);
    }
    for (t = 0; t < NTRK; t++)
        if (mute_mask >> t & 1u)
            trk[t].p[P_MUTE] = 1;
    if (keys_phrase)
        keys_write();
    *b = band_now();
    return 0;
}

static void finish(meter_t *m, result_t *r, band_t *b)
{
    uint32_t p;
    double n = (double)m->pframes;
    r->peak = dbfs(m->peak);
    r->rms = dbfs(sqrt(m->sum2 / (2.0 * n)));
    r->lufs = loud_lufs(&m->loud);
    r->dc[0] = m->sum[0] / n / 32768.0;
    r->dc[1] = m->sum[1] / n / 32768.0;
    r->full = m->full;
    r->near = m->near;
    r->silence_s = (double)m->quiet_best / FS;
    r->lim_pct = 100.0 * m->lim / n;
    r->lim6_pct = 100.0 * m->lim6 / n;
    r->lim_db = m->lim_max ? 20 * log10((double)m->lim_max / LIM_T) : 0;
    r->vmax = m->vmax;
    for (p = 0; p < NPART; p++)
        r->vpart[p] = m->vpart[p];
    r->hits_bar10 = (uint32_t)(10.0 * m->hits * bar_frames() / n + 0.5);
    *b = band_now();                             /* (as it played at the end) */
    r->layers = b->layers;
    r->band = b->idx;
    r->nbands = b->nbands;
    r->energy_pos = b->pos;
}

/* STOP and the tail (into the WAV too): voices left, the peak of the last 0.25 s */
static void stop_tail(result_t *r, meter_t *m)
{
    uint32_t p, i, n = (uint32_t)(tail_s * FS) / CTL * CTL, q = FS / 4u / CTL * CTL, left = 0;
    fm1_in.notes = 0;                            /* (--mash: every key up with the STOP) */
    transport_req = 2;
    run(m, n > q ? n - q : 0u, 0);
    m->peak = 0;
    run(m, q, 2);
    for (p = 0; p < NPART; p++)
        for (i = 0; i < NVOICE; i++)
            left += trk[p].v[i].active;
    for (i = 0; i < NDRUM; i++)
        left += drums.v[i].active;
    r->left_voices = left;
    for (p = 0; p < NPART; p++)                  /* (the pitch counts, H2: nothing holds a note any more) */
        for (i = 0; i < 128u; i++)
            r->mash_counts += vref[p][i] + vlive[p][i];
    r->tail_db = dbfs(m->peak);
}

/* the checks of a render (see the top) */
static void judge(result_t *r)
{
    uint32_t t;
    char *w = r->why;
#define BAD(...) do { r->fails++; w += snprintf(w, sizeof r->why - (size_t)(w - r->why), __VA_ARGS__); } while (0)
    if (r->rms < -45 || r->silence_s > 2.0)
        BAD(" silent(rms %.1f, %.1f s)", r->rms, r->silence_s);
    if (r->peak > peak_max)
        BAD(" peak %.2f", r->peak);
    if (r->lim6_pct > 5.0)
        BAD(" limiter over 6 dB %.1f %%", r->lim6_pct);
    if (r->full)
        BAD(" full-scale %u", r->full);
    if (fabs(r->dc[0]) > 0.001 || fabs(r->dc[1]) > 0.001)
        BAD(" dc %.4f/%.4f", r->dc[0], r->dc[1]);
    if (r->left_voices || r->tail_db > -60)
        BAD(" tail(%u voices, %.1f dB)", r->left_voices, r->tail_db);
    if (r->vmax > NVOICE)
        BAD(" voices %u", r->vmax);
    if (r->held && !keys_phrase && !mash)
        BAD(" %u held notes stolen", r->held);
    if (mash && (!r->mash_keys || r->mash_offkey || r->mash_range))
        BAD(" keys: %u notes, %u off the key, %u out of range", r->mash_keys, r->mash_offkey, r->mash_range);
    if (r->mash_counts)
        BAD(" %u pitch counts left after STOP", r->mash_counts);
    for (t = 0; t < NPART; t++) {
        uint32_t role = wctx.b[wctx.trk[t]], lo = 0, hi = 127;
        if (!r->notes[t])
            continue;
        if (r->out_scale[t])
            BAD(" %s: %u notes off the scale", ROLE[role], r->out_scale[t]);
        if (role == WF_ROLE_BASS)
            lo = 28, hi = 55;
        else if (role == WF_ROLE_PAD || role == WF_ROLE_CHORDS || role == WF_ROLE_KEYS)
            lo = 40, hi = 88;
        else if (role == WF_ROLE_TEXTURE)
            lo = 48, hi = 96;
        else if (role == WF_ROLE_LEAD)
            lo = 48, hi = 96;
        if (r->lo[t] < lo || r->hi[t] > hi)
            BAD(" %s range %u..%u", ROLE[role], r->lo[t], r->hi[t]);
    }
#undef BAD
}

/* one render in a fork: the result through a pipe */
static result_t render_one(uint32_t s, uint32_t v, double energy, const char *wav)
{
    result_t r;
    int fd[2];
    pid_t pid;
    memset(&r, 0, sizeof r);
    if (pipe(fd) || (pid = fork()) < 0) {
        perror("world_render");
        exit(2);
    }
    if (!pid) {
        meter_t m;
        band_t b;
        uint32_t t;
        int rc;
        close(fd[0]);
        memset(&m, 0, sizeof m);
        for (t = 0; t < NTRK; t++)
            r.lo[t] = 127;
        r.mash_lo = 127;
        mash_rs = mash_seed * 2654435761u + s * 40503u + v;
        m.r = &r;
        m.scene = s;
        loud_init(&m.loud);
        rc = scene_go(s, v, energy, &b);
        if (rc) {
            r.fails = 1;
            snprintf(r.why, sizeof r.why, " world_apply: WORLD ERROR %d", rc);
        } else {
            if (wav && !(m.wav = fopen(wav, "wb"))) {
                perror(wav);
                _exit(2);
            }
            if (m.wav)
                wav_begin(m.wav);
            stopped_play(&m, bars);
            finish(&m, &r, &b);
            stop_tail(&r, &m);
            if (m.wav)
                wav_end(m.wav, m.frames);
            r.steals = voice_kills;
            judge(&r);
        }
        if (write(fd[1], &r, sizeof r) != (ssize_t)sizeof r)
            _exit(2);
        _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], &r, sizeof r) != (ssize_t)sizeof r) {
        fprintf(stderr, "world_render: a render died\n");
        r.fails = 1;
        snprintf(r.why, sizeof r.why, " crashed");
    }
    close(fd[0]);
    waitpid(pid, 0, 0);
    return r;
}

/* ------------------------------------------------------------ the mappings --- */
/* --macros: the static check of design 6.4 on the real stage, one mapping at a time (macro.c sums them live, and
 * tests/macro_test.c checks the sums; tools/worldc.py check owns this check): for every MAPS record, every scene x
 * variation and every track it moves, base + the offset at 0 and at 1 (curves end at 0 and 1) inside the
 * descriptor, and not its maximum at 1 */
static const uint8_t EROLE[NENGINES][WF_NEROLES] = WF_ENG_ROLE;
static const char *const CTLN[WF_NCTL] = {"COLOR", "MOTION", "SPACE", "ENERGY", "SOFT", "SHORT", "BODY", "TAIL",
                                          "DRIFT", "WOBBLE", "PULSE", "RATE", "FILTER", "ECHO", "CRUSH", "FREEZE"};
static const uint16_t CTLHOME[WF_NCTL] = WF_CTL_HOME;
static int macros_check(void)
{
    const uint8_t *m = wctx.b + wctx.off[WF_S_MAPS];
    uint32_t i, n = wctx.have >> WF_S_MAPS & 1u ? wctx.cnt[WF_S_MAPS] : 0u, bad = 0;
    for (i = 0; i < n; i++, m += WF_MAP_LEN) {
        uint32_t kind = m[1] >> 5, mask = m[1] & WF_TMASK, s, v, t;
        int32_t lo = 0x7FFF, hi = -0x7FFF, dmin = 0, dmax = 0, at1 = (int8_t)m[5];
        int32_t at0 = CTLHOME[m[0]] ? (int8_t)m[4] : 0;               /* (a control resting at 0: no offset there) */
        int clampd = 0, sat = 0, any = 0;
        char what[48];
        if (kind == WF_K_BRIGHT || kind == WF_K_SHAPE) {
            printf("  %-6s %-14s %+4d..%+4d   (a brightness / shape offset in cutoff steps)\n", CTLN[m[0]],
                   kind == WF_K_BRIGHT ? "~bright" : "~shape", at0, at1);
            continue;
        }
        for (s = 0; s < WF_NSCENE; s++)
            for (v = 0; v < wctx.cnt[WF_S_VARS]; v++) {
                if (world_stage(s, v)) {
                    printf("macros: world_stage %u %u failed\n", s, v);
                    return 1;
                }
                wst.st = WST_FREE;
                for (t = 0; t < (kind == WF_K_GLOBAL ? 1u : NTRK); t++) {
                    const param_desc_t *d;
                    int32_t base, id = m[2];
                    if (kind == WF_K_GLOBAL) {
                        d = &GP[id];
                        base = wst.g[id];
                        snprintf(what, sizeof what, "g.%s", d->label);
                    } else {
                        if (!(mask >> t & 1u))
                            continue;
                        if (kind == WF_K_ROLE) {
                            if (EROLE[wst.eng[t]][id] == WF_NONE)
                                continue;
                            id = P_E0 + EROLE[wst.eng[t]][id];
                        }
                        d = t == TRK_DRUM ? (id == P_E0 ? &DRUM_KIT_DESC : &TP[id])
                                          : id >= P_E0 ? &ENGINES[wst.eng[t]]->edit[id - P_E0] : &TP[id];
                        base = wst.p[t][id];
                        snprintf(what, sizeof what, "%s%s", mask & (mask - 1u) ? "*." : "", d->label);
                    }
                    any = 1;
                    dmin = d->min;
                    dmax = d->max;
                    if (base + at0 < lo)
                        lo = base + at0;
                    if (base + at1 > hi)
                        hi = base + at1;
                    if (base + at0 < d->min || base + at0 > d->max || base + at1 < d->min || base + at1 > d->max)
                        clampd = 1;
                    if (at1 > 0 && base + at1 >= d->max)
                        sat = 1;
                }
            }
        if (!any)
            continue;
        if (kind != WF_K_GLOBAL)
            snprintf(what + strlen(what), sizeof what - strlen(what), " (trk %s%s%s%s)", mask & 1 ? "1" : "",
                     mask & 2 ? "2" : "", mask & 4 ? "3" : "", mask & 8 ? "4" : "");
        printf("  %-6s %-22s %+4d..%+4d  -> %4d..%-4d of %d..%d%s%s\n", CTLN[m[0]], what, at0, at1, lo, hi, dmin, dmax,
               clampd ? "  CLAMPED" : "", sat ? "  ALL-MAX AT 100 %" : "");
        bad += clampd || sat;
    }
    printf("macros: %u mappings, %u out of range or all-max at 100 %%\n", n, bad);
    return bad != 0;
}

/* ----------------------------------------------------------------- output --- */
static void layers_txt(char *o, uint32_t layers)
{
    uint32_t t;
    for (t = 0; t < NTRK; t++)
        *o++ = layers >> t & 1u ? "PBKD"[t] : '.';
    *o = 0;
}
static void line_head(void)
{
    printf("  scene var        band lay   peak   rms   LUFS  lim1  voices  steal/held  notes/track    hits/bar"
           "  fails\n");
}
static void line(uint32_t s, uint32_t v, const result_t *r)
{
    char lay[8];
    layers_txt(lay, r->layers);
    printf("  %c     %-10s %u/%u  %s %6.2f %6.2f %6.2f %5.1f  %u(%u%u%u) %5u/%-4u %4u %4u %4u  %6.1f   %s%s\n",
           'A' + s, (const char *)var_rec(v), r->nbands ? r->band + 1 : 0, r->nbands, lay, r->peak, r->rms, r->lufs,
           r->lim_pct, r->vmax, r->vpart[0], r->vpart[1], r->vpart[2], r->steals, r->held, r->notes[0], r->notes[1],
           r->notes[2], r->hits_bar10 / 10.0, r->fails ? "FAIL" : "ok", r->why);
}

static void detail(uint32_t s, uint32_t v, const result_t *r)
{
    uint32_t t;
    char lay[8];
    layers_txt(lay, r->layers);
    printf("render: scene %c (%s), variation %s, %u bars at %u BPM, ENERGY %.2f -> band %u of %u, layers %s%s\n",
           'A' + s, (const char *)scene_rec(s), (const char *)var_rec(v), bars, w_bpm(), r->energy_pos / 1000.0,
           r->nbands ? r->band + 1 : 0, r->nbands, lay, energy_emul ? "" : " (raw: no ENERGY arrangement)");
    printf("  peak     %7.2f dBFS, %u samples at full scale, %u within 0.1 dB\n", r->peak, r->full, r->near);
    printf("  rms      %7.2f dBFS, loudness %.2f LUFS\n", r->rms, r->lufs);
    printf("  dc       L %+.6f, R %+.6f\n", r->dc[0], r->dc[1]);
    printf("  limiter  over 1 dB %.1f %% of the time, over 6 dB %.1f %%, at most %.1f dB\n", r->lim_pct, r->lim6_pct,
           r->lim_db);
    printf("  silence  longest %.2f s while playing\n", r->silence_s);
    printf("  voices   at most %u (parts %u %u %u), %u given up (%u held notes)\n", r->vmax, r->vpart[0], r->vpart[1],
           r->vpart[2], r->steals, r->held);
    for (t = 0; t < NTRK; t++) {
        uint32_t role = wctx.b[wctx.trk[t]];
        if (t == TRK_DRUM) {
            printf("  drums    %.1f hits per bar, lanes %04x\n", r->hits_bar10 / 10.0, r->lanes);
            continue;
        }
        if (!r->notes[t]) {
            printf("  %-8s no notes\n", ROLE[role]);
            continue;
        }
        printf("  %-8s %u notes, %u..%u, %u off the scale, %u%% chord tones\n", ROLE[role], r->notes[t], r->lo[t],
               r->hi[t], r->out_scale[t], 100u * r->on_chord[t] / r->notes[t]);
    }
    if (mash)
        printf("  keys     %u notes mashed (%u black, %u refused by max_poly), %u..%u, %u%% chord tones, %u off the key, "
               "%u out of range\n", r->mash_keys, r->mash_black, r->mash_refused, r->mash_lo, r->mash_hi,
               r->mash_keys ? 100u * r->mash_chord / r->mash_keys : 0u, r->mash_offkey, r->mash_range);
    printf("  tail     %u voices sounding after %.1f s, the last 0.25 s peak %.1f dBFS\n", r->left_voices, tail_s,
           r->tail_db);
    printf("check: %s%s\n", r->fails ? "FAIL:" : "ok", r->why);
}

/* A -> B -> C -> D while playing, into one file: each scene asked for in the bars-th bar of the one before, committed
 * on its transition's bar line (world.c wreq_block, Phase 11) */
static int sequence(const char *path, uint32_t v)
{
    meter_t m;
    result_t r;
    band_t b;
    uint32_t s, t, n, bar = 0;
    int rc, fails = 0;
    memset(&m, 0, sizeof m);
    memset(&r, 0, sizeof r);
    m.r = &r;
    loud_init(&m.loud);
    if (!(m.wav = fopen(path, "wb"))) {
        perror(path);
        return 1;
    }
    wav_begin(m.wav);
    for (t = 0; t < NTRK; t++)
        r.lo[t] = 127;
    if ((rc = scene_go(0, v, energy_set, &b)) != 0) {
        printf("sequence: scene A: WORLD ERROR %d\n", rc);
        return 1;
    }
    for (t = 0; t < NTRK; t++)
        m.last_abs[t] = SEQ_NONE;
    transport_req = 1;
    for (s = 1; s <= WF_NSCENE; s++) {
        uint32_t b0 = bar;
        while (clk_beat < 4u * (bars - 1u) || !song.playing)
            run(&m, CTL, 1);
        if (s == WF_NSCENE)
            break;
        if ((rc = world_request(s, v)) != 0) {
            printf("sequence: scene %c: WORLD ERROR %d\n", 'A' + s, rc);
            return 1;
        }
        for (n = 0; wrt.scene != s;) {
            n = clk_beat;                        /* (the clock at the start of the block that commits) */
            run(&m, CTL, 1);
        }
        bar = b0 + n / 4u;
        world_service();
        printf("sequence: scene %c %s (transition %u) from bar %u\n", 'A' + s, world_scene_name(s), scene_rec(s)[14], bar);
        m.scene = s;
    }
    run(&m, bar_frames() / CTL * CTL, 1);
    finish(&m, &r, &b);
    n = (uint32_t)(tail_s * FS) / CTL * CTL;
    transport_req = 2;
    run(&m, n, 0);
    wav_end(m.wav, m.frames);
    printf("sequence: %s: scenes A B C D while playing, each from its bar line, variation %s: %.1f s, peak %.2f dBFS, "
           "rms %.2f dBFS, %.2f LUFS, %u at full scale\n", path, (const char *)var_rec(v), (double)m.frames / FS, r.peak,
           r.rms, r.lufs, r.full);
    fails += r.full != 0 || r.peak > -1.0;
    return fails;
}

/* --ctl C=X,..: COLOR=0.2,ENERGY=90 (0..1, or 0..100 above 1); 0 = ok */
static int ctl_parse(const char *arg)
{
    char buf[256], *tok, *save = 0;
    uint32_t c;
    snprintf(buf, sizeof buf, "%s", arg);
    for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(0, ",", &save)) {
        char *eq = strchr(tok, '=');
        double x;
        if (!eq)
            return -1;
        *eq = 0;
        for (c = 0; c < WF_NCTL && strcasecmp(CTLN[c], tok); c++)
            ;
        x = atof(eq + 1);
        if (c == WF_NCTL || x < 0 || x > 100)
            return -1;
        ctl_want[c] = x > 1 ? x / 100.0 : x;
    }
    return 0;
}

/* --extremes: the default scene and variation at each macro's ends, ENERGY 0 / 0.5 / 1, all at 0, all at 1 */
#define BAND_LU 1.5                              /* --bands: the most a band may be quieter than the one below */
#define EXT_LU_HALF 4.0                          /* ENERGY 1 at most this much louder than 0.5 (design 5.6, 12.3) */
#define EXT_LU_SPAN 6.0                          /* .. and than 0: denser, not just louder */
#define EXT_LU_FLOOR 12.0                        /* no extreme more than this under the defaults */
static int extremes(void)
{
    static const struct { const char *name; double c[4]; } X[] = {
        {"defaults", {-1, -1, -1, -1}}, {"COLOR 0", {0, -1, -1, -1}}, {"COLOR 1", {1, -1, -1, -1}},
        {"MOTION 0", {-1, 0, -1, -1}}, {"MOTION 1", {-1, 1, -1, -1}}, {"SPACE 0", {-1, -1, 0, -1}},
        {"SPACE 1", {-1, -1, 1, -1}}, {"ENERGY 0", {-1, -1, -1, 0}}, {"ENERGY 0.5", {-1, -1, -1, 0.5}},
        {"ENERGY 1", {-1, -1, -1, 1}}, {"all 0", {0, 0, 0, 0}}, {"all 1", {1, 1, 1, 1}}};
    uint32_t s = defaults()[0], v = defaults()[1], i, c, t;
    double lufs[3] = {0, 0, 0}, dens[3] = {0, 0, 0}, worst = -INFINITY, tail = -INFINITY, def = 0, low = INFINITY;
    int fails = 0, bad;
    peak_max = -0.5;
    printf("  extremes, scene %c %s, %s:  peak   rms   LUFS  lim1  tail   notes/bar hits/bar  band lay\n", 'A' + s,
           (const char *)scene_rec(s), (const char *)var_rec(v));
    for (i = 0; i < sizeof X / sizeof X[0]; i++) {
        result_t r;
        char lay[8];
        double notes = 0;
        for (c = 0; c < 4u; c++)
            ctl_want[c] = X[i].c[c];
        r = render_one(s, v, -1, 0);
        for (t = 0; t < NPART; t++)
            notes += r.notes[t];
        notes /= bars;
        layers_txt(lay, r.layers);
        printf("  %-12s %31s %6.2f %6.2f %6.2f %5.1f %6.1f %8.1f %8.1f   %u/%u  %s  %s%s\n", X[i].name, "", r.peak, r.rms,
               r.lufs, r.lim_pct, r.tail_db, notes, r.hits_bar10 / 10.0, r.nbands ? r.band + 1 : 0, r.nbands, lay,
               r.fails ? "FAIL" : "ok", r.why);
        fails += r.fails != 0;
        if (!i)
            def = r.lufs;
        if (r.lufs < low)
            low = r.lufs;
        if (r.peak > worst)
            worst = r.peak;
        if (r.tail_db > tail)
            tail = r.tail_db;
        for (c = 0; c < 3u; c++)
            if (X[i].c[3] == 0.5 * c && X[i].c[0] < 0) {
                lufs[c] = r.lufs;
                dens[c] = notes + r.hits_bar10 / 10.0;
            }
    }
    bad = lufs[2] - lufs[1] > EXT_LU_HALF || lufs[2] - lufs[0] > EXT_LU_SPAN || dens[2] <= dens[0] ||
          low < def - EXT_LU_FLOOR;
    printf("extremes: %s, ENERGY 0 / 0.5 / 1: %.2f / %.2f / %.2f LUFS (%+.2f LU over 0.5, %+.2f over 0), "
           "%.1f / %.1f / %.1f notes + hits a bar; the quietest extreme %.1f LU under the defaults; worst peak %.2f dBFS, "
           "tail %.1f dBFS: %s\n", (const char *)meta(), lufs[0], lufs[1], lufs[2], lufs[2] - lufs[1], lufs[2] - lufs[0],
           dens[0], dens[1], dens[2], def - low, worst, tail, fails || bad ? "FAIL" : "ok");
    if (bad)
        printf("extremes: ENERGY 1 must be denser than 0 and at most %.0f LU over 0.5 and %.0f LU over 0; no extreme "
               "more than %.0f LU under the defaults\n", EXT_LU_HALF, EXT_LU_SPAN, EXT_LU_FLOOR);
    return fails || bad;
}

static int read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    uint8_t *b;
    if (!f) {
        perror(path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)len);
    if (!b || fread(b, 1, (size_t)len, f) != (size_t)len)
        return 0;
    fclose(f);
    blob = b;
    blob_n = (uint32_t)len;
    return 1;
}

static void usage(void)
{
    fprintf(stderr, "usage: world_render [--scene A..D|all] [--var NAME|all] [--bars N] [--energy X] [--ctl C=X,..] "
                    "[--raw] [--keys | --mash [--seed N]] [--tail S] [--mute T,..]\n"
                    "                    [--wav OUT.wav | --sequence OUT.wav | --bands | --macros | --extremes] "
                    "[--check] WORLD.wblob\n");
    exit(2);
}

#ifndef WORLD_RENDER_LIB                         /* (tests/guard_sweep.c builds on this file without its main) */
int main(int argc, char **argv)
{
    const char *scene = 0, *var = 0, *wav = 0, *seq = 0, *path = 0;
    int check = 0, bands = 0, macros = 0, ext = 0, i, fails = 0, rc;
    uint32_t s0, s1, v0, v1, s, v, nv;
    double lufs_def = 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--scene") && i + 1 < argc)
            scene = argv[++i];
        else if (!strcmp(a, "--var") && i + 1 < argc)
            var = argv[++i];
        else if (!strcmp(a, "--bars") && i + 1 < argc)
            bars = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--energy") && i + 1 < argc)
            energy_set = atof(argv[++i]);
        else if (!strcmp(a, "--tail") && i + 1 < argc)
            tail_s = atof(argv[++i]);
        else if (!strcmp(a, "--wav") && i + 1 < argc)
            wav = argv[++i];
        else if (!strcmp(a, "--sequence") && i + 1 < argc)
            seq = argv[++i];
        else if (!strcmp(a, "--raw"))
            energy_emul = 0;
        else if (!strcmp(a, "--keys"))
            keys_phrase = 1;
        else if (!strcmp(a, "--mash"))
            mash = 1;
        else if (!strcmp(a, "--seed") && i + 1 < argc)
            mash_seed = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--bands"))
            bands = 1;
        else if (!strcmp(a, "--macros"))
            macros = 1;
        else if (!strcmp(a, "--extremes"))
            ext = 1;
        else if (!strcmp(a, "--ctl") && i + 1 < argc) {
            if (ctl_parse(argv[++i]))
                usage();
        }
        else if (!strcmp(a, "--check"))
            check = 1;
        else if (!strcmp(a, "--mute") && i + 1 < argc)
            for (const char *c = argv[++i]; *c; c++)
                mute_mask |= *c >= '1' && *c <= '4' ? 1u << (*c - '1') : 0u;
        else if (a[0] == '-' || path)
            usage();
        else
            path = a;
    }
    if (!path || !bars || bars > 64 || !read_all(path))
        usage();
    host_tracks_init();
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    song.playing = 0;
    transport_req = 0;
    rc = world_load(blob, blob_n);
    if (rc) {
        printf("world_render: %s: WORLD ERROR %d\n", path, rc);
        return 1;
    }
    nv = wctx.cnt[WF_S_VARS];
    printf("world: %s (%s), %u BPM, %u B, %u scenes x %u variations, defaults: scene %c, %s, ENERGY %.2f\n",
           (const char *)meta(), (const char *)meta() + WF_NAME_LEN, w_bpm(), blob_n, WF_NSCENE, nv,
           'A' + defaults()[0], (const char *)var_rec(defaults()[1]), defaults()[5] / 250.0);
    if (macros)
        return macros_check() && check;
    if (ext)
        return extremes() && check;
    if (seq) {
        v = var ? var_find(var) : defaults()[1];
        if (v == WF_NONE)
            usage();
        return sequence(seq, v) && check;
    }
    s0 = defaults()[0];
    s1 = s0 + 1;
    if (scene && !strcmp(scene, "all"))
        s0 = 0, s1 = WF_NSCENE;
    else if (scene && scene[0] >= 'A' && scene[0] <= 'D' && !scene[1])
        s0 = (uint32_t)(scene[0] - 'A'), s1 = s0 + 1;
    else if (scene)
        usage();
    v0 = defaults()[1];
    v1 = v0 + 1;
    if (var && !strcmp(var, "all"))
        v0 = 0, v1 = nv;
    else if (var && (v0 = var_find(var)) == WF_NONE)
        usage();
    else if (var)
        v1 = v0 + 1;
    if (bands) {                                 /* each band of each scene: notes and hits only rise */
        printf("  scene band from  lay   rms    LUFS   notes/bar hits/bar\n");
        for (s = 0; s < WF_NSCENE; s++) {
            const uint8_t *e = energy_rec(scene_rec(s)[13]);
            double prev_l = -INFINITY, prev_h = -1, prev_n = -1;
            uint32_t k;
            for (k = 0; e && k < e[2]; k++) {
                double at = e[WF_ENERGY_HDR + WF_BAND_LEN * k] / 250.0 + 0.004, nb;
                result_t r = render_one(s, 0, at, 0);
                char lay[8];
                int bad;
                nb = (double)(r.notes[0] + r.notes[1] + r.notes[2]) / bars;
                bad = r.fails || r.hits_bar10 / 10.0 < prev_h || nb < prev_n || r.lufs < prev_l - BAND_LU;
                layers_txt(lay, r.layers);
                printf("  %c     %u    %.2f  %s %6.2f %6.2f  %8.1f %8.1f  %s%s%s\n", 'A' + s, k + 1, at, lay, r.rms,
                       r.lufs, nb, r.hits_bar10 / 10.0, bad ? "FAIL" : "ok", r.why,
                       bad && !r.fails ? " (fewer notes or hits, or much quieter than the band below)" : "");
                fails += bad;
                prev_h = r.hits_bar10 / 10.0;
                prev_n = nb;
                prev_l = r.lufs;
            }
        }
        printf("bands: %s\n", fails ? "FAIL" : "ok");
        return check && fails;
    }
    if (s1 - s0 == 1 && v1 - v0 == 1) {
        result_t r = render_one(s0, v0, energy_set, wav);
        detail(s0, v0, &r);
        return check && r.fails;
    }
    line_head();
    for (s = s0; s < s1; s++)
        for (v = v0; v < v1; v++) {
            result_t r = render_one(s, v, energy_set, 0);
            line(s, v, &r);
            fails += r.fails != 0;
            if (s == defaults()[0] && v == defaults()[1])
                lufs_def = r.lufs;
        }
    printf("renders: %u, %d failed%s", (s1 - s0) * (v1 - v0), fails, fails ? "\n" : "");
    if (!fails && s0 <= defaults()[0] && defaults()[0] < s1 && v0 <= defaults()[1] && defaults()[1] < v1)
        printf("; at the defaults %.2f LUFS\n", lufs_def);
    else if (!fails)
        printf("\n");
    return check && fails;
}
#endif
