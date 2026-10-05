/* SPDX-License-Identifier: GPL-3.0-only */
/* guard_sweep: the Musical Guardrail Engine's sweeps (docs/guardrails.md; design 6, 12.3). Every factory World is
 * played through the real firmware (tests/world_render.c's harness: wb_check, world_load, world_apply, PLAY) at a
 * sample of the four macros' space, COLOR MOTION SPACE ENERGY 0..100, and each render is judged:
 *
 *   clipping        no output sample at full scale, the peak at most -0.3 dBFS
 *   garbage         an integer wrap or a runaway inside the mix: no jump of more than SLEW_MAX between two output
 *                   samples; |DC| at most 0.001 over the whole render, bars and tail (it starts and ends silent:
 *                   through the master's DC blocker any music averages to 0 there, while a 2-bar window would
 *                   measure the blocker's answer to a pulse wave's DC as the first note starts); the master
 *                   limiter's input at most 4 x LIM_T (design 12.3)
 *   feedback        after STOP the tail falls under -60 dBFS within 6 s, no voice left sounding
 *   silence         heard (RMS above -45 dBFS, no silence over 2 s) whenever the ENERGY band holds a layer besides
 *                   the Smart Keys track
 *   volume jumps    between two neighbours of the 3^4 grid (one macro half a turn apart) the loudness (BS.1770,
 *                   integrated over the bars) moves at most JUMP_LU with a player, JUMP_LU_ALONE without
 *   parameters      every effective value the overlay writes (read from its live table every 4 blocks, as
 *                   ov_apply writes it) inside its descriptor, its hard limit (guard_limits.h: delay feedback 120,
 *                   reverb size 127, a level 120, resonance 110) and its slot's GUARD range (a base the World put
 *                   past one stays); vmod offsets within +-64; every base value inside its descriptor
 *   voices          at most NVOICE synth voices sounding
 *   CPU             host instructions a sample (kernel-counted, as tests/regress.c, in a second pass of the same
 *                   bars without the meters): the mean over the bars at most GL_CPU_BUDGET (2,300: 1.23 x SLOOP's
 *                   heaviest reference mix, cpu/mix/3parts_full_drums 1,877 in tests/cpu_baseline.txt), no DMA half
 *                   (256 samples) over GL_CPU_FULL (the costliest half but one: the counter includes the kernel's
 *                   instructions, and a lone spike is its; a render over either is measured twice more and the
 *                   least kept); and guard.c's estimate never under the measured mean by more than EST_SLACK (the
 *                   CPU guard's input must not miss a load)
 *
 *   build/host/guard_sweep [--full] [--jobs N] [--seed N] [--bars N] [--tail S] [--var N] [--list] WORLD.wblob ...
 *                                                       (--var: one variation only; --list: every point's numbers;
 *                                                        --calib: the CPU estimate's data, per DMA half)
 *   build/host/guard_sweep --dump-slots WORLD.wblob    the slot tables at tools/worldc.py model's positions
 *   build/host/guard_sweep --selftest                  each detector fires on a signal made to fail it
 *
 * The sample (each point a render of --bars bars, default 2, then the tail):
 *   quick (tests/run_tests.sh)  every scene, its ORIGINAL variation: the 3^4 grid (0, 50, 100 % each macro), and 8
 *                               seeded random points per scene (any variation, any position, half with no player);
 *                               and the corners of controls 4..15 (Phase 13) with the four macros at 100 %: SOUND
 *                               SHAPE and MOVEMENT all at 100 % (RATE at 100 and at 0), LIVE FX FILTER at 0 and at
 *                               100 % with ECHO and CRUSH at 100 % (their tail: ECHO_TAIL s)
 *   --full (SWEEP=full)         every scene x every variation: the 3^4 grid (with the player, and again without) and
 *                               its 8 edge points (one macro at 25 or 75 %, the others at 50 %); and 300 seeded random
 *                               points per World, half of them near the corners (each macro within 10 % of an end),
 *                               half with no player
 * The player: world_render's --keys phrase on the Smart Keys track, as a recorded loop (the keys engine's cost, its
 * sound through the macros, the loop following the chord, H12). Points run in parallel (fork per point: each starts
 * from the same booted state). Exit 1 when a check fails. */
#define WORLD_RENDER_LIB 1
#include "world_render.c"
#ifdef __APPLE__
#include <libproc.h>
#endif

#define SLEW_MAX 29491                   /* 0.9 of full scale between two samples: a wrapped integer, not music (the
                                          * factory Worlds' music reaches 0.69, DUSTY CAFE's drums) */
