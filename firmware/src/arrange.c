/* SPDX-License-Identifier: GPL-3.0-only */
/* ENERGY as arrangement (PLAY MODE, docs/design/play-mode-architecture.md 5.7, decision D9): the ENERGY knob mostly
 * arranges. Its position (macro.c), plus the variation's bias, picks a band of the scene's ENERGY table, and the band
 * says what plays:
 *   layers    the tracks heard: the others are silent through wrt.mute (H1, core.h trk_silent: SLOOP's 8-block
 *             mute fade and its note block). The Smart Keys track never is
 *   lanes     the drum lanes allowed; and density: on the table's density lanes a hit sounds only on the steps of
 *             the band's 16-step mask (H13, seq.c seq_tick and seq_ratchets)
 *   play      per synth track a 16-step mask: a NOTE step whose bit is clear plays as a REST (H14)
 *   ratchets  the drum track's ratchets (without: each hit once)
 *   fills     the scene's fill on the last bar of every phrase (GUARD fills_every bars, else the progression's
 *             length): the drum track plays the fill's steps on that bar, through the same masks (H14)
 * When: the band follows the position with a hysteresis of ARR_HYST around each band's start, so that a knob resting
 * on an edge does not flap. A change then keeps GUARD's arrangement timing: the layers change on the next bar (every
 * second bar with mute_change 2), the lanes and masks on the next beat (the bar with density_change bar), and a change
 * begins no sooner than min_band_bars bars after the one before. Stopped, at once. A scene's commit (on the bar) brings
 * its own table and applies its band at once.
 *
 *   arr_stage(et, ...)   main loop (world.c world_stage): the scene's table, the variation's bias, the timing
 *   arr_commit(et)       world.c world_commit (the ISR on the bar, or IRQs off while stopped): the table, its band now
 *   arr_block()          audio ISR, seq.c events_block (H16), each block: the band and the changes on beats and bars
 *   arr_dskip, arr_dstep, arr_plays   audio ISR, the sequencer's hooks (H13, H14)
 * Phase 11 adds the rest of design 5.7 and 9.2: BEAT (MINIMAL GROOVE BUSY BREAK) intersected with these masks, and
 * the fills' place in scene transitions. */

#define ARR_HYST 20u                     /* per mille: a band starts this much past its edge, and ends this much below */

static uint32_t arr_u16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

/* main loop: ENERGY record e (0: the scene has none), GUARD's fixed bytes g (0: defaults), the scene's progression
 * length in beats, its fill (pool index or WF_NONE), the variation's bias -> the staged table et */
static void arr_stage(wetab_t *et, const uint8_t *e, const uint8_t *g, uint32_t beats, uint32_t fill, int32_t bias)
{
    uint32_t k, t, fe;
    memset(et, 0, sizeof *et);
    et->bias = (int8_t)bias;
    et->fill = (uint8_t)fill;
    et->mute_bars = (uint8_t)guard_byte(g, WF_G_MUTECHG);   /* GUARD's arrangement timing (guard.c) */
    et->dens_bar = (uint8_t)guard_byte(g, WF_G_DENSCHG);
    et->min_bars = (uint8_t)guard_byte(g, WF_G_BANDBARS);
    fe = guard_byte(g, WF_G_FILLS);
    et->phrase = (uint8_t)(fe ? fe : beats >= 4u ? beats / 4u : 1u);
    if (!et->mute_bars)
        et->mute_bars = 1;
    if (!e)
        return;
    et->dlanes = (uint16_t)arr_u16(e);
    et->n = (uint8_t)(e[2] <= WF_MAX_BANDS ? e[2] : WF_MAX_BANDS);
    for (k = 0; k < et->n; k++) {
        const uint8_t *b = e + WF_ENERGY_HDR + WF_BAND_LEN * k;
        et->from[k] = (uint16_t)(4u * b[0]);
        et->flags[k] = b[1];
        et->lanes[k] = (uint16_t)arr_u16(b + 2);
        et->dens[k] = (uint16_t)arr_u16(b + 4);
        for (t = 0; t < NPART; t++)
            et->play[k][t] = (uint16_t)arr_u16(b + 6u + 2u * t);
    }
}

