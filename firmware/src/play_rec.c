/* SPDX-License-Identifier: GPL-3.0-only */
/* PLAY REC (docs/design/play-mode-architecture.md 9.1, D14; UI spec 7): REC captures what the player just played on
 * the World's keys track as a loop, then lays more on it, with an undo ring. The recording is SLOOP's own (seq.c
 * song.rec, rec_note, rec_target, step_add, rec_hold, rec_release): the loop is the keys track's ordinary steps, so
 * ADVANCED shows and edits it in SLOOP's sequencer. Inert with no World; in ADVANCED, SLOOP's REC and undo apply.
 *
 *   states    EMPTY -REC, playing-> TAKE.  EMPTY -REC, stopped-> ARMED -a keys note (the transport starts, the note
 *             is step 1) or PLAY-> TAKE.  TAKE -REC-> LOOP on the next bar line (a press inside a bar's first step
 *             closes on that bar line: the one the player meant) -REC-> OVERDUB -REC-> LOOP.  STOP ends a take or
 *             an overdub (the take loops what was played).  REC held 1.5 s: CLEAR (EMPTY, an undo brings it back).
 *             A World switch, LEAVE WORLD: EMPTY, the ring emptied (prec_reset)
 *   the take  into the whole 64-step array (4 bars at 1/16) on the keys track's grid; its first bar is the first
 *             note's. Closed after n bars it loops 1, 2 or 4 bars (3: the close waits a bar), 4 bars close it by
 *             themselves. Step i folds onto i % len: the loop goes on from its first bar at the close, in phase
 *   guard     each note through guard.c's record guard: in key (Smart Keys: as it sounded) it stays, out of key the
 *             nearest safe tone; in the keys range; never the same note twice in a step or half a step from itself
 *             (no build-up); at most GUARD max_notes and max_poly a step; a held note ties on (rec_hold) and never
 *             round onto itself (guard_rec_len at the fold)
 *   quantise  gentle (D14): the nearest step, and the timing left over, scaled by 1 - GUARD quantize (0.75 by
 *             default), kept as the step's micro-timing: eighths of a step in step_t.flags bits 2-4 (early: the step
 *             before, late in it). A step's notes share it. H14: the step's trigger waits for it
 *   undo      4 snapshots (the loop's steps, length, layers): the first note of each pass that records pushes the
 *             loop as it was (H10), a CLEAR too. UNDO swaps the loop with the snapshot under the cursor, REDO with
 *             the one over it; a new layer drops the redo side, a fifth the oldest. Swaps: nothing lost
 *   phase     a scene's commit restarts the clock on its bar (world.c wreq_block); wrt.koff keeps the keys loop (and
 *             a take) going where it was (prec_rebase, seq.c trk_grid)
 *
 *   prec_block()                       seq.c events_block (H16), audio ISR, each block before the steps
 *   prec_owns, prec_note, prec_micro   seq.c rec_note (the keys track in PLAY)
 *   prec_mark()                        seq.c undo_mark (H10)
 *   prec_arm(t)                        seq.c arm_start: an armed REC's first note starts the transport
 *   prec_defer, prec_wait              seq.c seq_tick (H14)
 *   prec_rebase(), prec_reset()        world.c (a scene's clock restart; a World switch or none)
 *   prec_rec, prec_clear, prec_undo, prec_sync, prec_adv, prec_bars, prec_load     ui_play.c, world_store.c (main) */

enum { PR_EMPTY, PR_ARMED, PR_TAKE, PR_LOOP, PR_OVERDUB, PR_COUNT };
#define PR_RING 4
#define PR_TAKE_SESS 0xFFFFFFFEu         /* the take's pass: one layer however long */

