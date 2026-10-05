/* SPDX-License-Identifier: GPL-3.0-only */
/* Harmony (PLAY MODE, docs/design/play-mode-architecture.md 3): which chord of the World's progression is sounding,
 * once per audio block, and its chord-tone and safe-tone masks. Smart Keys (smartkeys.c) read it; nothing here
 * makes a sound.
 *
 *   harm_stage   main loop (world.c world_stage): the scene's progression (a PROGS record) into the staged buffer:
 *                per chord its root, quality, chord tones and safe tones, and the beat -> chord table
 *   harm_commit  the stage's commit (world.c world_commit: the audio ISR on a bar, or IRQs off while stopped):
 *                the staged buffer becomes the sounding one; the progression restarts at chord 0 because the
 *                commit's seq_reset_tracks zeroes clk_beat
 *   harm_block   audio ISR, seq.c events_block (H16), before keyboard_block: the chord at the clock. Inert while
 *                wrt.active == 0
 *   harm_chord_name   main loop (the Studio): "Am7", "Fmaj7", "Dsus4" (torn reads are harmless: display only)
 *
 * Masks are pitch-class sets, bit 0 = C (absolute, not relative to the World key). A chord tone is a tone of the
 * chord's quality (world_fmt.h WF_QUAL_MASK). A safe tone is a chord tone, or a tone of the World scale that is not
 * an avoid note over the chord. The avoid policy is GUARD avoid (design 6.1): classic, a scale tone a semitone
 * above a chord tone (the 4th over a major chord, the b6 over a fifth); strict, also a semitone below one; none. */

#define HARM_ANTICIP (BEAT_U / 8u)       /* a key pressed up to a 32nd note before a beat gets that beat's chord */
enum { HARM_CLASSIC, HARM_NONE, HARM_STRICT };   /* GUARD avoid (WF_AVOID_NAMES order) */

typedef struct {
    uint8_t n;                           /* chords, 0 = no progression (no World committed yet) */
    uint8_t beats;                       /* the progression's length: 4, 8, 16 or 32 beats */
    uint8_t key, flats;                  /* the World key's pitch class; chord names spelled with flats */
    uint16_t scale;                      /* the World scale's pitch classes */
    uint8_t pc[WF_MAX_CHORDS], q[WF_MAX_CHORDS];   /* per chord: root pitch class, quality (WF_Q_*) */
    uint16_t ct[WF_MAX_CHORDS], safe[WF_MAX_CHORDS];
    uint8_t lut[WF_CHORD_BEATS_MAX];     /* beat of the progression -> chord */
} hprog_t;

static hprog_t hprog[2];                 /* [hcur] sounds (the ISR reads it), the other is the stage's (main) */
static volatile uint8_t hcur;
static struct {
    volatile uint8_t ci;                 /* the chord sounding (index into hprog[hcur]); 255: to be read */
    volatile uint8_t ci_key;             /* the chord a key pressed now plays over (ci, or the next one HARM_ANTICIP
                                          * before its beat: players press early) */
    volatile uint8_t gen;                /* + 1 at every chord change (a UI redraws on it) */
    volatile uint16_t ct, safe;          /* the masks of chord ci */
} harm = {255, 0, 0, 0, 0};

static const uint16_t HQ_MASK[WF_NQUAL] = WF_QUAL_MASK;
_Static_assert(sizeof HQ_MASK / sizeof HQ_MASK[0] == WF_NQUAL, "a mask per quality");

static uint32_t pc_rot(uint32_t m, uint32_t r)   /* a pitch-class set transposed up r semitones */
{
    r %= 12u;
    m &= 0xFFFu;
    return r ? ((m << r) | (m >> (12u - r))) & 0xFFFu : m;
}

/* the parent major of each scale (SCALE_MASK order), in semitones above its root: the key signature, so that
 * chord names are spelled as the World's key spells them (D minor: Bb, A minor: G#) */
static const uint8_t HARM_PARENT[16] = {0, 0, 3, 10, 5, 0, 3, 3, 8, 7, 1, 3, 3, 0, 0, 0};

