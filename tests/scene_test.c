/* SPDX-License-Identifier: GPL-3.0-only */
/* Scene transitions and World switches on the host (Phase 11: firmware/src/world.c wreq_block / world_commit,
 * arrange.c fills and BEAT masks, macro.c commit glides, fx.c delay crossfade; design 7, 5.7, 9.2; UI spec 6): the
 * whole firmware through host/core.c (FELUCCA_WORLD 1), block by block, the main loop every HOST_FRAME_BLOCKS blocks
 * as on the device. Each scenario runs in its own process (per factory World where it says so):
 *   tour      every factory World playing with a keys loop and a held key: A->B, B->C, C->D, D->A, then random
 *             requests (scenes and variations, some replaced before their boundary): each commit on the first block
 *             of the bar line its transition names (1, 2, 4 bars or the phrase, counted from the section start; a
 *             variation the next bar line), the last request the one that lands; the clock restarted for a scene
 *             (step 0 once, the old pattern's step at the line never played), running on for a variation; the held
 *             key sounding through every commit and the keys loop in phase (every step the next one); no sample
 *             jump at a commit beyond the render's largest elsewhere (clicks), the wet bus not dropping (tails); after
 *             STOP no voice, gate or pitch count left
 *   fills     NEON RAIN: a scene change plays a fill in the bar before its line (the new scene's fill, else this
 *             one's), from the beat it was asked on when that is in the last bar; none without a drum pattern, with
 *             the drums muted, or with BEAT MINIMAL
 *   switch    every factory World to the next while playing, a key held and a loop: no stop, the commit on the next
 *             bar line, the new tempo and keys track there, the loop and the ring cleared, the held key released
 *             (and its key-up harmless), no silence gap, the wet bus going on, the macros at the new World's defaults
 *             ramping in from neutral; a switch replaced by another before its bar; back to scenes after it
 *   beat      BEAT masks: MINIMAL's lanes, BUSY's density and ratchets, BREAK's kick + snare with hats on every
 *             second eighth, intersected with the band; applied on the bar; MINIMAL never fills
 *   advanced  ADVANCED with world_immediate(1): a scene commits at the next block, the clock running on; PLAY ignores
 *             the flag
 *   phrase    a scene with transition "phrase" (the blob patched): on the line of the playing progression's length,
 *             the SCENES footer CHANGES NEXT PHRASE; the commit glides (each step at most 1/16 of the way, all there)
 *             and a delay time change crossfading without a click
 *   render    per factory World: A, B, C, D on their boundaries while playing (a loop, a key held across B->C), then
 *             a switch to the next World, STOP and a tail, into build/renders/worlds/<id>-scenes.wav, with the
 *             analysis of each commit (click ratio, wet change, the quietest 5 ms around it)
 *   build/host/scene_test [outdir [scenario]] */
#include <math.h>
#include <stdint.h>
#include <sys/wait.h>
#include "../host/core.c"

static int fails;
static const char *outdir = "build/host";
static void check(int ok, const char *what)
{
    printf("scene: %-98s %s\n", what, ok ? "ok" : "FAIL");
    fails += !ok;
}
static uint32_t rnd_s = 1103;
static uint32_t rnd(uint32_t n)
{
    rnd_s = rnd_s * 1103515245u + 12345u;
    return (rnd_s >> 16) % n;
}

/* ---- the audio, block by block, measured */
#define MAXB (44100u * 200u / CTL)               /* 200 s of blocks */
#define NCM 160
enum { CK_SCENE, CK_VAR, CK_WORLD, CK_NOW };
static uint32_t nb, nb0;                         /* blocks since boot; the first of the measured run */
static uint16_t dmx[MAXB];                       /* per block: the largest |sample - the one before| (L or R) */
static uint32_t dwx[MAXB];                       /* .. of the wet bus (fx.c wet[]) */
static int32_t lastw;
static float oen[MAXB], wen[MAXB];               /* per block: output and wet-bus mean squares */
static int32_t lastl, lastr, peak, full;       /* (peak, full-scale samples: since the render began) */
static FILE *wav;
static uint32_t wav_frames;
static struct {
    uint32_t blk, beat, pos, adv;                /* the commit's block; the clock at its start, its step then */
    uint8_t kind, sc;
} cm[NCM];
static uint32_t ncm;
static uint32_t gate_note = 128, gate_lost, keys_jumps, keys_steps, keys_prev = 0xFFFFFFFFu, keys_abs = SEQ_NONE;
static uint32_t drum_abs_before, drum_den;       /* the drum track's grid step before a commit, its steps a beat */
static int keys_follow = 1;

static void put16(FILE *f, uint32_t v) { fputc((int)(v & 255u), f); fputc((int)(v >> 8 & 255u), f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFFu); put16(f, v >> 16); }
static void wav_head(FILE *f, uint32_t frames)
{
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + frames * 4u);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, FS);
    put32(f, FS * 4u);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4u);
}
static uint64_t adv(void) { return (uint64_t)CTL * (uint32_t)song.g[G_BPM]; }
static track_t *keys(void) { return &trk[wrt.keys_trk % NPART]; }
static int note_gated(const track_t *t, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < NVOICE; i++)
        if (t->v[i].active && t->v[i].gate && t->v[i].note == n)
            return 1;
    return 0;
}
static int key_held(const track_t *t, uint32_t n)   /* a held key's note: counted, and sounding (POLY) or in the MONO */
{                                                   /* stack (a MONO lead: the loop's later note may sound over it) */
    uint32_t i, p = (uint32_t)(t - trk) % NPART, in = 0;   /* (a sample that ran out keeps its gate) */
    for (i = 0; t->p[P_VOICE] == V_POLY && i < NVOICE; i++)
        in |= t->v[i].gate && t->v[i].note == n;
    for (i = 0; t->p[P_VOICE] != V_POLY && i < t->nmono; i++)
        in |= t->mono_stack[i] == n;
    return in && vlive[p][n] == 1u && vref[p][n] >= 1u;
}

