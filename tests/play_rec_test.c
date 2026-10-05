/* SPDX-License-Identifier: GPL-3.0-only */
/* PLAY REC on the host (firmware/src/play_rec.c; design 9.1, D14; UI spec 7; Phase 10): the whole firmware through
 * host/core.c (FELUCCA_WORLD 1: the real UI, flash and audio), driven block by block where timing matters (the keys
 * reach the audio interrupt at the next block, as on the device). Each scenario runs in its own process:
 *   timing    a two-bar phrase with humanised timing (up to +-0.45 of a 1/16 step, about +-1/32 note) at the World's
 *             quantise, at 0 and at 1: each note on its nearest step, its micro-timing the leftover scaled by
 *             1 - quantize (eighths of a step), in key as it sounded; REC closes the take on the next bar line; each
 *             step plays at its micro-timing (at most a block late), each note once a pass, nothing missing or
 *             doubled at the loop point or during the take
 *   guard     the same note in a step or half a step from itself not stacked; a chord over max_poly; a step full at
 *             max_notes; an out-of-key note recorded as a safe tone; a note held over the close: its ties inside the
 *             loop; REC inside a bar's first step closes on that bar line (the loop's first step sounds once)
 *   layers    overdub passes as layers (LOOP n); UNDO / REDO through 4 levels and past them (the ring wraps): the
 *             steps and length exactly; a new layer drops REDO; CLEAR and its undo
 *   scenes    a take across a scene change (the clock restarts on the bar: the keys grid goes on), the loop's phase
 *             through another; over every chord of a progression each loop note safe over the sounding chord (H12)
 *   persist   the loop (steps, micro-timing, length, layers) across a reboot of the flash image; a World switch
 *             clears it and the ring
 *   modes     armed: the first note starts the transport and is step 1; PLAY while armed; STOP ends a take and an
 *             overdub; REC with a loop while stopped arms an overdub; no note left after STOP; ADVANCED sees the loop
 *             as the keys track's steps, plays it the same, and a project capture (FUN4) keeps its micro-timing
 *   fuzz      random REC / keys / EDIT / scenes / PLAY with audio over two Worlds: no crash, the state, the ring and
 *             the loop always valid, no note left after STOP
 *   build/host/play_rec_test [scenario] */
#include <math.h>
#include <stdint.h>
#include <sys/wait.h>
#include "../host/core.c"