static struct {
    volatile uint8_t st;                 /* PR_* */
    uint8_t arm;                         /* stopped: a keys note or PLAY starts the transport (ARMED, OVERDUB) */
    uint8_t creq;                        /* TAKE: REC asked to close it (the ISR picks the bar) */
    uint8_t cn;                          /* TAKE: it closes after this many bars, 0 = not asked */
    uint8_t layers;                      /* LOOP n */
    uint8_t rn, rc, rh;                  /* the ring: snapshots, the cursor (undo under it, redo from it), the oldest */
    uint8_t dm, m;                       /* H14: eighths the waiting step waits; the micro-timing being recorded */
    uint32_t b0;                         /* TAKE: the grid step of its first bar, SEQ_NONE = no note yet */
    uint32_t dabs;                       /* H14: the waiting step */
    uint32_t sess, pushed;               /* the pass of the note being recorded; the last one pushed */
} prec = {.b0 = SEQ_NONE, .pushed = SEQ_NONE};
typedef struct {
    int16_t len;
    uint8_t layers;
    step_t st[NSTEP];
} prsnap_t;
static prsnap_t prr[PR_RING];

static track_t *prec_trk(void) { return &trk[wrt.keys_trk % NPART]; }
static uint8_t prec_bit(void) { return (uint8_t)(1u << (wrt.keys_trk % NPART)); }
static int prec_owns(const track_t *t) { return wrt.active && wrt.keys_on && t == prec_trk(); }
static int prec_has(const track_t *t)    /* the loop holds a note */
{
    uint32_t i;
    for (i = 0; i < NSTEP; i++)
        if (t->step[i].time == ST_NOTE && t->step[i].n)
            return 1;
    return 0;
}
static int32_t prec_pos(const track_t *t)   /* TAKE: grid steps since its first bar */
{
    uint32_t into, slen;
    return (int32_t)(trk_grid(t, &into, &slen) - prec.b0);
}
static uint32_t prec_round(int32_t r)    /* the bars a take of r steps loops: the bar under way counts; 3 -> 4 */
{
    return r < 16 ? 1u : r < 32 ? 2u : 4u;
}

static void prec_reset(void)             /* a World switch, LEAVE WORLD: no loop, an empty ring (ISR or IRQs off) */
{
    memset(&prec, 0, sizeof prec);
    prec.b0 = prec.pushed = SEQ_NONE;
    song.rec = 0;
    wrt.koff = 0;
}

static void prec_take(track_t *t)        /* a take from now */
{
    t->p[P_SLEN] = NSTEP;
    t->p[P_SDIV] = WF_DIV_16;
    prec.st = PR_TAKE;
    prec.b0 = prec.pushed = SEQ_NONE;
    prec.creq = prec.cn = prec.arm = 0;
    song.rec |= prec_bit();
}

/* ---- the ring */
static void prec_swap(track_t *t, prsnap_t *s)   /* the loop <-> snapshot s */
{
    uint8_t *a = (uint8_t *)t->step, *b = (uint8_t *)s->st, x;
    uint32_t i;
    int16_t l = t->p[P_SLEN];
    for (i = 0; i < sizeof s->st; i++) {
        x = a[i];
        a[i] = b[i];
        b[i] = x;
    }
    t->p[P_SLEN] = s->len;
    s->len = l;
    x = prec.layers;
    prec.layers = s->layers;
    s->layers = x;
}
static void prec_push(const track_t *t)  /* the loop as it is onto the ring (its redo side dropped; full: the oldest) */
{
    prsnap_t *s;
    prec.rn = prec.rc;
    if (prec.rn == PR_RING) {
        prec.rh = (uint8_t)((prec.rh + 1u) % PR_RING);
        prec.rn--;
    }
    s = &prr[(prec.rh + prec.rn) % PR_RING];
    memcpy(s->st, t->step, sizeof s->st);
    s->len = t->p[P_SLEN];
    s->layers = prec.layers;
    prec.rc = ++prec.rn;
}
static void prec_mark(void)              /* H10: a note recorded; the first of its pass makes a layer */
{
    if (prec.sess == prec.pushed)
        return;
    prec.pushed = prec.sess;
    prec_push(prec_trk());
    prec.layers += prec.layers < 99u;
}

/* ---- recording (audio ISR) */
/* seq.c rec_note, the keys track in PLAY: where a note played now goes, through the record guard and gentle
 * quantise. *note: as it is recorded, *abs: its grid step, *later: that step has not sounded it yet. The step's
 * index, or NSTEP: not recorded (it still sounds) */