static void blk(void)
{
    static int16_t pcm[2 * CTL];
    uint32_t i, d = 0, dw = 0, st0 = wst.st, beat0 = clk_beat, pos0 = clk_pos, sc0 = wrt.scene, var0 = wrt.var;
    uint32_t sw0 = wst.sw, q0 = wst.q, adv0 = (uint32_t)adv(), dabs = TDRUM->seq_abs, dden = DIV_DEN[(uint32_t)TDRUM->p[P_SDIV] % 6u];
    double e = 0, w = 0;
    host_audio(pcm, CTL);
    for (i = 0; i < CTL; i++) {
        int32_t l = pcm[2 * i], r = pcm[2 * i + 1], a = l > lastl ? l - lastl : lastl - l;
        int32_t b = r > lastr ? r - lastr : lastr - r;
        d = (uint32_t)(a > (int32_t)d ? a : (int32_t)d);
        d = (uint32_t)(b > (int32_t)d ? b : (int32_t)d);
        lastl = l;
        lastr = r;
        a = l < 0 ? -l : l;
        b = r < 0 ? -r : r;
        peak = a > peak ? a : b > peak ? b : peak;
        full += (a >= 32767) + (b >= 32767);
        e += (double)l * l + (double)r * r;
        w += (double)wet[i] * wet[i];
        if ((uint32_t)(wet[i] > lastw ? wet[i] - lastw : lastw - wet[i]) > dw)
            dw = (uint32_t)(wet[i] > lastw ? wet[i] - lastw : lastw - wet[i]);
        lastw = wet[i];
        if (wav) {
            put16(wav, (uint16_t)l);
            put16(wav, (uint16_t)r);
        }
    }
    wav_frames += wav ? CTL : 0u;
    if (nb < MAXB) {
        dmx[nb] = (uint16_t)(d > 65535u ? 65535u : d);
        oen[nb] = (float)(e / (2.0 * CTL));
        wen[nb] = (float)(w / CTL);
        dwx[nb] = dw;
    }
    if (st0 == WST_READY && wst.st == WST_APPLIED && ncm < NCM) {   /* committed in this block */
        cm[ncm].blk = nb;
        cm[ncm].beat = beat0;
        cm[ncm].pos = pos0;
        cm[ncm].adv = adv0;
        cm[ncm].kind = (uint8_t)(sw0 ? CK_WORLD : !q0 ? CK_NOW : wrt.scene != sc0 ? CK_SCENE : CK_VAR);
        cm[ncm++].sc = (uint8_t)wrt.scene;
        drum_abs_before = dabs;
        drum_den = dden;
        (void)var0;
    }
    if (gate_note < 128u && !key_held(keys(), gate_note))
        gate_lost++;
    if (keys_follow && song.playing && keys()->seq_abs != keys_abs) {   /* the keys loop: every step the next one */
        uint32_t len = trk_len(keys()), idx = keys()->seq_idx;
        keys_abs = keys()->seq_abs;
        if (keys_prev != 0xFFFFFFFFu)
            keys_jumps += idx != (keys_prev + 1u) % len;
        keys_prev = idx;
        keys_steps++;
    }
    if (++nb % HOST_FRAME_BLOCKS == 0)
        host_ui_frame();
}
static void blocks(uint32_t n)
{
    while (n--)
        blk();
}
static void bars(double n) { blocks((uint32_t)(n * 4.0 * BEAT_U / (uint32_t)song.g[G_BPM] / CTL)); }

static void boot_world(const char *name)
{
    host_boot_device(1);
    if (host_boot(NULL) || host_world_load(host_world_factory_find(name))) {
        printf("scene: cannot boot %s\n", name);
        exit(1);
    }
    blocks(HOST_FRAME_BLOCKS);
}
static void keys_loop(void)                      /* a one-bar loop on the keys track, as PLAY REC leaves one */
{
    track_t *t = keys();
    const uint8_t *k = wctx.b + wctx.off[WF_S_KEYS];
    uint32_t i;
    static const uint8_t AT[4] = {0, 4, 10, 12}, DEG[4] = {0, 2, 4, 7};
    for (i = 0; i < NSTEP; i++) {
        memset(&t->step[i], 0, sizeof t->step[i]);
        t->step[i].time = ST_REST;
    }
    for (i = 0; i < 4u; i++) {
        step_t *s = &t->step[AT[i]];
        s->time = ST_NOTE;
        s->n = 1;
        s->note[0] = (uint8_t)(k[6] + DEG[i]);
        s->vel = 90;
        t->step[AT[i] + 1u].time = ST_TIE;
    }
    t->p[P_SLEN] = 16;
    t->p[P_SDIV] = WF_DIV_16;
    prec.st = PR_LOOP;
    prec.layers = 1;
}
static uint32_t hold_key(uint32_t k)             /* key k down (heard from the next block): the note it plays */
{
    host_key(k, 1);
    blk();
    gate_note = kb_kind[k] == KS_NOTE ? kb_nt[k][0] : 128u;
    return gate_note;
}
static void release_key(uint32_t k)
{
    gate_note = 128;
    host_key(k, 0);
    blk();
}
static int counts_zero(void)
{
    uint32_t p, n;
    for (p = 0; p < NPART; p++)
        for (n = 0; n < 128u; n++)
            if (vref[p][n] || vlive[p][n])
                return 0;
    return 1;
}
static int all_quiet(void)                       /* after STOP and the tail: no voice, no gate, no count */
{
    host_state_t st;
    uint32_t i;
    for (i = 0; i < 400u; i++) {
        host_state(&st);
        if (st.voices == 0 && st.gated == 0)
            break;
        blocks(HOST_FRAME_BLOCKS);
    }
    host_state(&st);
    return st.voices == 0 && st.gated == 0 && counts_zero();
}

/* ---- what the World says, read from the blob independently of world.c's stage */
static const uint8_t *scene_rec(uint32_t s) { return wctx.b + wctx.scene[s]; }
static uint32_t prog_bars(uint32_t s)            /* scene s's progression: bars */
{
    const uint8_t *p = wctx.b + wctx.off[WF_S_PROGS];
    uint32_t k, beats = 0;
    for (k = 0; k < scene_rec(s)[12]; k++)
        p += WF_PROG_HDR + 2u * p[0];
    for (k = 0; k < p[0]; k++)
        beats += p[2u + 2u * k];
    return beats >= 8u ? beats / 4u : 1u;
}
static uint32_t quantum(uint32_t s)              /* bars between the lines a change to scene s may land on */
{
    uint32_t t;
    if (s == wrt.scene)
        return 1;
    t = scene_rec(s)[14];
    return t ? t : prog_bars(wrt.scene);
}
static void not_on_a_line(void)                  /* (a request in a bar line's own block would land on that line) */
{
    while ((clk_beat & 3u) == 0u && clk_pos < adv())
        blk();
}
/* the bar line (since the section start) a request now for scene s lands on */
static uint32_t expect_bar(uint32_t s)
{
    uint32_t q = quantum(s);
    return ((clk_beat >> 2) / q + 1u) * q;
}
/* blocks until the next commit (at most limit); 1 when one came */
static int until_commit(uint32_t limit)
{
    uint32_t n0 = ncm;
    while (ncm == n0 && limit--)
        blk();
    return ncm != n0;
}

/* ---- the measurements around commit i: clicks, the wet bus, the quietest 5 ms */
#define WIN_PRE 2u
#define WIN_POST 6u
static double rest_max(uint32_t from, uint32_t to)   /* the largest |delta| outside every commit's window */
{
    uint32_t b, i, m = 0;
    for (b = from; b < to && b < MAXB; b++) {
        for (i = 0; i < ncm && !(b + 8u >= cm[i].blk && b < cm[i].blk + 48u); i++)
            ;
        if (i == ncm && dmx[b] > m)
            m = dmx[b];
    }
    return m ? m : 1;
}
static double click_ratio(uint32_t i, double rest)
{
    uint32_t b, m = 0, c = cm[i].blk;
    for (b = c - WIN_PRE; b < c + WIN_POST && b < MAXB; b++)
        m = dmx[b] > m ? dmx[b] : m;
    return m / rest;
}
static double wet_change(uint32_t i)             /* dB: the wet bus 12 ms after the commit over the 12 ms before */
{
    uint32_t b, c = cm[i].blk;
    double a = 1e-3, z = 1e-3;
    for (b = 0; b < 16u; b++) {
        z += wen[c - 1u - b];
        a += c + b < MAXB ? wen[c + b] : 0;
    }
    return 10.0 * log10(a / z);
}
static double quietest(uint32_t i)               /* dBFS: the quietest 5 ms (7 blocks) from 50 ms before to 200 after */
{
    uint32_t b, k, c = cm[i].blk;
    double lo = 1e30;
    for (b = c - 70u; b + 7u < c + 280u && b + 7u < MAXB; b++) {
        double e = 0;
        for (k = 0; k < 7u; k++)
            e += oen[b + k];
        lo = e / 7.0 < lo ? e / 7.0 : lo;
    }
    return 10.0 * log10(lo / (32768.0 * 32768.0) + 1e-12);
}