static int fails;
static void check(int ok, const char *what)
{
    printf("rec: %-98s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}

/* ---- time, block by block: the main loop every HOST_FRAME_BLOCKS blocks, as the device runs them */
#define SLEN ((uint64_t)BEAT_U / 4u)             /* a 1/16 step of the keys loop, clock units */
static uint32_t nblk;
static uint64_t adv(void) { return (uint64_t)CTL * (uint32_t)song.g[G_BPM]; }
static uint64_t gpos(void)                       /* the next block's start on the keys grid (clock units) */
{
    return (uint64_t)clk_beat * BEAT_U + clk_pos + (uint64_t)wrt.koff * SLEN;
}
static track_t *keys(void) { return &trk[wrt.keys_trk % NPART]; }

/* note-ons of the keys track (a voice started: its age), at the grid position of the block they sounded in */
#define NEV 2048
static struct {
    uint64_t p;
    uint8_t n;
} ev[NEV];
static uint32_t nev, ev_age, ev_on;
static void ev_start(void)
{
    nev = 0;
    ev_age = vage;
    ev_on = 1;
}
static void blk(void)
{
    static int16_t pcm[2 * CTL];
    const track_t *t = keys();
    uint32_t i, k, top;
    uint64_t p;
    host_audio(pcm, CTL);
    p = gpos() - (song.playing ? adv() : 0u);    /* (the block's start: the clock moved on by a block) */
    for (top = ev_age, i = 0; ev_on && i < NVOICE; i++)
        if (t->v[i].age > ev_age) {
            for (k = 0; k < nev && !(ev[k].p == p && ev[k].n == t->v[i].note); k++)
                ;
            if (k == nev && nev < NEV) {             /* (a unison part: one note, several voices) */
                ev[nev].p = p;
                ev[nev++].n = t->v[i].note;
            }
            top = t->v[i].age > top ? t->v[i].age : top;
        }
    ev_age = top;
    if (++nblk % HOST_FRAME_BLOCKS == 0)
        host_ui_frame();
}
static void frame(void)
{
    uint32_t i;
    for (i = 0; i < HOST_FRAME_BLOCKS; i++)
        blk();
}
static void wait_ms(uint32_t ms)
{
    uint32_t t0 = fm1_ms;
    while (fm1_ms - t0 < ms)
        frame();
}
static void press(uint32_t b) { host_button(b, 1); frame(); }
static void release(uint32_t b) { host_button(b, 0); frame(); }
static void tap(uint32_t b) { press(b); release(b); }
static void until_step(double at)                /* to the first block starting at grid step at, or after it */
{
    while (song.playing && gpos() < (uint64_t)(at * (double)SLEN))
        blk();
}
static void wait_bar(void)                       /* to just after the next bar line */
{
    until_step((double)((gpos() / SLEN / 16u + 1u) * 16u) + 0.05);
}
/* key k pressed so that it is heard (the firmware takes REC_LAT off) at grid step at, held hold steps: the start of
 * the block that saw it */
static uint64_t note_at(double at, uint32_t k, double hold)
{
    uint64_t p;
    until_step(at + (double)REC_LAT * (uint32_t)song.g[G_BPM] / (double)SLEN);
    host_key(k, 1);
    p = gpos();
    blk();
    until_step((double)p / (double)SLEN + hold);
    host_key(k, 0);
    blk();
    return p;
}
static int sounded(uint64_t p)                   /* the note that sounded in the block at p, -1: none */
{
    uint32_t i;
    for (i = 0; i < nev; i++)
        if (ev[i].p == p)
            return ev[i].n;
    return -1;
}

static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};   /* F3 .. G5 */
static uint32_t rnd_s = 2026;
static uint32_t rnd(uint32_t n)
{
    rnd_s = rnd_s * 1103515245u + 12345u;
    return (rnd_s >> 16) % n;
}

static void boot_world(const char *name)
{
    host_boot_device(1);
    if (host_boot(NULL) || host_world_load(host_world_factory_find(name))) {
        printf("rec: cannot boot %s\n", name);
        exit(1);
    }
    frame();
}
static uint8_t gfix[WF_GUARD_FIX];
static void set_guard(int follow, int quant, int recnotes)   /* the World's GUARD with these changed (-1: as it is) */
{
    if (gu.fix != gfix) {
        if (gu.fix)
            memcpy(gfix, gu.fix, sizeof gfix);
        else
            memset(gfix, WF_NONE, sizeof gfix);
    }
    if (follow >= 0)
        gfix[WF_G_FOLLOW] = (uint8_t)follow;
    if (quant >= 0)
        gfix[WF_G_QUANT] = (uint8_t)quant;
    if (recnotes >= 0)
        gfix[WF_G_RECNOTES] = (uint8_t)recnotes;
    gu.fix = gfix;
}

/* the loop is sound: a length of 1, 2 or 4 bars, no note past it, steps that can be, no note twice in a step, each
 * tie after a note and inside the loop */
static int loop_ok(const char **why)
{
    const track_t *t = keys();
    uint32_t len = trk_len(t), i, j, k;
    *why = "";
    if (len != 16u && len != 32u && len != 64u)
        return *why = "length", 0;
    for (i = 0; i < NSTEP; i++) {
        const step_t *s = &t->step[i];
        if (s->time > ST_REST || s->n > 4u)
            return *why = "a step that cannot be", 0;
        if (i >= len && s->time == ST_NOTE && s->n)
            return *why = "a note past the loop", 0;
        for (j = 0; s->time == ST_NOTE && j < s->n; j++)
            for (k = 0; k < j; k++)
                if (s->note[j] == s->note[k] || s->note[j] > 127u)
                    return *why = "a note twice in a step", 0;
        if (i < len && s->time == ST_TIE) {
            for (k = 1; k < len; k++) {
                const step_t *p = &t->step[(i + len - k) % len];
                if (p->time != ST_TIE)
                    break;
            }
            if (k == len || t->step[(i + len - k) % len].time != ST_NOTE || !t->step[(i + len - k) % len].n)
                return *why = "a tie after no note", 0;
        }
    }
    return 1;
}
static uint32_t loop_notes(void)
{
    const track_t *t = keys();
    uint32_t i, n = 0;
    for (i = 0; i < NSTEP; i++)
        n += t->step[i].time == ST_NOTE ? t->step[i].n : 0u;
    return n;
}
static int has(const step_t *s, uint32_t n)
{
    uint32_t i;
    for (i = 0; s->time == ST_NOTE && i < s->n; i++)
        if (s->note[i] == n)
            return 1;
    return 0;
}
static uint32_t micro(const step_t *s) { return (s->flags >> 2) & 7u; }
static int no_sound(void)                        /* after STOP: no voice sounding, none held */
{
    host_state_t st;
    host_state(&st);
    return st.voices == 0 && st.gated == 0;
}
static void quiet(void)
{
    uint32_t i;
    host_in_set(0, 0);
    host_stop();
    for (i = 0; i < 10u && !no_sound(); i++)
        wait_ms(1000);
}

/* where the firmware should put a note heard at grid position p (units) with quantise q (0..250), worked out
 * independently in floating point: its grid step and micro-timing. 0: on a rounding boundary (either is right) */
static int expect_at(uint64_t p, uint32_t q, uint64_t *abs, uint32_t *m, double *played, double *kept)
{
    uint64_t lat = (uint64_t)REC_LAT * (uint32_t)song.g[G_BPM], a = p / SLEN;
    double r, x;
    long qq;
    if (lat > SLEN / 2u)
        lat = SLEN / 2u;
    r = (double)(p - a * SLEN) - (double)lat;
    if (r > (double)(SLEN / 2u)) {
        a++;
        r -= (double)SLEN;
    }
    x = r * (250.0 - q) / 250.0 * 8.0 / (double)SLEN;
    qq = lround(x);
    *played = r / (double)SLEN;
    *kept = (double)qq / 8.0;
    if (qq < 0) {
        a--;
        qq += 8;
    }
    *abs = a;
    *m = (uint32_t)qq;
    return fabs(fabs(x) - floor(fabs(x)) - 0.5) > 0.02;
}

/* ================================================================== timing === */
static void sc_timing_q(int qset)
{
    static const uint8_t ST[] = {0, 2, 5, 7, 10, 12, 15, 17, 20, 22, 26, 29, 31};
    const uint32_t n = sizeof ST;
    uint64_t pp[sizeof ST], ab[sizeof ST], b0, pc = 0, lo, hi;
    uint32_t mm[sizeof ST], q, i, k, okstep = 1, okm = 1, okkey = 1, take_ev, cnt[sizeof ST] = {0}, stray = 0, late = 0;
    int nn[sizeof ST], amb = 0;
    double sp = 0, sk = 0, pl, kp;
    const track_t *t;
    char b[160];
    const char *why;
    boot_world("FROZEN LAKE");                   /* (a POLY keys part: every note-on its own voice; no swing) */
    set_guard(0, qset, -1);                      /* (loop follow off: the pitches come back as recorded) */
    q = guard_byte(gu.fix, WF_G_QUANT);
    t = keys();
    tap(B_PLAY);
    wait_bar();
    tap(B_REC);
    check(prec.st == PR_TAKE && t->p[P_SLEN] == 64, "REC playing: the take (the 64 steps: up to 4 bars)");
    b0 = (gpos() / SLEN / 16u + 1u) * 16u;       /* its first bar: the next (the first note's) */
    ev_start();
    for (i = 0; i < n; i++) {
        double j = (double)((int32_t)rnd(1801) - 900) / 2000.0;   /* -0.45 .. +0.45 of a step */
        if (i == 9u)
            host_button(B_REC, 1);               /* REC in the take's second bar: it closes on the next bar line */
        if (i == 11u)
            host_button(B_REC, 0);
        pp[i] = note_at((double)(b0 + ST[i]) + j, WHITE[(i * 5u) % 16u], 0.25);
        nn[i] = sounded(pp[i]);
    }
    while (prec.st == PR_TAKE && song.playing) {
        uint64_t p = gpos();
        blk();
        pc = p;
    }
    take_ev = 0;
    for (i = 0; i < nev; i++)
        take_ev += ev[i].p < pc;
    check(prec.st == PR_LOOP && pc >= (b0 + 32u) * SLEN && pc < (b0 + 32u) * SLEN + adv() && t->p[P_SLEN] == 32 &&
              prec.layers == 1, "the take closes on the bar line after REC (in its first block): a 2-bar loop, LOOP 1");
    check(take_ev == n, "during the take each note sounded once (none twice from the steps it went into)");
    for (i = 0; i < n; i++) {
        int sure = expect_at(pp[i] - (uint64_t)wrt.koff * 0u, q, &ab[i], &mm[i], &pl, &kp);
        const step_t *s = &t->step[ab[i] % 32u];
        amb += !sure;
        sp += fabs(pl);
        sk += fabs(kp);
        okstep &= nn[i] >= 0 && has(s, (uint32_t)nn[i]);
        okm &= !sure || micro(s) == mm[i];
        okkey &= nn[i] >= 0 && (hprog[hcur].scale >> (nn[i] % 12) & 1u);
        if ((nn[i] < 0 || !has(s, (uint32_t)nn[i]) || (sure && micro(s) != mm[i])) && fails < 20)
            printf("rec:   note %u at %.3f: step %llu micro %u (expected %llu %u), notes %u\n", i,
                   (double)pp[i] / SLEN - b0, (unsigned long long)(ab[i] % 32u), micro(s),
                   (unsigned long long)(ab[i] % 32u), mm[i], s->n);
    }
    snprintf(b, sizeof b, "quantise %u/250: %u notes on their nearest steps (the early ones late in the step before)",
             q, n);
    check(okstep && loop_notes() == n, b);
    snprintf(b, sizeof b, ".. micro-timing = leftover x (1 - quantize) in eighths (%d on a rounding edge); "
             "mean |played| %.3f -> |kept| %.3f step", amb, sp / n, sk / n);
    check(okm, b);
    check(q != 250u || sk == 0.0, ".. quantise 1: every note on the grid");
    check(sp == 0.0 || fabs(sk / sp - (250.0 - q) / 250.0) < 0.12 + 0.07 * (q == 0u),
          ".. the timing kept at the configured strength (kept / played = 1 - quantize, to the eighth)");
    check(okkey, ".. every recorded note in key, the note that sounded (Smart Keys)");
    check(loop_ok(&why), "the loop sound (lengths, ties, steps)");
    /* playback: two passes from the close */
    lo = (b0 + 32u) * SLEN;                      /* (the bar line: the close is in its first block) */
    hi = lo + 64u * SLEN;
    until_step((double)hi / (double)SLEN + 0.5);
    for (k = 0; k < nev; k++) {
        uint64_t a, into;
        int hit = 0;
        if (ev[k].p < lo || ev[k].p >= hi)
            continue;
        a = ev[k].p / SLEN;
        into = ev[k].p - a * SLEN;
        for (i = 0; i < n; i++)
            if (a % 32u == ab[i] % 32u && ev[k].n == nn[i]) {
                uint64_t want = mm[i] * SLEN / 8u;
                hit = 1;
                cnt[i]++;
                if (into < want || into >= want + adv() + 1u)
                    late++;
            }
        stray += !hit;
    }
    for (i = 0, k = 1; i < n; i++) {
        k &= cnt[i] == 2u;
        if (cnt[i] != 2u)
            printf("rec:   note %u (step %llu micro %u): %u times in two passes\n", i,
                   (unsigned long long)(ab[i] % 32u), mm[i], cnt[i]);
    }
    if (stray)
        for (i = 0; i < nev; i++)
            if (ev[i].p >= lo && ev[i].p < hi)
                printf("rec:   at %.3f note %u\n", (double)(ev[i].p - lo) / SLEN, ev[i].n);
    check(k && !stray, "playback, two passes: each note once a pass, none missing or doubled at the loop point");
    snprintf(b, sizeof b, ".. each at its step + micro-timing, at most one block (%u samples) late: %u late",
             (uint32_t)CTL, late);
    check(!late, b);
    quiet();
    check(no_sound(), "STOP: no note left sounding");
}
static void sc_timing(void)
{
    pid_t pid;
    int st, q[3] = {-1, 0, 250}, i;
    for (i = 0; i < 3; i++) {
        fflush(stdout);
        if (!(pid = fork())) {
            sc_timing_q(q[i]);
            fflush(stdout);
            _exit(fails != 0);
        }
        waitpid(pid, &st, 0);
        fails += !WIFEXITED(st) || WEXITSTATUS(st);
    }
}

/* =================================================================== guard === */
static void sc_guard(void)
{
    static const uint8_t CH[5] = {7, 9, 11, 12, 14};
    track_t *t;
    uint64_t b0, p, pc = 0;
    uint32_t i, k, n4, oo = 0, rec, base;
    const char *why;
    boot_world("FROZEN LAKE");                   /* (max_poly 4, max_notes 4) */
    set_guard(0, 0, -1);                         /* (quantise 0: the whole micro-timing, to reach the neighbours) */
    t = keys();
    tap(B_PLAY);
    wait_bar();
    tap(B_REC);
    b0 = (gpos() / SLEN / 16u + 1u) * 16u;
    ev_start();
    note_at((double)b0 + 4.0, WHITE[3], 0.2);
    note_at((double)b0 + 4.3, WHITE[3], 0.2);
    check(t->step[(b0 + 4u) % 64u].n == 1u, "the same note again in its step: not stacked");
    note_at((double)b0 + 10.7, WHITE[5], 0.2);  /* -> late in step 10 */
    note_at((double)b0 + 11.05, WHITE[5], 0.2); /* -> step 11, a quarter step after it: a duplicate */
    note_at((double)b0 + 13.0, WHITE[5], 0.2);
    check(t->step[(b0 + 10u) % 64u].n == 1u && micro(&t->step[(b0 + 10u) % 64u]) == 6u &&
          t->step[(b0 + 11u) % 64u].n == 0u && t->step[(b0 + 13u) % 64u].n == 1u,
          "the same note a quarter step from itself (in the next step): not stacked; two steps on: kept");
    until_step((double)b0 + 20.0 + 0.06);
    for (i = 0; i < 5u; i++)
        host_key(CH[i], 1);
    blk();
    for (i = 0; i < 10u; i++)
        blk();
    for (i = 0; i < 5u; i++)
        host_key(CH[i], 0);
    blk();
    n4 = t->step[(b0 + 20u) % 64u].n;
    check(n4 == 4u, "a chord of 5 keys over max_poly 4: 4 sound, 4 recorded");
    until_step((double)b0 + 24.0);
    for (k = 0; k < 12u && !oo; k++)             /* a note out of key, fed to the recorder as if it sounded */
        if (!((hprog[hcur].scale | harm.ct) >> k & 1u))
            oo = 60u + k;
    rec_note(t, oo, 100, 0, 0);
    {
        const step_t *s = &t->step[(b0 + 24u) % 64u];
        check(oo && s->n == 1u && s->note[0] != oo && (harm.safe >> (s->note[0] % 12u) & 1u) &&
                  s->note[0] >= skm[skcur].g.lo && s->note[0] <= skm[skcur].g.hi,
              "a note out of key: recorded as the nearest safe tone, in the keys range");
    }
    host_button(B_REC, 1);                       /* REC (in bar 2): closes on the next bar line */
    note_at((double)b0 + 26.0, WHITE[9], 0.2);
    host_button(B_REC, 0);
    until_step((double)b0 + 28.0);
    host_key(WHITE[11], 1);                      /* a note held over the close */
    while (prec.st == PR_TAKE) {
        p = gpos();
        blk();
        pc = p;
    }
    until_step((double)b0 + 40.0);
    host_key(WHITE[11], 0);
    blk();
    check(prec.st == PR_LOOP && pc >= (b0 + 32u) * SLEN && pc < (b0 + 32u) * SLEN + adv() && t->p[P_SLEN] == 32 &&
              loop_ok(&why), "the loop: 2 bars, closed on the bar line, sound");
    if (*why)
        printf("rec:   (%s)\n", why);
    {
        uint32_t s = (b0 + 28u) % 32u, ties = 0;
        while (t->step[(s + 1u + ties) % 32u].time == ST_TIE && ties < 40u)
            ties++;
        check(t->step[s].time == ST_NOTE && ties == 3u, "a note held over the close: tied to the loop's end, no further "
              "(a note never past the loop)");
    }
    /* a step full at max_notes: overdub a fifth note into the chord's step */
    tap(B_REC);
    base = loop_notes();
    rec = (uint32_t)((gpos() / SLEN / 32u + 1u) * 32u);
    note_at((double)(rec + (b0 + 20u) % 32u), WHITE[1], 0.2);
    check(t->step[(b0 + 20u) % 32u].n == 4u && loop_notes() == base, "overdub into the full step: refused (max_notes 4)");
    set_guard(-1, -1, 2);                        /* GUARD max_notes 2 */
    note_at((double)(rec + 32u + (b0 + 13u) % 32u), WHITE[8], 0.2);
    note_at((double)(rec + 64u + (b0 + 13u) % 32u), WHITE[10], 0.2);
    check(t->step[(b0 + 13u) % 32u].n == 2u, "GUARD max_notes 2: a second note into a step, not a third");
    tap(B_REC);
    check(prec.st == PR_LOOP, "REC: overdub off");
    quiet();
    check(no_sound(), "STOP: no note left sounding");
}

/* REC inside a bar's first step closes on that bar line: the loop's first step sounds there, once */
static void sc_guard_edge(void)
{
    track_t *t;
    uint64_t b0, line;
    uint32_t i, c0 = 0, n0;
    boot_world("FROZEN LAKE");
    t = keys();
    tap(B_PLAY);
    wait_bar();
    tap(B_REC);
    b0 = (gpos() / SLEN / 16u + 1u) * 16u;
    note_at((double)b0, WHITE[4], 0.2);
    note_at((double)b0 + 6.0, WHITE[7], 0.2);
    n0 = (uint32_t)sounded(0) + 0u;
    line = (b0 + 16u) * SLEN;
    until_step((double)(b0 + 16u) + 0.02);
    ev_start();
    host_button(B_REC, 1);
    for (i = 0; i < 2u * HOST_FRAME_BLOCKS && prec.st == PR_TAKE; i++)
        blk();
    host_button(B_REC, 0);
    check(prec.st == PR_LOOP && t->p[P_SLEN] == 16 && gpos() < line + SLEN,
          "REC inside a bar's first step: closed on that bar line (1 bar, not 2)");
    until_step((double)(b0 + 16u) + 1.0);
    for (i = 0; i < nev; i++)
        c0 += ev[i].p >= line && ev[i].p < line + SLEN;
    check(c0 == 1u && t->step[b0 % 16u].n == 1u, ".. the loop's first step sounds in it, once");
    (void)n0;
    quiet();
}

/* ================================================================== layers === */
typedef struct {
    step_t st[NSTEP];
    int16_t len;
    uint8_t layers;
} snap_t;
static void snap(snap_t *s)
{
    memcpy(s->st, keys()->step, sizeof s->st);
    s->len = keys()->p[P_SLEN];
    s->layers = prec.layers;
}
static int same(const snap_t *s)
{
    return !memcmp(s->st, keys()->step, sizeof s->st) && s->len == keys()->p[P_SLEN] && s->layers == prec.layers;
}
static void redo(void)
{
    press(B_EDIT);
    tap(B_OCTUP);
    release(B_EDIT);
}
static void sc_layers(void)
{
    static snap_t S[9], X;
    track_t *t;
    uint64_t b0, base;
    uint32_t i, ok;
    char b[120];
    boot_world("FROZEN LAKE");
    t = keys();
    tap(B_PLAY);
    wait_bar();
    tap(B_REC);
    b0 = (gpos() / SLEN / 16u + 1u) * 16u;
    note_at((double)b0 + 0.0, WHITE[2], 0.2);
    host_button(B_REC, 1);
    note_at((double)b0 + 8.0, WHITE[6], 0.2);
    host_button(B_REC, 0);
    while (prec.st == PR_TAKE)
        blk();
    check(prec.st == PR_LOOP && t->p[P_SLEN] == 16 && prec.layers == 1 && prec.rn == 1u, "the take: a 1-bar loop, "
          "LOOP 1, one undo");
    snap(&S[1]);
    tap(B_REC);
    base = (gpos() / SLEN / 16u + 1u) * 16u;
    for (i = 2; i <= 8u; i++) {                  /* seven overdub passes, a note each: layers 2..8 */
        note_at((double)(base + 16u * (i - 2u) + i), WHITE[(8u + i) % 16u], 0.2);
        snap(&S[i]);
    }
    tap(B_REC);
    snprintf(b, sizeof b, "seven overdub passes: LOOP 8, a layer a pass, the ring full (4 of 7 undos kept)");
    check(prec.layers == 8 && prec.rn == PR_RING && prec.rc == PR_RING, b);
    for (i = 1, ok = 1; i <= 4u; i++) {
        tap(B_EDIT);
        ok &= same(&S[8 - i]);
    }
    check(ok, "UNDO x 4: each the loop before its layer, exactly (steps, length, LOOP n)");
    tap(B_EDIT);
    check(same(&S[4]) && pl.toast_t && !strcmp(pl.toast, "NOTHING TO UNDO"), "a fifth UNDO: nothing (the ring kept 4)");
    for (i = 5, ok = 1; i <= 8u; i++) {
        redo();
        ok &= same(&S[i]);
    }
    check(ok, "REDO x 4 (EDIT + OCT+): each layer back, exactly");
    redo();
    check(same(&S[8]) && !strcmp(pl.toast, "NOTHING TO REDO"), "a fifth REDO: nothing");
    tap(B_EDIT);
    tap(B_EDIT);
    check(same(&S[6]), "UNDO x 2");
    tap(B_REC);
    base = (gpos() / SLEN / 16u + 1u) * 16u;
    note_at((double)base + 13.0, WHITE[15], 0.2);
    tap(B_REC);
    snap(&X);
    redo();
    check(same(&X) && prec.rc == prec.rn, "a new layer after UNDO: the REDO side dropped");
    tap(B_EDIT);
    check(same(&S[6]), "UNDO: before the new layer");
    redo();
    press(B_REC);
    wait_ms(1700);
    release(B_REC);
    check(prec.st == PR_EMPTY && !loop_notes() && prec.layers == 0, "REC held 1.5 s: CLEAR (EMPTY)");
    tap(B_EDIT);
    check(same(&X) && prec.st == PR_LOOP, "UNDO: the cleared loop back");
    quiet();
    check(no_sound(), "STOP: no note left sounding");
}

/* ================================================================== scenes === */
static void sc_scenes(void)
{
    track_t *t;
    uint64_t b0;
    uint32_t i, k, prev, jumps = 0, steps = 0, len, sc, notes = 0, unsafe = 0, chords = 0, ct2;
    int n1, n2;
    const char *why;
    boot_world("FROZEN LAKE");
    t = keys();
    tap(B_PLAY);
    wait_bar();
    tap(B_REC);
    b0 = (gpos() / SLEN / 16u + 1u) * 16u;
    ev_start();
    n1 = sounded(note_at((double)b0 + 2.0, WHITE[4], 0.2));
    sc = (wrt.scene + 1u) % WF_NSCENE;
    check(world_request(sc, wrt.var) == WE_OK, "a scene asked for during the take (on the next bar)");
    until_step((double)b0 + 16.5);
    check(wrt.scene == sc && wrt.koff == b0 + 16u, "the scene commits on the bar: the clock restarts, the keys grid goes "
          "on (koff)");
    n2 = sounded(note_at((double)b0 + 18.0, 13, 0.2));   /* a black key: a tone of the new scene's first chord */
    ct2 = harm.ct;
    host_button(B_REC, 1);
    blk();
    note_at((double)b0 + 21.0, WHITE[1], 0.2);
    host_button(B_REC, 0);
    while (prec.st == PR_TAKE)
        blk();
    check(prec.st == PR_LOOP && t->p[P_SLEN] == 32 && n1 >= 0 && n2 >= 0 && has(&t->step[(b0 + 2u) % 32u], (uint32_t)n1) &&
              has(&t->step[(b0 + 18u) % 32u], (uint32_t)n2) && (ct2 >> (n2 % 12) & 1u) && loop_ok(&why),
          "a take across a scene (and chord) change: each note in place (a bar apart) as it sounded (the black key: "
          "the new chord's tone), 2 bars");
    /* the loop's phase through another scene change: each step entry the next one */
    len = trk_len(t);
    prev = t->seq_idx;
    sc = (wrt.scene + 1u) % WF_NSCENE;
    world_request(sc, wrt.var);
    for (i = 0; i < 2u * 16u * 3u * 400u && steps < 96u; i++) {
        uint32_t a = t->seq_abs;
        blk();
        if (t->seq_abs != a) {
            jumps += t->seq_idx != (prev + 1u) % len;
            prev = t->seq_idx;
            steps++;
        }
    }
    check(wrt.scene == sc && steps >= 96u && !jumps, "the loop through a scene change: every step the next one (in "
          "phase, no restart)");
    /* loop follow (H12): over every chord of the progression, each note the loop plays safe over the chord */
    ev_start();
    {
        uint32_t beats = hprog[hcur].beats ? hprog[hcur].beats : 16u, gen = harm.gen;
        uint64_t end = gpos() + (uint64_t)beats * 4u * SLEN + 4u * SLEN;
        uint32_t seen = 0;
        while (gpos() < end) {
            uint32_t n0 = nev, safe;
            blk();
            safe = harm.safe;
            for (k = n0; k < nev; k++) {
                notes++;
                unsafe += !(safe >> (ev[k].n % 12u) & 1u);
            }
            if (harm.gen != gen) {
                gen = harm.gen;
                chords++;
            }
        }
        (void)seen;
    }
    {
        char b[128];
        snprintf(b, sizeof b, "loop follow: over the whole progression (%u chord changes) its %u notes all safe over "
                 "the sounding chord", chords, notes);
        check(notes > 8u && chords >= 1u && !unsafe, b);
    }
    quiet();
    check(no_sound(), "STOP: no note left sounding");
}

/* ================================================================= persist === */
static const char *outdir = "build/host";
static void sc_persist(void)
{
    static snap_t S, R;
    static char img[512], sp[512];
    pid_t pid;
    int st;
    FILE *f;
    snprintf(img, sizeof img, "%s/play-rec-session.bin", outdir);
    snprintf(sp, sizeof sp, "%s/play-rec-session.loop", outdir);
    fflush(stdout);
    if (!(pid = fork())) {                       /* a loop recorded, the session saved when quiet */
        track_t *t;
        uint64_t b0;
        uint32_t i, c0, mic = 0;
        boot_world("FROZEN LAKE");
        set_guard(-1, 0, -1);                    /* (quantise 0: micro-timing on most steps, to keep) */
        t = keys();
        tap(B_PLAY);
        wait_bar();
        tap(B_REC);
        b0 = (gpos() / SLEN / 16u + 1u) * 16u;
        for (i = 0; i < 6u; i++)
            note_at((double)(b0 + 3u * i) + 0.3, WHITE[i * 2u], 0.2);
        host_button(B_REC, 1);
        blk();
        host_button(B_REC, 0);
        while (prec.st == PR_TAKE)
            blk();
        tap(B_REC);
        note_at((double)((gpos() / SLEN / 16u + 1u) * 16u) + 5.0, WHITE[13], 0.2);
        tap(B_REC);
        for (i = 0; i < NSTEP; i++)
            mic += t->step[i].n && micro(&t->step[i]);
        snap(&S);
        host_stop();
        c0 = host_flash_changes();
        for (i = 0; i < 40u && host_flash_changes() == c0; i++)
            wait_ms(1000);
        check(host_flash_changes() != c0 && mic >= 3u && S.layers == 2,
              "a loop with micro-timing and 2 layers, stopped and quiet: the session saved");
        host_flash_write(img);
        if ((f = fopen(sp, "wb")) != NULL) {
            fwrite(&S, sizeof S, 1, f);
            fclose(f);
        }
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
    fflush(stdout);
    if (!(pid = fork())) {                       /* the reboot */
        uint32_t i, n;
        f = fopen(sp, "rb");
        n = f && fread(&S, sizeof S, 1, f) == 1;
        if (f)
            fclose(f);
        host_boot_device(1);
        if (host_boot(img))
            exit(1);
        frame();
        check(n && wrt.id == WORLD_INDEX[host_world_factory_find("FROZEN LAKE")].id && prec.st == PR_LOOP && same(&S) &&
                  !song.playing && prec.rn == 0u,
              "reboot: FROZEN LAKE, the loop as it was (steps, micro-timing, length, LOOP 2), stopped, no undo");
        ev_start();
        tap(B_PLAY);
        wait_ms(4500);
        for (i = 0, n = 0; i < nev; i++)
            n++;
        check(n >= 7u, "PLAY: the loop plays");
        snap(&R);
        host_stop();
        wait_ms(500);
        tap(B_REC);                              /* (something in the ring, then another World) */
        tap(B_REC);
        check(host_world_load(host_world_factory_find("DUSTY CAFE")) == 0 && wrt.active, "another World");
        check(!loop_notes() && prec.st == PR_EMPTY && prec.rn == 0u && prec.layers == 0u && !wrt.koff,
              "a World switch clears the loop and the ring");
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
}

/* =================================================================== modes === */
static void sc_modes(void)
{
    static project_t pj;
    track_t *t;
    uint32_t i, ok, n, k0 = WHITE[5], c0, saw[3] = {0};
    uint64_t b0;
    const char *why;
    boot_world("FROZEN LAKE");
    t = keys();
    tap(B_REC);
    for (i = 0; i < 40u; i++, frame())
        saw[host_button_led(B_REC)]++;
    check(prec.st == PR_ARMED && !song.playing && saw[0] && saw[2] && pl.scr == PS_REC,
          "REC stopped: ARMED (REC blinks, RECORDING), the transport waits");
    ev_start();
    host_key(k0, 1);
    blk();
    check(song.playing && prec.st == PR_TAKE && has(&t->step[0], (uint32_t)sounded(0)) && t->step[0].n == 1u,
          "armed, a keys note: the transport starts, the note is step 1 of the take");
    for (i = 0; i < 20u; i++)
        blk();
    host_key(k0, 0);
    note_at(6.0, WHITE[8], 0.2);
    until_step(9.0);
    tap(B_PLAY);
    frame();
    check(!song.playing && prec.st == PR_LOOP && t->p[P_SLEN] == 16 && loop_notes() == 2u && loop_ok(&why),
          "STOP during the take: what was played loops (the bar under way: 1 bar)");
    ev_start();
    tap(B_PLAY);
    until_step(15.5);
    for (i = 0, n = 0; i < nev; i++)
        n++;
    check(n == 2u, "PLAY: the loop plays its 2 notes");
    tap(B_REC);
    note_at(20.0, WHITE[10], 3.0);               /* (held: its ties) */
    host_key(WHITE[12], 1);
    blk();
    tap(B_PLAY);                                 /* STOP while overdubbing, a key still held */
    host_key(WHITE[12], 0);
    frame();
    check(!song.playing && prec.st == PR_LOOP && host_button_led(B_REC) == 1, "STOP during an overdub: LOOP (REC dim)");
    quiet();
    check(no_sound(), "STOP with a held recorded note: no note left sounding");
    tap(B_REC);
    for (i = 0, saw[0] = saw[2] = 0; i < 40u; i++, frame())
        saw[host_button_led(B_REC)]++;
    check(prec.st == PR_OVERDUB && prec.arm && !song.playing && saw[0] && saw[2],
          "REC with a loop, stopped: an overdub armed (REC blinks)");
    c0 = loop_notes();
    host_key(WHITE[14], 1);
    blk();
    host_key(WHITE[14], 0);
    blk();
    check(song.playing && prec.st == PR_OVERDUB && !prec.arm && loop_notes() == c0 + 1u && has(&t->step[0], (uint32_t)sounded(0)),
          ".. a keys note starts the transport and is recorded on step 1");
    quiet();
    press(B_REC);
    wait_ms(1700);
    release(B_REC);
    check(prec.st == PR_EMPTY, "CLEAR");
    tap(B_REC);
    tap(B_PLAY);
    frame();
    check(song.playing && prec.st == PR_TAKE && prec.b0 == SEQ_NONE, "ARMED, then PLAY: the take starts with the transport");
    host_stop();
    frame();
    check(prec.st == PR_EMPTY && !loop_notes(), "STOP before a note: no take, EMPTY");
    /* ADVANCED: the loop is the keys track's steps */
    set_guard(-1, 0, -1);
    tap(B_PLAY);
    wait_bar();
    tap(B_REC);
    b0 = (gpos() / SLEN / 16u + 1u) * 16u;
    for (i = 0; i < 5u; i++)
        note_at((double)(b0 + 3u * i) + 0.35, WHITE[1 + 2u * i], 0.2);
    host_button(B_REC, 1);
    blk();
    host_button(B_REC, 0);
    while (prec.st == PR_TAKE)
        blk();
    {
        static snap_t S;
        uint32_t mic = 0, len = trk_len(t), events;
        snap(&S);
        for (i = 0; i < NSTEP; i++)
            mic += t->step[i].n && micro(&t->step[i]);
        play_adv_enter();
        frame();
        check(wrt.mode == WM_ADV && song.sel == wrt.keys_trk && !wrt.keys_on && TSEL == t && same(&S) && mic >= 3u,
              "ADVANCED: SLOOP's sequencer on the keys track, its steps the loop (with its micro-timing)");
        ev_start();
        until_step((double)((gpos() / SLEN / len + 1u) * len));
        ev_start();
        until_step((double)((gpos() / SLEN / len + 1u) * len));
        for (i = 0, events = 0; i < nev; i++)
            events++;
        check(events == loop_notes(), "ADVANCED: the loop plays the same (a pass: each note once)");
        proj_capture(&pj);                       /* TOOLS > SAVE's snapshot (FUN4) */
        check(!memcmp(pj.t[wrt.keys_trk].step, S.st, sizeof S.st) && pj.t[wrt.keys_trk].p[P_SLEN] == S.len,
              "a project capture (FUN4) keeps the loop's steps and micro-timing bit for bit");
        play_adv_exit();
        frame();
        check(wrt.mode == WM_PLAY && prec.st == PR_LOOP && same(&S), "back to PLAY: LOOP, the loop untouched");
    }
    ok = 1;
    quiet();
    check(ok && no_sound(), "STOP: no note left sounding");
}

/* ==================================================================== fuzz === */
static void sc_fuzz(uint32_t frames_n)
{
    uint32_t i, r, keysd = 0, bad = 0, closes = 0, undos = 0, takes = 0, last = PR_EMPTY, worlds = 0;
    const char *why = "";
    boot_world("NEON RAIN");                     /* (a LEGATO keys part, max_poly 2) */
    tap(B_PLAY);
    for (i = 0; i < frames_n; i++) {
        r = rnd(1000);
        if (r < 260) {
            keysd ^= 1u << WHITE[rnd(16)] | (rnd(4) == 0 ? 1u << (1u + 2u * rnd(8)) : 0u);
            host_in_set(keysd & 0x7FFFFFFu, fm1_in.buttons);
        } else if (r < 300) {
            tap(B_REC);
        } else if (r < 304) {
            press(B_REC);
            wait_ms(800 + rnd(1000));
            release(B_REC);
        } else if (r < 330) {
            tap(B_EDIT);
            undos++;
        } else if (r < 345) {
            redo();
        } else if (r < 360) {
            world_request(rnd(WF_NSCENE), wrt.var);
        } else if (r < 372) {
            tap(B_PLAY);
        } else if (r < 374 && rnd(3) == 0) {
            host_in_set(0, 0);
            keysd = 0;
            host_world_load(host_world_factory_find(worlds++ & 1u ? "NEON RAIN" : "DUSTY CAFE"));   /* (swing 34) */
            tap(B_PLAY);
        }
        frame();
        takes += prec.st == PR_TAKE && last != PR_TAKE;
        closes += last == PR_TAKE && prec.st == PR_LOOP;
        last = prec.st;
        if (prec.st >= PR_COUNT || prec.rc > prec.rn || prec.rn > PR_RING || prec.rh >= PR_RING || prec.layers > 99u ||
            (prec.st == PR_TAKE && keys()->p[P_SLEN] != 64) ||
            (prec.st != PR_TAKE && !loop_ok(&why)) || (prec.st == PR_EMPTY && loop_notes()) ||
            ((prec.st == PR_LOOP || prec.st == PR_OVERDUB) && !loop_notes()) ||
            (song.rec & ~(1u << wrt.keys_trk)) || wrt.mode != WM_PLAY) {
            if (!bad++)
                printf("rec: fuzz: invalid at frame %u: state %u ring %u/%u/%u len %d notes %u rec %u: %s\n", i, prec.st,
                       prec.rc, prec.rn, prec.rh, keys()->p[P_SLEN], loop_notes(), song.rec, why);
        }
    }
    printf("rec: fuzz: %u frames, %u takes, %u closed into loops, %u EDIT taps, %u World switches\n", frames_n, takes,
           closes, undos, worlds);
    check(!bad, "fuzz: the state, the ring and the loop always valid; the recording only on the keys track");
    check(takes > 5u && closes > 2u, "fuzz: takes recorded and closed");
    quiet();
    check(no_sound(), "fuzz: after STOP no note left (no stuck notes)");
}
static void sc_fuzz_main(void)
{
    const char *e = getenv("REC_FUZZ");
    sc_fuzz(e ? (uint32_t)atoi(e) : 6000u);
}

static int run(const char *name, void (*fn)(void))
{
    pid_t pid;
    int st;
    fflush(stdout);
    if (!(pid = fork())) {
        fn();
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("rec: scenario %s FAILED%s\n", name, WIFEXITED(st) ? "" : " (crashed)");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } SC[] = {{"timing", sc_timing}, {"guard", sc_guard}, {"edge", sc_guard_edge}, {"layers", sc_layers},
              {"scenes", sc_scenes}, {"persist", sc_persist}, {"modes", sc_modes}, {"fuzz", sc_fuzz_main}};
    const char *only = argc > 2 ? argv[2] : "";
    int bad = 0;
    uint32_t i;
    if (argc > 1)
        outdir = argv[1];
    for (i = 0; i < sizeof SC / sizeof SC[0]; i++)
        if (!*only || !strcmp(only, SC[i].name))
            bad += run(SC[i].name, SC[i].fn);
    printf(bad ? "PLAY REC TEST FAILED\n" : "play rec test: all checks passed\n");
    return bad != 0;
}