static uint32_t prec_note(track_t *t, uint32_t *note, uint32_t *abs, uint32_t *later)
{
    uint32_t into, slen, len = trk_len(t), a = trk_grid(t, &into, &slen), lat = REC_LAT * (uint32_t)song.g[G_BPM];
    uint32_t idx, m, j, n;
    int32_t r, half = (int32_t)(slen / 2u);
    const step_t *s;
    r = (int32_t)into - (int32_t)(lat < slen / 2u ? lat : slen / 2u);   /* as heard (rec_target) */
    if (r > half) {
        a++;                                             /* the nearest step */
        r -= (int32_t)slen;
    }
    if (prec.st == PR_TAKE && prec.b0 == SEQ_NONE)
        prec.b0 = a & ~15u;                              /* the take's first bar: the first note's */
    prec.sess = prec.st == PR_TAKE ? PR_TAKE_SESS : a / len;
    r = guard_rec_quant(r) * 8;                          /* what is left, scaled: eighths of a step, rounded */
    r = (r + (r < 0 ? -half : half)) / (int32_t)slen;
    idx = a % len;
    m = (uint32_t)r;
    if (r < 0) {                                         /* early: late in the step before */
        a--;
        idx = (idx + len - 1u) % len;
        m = (uint32_t)(r + 8);
    }
    n = *note = guard_rec_note(&skm[skcur].g, *note);
    for (j = len - 1u; j <= len + 1u; j++) {             /* the same note in the step, or half a step from it */
        s = &t->step[(idx + j) % len];
        r = (int32_t)(j - len) * 8 + (int32_t)((s->flags >> 2) & 7u) - (int32_t)m;
        if (guard_rec_dup(s, n) && (j == len || (r < 4 && r > -4)))
            return NSTEP;
    }
    s = &t->step[idx];
    if (!guard_rec_room(s) || (s->time == ST_NOTE && s->n >= skm[skcur].g.poly))
        return NSTEP;                                    /* the step is full (max_notes, max_poly) */
    prec.m = (uint8_t)m;
    r = (int32_t)(a - t->seq_abs);
    *later = t->seq_abs == SEQ_NONE || r > 0 || (!r && prec.dm && prec.dabs == a);
    *abs = a;
    return idx;
}
static void prec_micro(step_t *s)        /* after step_add: a step the note began takes its micro-timing */
{
    if (s->n == 1u)
        s->flags = (uint8_t)((s->flags & 3u) | prec.m << 2);
}

/* the take -> a loop of bars bars: the steps fold onto it, a note never ties round onto itself */
static void prec_close(track_t *t, uint32_t bars, int now)
{
    uint32_t len = 16u * bars, i, k, into, slen, a = trk_grid(t, &into, &slen);
    int32_t run;
    step_t *s, *d;
    if (now) {                                           /* on the bar line just passed: the loop's first step now, */
        s = &t->step[a % NSTEP];                         /* without what sounds live there already */
        t->rskip_abs = a;
        t->rskip_n = 0;
        for (k = 0; s->time == ST_NOTE && k < s->n; k++)
            t->rskip[t->rskip_n++] = s->note[k];
        t->seq_abs = SEQ_NONE;
    }
    for (i = len; i < NSTEP; i++) {
        s = &t->step[i];
        d = &t->step[i % len];
        for (k = 0; s->time == ST_NOTE && k < s->n; k++)
            if (!guard_rec_dup(d, s->note[k]) && guard_rec_room(d)) {
                uint32_t fresh = d->time != ST_NOTE || !d->n;
                step_add(t, i % len, s->note[k], s->vel, (s->lvl >> (2u * k)) & 3u, (s->rat >> (2u * k)) & 3u);
                if (fresh)
                    d->flags = s->flags;
            }
        if (s->time == ST_TIE && d->time != ST_NOTE)
            d->time = ST_TIE;
        memset(s, 0, sizeof *s);
        s->time = ST_REST;
    }
    for (i = 0; i < len && !(t->step[i].time == ST_NOTE && t->step[i].n); i++)
        ;                                                /* lengths, from a note on: a tie only after one (the */
    for (k = 0, run = -1; k < len; k++) {                /* fold's leftovers go), at most the loop less a step */
        d = &t->step[(i + k) % len];
        if (d->time == ST_NOTE && d->n) {
            run = 0;
        } else if (d->time == ST_TIE && run >= 0 && guard_rec_len((uint32_t)run + 1u, len - 1u) > (uint32_t)run) {
            run++;
        } else {
            if (d->time == ST_TIE)
                d->time = ST_REST;
            run = -1;
        }
    }
    t->p[P_SLEN] = (int16_t)len;
    t->rh_n = 0;                                         /* (a note held over the close: its ties end here) */
    song.rec &= (uint8_t)~prec_bit();
    prec.st = PR_LOOP;
    prec.cn = prec.creq = 0;
}
static void prec_end(track_t *t)         /* the take ends now (STOP): what was played loops; nothing: EMPTY */
{
    if (prec.b0 == SEQ_NONE) {
        prec.st = PR_EMPTY;
        song.rec &= (uint8_t)~prec_bit();
    } else {
        prec_close(t, prec_round(prec_pos(t)), 0);
    }
}