#define JUMP_LU 9.0                      /* the most a half turn of one macro may move the loudness, a player on */
#define JUMP_LU_ALONE 12.0               /* .. and with no player (a lone dark pad opening up moves more) */
#define EST_SLACK 0.15                   /* guard_cpu_est may be at most 15 % under the measured mean */
#define TAIL_DB (-60.0)
#define ECHO_TAIL 10.0                   /* s: the tail of LIVE ECHO at 100 % (tests/livefx_test.c: a slow tempo's echo) */
#define PEAK_DB (-0.3)

typedef struct {
    uint8_t w, s, v, grid;               /* World, scene, variation; on the 3^4 grid */
    uint8_t keys;                        /* a player's loop on the Smart Keys track */
    uint8_t ext;                         /* the corner of controls 4..15 (0: the World's defaults; see EXT) */
    uint16_t pos[4];
    /* results */
    double peak, rms, lufs, dc[2], tail_db, silence_s, slew, limin, cpu_mean, cpu_half;
    uint32_t full, vmax, left, params, checked, est, hold, layers, nonkeys, fails, done;
    char why[200];
} pt_t;

static pt_t *pts;
static uint32_t npts;
#define SW_MAXW 64                               /* Worlds a run (the factory library of 30: WORLDS=all) */
static const char *wpath[SW_MAXW];
static uint8_t *wblob[SW_MAXW];
static uint32_t wlen[SW_MAXW], nworld;
static uint32_t sw_bars = 2;
static double sw_tail = 6;
static int sw_list;                      /* --list: every point's numbers */
static int sw_calib;                     /* --calib: per DMA half of the CPU pass, the cost and what sounds (CSV) */
static int sw_var = -1;                  /* --var N: only that variation (tuning a World's data) */

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
static uint64_t instr_cost;              /* what one instr_now costs itself (the least of 200 back to back) */
static void instr_calibrate(void)
{
    uint32_t i;
    instr_cost = ~0ull;
    for (i = 0; i < 200u; i++) {
        uint64_t a = instr_now(), b = instr_now();
        if (b - a < instr_cost)
            instr_cost = b - a;
    }
}

/* ------------------------------------------------------------ the analysis --- */
/* what the detectors gather from the output, a sample at a time (also fed by --selftest) */
typedef struct {
    loud_t loud;
    double sum2, sum[2], dsum[2];        /* dsum: the DC's sum, over the bars and the tail */
    uint32_t dn;
    int32_t peak, prev[2], slew;
    uint32_t n, full, quiet, quiet_best;
} ana_t;
static void ana_init(ana_t *a)
{
    memset(a, 0, sizeof *a);
    loud_init(&a->loud);
}
static void ana_put(ana_t *a, int32_t l, int32_t r)
{
    int32_t sl = l > 32767 ? 32767 : l < -32768 ? -32768 : l, sr = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
    int32_t al = sl < 0 ? -sl : sl, ar = sr < 0 ? -sr : sr, m = al > ar ? al : ar, d;
    if (a->n) {
        d = sl - a->prev[0];
        if ((d < 0 ? -d : d) > a->slew)
            a->slew = d < 0 ? -d : d;
        d = sr - a->prev[1];
        if ((d < 0 ? -d : d) > a->slew)
            a->slew = d < 0 ? -d : d;
    }
    a->prev[0] = sl;
    a->prev[1] = sr;
    if (m > a->peak)
        a->peak = m;
    a->full += (sl >= 32767 || sl <= -32767) + (sr >= 32767 || sr <= -32767);
    a->sum[0] += sl;
    a->sum[1] += sr;
    a->dsum[0] += sl;
    a->dsum[1] += sr;
    a->dn++;
    a->sum2 += (double)sl * sl + (double)sr * sr;
    a->quiet = m <= 2 ? a->quiet + 1 : 0;
    if (a->quiet > a->quiet_best)
        a->quiet_best = a->quiet;
    a->n++;
    loud_put(&a->loud, sl, sr);
}
static void ana_finish(const ana_t *a, pt_t *p)
{
    double n = a->n ? (double)a->n : 1;
    p->peak = dbfs(a->peak);
    p->rms = dbfs(sqrt(a->sum2 / (2.0 * n)));
    p->lufs = loud_lufs(&a->loud);
    p->dc[0] = a->dsum[0] / (a->dn ? a->dn : 1) / 32768.0;
    p->dc[1] = a->dsum[1] / (a->dn ? a->dn : 1) / 32768.0;
    p->full = a->full;
    p->silence_s = (double)a->quiet_best / FS;
    p->slew = a->slew / 32768.0;
}