/* =================================================================== tour === */
static uint32_t world_i;                         /* the factory World a per-World scenario plays */
static const char *wname(void) { return world_name(); }

static void req_scene(uint32_t s, uint32_t v)
{
    char m[120];
    snprintf(m, sizeof m, "%s: request %c var %u", wname(), 'A' + s, v);
    if (world_request(s, v) != WE_OK)
        check(0, m);
}
/* request s / v now and play until it lands; checks the line and the step 0 */
static uint32_t bad_line, bad_step, bad_last, lands;
static void go(uint32_t s, uint32_t v)
{
    uint32_t e, c, t, q = quantum(s), scene_chg = s != wrt.scene;
    not_on_a_line();
    e = expect_bar(s);
    req_scene(s, v);
    if (!until_commit(9u * 4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL)) {
        bad_line++;
        return;
    }
    c = ncm - 1u;
    lands++;
    bad_last += cm[c].sc != s || wrt.var != v;
    bad_line += cm[c].beat != 4u * e || cm[c].pos >= cm[c].adv;
    if (scene_chg) {                             /* the clock restarted: every track from step 0 in this block */
        for (t = 0; t < NTRK; t++)
            bad_step += t != wrt.keys_trk && (trk[t].seq_idx != 0 || trk[t].seq_abs != 0);
        bad_step += clk_beat != 0 || drum_abs_before != 4u * drum_den * e - 1u;
    } else {                                     /* a variation: the clock runs on */
        bad_step += clk_beat < 4u * e;
    }
    (void)q;
}

static void sc_tour(void)
{
    static const char *const W[] = {"NEON RAIN", "MIDNIGHT DRIVE", "FROZEN LAKE", "DUSTY CAFE"};
    uint32_t i, k, s, v, nvar, replaced = 0;
    double rest, worst_click = 0, worst_wet = 0;
    char m[200];
    boot_world(W[world_i]);
    rnd_s += world_i * 7919u;
    nvar = world_nvar();
    keys_loop();
    host_play();
    blocks(40);
    nb0 = nb;
    hold_key(9);                                 /* D4: held through the whole tour */
    bars(0.6);
    for (i = 0; i < 4u; i++)                     /* the cyclic pairs, from the default scene */
        go((wrt.scene + 1u) % WF_NSCENE, wrt.var);
    for (i = 0; i < 12u; i++) {                  /* random requests; a third of them replaced before their line */
        blocks(rnd(3u * 4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL / 2u) + 1u);
        s = rnd(WF_NSCENE);
        v = rnd(4u) ? wrt.var : rnd(nvar);
        if (s == wrt.scene && v == wrt.var)
            s = (s + 1u) % WF_NSCENE;
        not_on_a_line();
        if (!rnd(3u) && quantum(s) > 1u && (clk_beat >> 2) % quantum(s) != quantum(s) - 1u) {
            req_scene(s, v);                     /* (a bar or more from its line: the next request replaces it) */
            blocks(rnd(200u) + 1u);
            k = (s + 1u + rnd(WF_NSCENE - 1u)) % WF_NSCENE;
            if (k == wrt.scene && v == wrt.var)
                k = (k + 1u) % WF_NSCENE;
            s = k;
            replaced++;
        }
        go(s, v);
    }
    bars(1);
    check(!bad_line && lands == 16u, (snprintf(m, sizeof m, "%s: 16 requests (4 cyclic pairs, 12 random, %u replaced on "
          "their way): each commit on its line's first block (%u off)", wname(), replaced, bad_line), m));
    check(!bad_last, (snprintf(m, sizeof m, "%s: the last request is the one that lands (%u not)", wname(), bad_last), m));
    check(!bad_step, (snprintf(m, sizeof m, "%s: a scene from step 0 once (the old step at the line never played), a "
          "variation phase-locked (%u not)", wname(), bad_step), m));
    check(gate_note < 128u && !gate_lost, (snprintf(m, sizeof m, "%s: the held key sounding through every commit (%u "
          "blocks without it)", wname(), gate_lost), m));
    check(keys_steps > 200u && !keys_jumps, (snprintf(m, sizeof m, "%s: the keys loop in phase: %u steps, each the "
          "next one (%u jumps)", wname(), keys_steps, keys_jumps), m));
    rest = rest_max(nb0, nb);
    for (i = 0; i < ncm; i++) {
        double r = click_ratio(i, rest), w = wet_change(i);
        worst_click = r > worst_click ? r : worst_click;
        worst_wet = w < worst_wet ? w : worst_wet;
    }
    printf("scene: %s: %u commits: the largest sample step at a commit %.2f x the render's largest elsewhere (%.0f); "
           "the wet bus at worst %+.1f dB over 12 ms\n", wname(), ncm, worst_click, rest, worst_wet);
    check(worst_click <= 1.0, (snprintf(m, sizeof m, "%s: no click: no sample step at a commit beyond the largest "
          "elsewhere (%.2f x)", wname(), worst_click), m));
    check(worst_wet > -6.0, (snprintf(m, sizeof m, "%s: tails: the wet bus never drops 6 dB in the 12 ms after a commit "
          "(%+.1f dB)", wname(), worst_wet), m));
    release_key(9);
    host_stop();
    blocks(8);
    check(all_quiet(), (snprintf(m, sizeof m, "%s: after STOP no voice, gate or pitch count left", wname()), m));
}