/* seq.c arm_start: armed and stopped, a keys note starts the transport (it is step 1) and the take */
static int prec_arm(track_t *t)
{
    if (!prec.arm || !prec_owns(t) || song.playing)
        return 0;
    seq_start();
    if (song.playing) {
        prec.arm = 0;
        if (prec.st == PR_ARMED)
            prec_take(t);
    }
    return 1;
}

/* H16, audio ISR, each block before the steps (a World active, PLAY or ADVANCED) */
static void prec_block(void)
{
    track_t *t = prec_trk();
    int32_t r;
    uint32_t n;
    if (!wrt.active)
        return;
    if (!song.playing) {
        if (prec.st == PR_TAKE)
            prec_end(t);
        else if (prec.st == PR_OVERDUB && !prec.arm)
            prec.st = PR_LOOP;
        wrt.koff = 0;                                    /* (PLAY starts the loop from its step 0) */
        return;
    }
    if (prec.arm) {                                      /* PLAY while armed */
        prec.arm = 0;
        if (prec.st == PR_ARMED)
            prec_take(t);
    }
    if (prec.st != PR_TAKE || prec.b0 == SEQ_NONE)
        return;
    r = prec_pos(t);
    if (prec.creq) {
        prec.creq = 0;
        n = r < 0 ? 0u : (uint32_t)r / 16u;
        if (!(r & 15) && (n == 1u || n == 2u)) {
            prec_close(t, n, 1);
            return;
        }
        prec.cn = (uint8_t)prec_round(r);
    }
    if (r >= 16 * (prec.cn ? prec.cn : 4))
        prec_close(t, prec.cn ? prec.cn : 4u, 0);
}

/* H14, seq.c seq_tick: a new step idx at grid step abs. 1 = it sounds later in its step (prec_wait) */
static int prec_defer(const track_t *t, uint32_t idx, uint32_t abs)
{
    if (!wrt.active || t != prec_trk())
        return 0;
    prec.dm = (uint8_t)(t->step[idx].time == ST_NOTE ? (t->step[idx].flags >> 2) & 7u : 0u);
    prec.dabs = abs;
    return prec.dm != 0;
}
static int prec_wait(track_t *t, uint32_t abs, uint32_t into, uint32_t slen)   /* H14: 1 = it waits still */
{
    if (!prec.dm || t != prec_trk())
        return 0;
    if (abs == prec.dabs && into * 8u < prec.dm * slen)
        return 1;
    prec.dm = 0;
    if (abs == prec.dabs)
        syn_step(t, abs % trk_len(t), abs, slen);
    return 0;
}

/* world.c wreq_block: a scene's commit is about to restart the clock on its bar; the keys grid goes on from here */
static void prec_rebase(void)
{
    uint32_t into, slen;
    wrt.koff = trk_grid(prec_trk(), &into, &slen);
}