/* the checks of one render (the volume jumps are judged across points, later) */
#define BAD(...) do { p->fails++; w += snprintf(w, sizeof p->why - (size_t)(w - p->why), __VA_ARGS__); } while (0)
static void judge_pt(pt_t *p)
{
    char *w = p->why + strlen(p->why);
    if (p->full || p->peak > PEAK_DB)
        BAD(" clip(peak %.2f dBFS, %u at full scale)", p->peak, p->full);
    if (p->slew * 32768.0 > SLEW_MAX)
        BAD(" garbage(a jump of %.2f of full scale)", p->slew);
    if (fabs(p->dc[0]) > 0.001 || fabs(p->dc[1]) > 0.001)
        BAD(" dc %.4f/%.4f", p->dc[0], p->dc[1]);
    if (p->limin > 4.0 * LIM_T)
        BAD(" limiter input %.1f x LIM_T", p->limin / LIM_T);
    if (p->left || p->tail_db > TAIL_DB)
        BAD(" feedback(tail %.1f dBFS, %u voices)", p->tail_db, p->left);
    if (p->nonkeys && (p->rms < -45 || p->silence_s > 2.0))
        BAD(" silent(rms %.1f, %.1f s)", p->rms, p->silence_s);
    if (p->params)
        BAD(" %u invalid parameter values", p->params);
    if (p->vmax > NVOICE)
        BAD(" voices %u", p->vmax);
    if (p->cpu_mean > GL_CPU_BUDGET || p->cpu_half > GL_CPU_FULL)
        BAD(" cpu(mean %.0f, half %.0f)", p->cpu_mean, p->cpu_half);
    if (p->cpu_mean > 0 && p->est < p->cpu_mean * (1.0 - EST_SLACK))
        BAD(" cpu estimate %u under %.0f", p->est, p->cpu_mean);
}
#undef BAD

/* ---------------------------------------------------------- the parameters --- */
static const param_desc_t *slot_desc(const ov_slot_t *s)
{
    uint32_t id;
    if (s->kind == OV_G)
        return &GP[s->ptr - song.g];
    id = (uint32_t)(s->ptr - trk[s->part].p);
    return s->part == TRK_DRUM || id < P_E0 ? &TP[id] : &ENGINES[trk[s->part].eng_req % NENGINES]->edit[id - P_E0];
}
/* the overlay's live table as ov_apply writes it: every value inside its descriptor, its hard limit, its range */
static uint32_t params_check(uint32_t *checked)
{
    const ov_tab_t *tb = &ovb[ov_live];
    uint32_t i, t, bad = 0;
    for (i = 0; i < tb->n; i++) {
        const ov_slot_t *s = &tb->s[i];
        int32_t c = ov_cur[ov_live][i], b, v, id;
        const param_desc_t *d;
        (*checked)++;
        if (s->kind >= OV_VCUT) {
            bad += c < -(GL_VMOD_MAX << 8) || c > GL_VMOD_MAX << 8 || c < s->lo * 256 - 256 || c > s->hi * 256 + 256;
            continue;
        }
        d = slot_desc(s);
        b = *s->ptr;
        v = c ? ov_effective(s, b, c) : b;
        id = s->kind == OV_G ? (int32_t)(s->ptr - song.g) : (int32_t)(s->ptr - trk[s->part].p);
        bad += v < d->min || v > d->max;
        bad += v > s->hi && v > b;
        bad += v < s->lo && v < b;
        if (s->kind == OV_G)
            bad += (id == G_DFDBK && v > GL_DFDBK_MAX) || (id == G_RSIZE && v > GL_RSIZE_MAX);
        else if (id == P_LEVEL)
            bad += v > GL_LEVEL_MAX && v > b;
        else if (s->part < NPART && id >= P_E0 && GU_ROLE[trk[s->part].eng_req % NENGINES][WF_EROLE_RESO] == id - P_E0)
            bad += v > GL_RESO_MAX && v > b;
    }
    for (t = 0; t < NTRK; t++)                   /* the bases: inside their descriptors (the stage clamps them) */
        for (i = 0; i < P_COUNT; i++) {
            const param_desc_t *d = t == TRK_DRUM ? (i == P_E0 ? &DRUM_KIT_DESC : i > P_E0 ? 0 : &TP[i])
                                                  : i >= P_E0 ? &ENGINES[trk[t].eng_req % NENGINES]->edit[i - P_E0] : &TP[i];
            if (d && (trk[t].p[i] < d->min || trk[t].p[i] > d->max))
                bad++;
        }
    return bad;
}

/* ------------------------------------------------------------- one point --- */
/* the corners of controls 4..15 (SOFT .. FREEZE), -1 the World's default: SOUND SHAPE and MOVEMENT (RATE up, down),
 * LIVE FX (FILTER low-pass, high-pass) */
static const int8_t EXT[5][12] = {{0}, {1, 1, 1, 1, 1, 1, 1, 1, -1, -1, -1, -1}, {1, 1, 1, 1, 1, 1, 1, 0, -1, -1, -1, -1},
                                  {-1, -1, -1, -1, -1, -1, -1, -1, 0, 1, 1, -1}, {-1, -1, -1, -1, -1, -1, -1, -1, 1, 1, 1, -1}};