/* =================================================================== fills === */
static int phrase_fill_bar(void)                 /* this bar fills anyway: the band's fill on its phrase's end */
{
    return arr.fills && arr.et.fill < WF_MAX_PAT && (clk_beat >> 2) % arr.et.phrase == arr.et.phrase - 1u;
}
static uint32_t xfills(void)                     /* blocks of a fill into a scene change, until it lands */
{
    uint32_t n = 0;
    while (wst.st == WST_READY && song.playing) {
        blk();
        n += arr.fill_now && !phrase_fill_bar();
    }
    return n;
}
static void sc_fills(void)
{
    uint32_t b, fill_bar = 0, other = 0, last_bar, e, bpb;
    boot_world("NEON RAIN");
    bpb = 4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL;
    macro_set(MC_ENERGY, 1000);                  /* (every layer: the drums play) */
    host_play();
    blocks(40);
    /* B -> D (D has no fill: B's). Asked early in an even bar (D: 2 bars): the whole odd bar before its line fills */
    check(wrt.scene == 1 && scene_rec(1)[15] < WF_MAX_PAT && scene_rec(3)[15] == WF_NONE && scene_rec(3)[14] == 2u,
          "NEON RAIN: B plays, B has a fill; D has none, a 2-bar transition");
    bars(1);
    while ((clk_beat >> 2) % 2u || (clk_beat & 3u) != 1u)
        blk();
    e = expect_bar(3);
    req_scene(3, wrt.var);
    last_bar = e - 1u;
    while (wst.st == WST_READY) {
        b = clk_beat >> 2;                       /* (the bar this block plays) */
        blk();
        if (wst.st != WST_READY)
            break;
        if (b == last_bar)
            fill_bar += arr.fill_now && arr.fp == scene_rec(1)[15];
        else
            other += arr.fill_now && !phrase_fill_bar();
    }
    check(fill_bar >= bpb * 9u / 10u && !other, "a scene change: the fill (B's, D has none) on the whole bar before its "
          "line, none before");
    check(!arr.fill_now && wrt.scene == 3, "the commit ends the fill");
    /* D -> A: neither has a fill: none */
    bars(1.2);
    not_on_a_line();
    req_scene(0, wrt.var);
    check(!xfills() && scene_rec(0)[15] == WF_NONE, "no fill where neither scene has one");
    /* A has no drums: A -> B plays no fill (the drums are not playing) */
    bars(1.1);
    not_on_a_line();
    req_scene(1, wrt.var);
    check(!xfills() && wrt.cur_pat[TRK_DRUM] < WF_MAX_PAT && wrt.scene == 1, "A (no drum pattern) -> B: no fill");
    /* B -> C, the drum track muted: none */
    bars(1.1);
    TDRUM->p[P_MUTE] = 1;
    not_on_a_line();
    req_scene(2, wrt.var);
    check(!xfills(), "the drums muted: no fill");
    TDRUM->p[P_MUTE] = 0;
    /* C -> B (1 bar) asked on beat 3 of a bar that is no phrase end: the fill from the next beat */
    do {
        blk();
    } while ((clk_beat & 3u) != 2u || phrase_fill_bar() || arr.fill_now);
    e = expect_bar(1);
    req_scene(1, wrt.var);
    b = clk_beat;
    check(!arr.fill_now, "(beat 3: no fill yet)");
    while (clk_beat == b)
        blk();
    check(e == (clk_beat >> 2) + 1u && arr.fill_now && arr.fp == scene_rec(1)[15],
          "asked on beat 3 of the last bar: the fill from the next beat");
    until_commit(2u * bpb);
    /* BEAT MINIMAL: no fill */
    world_beat(WF_BEAT_MINIMAL);
    bars(1.1);
    not_on_a_line();
    req_scene(3, wrt.var);
    check(!xfills() && (arr.et.bm & 3u) == WF_BEAT_MINIMAL && !arr.fill_now, "BEAT MINIMAL: no fill");
    host_stop();
    blocks(8);
    check(all_quiet(), "fills: after STOP no voice, gate or pitch count left");
}

/* ================================================================== switch === */
static int live_zero(void)                       /* no live note counted (no key held) */
{
    uint32_t p, n;
    for (p = 0; p < NPART; p++)
        for (n = 0; n < 128u; n++)
            if (vlive[p][n])
                return 0;
    return 1;
}
static void sc_switch(void)
{
    static const char *const W[] = {"NEON RAIN", "MIDNIGHT DRIVE", "FROZEN LAKE", "DUSTY CAFE"};
    uint32_t i, k, c, n, e, age0, ol, bad_stop = 0, bad_sw = 0, bad_state = 0, bad_keys = 0, bad_macro = 0;
    uint32_t bad_ramp = 0, bad_up = 0, ramps = 0, xf = 0;
    double rest, worst_click = 0, worst_wet = 0, worst_gap = 0;
    char m[200];
    boot_world(W[0]);
    keys_loop();
    host_play();
    blocks(40);
    nb0 = nb;
    for (i = 1; i <= 4u; i++) {
        int fi = host_world_factory_find(W[i % 4u]);
        const uint8_t *def;
        uint32_t id = WORLD_INDEX[fi].id, bpm, dl0 = fx.dln;
        bars(1.3);
        hold_key(11);
        bars(0.2);
        not_on_a_line();
        e = (clk_beat >> 2) + 1u;
        if (i == 2) {                            /* replaced on its way: another World first */
            host_world_load(host_world_factory_find(W[0]));
            blocks(30);
        }
        if (host_world_load(fi) < 0)
            check(0, "host_world_load");
        age0 = vage;
        for (n = 0; !ncm || cm[ncm - 1u].kind != CK_WORLD || cm[ncm - 1u].blk + 1u != nb; n++) {
            bad_stop += !song.playing || transport_req == 2u;
            age0 = vage;
            blk();
            if (n > 2u * 4u * BEAT_U / 60u / CTL)
                break;
        }
        c = ncm - 1u;
        if (cm[c].kind != CK_WORLD || cm[c].beat != 4u * e || cm[c].pos >= cm[c].adv)
            printf("scene: switch %u: kind %u at beat %u pos %u, expected beat %u\n", i, cm[c].kind, cm[c].beat,
                   cm[c].pos, 4u * e);
        bad_sw += cm[c].kind != CK_WORLD || cm[c].beat != 4u * e || cm[c].pos >= cm[c].adv;
        bad_stop += !song.playing;
        /* in the commit's block: the clock restarted, the new keys track, the loop and the ring cleared, every voice
         * from before released (a gated one started in this block), no live count */
        bad_state += clk_beat != 0 || wst.kt != wrt.keys_trk || prec.st != PR_EMPTY || wrt.koff;
        for (k = 0; k < NSTEP; k++)
            bad_keys += keys()->step[k].time != ST_REST;
        for (k = 0; k < NPART * NVOICE; k++)
            bad_keys += trk[k / NVOICE].v[k % NVOICE].gate && trk[k / NVOICE].v[k % NVOICE].age <= age0;
        bad_keys += !live_zero();
        xf += fx.dln != dl0 && fx.dxf > 0;       /* (a new tempo: the delay's taps crossfade) */
        gate_note = 128;
        while (wrt.swp && nb - cm[c].blk < 200u)  /* (the main loop's part: the name, the macros, GUARD) */
            blk();
        ol = ov_live;
        while (ov_live == ol && nb - cm[c].blk < 200u)   /* (the new World's first table taken) */
            blk();
        {   /* the overlay: the new World's table ramps in from neutral (no slot starts at its target) */
            const ov_tab_t *tb = &ovb[ov_live];
            bad_ramp += tb->snap;
            for (k = 0; k < tb->n; k++)
                if ((tb->s[k].tgt > 512 || tb->s[k].tgt < -512) && tb->s[k].cls != WF_CLASS_STEPPED) {
                    ramps++;
                    bad_ramp += ov_cur[ov_live][k] == tb->s[k].tgt;
                }
        }
        def = wctx.b + wctx.off[WF_S_DEFAULTS];
        bpm = wctx.b[wctx.off[WF_S_META] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN];
        bad_state += wrt.id != id || (uint32_t)song.g[G_BPM] != bpm || wreq.sw || wrt.swp || wrt.scene != def[0];
        for (k = 0; k < 4u; k++)
            bad_macro += macro_pos(k) != def[2u + k] * 4u;
        host_key(11, 0);                         /* its key-up: nothing left to release, no count goes wrong */
        blk();
        bad_up += !live_zero();
        keys_loop();                             /* (a new loop for the next switch) */
        if (i == 3)                              /* a scene of the new World right after: on its line */
            go((wrt.scene + 1u) % WF_NSCENE, wrt.var);
    }
    bars(1);
    check(!bad_stop, "4 World switches while playing: the transport never stopped");
    check(!bad_sw, "each on the first block of the next bar line (one replaced on its way by another: that one)");
    check(!bad_state, "there: the clock restarted, the new keys track, the loop and the ring cleared; then the new "
          "name, tempo, default scene");
    check(!bad_keys, "the keys loop cleared, every voice from before released on the bar (the held key too)");
    check(!bad_macro, "the macros at the new World's defaults");
    check(!bad_ramp && ramps, (snprintf(m, sizeof m, "the overlay ramps in from neutral (%u moving slots, none "
          "snapped)", ramps), m));
    check(!bad_up, "the held key's key-up after the switch: harmless (no count)");
    check(xf >= 2u, (snprintf(m, sizeof m, "a new tempo: the delay crossfades its taps (%u switches)", xf), m));
    check(!bad_line && !bad_step && lands == 1u, "a scene right after a switch: on its line");
    rest = rest_max(nb0, nb);
    for (i = 0, k = 0; i < ncm; i++) {
        double r = click_ratio(i, rest), w = wet_change(i), g = quietest(i);
        if (cm[i].kind != CK_WORLD)
            continue;
        worst_click = r > worst_click ? r : worst_click;
        worst_wet = w < worst_wet ? w : worst_wet;
        worst_gap = !k++ || g < worst_gap ? g : worst_gap;
    }
    printf("scene: switches: the largest sample step at a switch %.2f x the largest elsewhere; the wet bus at worst "
           "%+.1f dB over 12 ms; the quietest 5 ms around a switch %.1f dBFS\n", worst_click, worst_wet, worst_gap);
    check(worst_click <= 1.0, (snprintf(m, sizeof m, "no click at a switch (%.2f x)", worst_click), m));
    check(worst_wet > -6.0, (snprintf(m, sizeof m, "the FX tails go on through a switch (%+.1f dB)", worst_wet), m));
    check(worst_gap > -50.0, (snprintf(m, sizeof m, "no silence gap at a switch (quietest 5 ms %.1f dBFS)", worst_gap),
                              m));
    host_stop();
    blocks(8);
    check(all_quiet(), "switches: after STOP no voice, gate or pitch count left");
}

