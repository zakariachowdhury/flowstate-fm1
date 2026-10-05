/* SPDX-License-Identifier: GPL-3.0-only */
/* Smart Keys (PLAY MODE, docs/design/play-mode-architecture.md 4, decision D7): what each of the 27 keys plays
 * while a World is active. SMART MELODY, the default mode:
 *
 *   white keys  the World's melody scale (KEYS melody_mask, by default its pentatonic), fixed: the C4 key plays the
 *               tonic and the degrees ascend across the white keys, below C4 too. White key w (0 = F3 .. 15 = G5) is
 *               d = w - 4 white keys from C4; with m tones in the melody scale, q = floor(d / m), r = d - q m, it
 *               plays tonic + 12 q + (the r-th tone of the scale). 16 white keys: about three octaves of melody.
 *   black keys  the tones of the CURRENT chord (harmony.c), ascending in the same register: black key b plays the
 *               lowest chord tone above both its left white key and black key b - 1. With KEYS black chord+9, the
 *               chord's 9th joins when it is a safe tone of the World scale. When the chord changes the black keys
 *               move to its tones; the white keys never move (white: scale; white: safe walks only the chord's
 *               safe tones, for Worlds with a 7-note melody scale).
 *   every note  + 12 x OCT (song.octave), then folded by octaves into the keys track's range (GUARD range,
 *               smart_keys.range; default C3..C6; a range under an octave is widened upward to one, so that every
 *               pitch class has a place in it and no key ever leaves the key). The note guard is guard.c's
 *               (gnote_t: guard_note_stage, guard_fold, guard_admit).
 *
 * A key pressed while a note is held never moves that note: seq.c key_down keeps what each key sounded
 * (kb_nt / kb_n / kb_trk) and key_up releases exactly that, whatever the chord, scene or octave is by then.
 * Two keys (or a key and the sequencer) on one pitch share its voice: voice.c counts the holders (H2, vref).
 *
 * The tables: per chord of the scene's progression, the note of each of the 27 keys (rel[ci][k], as a distance from
 * the tonic) built by sk_stage in the main loop with the stage (world.c world_stage) and flipped in by sk_commit
 * with the harmony (world_commit), so the audio ISR only reads a table (sk_note). MIDI in on the keys track (H8)
 * plays MIDI note v as a virtual key of v's colour: the device's 27 keys (F3..G5) as they are, outside them the
 * same formula (a black key: the lowest chord tone above its left white key).
 *
 *   sk_stage / sk_commit       main loop / the commit
 *   sk_on, sk_active(t)        a World plays, Smart Keys are on (PLAY; ADV_WORLD clears wrt.keys_on: kb_map)
 *   sk_note(k)                 ISR (H7): key k's note now (the chord a key gets: harm.ci_key)
 *   sk_voicing(k, n, nt)       ISR (H7): the notes the key plays (MELODY: n alone; the extension point of the modes)
 *   sk_admit()                 ISR (H9): the keys track's polyphony cap (GUARD max_poly, guard.c guard_admit)
 *   sk_midi_map(t, &v, on)     ISR (H8): MIDI in on the keys track
 *   sk_pulse(t, list)          ISR (H11): PULSE with one key held
 *   sk_chord_keys()            main: the key LEDs (design 8.4)
 *
 * Later modes (design 4.6, KEYS mode / sk_set_mode): SMART CHORDS, BASS and DRUMS play other tracks with other
 * maps. Until they exist every mode plays as MELODY (sk_voicing, sk_target are where they plug in). */

#define SK_NKEY 27u                      /* the keys: F3 (53) .. G5 (79) */
#define SK_LO 53u
#define SK_BLACK 0x54Au                  /* black pitch classes: C# D# F# G# A# */
enum { SK_MELODY, SK_CHORDS, SK_BASS, SK_DRUMS };   /* KEYS mode (WF_KMODE_NAMES order) */

typedef struct {
    uint8_t n;                           /* chords (as harmony's progression), 0 = no map */
    uint8_t tonic;                       /* the note of the C4 key: KEYS tonic (the World root at or below it) */
    gnote_t g;                           /* the note guard: range, max_poly (guard.c) */
    uint8_t mode;                        /* KEYS mode */
    uint16_t wm[WF_MAX_CHORDS];          /* per chord: the white keys' pitch classes, relative to the tonic */
    uint16_t bs[WF_MAX_CHORDS];          /* per chord: the black keys' pitch classes (absolute) */
    int8_t rel[WF_MAX_CHORDS][SK_NKEY];  /* per chord: each key's note - tonic (OCT 0, before the fold) */
} skmap_t;