static void pt_go(const pt_t *p)                 /* the World's positions, stage + commit (stopped) */
{
    uint32_t c;
    band_t b;
    keys_phrase = p->keys;                       /* a player's loop on the Smart Keys track (world_render --keys) */
    for (c = 0; c < WF_NCTL; c++)
        ctl_want[c] = c < 4u ? p->pos[c] / 1000.0 : p->ext ? EXT[p->ext % 5u][c - 4u] : -1;
    if (scene_go(p->s, p->v, -1, &b)) {
        printf("guard_sweep: world_apply %u %u failed\n", p->s, p->v);
        _exit(2);
    }
}

/* the CPU pass (a fork of the point's state): the bars again, nothing but mix_block between the counts */
static void touch(volatile int16_t *b, uint32_t n)   /* every page written once: no copy-on-write fault counted */
{
    uint32_t i;
    for (i = 0; i < n; i += 1024u)
        b[i] = b[i];
}
static void cpu_pass(pt_t *p)
{
    int fd[2];
    pid_t pid;
    double r[2] = {0, 0};
    if (pipe(fd) || (pid = fork()) < 0)
        _exit(2);
    if (!pid) {
        uint32_t f, n = sw_bars * bar_frames(), k = 0, h = 0;
        uint64_t i0, ih, i1, best = 0, best2 = 0, calls = 0;
        int32_t o[2 * CTL];
        close(fd[0]);
        touch(dly_buf, DLY_LEN);                 /* (the fork's pages: the buffers the buses walk through) */
        touch(cho_buf, CHO_LEN);
        touch(rev_comb, sizeof rev_comb / 2u);
        touch(rev_ap, sizeof rev_ap / 2u);
        touch((volatile int16_t *)trk, sizeof trk / 2u);
        touch((volatile int16_t *)&drums, sizeof drums / 2u);
        transport_req = 1;
        i0 = ih = instr_now();
        for (f = 0; f < n; f += CTL) {
            mix_block(o, CTL);
            if (++k == 8u) {                     /* a DMA half: 8 blocks, 256 samples */
                uint64_t t = instr_now();
                calls++;
                if (++h > 2u && t - ih - instr_cost > best2) {   /* (the first two halves: the fork's touches) */
                    best2 = t - ih - instr_cost;
                    if (best2 > best) {
                        uint64_t x = best;
                        best = best2;
                        best2 = x;
                    }
                }
                if (sw_calib) {                  /* instructions a sample, then per engine its voices, ... */
                    uint32_t e, pp, vi, nv[NENGINES] = {0}, dist = 0, sl = 0, dr = 0, gd = 0;
                    for (pp = 0; pp < NPART; pp++) {
                        uint32_t v = 0;
                        for (vi = 0; vi < NVOICE; vi++)
                            v += trk[pp].v[vi].active != 0;
                        nv[trk[pp].engine % NENGINES] += v;
                        dist += trk[pp].p[P_DIST] > 0 && (v || trk[pp].tail);
                        sl += trk[pp].p[P_SLCR] != 0;
                        if (trk[pp].engine == WF_ENG_GRAIN)
                            gd += v * (uint32_t)clamp(trk[pp].p[P_E0 + WF_GRAIN_DENS], 0, 127);
                    }
                    for (vi = 0; vi < NDRUM; vi++)
                        dr += drums.v[vi].active != 0;
                    printf("calib %.1f", (double)(t - ih - instr_cost) / (8.0 * CTL));
                    for (e = 0; e < NENGINES; e++)
                        printf(" %u", nv[e]);
                    printf(" %u %u %u %u %u %u\n", dist, sl, dr, song.g[G_DUST] > 0, gd, guard_cpu_est());
                    t = instr_now();             /* (the line's own cost out of the next half) */
                    calls++;
                }
                ih = t;
                k = 0;
            }
        }
        i1 = instr_now();
        r[0] = (double)(i1 - i0 - (calls + 1u) * instr_cost) / ((n + CTL - 1u) / CTL * CTL);
        r[1] = (double)best2 / (8.0 * CTL);    /* the costliest half but one: a lone spike is the kernel's */
        if (write(fd[1], r, sizeof r) != (ssize_t)sizeof r)
            _exit(2);
        _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], r, sizeof r) != (ssize_t)sizeof r)
        r[0] = r[1] = -1;
    close(fd[0]);
    waitpid(pid, 0, 0);
    p->cpu_mean = r[0];
    p->cpu_half = r[1];
}