/* ==================================================================== beat === */
static uint32_t lanes_at(uint32_t idx)           /* the lanes the drum track's step idx sounds (seq.c seq_tick) */
{
    const dstep_t *s = arr_dstep(&TDRUM->dstep[idx % NSTEP], idx);
    return (uint32_t)(s->on[0] | s->on[1] << 8) & ~arr_dskip(idx) & 0xFFFFu;
}
static void sc_beat(void)
{
    static const uint8_t ORDER[4] = {WF_BEAT_MINIMAL, WF_BEAT_BUSY, WF_BEAT_BREAK, WF_BEAT_GROOVE};
    uint32_t i, idx, bad = 0, all = 0, at_line = 0, changes = 0, busy_d = 0, groove_d = 0, rat = 1;
    const uint8_t *sc;
    boot_world("MIDNIGHT DRIVE");
    macro_set(MC_ENERGY, 1000);
    host_play();
    bars(1.2);
    sc = scene_rec(wrt.scene);
    for (idx = 0; idx < 16u; idx++)
        all |= lanes_at(idx), groove_d += (uint32_t)__builtin_popcount(lanes_at(idx));
    check((arr.et.bm & 3u) == WF_BEAT_GROOVE && all, "GROOVE: no BEAT mask, the drums play");
    for (i = 0; i < 4u; i++) {
        uint32_t b = ORDER[i], before = arr.et.bm, beat0 = 0, pos0 = 0;
        not_on_a_line();
        world_beat(b);
        check(arr.et.bm == before, "a BEAT waits for the bar");
        while (arr.et.bm == before) {
            beat0 = clk_beat;
            pos0 = clk_pos;
            blk();
        }
        changes++;
        at_line += (beat0 & 3u) == 0u && pos0 < adv() &&
                   arr.et.bm == (b | (sc[19u + b] != WF_NONE) << 2);
        for (idx = 0; idx < 16u; idx++) {
            uint32_t l = lanes_at(idx);
            if (b == WF_BEAT_MINIMAL)
                bad += (l & ~0x008Fu) != 0;
            if (b == WF_BEAT_BREAK)
                bad += (l & ~(0x0107u | ((idx & 3u) == 2u ? 0x0050u : 0u))) != 0;
            if (b == WF_BEAT_BUSY)
                busy_d += (uint32_t)__builtin_popcount(l);
            if (b != WF_BEAT_BUSY)
                bad += (l & ~all) != 0;
        }
        if (b == WF_BEAT_BUSY)
            rat = arr.ratchets;
        bars(1);
    }
    check(at_line == 4u && changes == 4u, "MINIMAL, BUSY, BREAK, GROOVE: each from the first block of the next bar line");
    check(!bad, "MINIMAL: kick, kick 2, snare, clap, rim; BREAK: kick, kick 2, snare, snare 2 and the hats on the second "
          "eighth of each beat; both within the band's lanes");
    check(busy_d >= groove_d && rat, "BUSY: every density step (at least the GROOVE's hits), ratchets allowed");
    {   /* the masks themselves, on a synthetic band (density 1/8 on the hat) */
        uint32_t ok = 1;
        arr.lanes = 0xFFFFu;
        arr.dens = 0x5555u;
        arr.et.dlanes = 0x0010u;
        for (idx = 0; idx < 16u; idx++) {
            arr.et.bm = WF_BEAT_MINIMAL;
            ok &= (~arr_dskip(idx) & 0xFFFFu) == 0x008Fu;
            arr.et.bm = WF_BEAT_BUSY;
            ok &= (~arr_dskip(idx) & 0xFFFFu) == 0xFFFFu;
            arr.et.bm = WF_BEAT_GROOVE;
            ok &= (~arr_dskip(idx) & 0xFFFFu) == (idx & 1u ? 0xFFEFu : 0xFFFFu);
            arr.et.bm = WF_BEAT_BREAK;
            ok &= (~arr_dskip(idx) & 0xFFFFu) == ((idx & 3u) == 2u ? 0x0157u : 0x0107u);
            arr.et.bm = WF_BEAT_BREAK | 4u;  /* (the scene's own BREAK pattern: no mask) */
            ok &= (~arr_dskip(idx) & 0xFFFFu) == (idx & 1u ? 0xFFEFu : 0xFFFFu);
        }
        check(ok, "arr_dskip: MINIMAL / BUSY / GROOVE / BREAK on a band (BUSY overrides its density), a scene's own "
              "pattern unmasked");
    }
    host_stop();
    blocks(8);
}