static uint32_t arr_pos(void)            /* the ENERGY position with the variation's bias, 0..1000 */
{
    return (uint32_t)clamp((int32_t)arr.req + 4 * arr.et.bias, 0, 1000);
}
static uint32_t arr_band(uint32_t pos, uint32_t k, int hyst)   /* the band at pos, from band k (hyst: with hysteresis) */
{
    uint32_t h = hyst ? ARR_HYST : 0u;
    while (k + 1u < arr.et.n && pos >= arr.et.from[k + 1u] + h)
        k++;
    while (k && pos + h < arr.et.from[k])
        k--;
    return k;
}

static void arr_masks(uint32_t k)        /* band k's lanes, density, play masks, ratchets and fills */
{
    uint32_t t;
    arr.db = (uint8_t)k;
    if (!arr.et.n) {                                     /* no table: everything plays */
        arr.lanes = arr.dens = 0xFFFF;
        for (t = 0; t < NPART; t++)
            arr.play[t] = 0xFFFF;
        arr.ratchets = 1;
        arr.fills = 0;
        return;
    }
    arr.lanes = arr.et.lanes[k];
    arr.dens = arr.et.dens[k];
    for (t = 0; t < NPART; t++)
        arr.play[t] = arr.et.play[k][t];
    arr.ratchets = (arr.et.flags[k] & WF_B_RATCHETS) != 0;
    arr.fills = (arr.et.flags[k] & WF_B_FILLS) != 0;
}
static void arr_layers(uint32_t k)       /* band k's layers: wrt.mute, never the Smart Keys track */
{
    arr.mb = (uint8_t)k;
    wrt.mute = arr.et.n ? (uint8_t)(~(uint32_t)arr.et.flags[k] & WF_B_LAYERS & ~(1u << wrt.keys_trk)) : 0u;
}

static void arr_commit(const wetab_t *et)   /* the commit: the new table, its band at the position, at once */
{
    uint32_t k;
    arr.et = *et;
    k = arr_band(arr_pos(), 0, 0);
    arr.sel = arr.tgt = (uint8_t)k;
    arr_masks(k);
    arr_layers(k);
    arr.bars = 255;
    arr.beat = 0xFFFFFFFFu;
    arr.fill_now = 0;
}

static void arr_block(void)              /* H16: seq.c events_block, every block, before the steps */
{
    uint32_t k, bar, newbar;
    if (!wrt.active || !arr.et.n)
        return;
    k = arr.sel = (uint8_t)arr_band(arr_pos(), arr.sel, 1);
    if (!song.playing) {                                 /* stopped: at once */
        if (arr.mb != k || arr.db != k) {
            arr.tgt = (uint8_t)k;
            arr_masks(k);
            arr_layers(k);
        }
        arr.bars = 255;
        arr.beat = 0xFFFFFFFFu;
        arr.fill_now = 0;
        return;
    }
    if (clk_beat == arr.beat)
        return;                                          /* (changes happen on beats) */
    arr.beat = clk_beat;
    bar = clk_beat >> 2;
    newbar = !(clk_beat & 3u);
    if (newbar && arr.bars < 255u)
        arr.bars++;
    if (k != arr.tgt && arr.bars >= arr.et.min_bars) {  /* a change begins */
        arr.tgt = (uint8_t)k;
        arr.bars = 0;
    }
    if (arr.db != arr.tgt && (newbar || !arr.et.dens_bar))
        arr_masks(arr.tgt);                              /* lanes, density, play masks: this beat (or bar) */
    if (arr.mb != arr.tgt && newbar && !(bar % arr.et.mute_bars))
        arr_layers(arr.tgt);                             /* layers: this bar */
    if (newbar)
        arr.fill_now = (uint8_t)(arr.fills && arr.et.fill < WF_MAX_PAT && bar % arr.et.phrase == arr.et.phrase - 1u);
}

/* ---- the sequencer's hooks (audio ISR) */
static uint32_t arr_dskip(uint32_t idx)  /* H13: the drum lanes band db leaves out at step idx */
{
    uint32_t keep = arr.lanes;
    if (!(arr.dens >> (idx & 15u) & 1u))
        keep &= ~(uint32_t)arr.et.dlanes;
    return ~keep & 0xFFFFu;
}
static const dstep_t *arr_dstep(const dstep_t *s, uint32_t idx)   /* H14: the fill's step on a fill bar */
{
    return arr.fill_now ? &wpool[arr.et.fill].dstep[idx & 15u] : s;
}
static int arr_plays(uint32_t t, uint32_t idx)   /* H14: synth track t's step idx plays (the Smart Keys track always) */
{
    return t >= NPART || t == wrt.keys_trk || (arr.play[t] >> (idx & 15u) & 1u);
}