static void pt_render(pt_t *p)
{
    ana_t a;
    uint32_t f, t, i, n, blk = 0, q, keys = 1u << wrt.keys_trk;
    int32_t o[2 * CTL], lim_max = 0;
    band_t b;
    if (world_load(wblob[p->w], wlen[p->w])) {
        printf("guard_sweep: world_load failed\n");
        _exit(2);
    }
    pt_go(p);
    cpu_pass(p);
    for (i = 0; i < 2u && (p->cpu_mean > GL_CPU_BUDGET || p->cpu_half > GL_CPU_FULL); i++) {
        pt_t q = *p;                             /* over: measured again (a busy machine's kernel adds, never takes) */
        cpu_pass(&q);
        p->cpu_mean = q.cpu_mean < p->cpu_mean ? q.cpu_mean : p->cpu_mean;
        p->cpu_half = q.cpu_half < p->cpu_half ? q.cpu_half : p->cpu_half;
    }
    b = band_now();
    p->layers = b.layers;
    for (t = 0; t < NTRK; t++) {                 /* a layer besides the keys track with something to play */
        uint32_t has = 0;
        if (!(b.layers >> t & 1u) || (keys >> t & 1u))
            continue;
        for (i = 0; i < NSTEP && !has; i++)
            has = t == TRK_DRUM ? dstep_mask(&trk[t].dstep[i]) != 0 : trk[t].step[i].time == ST_NOTE;
        p->nonkeys += has;
    }
    ana_init(&a);
    n = sw_bars * bar_frames();
    transport_req = 1;
    for (f = 0; f < n; f += CTL) {
        uint32_t v = 0;
        mix_block(o, CTL);
        if (lim_env > lim_max)
            lim_max = lim_env;
        for (i = 0; i < CTL; i++)
            ana_put(&a, o[2 * i], o[2 * i + 1]);
        for (t = 0; t < NPART; t++)
            v += voices_part(t);
        if (v > p->vmax)
            p->vmax = v;
        if (gcpu.est > p->est)
            p->est = gcpu.est;
        p->hold |= wg.hold;
        if (!(blk++ & 3u))
            p->params += params_check(&p->checked);
    }
    ana_finish(&a, p);
    p->limin = lim_max;
    /* STOP and the tail: the peak of its last 0.25 s, voices left; the DC over all of it */
    transport_req = 2;
    n = (uint32_t)((p->ext >= 3u ? ECHO_TAIL : sw_tail) * FS) / CTL * CTL;
    q = FS / 4u / CTL * CTL;
    a.peak = 0;
    for (f = 0; f < n; f += CTL) {
        mix_block(o, CTL);
        for (i = 0; i < 2u * CTL; i++) {
            int32_t x = o[i] < 0 ? -o[i] : o[i];
            a.dsum[i & 1u] += o[i] > 32767 ? 32767 : o[i] < -32768 ? -32768 : o[i];
            if (f + q >= n && x > a.peak)
                a.peak = x;
        }
        a.dn += CTL;
    }
    p->dc[0] = a.dsum[0] / a.dn / 32768.0;
    p->dc[1] = a.dsum[1] / a.dn / 32768.0;
    p->tail_db = dbfs(a.peak);
    for (t = 0; t < NPART; t++)
        for (i = 0; i < NVOICE; i++)
            p->left += trk[t].v[i].active;
    for (i = 0; i < NDRUM; i++)
        p->left += drums.v[i].active;
    judge_pt(p);
}