/* ================================================================ advanced === */
static void sc_advanced(void)
{
    uint32_t b0, s, ok;
    boot_world("DUSTY CAFE");
    keys_loop();
    host_play();
    bars(1.4);
    world_immediate(1);                          /* PLAY: ignored */
    not_on_a_line();
    s = (wrt.scene + 1u) % WF_NSCENE;
    req_scene(s, wrt.var);
    check(wst.q >= 1u, "PLAY: world_immediate is ignored (the scene waits for its line)");
    until_commit(9u * 4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL);
    play_adv_enter();
    check(wrt.mode == WM_ADV && !wnow, "ADVANCED (entering it: on their lines again, Phase 14)");
    world_immediate(1);                          /* (SAVE + key 5 on the device) */
    bars(1.3);
    while ((clk_beat & 3u) != 1u)
        blk();
    b0 = clk_beat;
    keys_prev = 0xFFFFFFFFu;
    keys_jumps = keys_steps = 0;
    s = (wrt.scene + 1u) % WF_NSCENE;
    req_scene(s, wrt.var);
    check(wst.q == 0u, "ADVANCED + world_immediate: at once (q 0)");
    ok = until_commit(2);
    check(ok && cm[ncm - 1u].kind == CK_NOW && cm[ncm - 1u].blk == nb - 1u && clk_beat >= b0 && wrt.scene == s,
          "ADVANCED: the scene commits at the next block, mid-bar, the clock running on");
    bars(1);
    check(!keys_jumps && keys_steps > 10u, "the keys loop in phase through it");
    world_immediate(0);
    not_on_a_line();
    s = (wrt.scene + 1u) % WF_NSCENE;
    req_scene(s, wrt.var);
    check(wst.q >= 1u, "world_immediate(0): on its line again");
    host_stop();
    blocks(8);
    check(all_quiet(), "advanced: after STOP no voice, gate or pitch count left");
}

/* ============================================================== phrase, glides, delay === */
static void sc_phrase(void)
{
    const uint8_t *b;
    uint32_t n, e, q, i, ph_bars, moved = 0, bad_step = 0, left;
    uint8_t *c;
    int ph = 0, fi = host_world_factory_find("FROZEN LAKE");
    char m[200];
    boot_world("FROZEN LAKE");
    world_factory((uint32_t)fi, &b, &n);
    c = malloc(n);
    memcpy(c, b, n);
    c[wctx.scene[2] + 14u] = WF_TRANS_PHRASE;    /* (scene C's transition: the phrase; the CRC again) */
    {
        uint32_t crc = wb_crc(c, n);
        c[WF_CRC_AT] = (uint8_t)crc;
        c[WF_CRC_AT + 1] = (uint8_t)(crc >> 8);
        c[WF_CRC_AT + 2] = (uint8_t)(crc >> 16);
        c[WF_CRC_AT + 3] = (uint8_t)(crc >> 24);
    }
    check(host_world_load_blob(c, n) >= 0 && wctx.b == c, "FROZEN LAKE with C's transition \"phrase\": loads");
    host_play();
    bars(1.3);
    ph_bars = prog_bars(wrt.scene);
    not_on_a_line();
    e = expect_bar(2);
    req_scene(2, wrt.var);
    q = wst.q;
    left = world_bars_left(&ph);
    check(q == ph_bars && ph && wst.qp && left == e - (clk_beat >> 2), (snprintf(m, sizeof m, "C on the phrase: every %u "
          "bars (B's progression), %u bar lines to go, CHANGES NEXT PHRASE", ph_bars, left), m));
    until_commit(9u * 4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL);
    check(ncm && cm[ncm - 1u].beat == 4u * e && wrt.scene == 2, "the commit on the phrase's line");
    /* the commit's glides: each step at most 1/16 of the way (and 1), every one at its target soon */
    {
        int16_t was[WGL_MAX];
        uint32_t k;
        n = wgl_n;
        for (k = 0; k < 160u && wgl_n; k++) {
            for (i = 0; i < wgl_n; i++)
                was[i] = wgl[i].at;
            moved = wgl_n;
            blk();
            for (i = 0; i < wgl_n && wgl_n == moved; i++) {
                int32_t d = wgl[i].to - was[i], st = wgl[i].at - was[i];
                bad_step += (d > 0 ? st < 0 || st > d / 16 + 1 : st > 0 || -st > -d / 16 + 1);
            }
        }
        check(n > 0 && !wgl_n && !bad_step, (snprintf(m, sizeof m, "the commit's %u glides: each step at most 1/16 of "
              "the way, all at their targets within %u blocks", n, k), m));
    }
    host_stop();
    blocks(8);
}

/* ================================================================== smooth === */
/* the click detector against what Phase 11 smooths, on a lone held note (every other track muted, no loop): a level,
 * pan and delay-mix jump as it sounded before Phase 11 (written in, the mix's per-sample ramp bypassed) against the
 * same jump through a commit (glided, ramped); a delay time jump with the taps' crossfade cancelled against one
 * crossfaded (on the wet bus, the note sent to the delay only). Each: the largest sample step from 2 blocks before to
 * 6 after over the largest in the 300 blocks before */
static double jump_ratio(uint32_t at, int wetbus)
{
    uint32_t b, m = 0, base = 1;
    for (b = at - 300u; b < at - 2u; b++)
        base = (wetbus ? dwx[b] : dmx[b]) > base ? (wetbus ? dwx[b] : dmx[b]) : base;
    for (b = at - 2u; b < at + 6u; b++)
        m = (wetbus ? dwx[b] : dmx[b]) > m ? (wetbus ? dwx[b] : dmx[b]) : m;
    return (double)m / base;
}
static void sc_smooth(void)
{
    uint32_t t, kt, at, n;
    int16_t lv, pan, dmix;
    double r_raw, r_glide, r_tap, r_xf;
    char m[200];
    boot_world("NEON RAIN");
    kt = wrt.keys_trk;
    for (t = 0; t < NTRK; t++)
        trk[t].p[P_MUTE] = t != kt;
    trk[kt].p[P_DLY] = 100;                      /* (the held note into the delay only: something to cut) */
    trk[kt].p[P_REV] = trk[kt].p[P_CHOR] = 0;
    song.g[G_DMIX] = 100;
    song.g[G_DFDBK] = 60;
    host_play();
    hold_key(7);
    bars(1.5);
    lv = trk[kt].p[P_LEVEL];
    pan = trk[kt].p[P_PAN];
    dmix = song.g[G_DMIX];
    /* (a) the jump as before Phase 11: written in, the mix's ramps told it was already there */
    at = nb;
    trk[kt].p[P_LEVEL] = (int16_t)(lv - 60);
    trk[kt].p[P_PAN] = 50;
    song.g[G_DMIX] = 20;
    lv_was[kt][0] = LEVEL_Q12[lv - 60];
    lv_was[kt][1] = 4096 - 50 * 64;
    lv_was[kt][2] = 4096;
    fx.dmix = 20 * 258;
    blocks(8);
    r_raw = jump_ratio(at, 0);
    trk[kt].p[P_LEVEL] = lv;
    trk[kt].p[P_PAN] = pan;
    song.g[G_DMIX] = dmix;
    bars(0.6);
    /* (b) the same jump through a commit (the same scene and variation staged again, these three changed) */
    not_on_a_line();
    req_scene(wrt.scene, wrt.var);
    wst.p[kt][P_LEVEL] = (int16_t)(lv - 60);
    wst.p[kt][P_PAN] = 50;
    wst.g[G_DMIX] = 20;
    for (t = 0; t < NTRK; t++)                   /* (and the player's mutes and sends as they are now) */
        wst.p[t][P_MUTE] = trk[t].p[P_MUTE];
    wst.p[kt][P_DLY] = 100;
    wst.p[kt][P_REV] = wst.p[kt][P_CHOR] = 0;
    for (n = ncm; ncm == n;)
        blk();
    at = cm[ncm - 1u].blk;
    blocks(8);
    r_glide = jump_ratio(at, 0);
    check(key_held(keys(), gate_note), "(the note still held)");
    bars(0.6);
    /* (c) a delay time jump with the crossfade cancelled; (d) crossfaded */
    at = nb;
    song.g[G_DTIME] = (int16_t)(song.g[G_DTIME] == 2 ? 3 : 2);
    fx.dln = delay_samples();                    /* (fx_buses sees no change: the tap jumps) */
    blocks(8);
    r_tap = jump_ratio(at, 1);
    bars(0.6);
    at = nb;
    song.g[G_DTIME] = (int16_t)(song.g[G_DTIME] == 2 ? 3 : 2);
    blk();
    check(fx.dxf > 0 && fx.dxf < DXF, "a delay time change: the taps crossfade");
    blocks(7);
    r_xf = jump_ratio(at, 1);
    printf("scene: a lone held note, the largest sample step over the steady note's: level / pan / delay mix jump "
           "written in %.2f x, through a commit (glided) %.2f x; delay time jump %.2f x, crossfaded %.2f x\n", r_raw,
           r_glide, r_tap, r_xf);
    check(r_raw > 2.0 && r_tap > 2.0, (snprintf(m, sizeof m, "the click detector sees the unsmoothed jumps (%.2f x, "
          "%.2f x)", r_raw, r_tap), m));
    check(r_glide <= 1.1, (snprintf(m, sizeof m, "a commit's level, pan and delay mix glide: no click (%.2f x)", r_glide),
                           m));
    check(r_xf <= 1.1, (snprintf(m, sizeof m, "a delay time change crossfades: no click (%.2f x)", r_xf), m));
    release_key(7);
    host_stop();
    blocks(8);
    check(all_quiet(), "smooth: after STOP nothing left");
}