static skmap_t skm[2];                   /* [skcur] plays (ISR), the other is the stage's (main), as hprog */
static volatile uint8_t skcur;
static uint8_t sk_midi[128];             /* ISR: incoming MIDI note -> the note it sounded + 1, 0 = none */
static volatile uint8_t sk_mode_req = 255;   /* main: a keys mode the player chose (255: the World's) */

static const uint8_t SK_WIDX[12] = {0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6};   /* white keys from C to (the left of) pc */

static int sk_on(void) { return wrt.active && wrt.keys_on && skm[skcur].n; }
static int sk_active(const track_t *t) { return sk_on() && t == &trk[wrt.keys_trk % NPART]; }

static uint32_t sk_mode(void)            /* the keys mode playing (the later modes play as MELODY until they exist) */
{
    return sk_mode_req < 4u ? sk_mode_req : skm[skcur].mode;
}
static void sk_set_mode(uint32_t mode) { sk_mode_req = (uint8_t)(mode < 4u ? mode : 255u); }

static uint32_t sk_ci(const skmap_t *m)  /* the chord a key pressed now plays over */
{
    return harm.ci_key < m->n ? harm.ci_key : 0u;
}

static uint32_t sk_fold(const skmap_t *m, int32_t n) { return guard_fold(&m->g, n); }   /* into the range */

/* the white key d white keys from C4 (any sign) over melody mask wm (relative to the tonic, not empty) */
static int32_t sk_white(uint32_t tonic, uint32_t wm, int32_t d)
{
    uint32_t i, cnt = 0, r;
    int32_t q;
    for (i = 0; i < 12u; i++)
        cnt += wm >> i & 1u;
    if (!cnt) {
        wm = 1;
        cnt = 1;
    }
    q = d >= 0 ? d / (int32_t)cnt : -((-d + (int32_t)cnt - 1) / (int32_t)cnt);
    r = (uint32_t)(d - q * (int32_t)cnt);
    for (i = 0; i < 12u; i++)
        if (wm >> i & 1u) {
            if (!r)
                break;
            r--;
        }
    return (int32_t)tonic + 12 * q + (int32_t)i;
}

static int32_t sk_above(uint32_t bs, int32_t p)   /* the lowest note above p with its pitch class in bs */
{
    uint32_t g = 12;
    for (p++; g-- && !(bs >> (uint32_t)((p % 12 + 12) % 12) & 1u); p++)
        ;
    return p;
}

/* main loop, world_stage, after harm_stage: the staged progression's key maps. keys: the KEYS record (trk, mode,
 * u16 melody_mask, white, black, tonic, loop_pat); guard: the GUARD fixed bytes, or 0 */
static void sk_stage(const uint8_t *keys, const uint8_t *guard)
{
    const hprog_t *h = &hprog[hcur ^ 1u];
    skmap_t *m = &skm[skcur ^ 1u];
    uint32_t kt = keys[0] % NPART, mel = ((uint32_t)keys[2] | (uint32_t)keys[3] << 8) & 0xFFFu, ci, k;
    uint32_t tonic = keys[6];
    memset(m, 0, sizeof *m);
    tonic -= (tonic + 12u - h->key) % 12u;          /* (worldc makes it the root; a blob might not) */
    if (tonic > 127u)
        tonic += 12u;
    m->tonic = (uint8_t)tonic;
    m->mode = (uint8_t)(keys[1] < 4u ? keys[1] : SK_MELODY);
    guard_note_stage(&m->g, guard, kt);            /* range (an octave at least: the keys stay in key), max_poly */
    if (!(mel & 1u))
        mel |= 1u;
    for (ci = 0; ci < h->n; ci++) {
        uint32_t wm = mel, bs = h->ct[ci], nine = (h->pc[ci] + 2u) % 12u, w = 0;
        int32_t white = 0, black = -1000;
        if (keys[4] == 1u) {                          /* white: safe: the melody's safe tones over this chord */
            uint32_t s = mel & pc_rot(h->safe[ci], 12u - h->key);
            if (s)
                wm = s;
        }
        if (keys[5] == 1u && (h->scale & h->safe[ci]) >> nine & 1u)
            bs |= 1u << nine;                         /* black: chord+9: the 9th, when it is safe */
        m->wm[ci] = (uint16_t)wm;
        m->bs[ci] = (uint16_t)bs;
        for (k = 0; k < SK_NKEY; k++) {
            int32_t n;
            if (SK_BLACK >> ((SK_LO + k) % 12u) & 1u)
                n = black = sk_above(bs, white > black ? white : black);   /* (its left white key: k - 1) */
            else
                n = white = sk_white(tonic, wm, (int32_t)w++ - 4);
            n -= (int32_t)tonic;
            m->rel[ci][k] = (int8_t)(n > 127 ? 127 : n < -128 ? -128 : n);
        }
    }
    m->n = h->n;
}