/* main loop: progression record p (nchords, {root << 4 | quality, beats} x n; wb_check checked it) in the World
 * key root / scale, avoid policy avoid (GUARD byte, WF_NONE = the default) -> the staged buffer */
static void harm_stage(const uint8_t *p, uint32_t root, uint32_t scale, uint32_t avoid)
{
    hprog_t *h = &hprog[hcur ^ 1u];
    uint32_t i, b, at = 0, sc, par;
    root %= 12u;
    sc = pc_rot(SCALE_MASK[scale < NSCALES ? scale : 0u], root);
    if (avoid > HARM_STRICT)
        avoid = HARM_CLASSIC;
    memset(h, 0, sizeof *h);
    h->n = (uint8_t)(p[0] <= WF_MAX_CHORDS ? p[0] : WF_MAX_CHORDS);
    h->key = (uint8_t)root;
    h->scale = (uint16_t)sc;
    par = (root + HARM_PARENT[scale & 15u]) % 12u;
    h->flats = par == 5u || par == 10u || par == 3u || par == 8u || par == 1u || par == 6u;   /* F Bb Eb Ab Db Gb */
    for (i = 0; i < h->n; i++) {
        uint32_t c = p[1u + 2u * i], beats = p[2u + 2u * i], pc = (root + (c >> 4)) % 12u, ct, av = 0;
        ct = pc_rot(HQ_MASK[c & 15u], pc);
        if (avoid != HARM_NONE)
            av = sc & ~ct & (pc_rot(ct, 1) | (avoid == HARM_STRICT ? pc_rot(ct, 11) : 0u));
        h->pc[i] = (uint8_t)pc;
        h->q[i] = (uint8_t)(c & 15u);
        h->ct[i] = (uint16_t)ct;
        h->safe[i] = (uint16_t)((sc & ~av) | ct);
        for (b = 0; b < beats && at < WF_CHORD_BEATS_MAX; b++)
            h->lut[at++] = (uint8_t)i;
    }
    h->beats = (uint8_t)(at ? at : 1u);
}

static void harm_commit(void)            /* world_commit (ISR, or IRQs off): the staged progression sounds */
{
    hcur ^= 1u;
    harm.ci = 255;                       /* (harm_block reads the new chord 0 before the keys of this block) */
}

/* audio ISR, once per block, before the keys (H16): b = beats since the scene began (seq_reset_tracks zeroes
 * clk_beat at every commit and at PLAY); stopped, the progression rests on chord 0 (home) */
static void harm_block(void)
{
    const hprog_t *h = &hprog[hcur];
    uint32_t b, ci, ck;
    if (!wrt.active || !h->n)
        return;
    b = song.playing ? clk_beat % h->beats : 0u;
    ci = h->lut[b];
    ck = song.playing && clk_pos + HARM_ANTICIP >= BEAT_U ? h->lut[(b + 1u) % h->beats] : ci;
    harm.ci_key = (uint8_t)ck;
    if (ci != harm.ci) {
        harm.ct = h->ct[ci];
        harm.safe = h->safe[ci];
        harm.ci = (uint8_t)ci;
        harm.gen++;
    }
}

/* the sounding chord's name, "" when no World plays: "Am7", "Fmaj7", "Bbmaj7", "Dsus4", "F#madd9" (<= 7 chars) */
static void harm_chord_name(char out[8])
{
    static const char SHARP[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char FLAT[12][3] = {"C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"};
    static const char *const SUFFIX[WF_NQUAL] = {"", "m", "dim", "aug", "sus2", "sus4", "maj7", "m7", "7", "m7b5",
                                                 "6", "m6", "add9", "madd9", "5", "dim7"};
    const hprog_t *h = &hprog[hcur];
    uint32_t ci = harm.ci, k = 0;
    const char *s;
    out[0] = 0;
    if (!wrt.active || !h->n)
        return;
    if (ci >= h->n)
        ci = 0;                          /* (between a commit and the next block) */
    for (s = (h->flats ? FLAT : SHARP)[h->pc[ci] % 12u]; *s && k < 7u; s++)
        out[k++] = *s;
    for (s = SUFFIX[h->q[ci] & 15u]; *s && k < 7u; s++)
        out[k++] = *s;
    out[k] = 0;
}