/* ============================================================== variations === */
/* Phase 12: every variation of the World against ORIGINAL while playing (the default scene): on the next bar line, the
 * clock running on; the same tempo, the same progression (harmony.c's table), every sequencer note in the World's
 * scale; its macro defaults taken by the controls the player has not turned, gliding (the overlay ramps, no click);
 * ORIGINAL / variation / ORIGINAL / ... on the bars returns exactly to ORIGINAL's parameters, patterns and controls;
 * a control the player turned stays where it is; nothing left after STOP */
typedef struct {
    int16_t p[NTRK][P_COUNT], g[G_COUNT];
    step_t st[NPART][NSTEP];
    dstep_t ds[NSTEP];
    uint16_t pos[4];
    hprog_t h;
} vsnap_t;
static void vsnap(vsnap_t *v)
{
    uint32_t t;
    for (t = 0; t < NTRK; t++)
        memcpy(v->p[t], trk[t].p, sizeof v->p[t]);
    memcpy(v->g, song.g, sizeof v->g);
    for (t = 0; t < NPART; t++)
        memcpy(v->st[t], trk[t].step, sizeof v->st[t]);
    memcpy(v->ds, TDRUM->dstep, sizeof v->ds);
    memcpy(v->pos, mac.pos, sizeof v->pos);
    v->h = hprog[hcur];
}
static uint32_t notes_seen, notes_out;
static uint32_t vage_seen;
static void vblk(void)                           /* a block; each new sequencer voice's note in the World's scale */
{
    uint32_t t, i, top = vage_seen;
    blk();
    for (t = 0; t < NPART; t++)
        for (i = 0; i < NVOICE; i++)
            if (trk[t].v[i].age > vage_seen && t != wrt.keys_trk) {
                notes_seen++;
                notes_out += !(hprog[hcur].scale >> (trk[t].v[i].note % 12u) & 1u);
                top = trk[t].v[i].age > top ? trk[t].v[i].age : top;
            }
    vage_seen = top;
}
static void vbars(double n)
{
    uint32_t k = (uint32_t)(n * 4.0 * BEAT_U / (uint32_t)song.g[G_BPM] / CTL);
    while (k--)
        vblk();
}
static int settled(void)                         /* the glides done, the overlay at its targets, the table taken */
{
    uint32_t i;
    if (wgl_n || ov_pub != ov_seen || mac.dirty || wst.st != WST_FREE)
        return 0;
    for (i = 0; i < ovb[ov_live].n; i++)
        if (ov_cur[ov_live][i] != ovb[ov_live].s[i].tgt)
            return 0;
    return 1;
}
static void var_defaults(uint32_t v, uint32_t *want)   /* variation v's macro positions, from the blob */
{
    const uint8_t *def = wctx.b + wctx.off[WF_S_DEFAULTS], *vr = wctx.b + wctx.var[v] + WF_LABEL_LEN + 1u, *q;
    uint32_t k, n;
    for (k = 0; k < 4u; k++)                     /* (the World's DEFAULTS, then the variation's "macros") */
        want[k] = def[2u + k] * 4u;
    q = vr + 2u + 2u * vr[0];
    q += 2u * q[-1];
    for (n = *q++; n; n--, q += WF_SPAIR)
        if (q[0] == WF_SCOPE_CTL)
            want[q[1]] = q[2] * 4u;
}
static void sc_variations(void)
{
    static const char *const W[] = {"NEON RAIN", "MIDNIGHT DRIVE", "FROZEN LAKE", "DUSTY CAFE"};
    static vsnap_t orig, now;
    uint32_t v, k, n, nv, bpm, bad_tempo = 0, bad_prog = 0, bad_pos = 0, bad_back = 0, ramps = 0, snaps = 0, rounds = 0;
    double rest, worst = 0;
    char m[200];
    boot_world(W[world_i]);
    nv = world_nvar();
    keys_loop();
    host_play();
    for (n = 0; n < 4000u && !settled(); n++)
        vblk();
    vbars(1.2);
    for (n = 0; n < 4000u && !settled(); n++)
        vblk();
    vsnap(&orig);
    bpm = (uint32_t)song.g[G_BPM];
    nb0 = nb;
    for (v = 1; v < nv; v++) {
        uint32_t want[4], ol;
        var_defaults(v, want);
        for (k = 0; k < 2u; k++, rounds++) {     /* ORIGINAL -> v -> ORIGINAL, twice: A / B / A / B */
            ol = ov_pub;
            go(wrt.scene, v);
            for (n = 0; n < 60u && ov_pub == ol; n++)   /* (the main loop: the defaults, the new table) */
                vblk();
            snaps += ovb[ov_live ^ (ov_pub != ov_seen)].snap;
            for (n = 0; n < 30u; n++) {          /* the overlay ramps toward them (each slot at its class) */
                uint32_t i;
                vblk();
                for (i = 0; i < ovb[ov_live].n; i++)
                    ramps += n == 2u && ov_cur[ov_live][i] != ovb[ov_live].s[i].tgt;
            }
            for (n = 0; n < 4u; n++)
                bad_pos += mac.pos[n] != want[n];
            bad_tempo += (uint32_t)song.g[G_BPM] != bpm;
            bad_prog += memcmp(&hprog[hcur], &orig.h, sizeof orig.h) != 0;
            vbars(2.2);                          /* (a 2-bar drum pattern's downbeat again, outside the windows) */
            go(wrt.scene, 0);
            for (n = 0; n < 4000u && !settled(); n++)
                vblk();
            vsnap(&now);
            bad_back += memcmp(now.p, orig.p, sizeof now.p) != 0 || memcmp(now.g, orig.g, sizeof now.g) != 0 ||
                        memcmp(now.st, orig.st, sizeof now.st) != 0 || memcmp(now.ds, orig.ds, sizeof now.ds) != 0 ||
                        memcmp(now.pos, orig.pos, sizeof now.pos) != 0 || memcmp(&now.h, &orig.h, sizeof now.h) != 0;
            bad_tempo += (uint32_t)song.g[G_BPM] != bpm;
            vbars(0.6);
        }
    }
    check(lands == 2u * rounds && !bad_line && !bad_step && !bad_last, (snprintf(m, sizeof m, "%s: %u variation "
          "changes (ORIGINAL / each / ORIGINAL / each): each on the next bar line, the clock running on", wname(),
          lands), m));
    check(!bad_tempo && !bad_prog, (snprintf(m, sizeof m, "%s: the same tempo and progression in every variation",
          wname()), m));
    check(notes_seen > 30u && !notes_out, (snprintf(m, sizeof m, "%s: %u sequencer notes, every one in the World's "
          "scale (%u out)", wname(), notes_seen, notes_out), m));
    check(!bad_pos, (snprintf(m, sizeof m, "%s: the controls at each variation's macro defaults (else the World's)",
          wname()), m));
    check(!snaps, (snprintf(m, sizeof m, "%s: the macro defaults glide: the overlay ramps (%u slots still moving 2 "
          "blocks after a table), none snaps", wname(), ramps), m));
    check(!bad_back, (snprintf(m, sizeof m, "%s: back to ORIGINAL %u times: its parameters, patterns, controls and "
          "progression exactly", wname(), rounds), m));
    rest = rest_max(nb0, nb);
    for (k = 0; k < ncm; k++)
        worst = click_ratio(k, rest) > worst ? click_ratio(k, rest) : worst;
    /* (1.25: a variation that brings a drum kit and darkens, DUSTY CAFE's DARK, plays the kit's first downbeat through
     * the old filter and COLOR while the new ones glide in: a brighter hit than its later ones, not a click; a kit
     * changed straight in at a line shows no excess, and the lone-note test (smooth) finds the clicks) */
    check(worst <= 1.25, (snprintf(m, sizeof m, "%s: no click at a variation change (the largest sample step %.2f x the "
          "largest elsewhere)", wname(), worst), m));
    /* the player turns SPACE: a variation leaves it there and moves the others to its defaults */
    {
        uint32_t want[4];
        v = nv - 1u;
        var_defaults(v, want);
        macro_set(2, 130);
        go(wrt.scene, v);
        for (n = 0; n < 60u; n++)
            vblk();
        check(mac.pos[2] == 130 && mac.pos[0] == want[0] && mac.pos[1] == want[1] && mac.pos[3] == want[3],
              (snprintf(m, sizeof m, "%s: a control the player turned stays where it is, the others go to the "
                        "variation's defaults", wname()), m));
    }
    host_stop();
    blocks(8);
    check(all_quiet(), (snprintf(m, sizeof m, "%s: variations: after STOP nothing left", wname()), m));
}