/* ---------------------------------------------------------- the points --- */
static uint32_t sw_rs = 1;
static uint32_t sw_rnd(void)
{
    sw_rs = sw_rs * 1664525u + 1013904223u;
    return sw_rs >> 8;
}
static void add_pt(uint32_t w, uint32_t s, uint32_t v, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int grid, int keys)
{
    pt_t *p;
    if (sw_var >= 0 && v != (uint32_t)sw_var)
        return;
    pts = realloc(pts, (npts + 1u) * sizeof *pts);
    p = &pts[npts++];
    memset(p, 0, sizeof *p);
    p->w = (uint8_t)w, p->s = (uint8_t)s, p->v = (uint8_t)v, p->grid = (uint8_t)grid, p->keys = (uint8_t)keys;
    p->pos[0] = (uint16_t)a, p->pos[1] = (uint16_t)b, p->pos[2] = (uint16_t)c, p->pos[3] = (uint16_t)d;
}
static uint32_t rnd_pos(int near)
{
    uint32_t x = sw_rnd() % 1001u;
    return near ? (x % 2u ? 1000u - x % 101u : x % 101u) : x;
}
static void make_points(int full)
{
    static const uint16_t G[3] = {0, 500, 1000};
    uint32_t w, s, v, a, b, c, d, k, nv, kp;
    for (w = 0; w < nworld; w++) {
        world_load(wblob[w], wlen[w]);
        nv = wctx.cnt[WF_S_VARS];
        for (s = 0; s < WF_NSCENE; s++)
            for (v = 0; v < (full ? nv : 1u); v++) {
                for (kp = 0; kp < (full ? 2u : 1u); kp++)        /* (full: the grid with no player too) */
                    for (a = 0; a < 3; a++)
                        for (b = 0; b < 3; b++)
                            for (c = 0; c < 3; c++)
                                for (d = 0; d < 3; d++)
                                    add_pt(w, s, v, G[a], G[b], G[c], G[d], 1, !kp);
                if (full)
                    for (k = 0; k < 8; k++) {            /* the edges: one macro at 25 or 75 %, the others at 50 % */
                        uint16_t q[4] = {500, 500, 500, 500};
                        q[k / 2u] = k % 2u ? 750 : 250;
                        add_pt(w, s, v, q[0], q[1], q[2], q[3], 0, 1);
                    }
                if (!full)
                    for (k = 0; k < 8; k++)
                        add_pt(w, s, sw_rnd() % nv, rnd_pos(0), rnd_pos(0), rnd_pos(0), rnd_pos(0), 0, k % 2u);
                if (!full && v == 0 && sw_var <= 0)
                    for (k = 1; k < 5u; k++) {           /* controls 4..15's corners, the macros at 100 % */
                        add_pt(w, s, v, 1000, 1000, 1000, 1000, 0, 1);
                        pts[npts - 1u].ext = (uint8_t)k;
                    }
            }
        if (full)
            for (k = 0; k < 300; k++) {
                int near = k % 2u;
                s = sw_rnd() % WF_NSCENE;
                v = sw_rnd() % nv;
                add_pt(w, s, v, rnd_pos(near), rnd_pos(near), rnd_pos(near), rnd_pos(near), 0, k / 2u % 2u);
            }
    }
}

/* every point in a fork, jobs at a time; each child writes its pt_t back through a pipe */
static void run_points(uint32_t jobs)
{
    pid_t *pid = calloc(jobs, sizeof *pid);
    int *fd = calloc(jobs, sizeof *fd);
    uint32_t *idx = calloc(jobs, sizeof *idx), next = 0, running = 0, j;
    while (next < npts || running) {
        while (next < npts && running < jobs) {
            int p2[2];
            for (j = 0; pid[j]; j++)
                ;
            if (pipe(p2) || (pid[j] = fork()) < 0) {
                perror("guard_sweep");
                exit(2);
            }
            if (!pid[j]) {
                close(p2[0]);
                pt_render(&pts[next]);
                pts[next].done = 1;
                if (write(p2[1], &pts[next], sizeof pts[next]) != (ssize_t)sizeof pts[next])
                    _exit(2);
                _exit(0);
            }
            close(p2[1]);
            fd[j] = p2[0];
            idx[j] = next++;
            running++;
        }
        {
            int st;
            pid_t got = waitpid(-1, &st, 0);
            for (j = 0; j < jobs && pid[j] != got; j++)
                ;
            if (j == jobs)
                continue;
            if (read(fd[j], &pts[idx[j]], sizeof pts[idx[j]]) != (ssize_t)sizeof pts[idx[j]] || !pts[idx[j]].done) {
                pts[idx[j]].fails++;
                snprintf(pts[idx[j]].why, sizeof pts[idx[j]].why, " crashed");
            }
            close(fd[j]);
            pid[j] = 0;
            running--;
        }
    }
    free(pid);
    free(fd);
    free(idx);
}

/* the volume jumps: grid neighbours (same World, scene, variation; one macro 0 <-> 50 or 50 <-> 100 %) */
static void jumps(uint32_t w, uint32_t *bad, double *worst)   /* worst[0]: with the player, [1]: without */
{
    uint32_t i, j, k, diff;
    worst[0] = worst[1] = 0;
    for (i = 0; i < npts; i++) {
        if (pts[i].w != w || !pts[i].grid)
            continue;
        for (j = i + 1; j < npts && pts[j].w == w; j++) {   /* (a World's points are contiguous) */
            if (!pts[j].grid || pts[j].s != pts[i].s || pts[j].v != pts[i].v ||
                pts[j].keys != pts[i].keys)
                continue;
            for (k = 0, diff = 0; k < 4; k++)
                diff += pts[i].pos[k] != pts[j].pos[k] ? (abs(pts[i].pos[k] - pts[j].pos[k]) == 500 ? 1u : 9u) : 0u;
            if (diff == 1 && isfinite(pts[i].lufs) && isfinite(pts[j].lufs)) {
                double dl = fabs(pts[i].lufs - pts[j].lufs);
                if (dl > worst[!pts[i].keys])
                    worst[!pts[i].keys] = dl;
                if (dl > (pts[i].keys ? JUMP_LU : JUMP_LU_ALONE)) {
                    if ((*bad)++ < 8)
                        printf("  JUMP %s %c/%u%s: %u %u %u %u -> %u %u %u %u: %.1f LU\n", wpath[w], 'A' + pts[i].s,
                               pts[i].v, pts[i].keys ? "" : " (no player)", pts[i].pos[0], pts[i].pos[1], pts[i].pos[2],
                               pts[i].pos[3], pts[j].pos[0], pts[j].pos[1], pts[j].pos[2], pts[j].pos[3], dl);
                }
            }
        }
    }
}