static void sk_commit(int world_switch)  /* world_commit, with harm_commit: the staged maps play */
{
    skcur ^= 1u;
    if (world_switch)
        memset(sk_midi, 0, sizeof sk_midi);   /* (a new instrument: world_commit releases every note) */
}

/* ------------------------------------------------------------------ audio ISR --- */
static uint32_t sk_note(uint32_t k)      /* H7: key k (0 = F3 .. 26 = G5) now */
{
    const skmap_t *m = &skm[skcur];
    return sk_fold(m, (int32_t)m->tonic + m->rel[sk_ci(m)][k % SK_NKEY] + 12 * song.octave);
}

/* H7: the notes key k plays into nt[] (n: sk_note(k)), how many. MELODY: the note. (SMART CHORDS: a voicing of
 * up to 4 notes, the most kb_nt keeps, design 4.6) */
static uint32_t sk_voicing(uint32_t k, uint32_t n, uint8_t *nt)
{
    (void)k;
    nt[0] = (uint8_t)n;
    return 1;
}

/* H9: may one more note sound on the keys track? Keys held there plus MIDI notes held, under max_poly. A key
 * refused stays silent until it is pressed again (its release is a no-op) */
static int sk_admit(void)
{
    uint32_t k, n = 0;
    for (k = 0; k < 128u; k++)
        n += sk_midi[k] != 0;
    return guard_admit(skm[skcur].g.poly, n);
}

static uint32_t sk_vnote(uint32_t v)     /* MIDI note v as a virtual key (C4 = 60 is the C4 key) */
{
    const skmap_t *m = &skm[skcur];
    uint32_t ci = sk_ci(m), pc = v % 12u;
    int32_t n;
    if (v >= SK_LO && v < SK_LO + SK_NKEY)
        return sk_fold(m, (int32_t)m->tonic + m->rel[ci][v - SK_LO]);   /* the device's keys, as they are */
    n = sk_white(m->tonic, m->wm[ci], 7 * ((int32_t)(v / 12u) - 5) + SK_WIDX[pc]);
    if (SK_BLACK >> pc & 1u)
        n = sk_above(m->bs[ci], n);      /* (SK_WIDX of a black key is its left white key's) */
    return sk_fold(m, n);
}

/* H8: MIDI in on track t. *note: the incoming note in, the note to play / release out. 1: drop the event.
 * A note-on remembers what it sounded (sk_midi), so its note-off releases that, whatever the chord is then */
static int sk_midi_map(track_t *t, uint32_t *note, int on)
{
    uint32_t v = *note & 127u, s = sk_midi[v];
    if (s) {
        sk_midi[v] = 0;
        if (!on) {
            *note = s - 1u;
            return 0;
        }
        input_off(t, s - 1u);            /* (a note-on again without its note-off: the first one ends) */
    }
    if (!sk_active(t))
        return 0;                        /* another track, or Smart Keys off: SLOOP's raw note */
    if (!on || !sk_admit())
        return 1;                        /* an off with nothing sounding (its on was refused) / the cap */
    *note = sk_vnote(v);
    sk_midi[v] = (uint8_t)(*note + 1u);
    return 0;
}

/* H11, PULSE (design 9.2): one key held on the keys track, the arp plays it and the next chord tones above it
 * (up to 3, inside the range), read again at every arp step: one finger follows the progression, and the held
 * note itself is never re-pitched. list[0]: the held note. Returns the notes in list */
static uint32_t sk_pulse(const track_t *t, uint32_t *list)
{
    const skmap_t *m = &skm[skcur];
    uint32_t n = 1, ct = harm.ct;
    int32_t p = (int32_t)list[0];
    if (!sk_active(t) || t->nheld != 1u || harm.ci >= hprog[hcur].n || !ct)
        return 1;
    while (n < 4u) {
        p = sk_above(ct, p);
        if (p > (int32_t)m->g.hi || p > 127)
            break;
        list[n++] = (uint32_t)p;
    }
    return n;
}

/* ------------------------------------------------------------------- main loop --- */
/* the key LEDs (design 8.4): bit k = white key k plays a tone of the sounding chord, and the C4 key (the tonic) */
static uint32_t sk_chord_keys(void)
{
    const skmap_t *m = &skm[skcur];
    const hprog_t *h = &hprog[hcur];
    uint32_t k, out = 1u << (60u - SK_LO), ci = harm.ci < m->n ? harm.ci : 0u, ct;
    if (!sk_on() || ci >= h->n)
        return 0;
    ct = h->ct[ci];
    for (k = 0; k < SK_NKEY; k++)
        if (!(SK_BLACK >> ((SK_LO + k) % 12u) & 1u) &&
            ct >> (sk_fold(m, (int32_t)m->tonic + m->rel[ci][k] + 12 * song.octave) % 12u) & 1u)
            out |= 1u << k;
    return out;
}