/* ================================================================== render === */
static void sc_render(void)
{
    static const char *const W[] = {"NEON RAIN", "MIDNIGHT DRIVE", "FROZEN LAKE", "DUSTY CAFE"};
    static const char *const ID[] = {"neon_rain", "midnight_drive", "frozen_lake", "dusty_cafe"};
    char path[256], m[200];
    uint32_t s, i;
    double rest;
    boot_world(W[world_i]);
    snprintf(path, sizeof path, "build/renders/worlds/%s-scenes.wav", ID[world_i]);
    if (!(wav = fopen(path, "wb"))) {
        perror(path);
        exit(1);
    }
    wav_head(wav, 0);
    req_scene(0, wrt.var);                       /* (stopped: A at once) */
    keys_loop();
    host_play();
    nb0 = nb;
    peak = full = 0;
    for (s = 1; s <= WF_NSCENE; s++) {
        while (clk_beat < 4u || !song.playing)  /* (each scene's second bar: the next one asked for) */
            blk();
        bars(0.3);
        if (s == 2)
            hold_key(14);                        /* (a key held across B -> C) */
        if (s == WF_NSCENE)
            break;
        req_scene(s, wrt.var);
        until_commit(9u * 4u * BEAT_U / (uint32_t)song.g[G_BPM] / CTL);
        if (s == 3)
            release_key(14);
    }
    host_world_load(host_world_factory_find(W[(world_i + 1u) % 4u]));
    until_commit(9u * 4u * BEAT_U / 60u / CTL);
    blocks(HOST_FRAME_BLOCKS * 2u);
    bars(2);
    host_stop();
    blocks(4u * FS / CTL);
    fseek(wav, 0, SEEK_SET);
    wav_head(wav, wav_frames);
    fclose(wav);
    wav = NULL;
    rest = rest_max(nb0, nb);
    printf("render: %s: %.1f s, peak %.2f dBFS, %d samples at full scale; commits:\n", path, (double)wav_frames / FS,
           20.0 * log10(peak / 32768.0 + 1e-12), (int)full);
    check(full == 0 && peak < 29205, (snprintf(m, sizeof m, "%s: peak under -1 dBFS, nothing at full scale", W[world_i]),
                                      m));
    for (i = 0; i < ncm; i++) {
        double r = click_ratio(i, rest), w = wet_change(i), g = quietest(i);
        printf("render:   %-13s %c at %6.2f s: the largest sample step %.2f x the largest elsewhere, the wet bus %+.1f dB, "
               "the quietest 5 ms %.1f dBFS\n", cm[i].kind == CK_WORLD ? "World, scene" : "scene", 'A' + cm[i].sc,
               (double)(cm[i].blk - nb0) * CTL / FS, r, w, g);
        check(r <= 1.0 && w > -6.0 && g > -60.0, (snprintf(m, sizeof m, "%s: commit %u clean (no click, tails, no gap)",
              W[world_i], i + 1u), m));
    }
    check(ncm == 4u, (snprintf(m, sizeof m, "%s: A->B->C->D and a World switch, each on its line (%u commits)",
          W[world_i], ncm), m));
    check(all_quiet(), (snprintf(m, sizeof m, "%s: after STOP and the tail nothing left", W[world_i]), m));
}

/* ============================================================== the runner === */
static int run(const char *name, void (*fn)(void), uint32_t wi)
{
    pid_t pid;
    int st;
    fflush(stdout);
    if (!(pid = fork())) {
        world_i = wi;
        fn();
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("scene: scenario %s (%u) FAILED%s\n", name, wi, WIFEXITED(st) ? "" : " (crashed)");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        void (*fn)(void);
        uint32_t per_world;
    } SC[] = {{"tour", sc_tour, 4}, {"fills", sc_fills, 1}, {"switch", sc_switch, 1}, {"beat", sc_beat, 1},
              {"advanced", sc_advanced, 1}, {"phrase", sc_phrase, 1}, {"smooth", sc_smooth, 1},
              {"variations", sc_variations, 4},
              {"render", sc_render, 4}};
    const char *only = argc > 2 ? argv[2] : "";
    int bad = 0;
    uint32_t i, w;
    if (argc > 1)
        outdir = argv[1];
    for (i = 0; i < sizeof SC / sizeof SC[0]; i++)
        if (!*only || !strcmp(only, SC[i].name))
            for (w = 0; w < SC[i].per_world; w++)
                bad += run(SC[i].name, SC[i].fn, w);
    printf(bad ? "SCENE TEST FAILED\n" : "scene test: all checks passed\n");
    return bad != 0;
}