/* ------------------------------------------------------------ --dump-slots --- */
/* the slot tables tools/worldc.py model prints: every scene x variation at its positions (the 3^4 grid, then 24
 * seeded points), each on a fresh overlay (no target still ramping home), the other controls at the defaults */
static int dump_slots(const char *path)
{
    static const uint16_t G[3] = {0, 500, 1000};
    uint32_t s, v, k, i, x;
    if (!read_all(path) || world_load(blob, blob_n))
        return 1;
    for (s = 0; s < WF_NSCENE; s++)
        for (v = 0; v < wctx.cnt[WF_S_VARS]; v++) {
            if (world_apply(s, v))
                return 1;
            for (k = 0, x = 1; k < 81u + 24u; k++) {
                uint32_t q[4];
                const ov_tab_t *nt;
                if (k < 81u) {
                    q[0] = G[k / 27u], q[1] = G[k / 9u % 3u], q[2] = G[k / 3u % 3u], q[3] = G[k % 3u];
                } else {
                    for (i = 0; i < 4; i++) {
                        x = x * 1664525u + 1013904223u;
                        q[i] = (x >> 8) % 1001u;
                    }
                }
                ov_reset();
                for (i = 0; i < 4; i++)
                    mac.pos[i] = (uint16_t)q[i];
                mac.dirty = 1;
                macro_eval();
                nt = &ovb[ov_live ^ 1u];
                printf("@ %u %u %u %u %u %u %u %u\n", s, v, q[0], q[1], q[2], q[3], nt->n, mac.over);
                for (i = 0; i < nt->n; i++) {
                    const ov_slot_t *o = &nt->s[i];
                    int pid = o->kind == OV_P ? (int)(o->ptr - trk[o->part].p) : o->kind == OV_G ? (int)(o->ptr - song.g) : 0;
                    int eff = o->kind >= OV_VCUT ? o->tgt : ov_effective(o, *o->ptr, o->tgt);
                    printf("%u %u %d %d %d %u %d %d\n", o->kind, o->part, pid, o->lo, o->hi, o->cls, o->tgt, eff);
                }
            }
        }
    ov_reset();
    return 0;
}

/* -------------------------------------------------------------- --selftest --- */
/* each detector against a signal made to fail it (and a clean one that passes) */
static int selftest(void)
{
    static const char *const NAME[] = {"clean", "clip", "wrap", "dc", "feedback", "silence", "params", "cpu", "estimate"};
    uint32_t k, i, fails = 0;
    for (k = 0; k < sizeof NAME / sizeof NAME[0]; k++) {
        pt_t p;
        ana_t a;
        memset(&p, 0, sizeof p);
        ana_init(&a);
        for (i = 0; i < 3u * FS; i++) {
            double ph = 2 * M_PI * 220.0 * i / FS;
            int32_t x = (int32_t)(8000 * sin(ph));
            if (k == 1 && i == FS)
                x = 32767;                       /* a clipped sample */
            if (k == 2 && i == FS)
                x = -30000;                      /* a wrapped integer: a jump across most of the scale */
            if (k == 3)
                x += 100;                        /* DC */
            if (k == 5)
                x = i < FS / 2u ? x : 0;         /* silence after 0.5 s */
            ana_put(&a, x, x);
        }
        ana_finish(&a, &p);
        p.nonkeys = 1;
        p.tail_db = k == 4 ? -40 : -90;         /* a tail still at -40 dBFS after 6 s */
        p.params = k == 6;
        p.cpu_mean = k == 7 ? GL_CPU_BUDGET + 100 : 1500;
        p.cpu_half = 1600;
        p.est = k == 8 ? 1000 : 1700;
        judge_pt(&p);
        printf("selftest: %-9s %s%s\n", NAME[k], p.fails ? "flagged:" : "passes", p.why);
        fails += k ? !p.fails : p.fails != 0;
    }
    printf("selftest: %s\n", fails ? "FAILED" : "every detector fires on its fault, the clean signal passes");
    return fails != 0;
}