/* ---- the main loop's (ui_play.c), IRQs off inside */
static void prec_rec(void)               /* REC pressed */
{
    track_t *t = prec_trk();
    fm1_irq_off();
    switch (prec.st) {
    case PR_EMPTY:
        if (song.playing) {
            prec_take(t);
        } else {
            prec.st = PR_ARMED;
            prec.arm = 1;
        }
        break;
    case PR_ARMED:
        prec.st = PR_EMPTY;
        prec.arm = 0;
        break;
    case PR_TAKE:
        if (prec.b0 == SEQ_NONE) {                       /* nothing played: no take */
            prec.st = PR_EMPTY;
            song.rec &= (uint8_t)~prec_bit();
        } else {
            prec.creq = 1;
        }
        break;
    case PR_LOOP:
        prec.st = PR_OVERDUB;
        prec.pushed = SEQ_NONE;
        prec.arm = !song.playing;
        song.rec |= prec_bit();
        break;
    default:
        prec.st = PR_LOOP;
        prec.arm = 0;
        song.rec &= (uint8_t)~prec_bit();
        break;
    }
    fm1_irq_on();
}
static void prec_sync(void)              /* LOOP, OVERDUB or EMPTY for what the loop holds (an undo, PLAY again) */
{
    const track_t *t = prec_trk();
    if (!wrt.active || prec.st == PR_ARMED || prec.st == PR_TAKE)
        return;                                          /* (no World yet: its commit resets this) */
    if (!prec_has(t)) {
        prec.st = PR_EMPTY;
        prec.arm = 0;
        song.rec &= (uint8_t)~prec_bit();
    } else if (prec.st != PR_OVERDUB) {
        prec.st = PR_LOOP;
    }
}
static void prec_clear(void)             /* REC held 1.5 s: the loop cleared (UNDO brings it back) */
{
    track_t *t = prec_trk();
    fm1_irq_off();
    if (prec_has(t))
        prec_push(t);
    steps_clear(t);
    t->rh_n = 0;                                         /* (a held note ties into no step of the old loop) */
    prec.layers = prec.arm = prec.dm = 0;
    prec.pushed = SEQ_NONE;
    prec.st = PR_EMPTY;
    song.rec &= (uint8_t)~prec_bit();
    fm1_irq_on();
}
static int prec_undo(int redo)           /* EDIT tapped (UNDO) / EDIT + OCT+ (REDO): 1 = done */
{
    int ok;
    fm1_irq_off();
    ok = prec.st != PR_ARMED && prec.st != PR_TAKE && (redo ? prec.rc < prec.rn : prec.rc > 0u);
    if (ok) {
        prec.rc = (uint8_t)(prec.rc - !redo);
        prec_swap(prec_trk(), &prr[(prec.rh + prec.rc) % PR_RING]);
        prec_trk()->rh_n = 0;                            /* (a held note: no tie into the other loop) */
        prec.rc = (uint8_t)(prec.rc + !!redo);
        prec.pushed = SEQ_NONE;                          /* (the next note: a new layer) */
        prec_sync();
    }
    fm1_irq_on();
    return ok;
}
static void prec_adv(void)               /* PLAY -> ADVANCED: an overdub ends, a take closes on its bar */
{
    fm1_irq_off();
    if (prec.st == PR_TAKE)
        prec.creq = 1;
    else if (prec.st == PR_ARMED)
        prec.st = PR_EMPTY;
    else if (prec.st == PR_OVERDUB)
        prec.st = PR_LOOP;
    prec.arm = 0;
    fm1_irq_on();
}
static uint32_t prec_bars(void)          /* TAKE: its bars so far, the one under way included (0: no note yet) */
{
    int32_t r;
    if (prec.st != PR_TAKE || prec.b0 == SEQ_NONE)
        return 0;
    r = prec_pos(prec_trk());
    return r < 0 ? 1u : r >= 64 ? 4u : (uint32_t)r / 16u + 1u;
}
/* world_store.c wsession_boot: the session's loop (n steps of 1/16, layers) back on the keys track, stopped */
static void prec_load(const step_t *st, uint32_t n, uint32_t layers)
{
    track_t *t = prec_trk();
    uint32_t i, k;
    if (!n || n > NSTEP)
        return;
    fm1_irq_off();
    steps_clear(t);
    t->rh_n = 0;
    for (i = 0; i < n; i++)
        if (st[i].time <= ST_REST && st[i].n <= 4u) {    /* (a step that cannot be: a REST) */
            t->step[i] = st[i];
            for (k = 0; k < 4u; k++)
                t->step[i].note[k] &= 127u;
        }
    t->p[P_SLEN] = (int16_t)n;
    t->p[P_SDIV] = WF_DIV_16;
    prec.layers = (uint8_t)layers;
    prec.st = PR_LOOP;
    prec_sync();
    fm1_irq_on();
}