/* ------------------------------------------------------------------- main --- */
static void usage_sweep(void)
{
    fprintf(stderr, "usage: guard_sweep [--full] [--jobs N] [--seed N] [--bars N] [--tail S] WORLD.wblob ...\n"
                    "       guard_sweep --dump-slots WORLD.wblob | --selftest\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int full = 0, i;
    uint32_t jobs = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN), w, fails = 0;
    host_tracks_init();
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    song.playing = 0;
    transport_req = 0;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--full"))
            full = 1;
        else if (!strcmp(a, "--jobs") && i + 1 < argc)
            jobs = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--seed") && i + 1 < argc)
            sw_rs = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--bars") && i + 1 < argc)
            sw_bars = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(a, "--tail") && i + 1 < argc)
            sw_tail = atof(argv[++i]);
        else if (!strcmp(a, "--dump-slots") && i + 1 < argc)
            return dump_slots(argv[++i]);
        else if (!strcmp(a, "--selftest"))
            return selftest();
        else if (!strcmp(a, "--list"))
            sw_list = 1;
        else if (!strcmp(a, "--var") && i + 1 < argc)
            sw_var = atoi(argv[++i]);
        else if (!strcmp(a, "--calib"))
            sw_calib = 1, setvbuf(stdout, 0, _IOLBF, 0);
        else if (a[0] == '-' || nworld >= SW_MAXW)
            usage_sweep();
        else {
            if (!read_all(a))
                usage_sweep();
            wpath[nworld] = a;
            wblob[nworld] = (uint8_t *)blob;
            wlen[nworld++] = blob_n;
        }
    }
    if (!nworld || !sw_bars || jobs < 1)
        usage_sweep();
    if (getenv("SWEEP") && !strcmp(getenv("SWEEP"), "full"))
        full = 1;
    instr_calibrate();
    make_points(full);
    printf("guard_sweep: %s, %u points over %u World(s), %u bars + %.0f s tail each, %u jobs\n",
           full ? "full" : "quick", npts, nworld, sw_bars, sw_tail, jobs);
    fflush(stdout);
    run_points(jobs);
    printf("  %-24s %6s %8s %8s %16s %10s %13s %8s %9s %5s\n", "World", "points", "peak", "tail", "loudness (LUFS)",
           "jump/alone", "CPU mean/half", "estimate", "values", "fails");
    for (w = 0; w < nworld; w++) {
        double pk = -INFINITY, tl = -INFINITY, lo = INFINITY, hi = -INFINITY, cm = 0, ch = 0, jw[2];
        uint32_t i2, n = 0, f = 0, jb = 0, est = 0, chk = 0, holds = 0;
        const char *nm = strrchr(wpath[w], '/') ? strrchr(wpath[w], '/') + 1 : wpath[w];
        for (i2 = 0; i2 < npts; i2++) {
            pt_t *p = &pts[i2];
            if (p->w != w)
                continue;
            n++;
            pk = p->peak > pk ? p->peak : pk;
            tl = p->tail_db > tl ? p->tail_db : tl;
            if (isfinite(p->lufs)) {
                lo = p->lufs < lo ? p->lufs : lo;
                hi = p->lufs > hi ? p->lufs : hi;
            }
            cm = p->cpu_mean > cm ? p->cpu_mean : cm;
            ch = p->cpu_half > ch ? p->cpu_half : ch;
            est = p->est > est ? p->est : est;
            chk += p->checked;
            holds += p->hold;
            if (sw_list)
                printf("  %s %c/%u %4u %4u %4u %4u: peak %6.2f tail %6.1f LUFS %6.1f rms %6.1f slew %.2f lim %.2f cpu %5.0f/%-5.0f "
                       "est %4u vmax %u lay %x%s\n", nm, 'A' + p->s, p->v, p->pos[0], p->pos[1], p->pos[2], p->pos[3], p->peak,
                       p->tail_db, p->lufs, p->rms, p->slew, p->limin / LIM_T, p->cpu_mean, p->cpu_half, p->est, p->vmax,
                       p->layers, p->why);
            if (p->fails) {
                if (f++ < 12)
                    printf("  FAIL %s %c/%u at %u %u %u %u%s:%s\n", nm, 'A' + p->s, p->v, p->pos[0], p->pos[1], p->pos[2],
                           p->pos[3], p->ext ? (p->ext < 3u ? " (SHAPE+MOVEMENT)" : " (LIVE FX)") : "", p->why);
            }
        }
        jumps(w, &jb, jw);
        printf("  %-24s %6u %8.2f %8.1f %7.1f..%-7.1f %5.1f/%-4.1f %6.0f/%-6.0f %8u %9u %5u%s\n", nm, n, pk, tl, lo, hi, jw[0],
               jw[1], cm, ch, est, chk, f + jb, holds ? " (CPU guard held)" : "");
        fails += f + jb;
    }
    printf("guard_sweep: %u points, %s\n", npts, fails ? "FAILED" : "all clean");
    return fails != 0;
}
