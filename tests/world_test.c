/* SPDX-License-Identifier: GPL-3.0-only */
/* Host test of the Musical World data path (firmware/src/world.c, FELUCCA_WORLD=1): the FWD1 check, the load
 * into the pattern pool, the stage and the commit while stopped. Run by tests/run_tests.sh, also under
 * -fsanitize=address,undefined:
 *   build/host/world_test MINIMAL.wblob FULL.wblob     (worlds/test/minimal, full .world.json, by tools/worldc.py)
 *  1. both blobs pass wb_check; params.c preset_fill equals the preset load the regression renders use, for
 *     every engine x preset;
 *  2. load + stage + commit of worlds/test/full.world.json: tempo, key, preset-derived and overridden
 *     parameters, globals, the steps after chord-token resolution per progression, drum lanes, the keys loop
 *     kept across scenes, the pool write-back, variation sounds, swaps and pairs, scene pairs and fx;
 *  3. every corruption is rejected or harmless: every truncation (also with L and the CRC patched), every
 *     byte flipped (the CRC catches it) and flipped again with the CRC recomputed (the structural checks),
 *     targeted bad indices, lengths and L, 20000 random corruptions with the CRC recomputed; whatever passes
 *     the check loads, stages and commits inside every descriptor;
 *  4. proj_slot (the user's projects) is never touched; the busy paths refuse. */
#define FELUCCA_WORLD 1
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

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
                                        printf("\n"); } } while (0)

static uint8_t *read_file(const char *path, uint32_t *n)
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

static void put_crc(uint8_t *b)                  /* recompute a blob's CRC (after a deliberate corruption) */
{
    uint32_t L = wb_u16(b + 6), c = wb_crc(b, L);
    b[12] = (uint8_t)c;
    b[13] = (uint8_t)(c >> 8);
    b[14] = (uint8_t)(c >> 16);
    b[15] = (uint8_t)(c >> 24);
}

static uint32_t rnd_state = 12345;
static uint32_t rnd(void)
{
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

/* the instrument as SLOOP leaves it: defaults, presets, a pattern on every track (the World replaces them) */
static void sloop_state(void)
{
    uint32_t t, k;
    memset(&wrt, 0, sizeof wrt);
    memset(&wst, 0, sizeof wst);
    host_tracks_init();
    host_preset(&trk[0], 1, 2);
    host_preset(&trk[1], 4, 1);
    host_preset(&trk[2], 7, 0);
    TDRUM->p[P_E0] = DRUM_DEFAULT_KIT;
    for (t = 0; t < NPART; t++)
        for (k = 0; k < NSTEP; k++) {
            trk[t].step[k].time = ST_NOTE;
            trk[t].step[k].n = 1;
            trk[t].step[k].note[0] = (uint8_t)(40 + t + k % 12);
        }
    for (k = 0; k < NSTEP; k += 2)
        dstep_set(&TDRUM->dstep[k], k % 16, LV_NORM, 0);
    song.playing = 0;
    transport_req = 0;
}

/* every value inside its descriptor, every step well formed (after any accepted blob) */
static int sane(void)
{
    uint32_t t, i;
    for (t = 0; t < NTRK; t++) {
        for (i = 0; i < P_COUNT; i++) {
            const param_desc_t *d = t == TRK_DRUM ? (i == P_E0 ? &DRUM_KIT_DESC : i > P_E0 ? 0 : &TP[i])
                                                  : i >= P_E0 ? &ENGINES[trk[t].eng_req]->edit[i - P_E0] : &TP[i];
            if (d && (trk[t].p[i] < d->min || trk[t].p[i] > d->max))
                return 0;
        }
        if (t < NPART && trk[t].eng_req >= NENGINES)
            return 0;
        for (i = 0; i < NSTEP && t != TRK_DRUM; i++) {
            const step_t *s = &trk[t].step[i];
            if (s->n > 4 || s->time > ST_REST || s->note[0] > 127 || s->note[1] > 127 || s->note[2] > 127 ||
                s->note[3] > 127)
                return 0;
        }
    }
    for (i = 0; i < G_COUNT; i++)
        if (song.g[i] < GP[i].min || song.g[i] > GP[i].max)
            return 0;
    return 1;
}

static int notes_are(const step_t *s, uint32_t n, int a, int b, int c)
{
    return s->time == ST_NOTE && s->n == n && s->note[0] == a && (n < 2 || s->note[1] == b) && (n < 3 || s->note[2] == c);
}

/* ---------------------------------------------------------------- 1. checks --- */
static void test_presets(void)
{
    uint32_t e, pi, i, bad = 0;
    for (e = 0; e < NENGINES; e++)
        for (pi = 0; pi < ENGINES[e]->npresets; pi++) {
            int16_t a[P_COUNT];
            track_t *t = &trk[0];
            for (i = 0; i < P_E0; i++)
                a[i] = t->p[i] = TP[i].def;
            preset_fill(a, ENGINES[e], pi);
            host_preset_req(t, e, pi);              /* hostsim.c: apply_preset_to as the goldens render it */
            for (i = 0; i < P_COUNT; i++)
                bad += a[i] != t->p[i];
        }
    CHECK(!bad, "preset_fill differs from the preset load in %u values", bad);
    printf("preset_fill: every engine x preset as hostsim's preset load\n");
}

/* ------------------------------------------------------------ 2. full World --- */
static void test_full(const uint8_t *blob, uint32_t n)
{
    static project_t before[4];
    uint32_t k;
    int rc;
    const preset_t *warm = &ENGINES[0]->presets[9], *dark = &ENGINES[0]->presets[10];
    CHECK(!strcmp(warm->name, "WARM PAD") && !strcmp(dark->name, "DARK STR"), "ANALOG preset order");
    CHECK(!strcmp(ENGINES[6]->presets[3].name, "SYNC LEAD") && !strcmp(DRUM_KIT_NAMES[5], "808") &&
          !strcmp(DRUM_KIT_NAMES[6], "909"), "preset / kit order");
    memset(proj_slot, 0xA5, sizeof proj_slot);
    memcpy(before, proj_slot, sizeof before);
    sloop_state();
    rc = world_start(blob, n);
    CHECK(rc == WE_OK, "world_start: %d", rc);
    CHECK(wrt.active && wrt.loaded && wrt.scene == 1 && wrt.var == 0 && wrt.keys_trk == 2 && song.sel == 2, "state");
    CHECK(wst.st == WST_FREE && !wst.sw, "the stage is free after the commit");
    CHECK(panic_req == 15, "a World switch releases every track (%u)", panic_req);
    /* META, globals */
    CHECK(song.g[G_BPM] == 96 && song.g[G_SWING] == 10, "tempo %d, swing %d", song.g[G_BPM], song.g[G_SWING]);
    CHECK(song.g[G_DTIME] == 1 && song.g[G_DFDBK] == 50 && song.g[G_DMIX] == 80 && song.g[G_RSIZE] == 96 &&
          song.g[G_RDAMP] == 60 && song.g[G_DUST] == 3, "World fx");
    CHECK(song.g[G_DRLVL] == 104 && song.g[G_DRREV] == 12, "drum level / reverb (drums.level, drums.rev)");
    CHECK(song.g[G_DUCK] == GP[G_DUCK].def && song.g[G_DCOLOR] == GP[G_DCOLOR].def && song.g[G_FILT] == 0,
          "unset whitelisted globals at their defaults");
    for (k = 0; k < NPART; k++)
        CHECK(trk[k].p[P_ROOT] == 9 && trk[k].p[P_SCALE] == 2, "track %u key A MIN", k);
    /* sounds: engine, preset, preset-derived values, the World's overrides */
    CHECK(trk[0].eng_req == 0 && trk[1].eng_req == 0 && trk[2].eng_req == 6, "engines");
    CHECK(trk[0].preset == 9 && trk[1].preset == 2 && trk[2].preset == 3, "presets");
    CHECK(trk[0].p[P_E4] == 60 && trk[0].p[P_REV] == 50 && trk[0].p[P_LEVEL] == 92, "pad CUT / rev / level");
    CHECK(trk[0].p[P_E0] == 1 && trk[0].p[P_LWAVE] == 1, "enum values by name (WAVE SQR, lwave TRI)");
    CHECK(trk[0].p[P_E5] == warm->e[5] && trk[0].p[P_ATK] == warm->env[0] && trk[0].p[P_REL] == warm->env[3],
          "pad RES / ATK / REL from WARM PAD (%d %d %d)", trk[0].p[P_E5], trk[0].p[P_ATK], trk[0].p[P_REL]);
    CHECK(trk[1].p[P_E4] == 70, "bass CUT");
    CHECK(trk[2].p[P_DLY] == 40 && trk[2].p[P_REV] == 30, "keys sends");
    CHECK(trk[2].p[P_AMODE] == 1 && trk[2].p[P_ARATE] == 1 && trk[2].p[P_AOCT] == 1 && trk[2].p[P_AGATE] == 60 &&
          !trk[2].p[P_AHOLD], "keys arp group = PULSE SLOW (defaults)");
    CHECK(trk[2].p[P_SLEN] == 64 && trk[2].p[P_SDIV] == 2, "keys loop: 4 bars at 1/16 (%d)", trk[2].p[P_SLEN]);
    CHECK(TDRUM->p[P_E0] == 5 && TDRUM->p[P_PAN] == -8, "drum kit 808, pan");
    /* steps: chord tokens resolved over progression main (i VI III VII in A minor) */
    CHECK(trk[1].p[P_SLEN] == 64 && trk[1].p[P_SDIV] == 2, "bass_main unrolled to 64 steps");
    CHECK(notes_are(&trk[1].step[0], 1, 33, 0, 0) && trk[1].step[1].time == ST_REST &&
          notes_are(&trk[1].step[6], 1, 40, 0, 0), "bass bar 1: A1 . E2 (i)");
    CHECK(notes_are(&trk[1].step[16], 1, 29, 0, 0) && notes_are(&trk[1].step[22], 1, 36, 0, 0), "bar 2: F1 C2 (VI)");
    CHECK(notes_are(&trk[1].step[32], 1, 36, 0, 0) && notes_are(&trk[1].step[48], 1, 31, 0, 0), "bars 3, 4: C2, G1");
    CHECK(notes_are(&trk[0].step[0], 3, 57, 60, 64) && trk[0].step[1].time == ST_TIE &&
          notes_are(&trk[0].step[16], 3, 53, 57, 60) && notes_are(&trk[0].step[32], 3, 60, 64, 67) &&
          notes_are(&trk[0].step[48], 3, 55, 59, 62), "pad triads Am F C G, tied");
    CHECK(TDRUM->p[P_SLEN] == 16 && dstep_mask(&TDRUM->dstep[0]) == 0x11 && dstep_mask(&TDRUM->dstep[4]) == 0x15 &&
          dstep_mask(&TDRUM->dstep[2]) == 0x10 && !dstep_mask(&TDRUM->dstep[1]), "drums: dr_main (GROOVE)");
    for (k = 0; k < NSTEP; k++)
        CHECK(trk[2].step[k].time == ST_REST && !trk[2].step[k].n, "the keys loop is cleared at a World switch");
    CHECK(sane(), "every value in range");

    /* the keys loop and edits survive a scene change; the outgoing pattern is written back */
    trk[2].step[5].time = ST_NOTE;
    trk[2].step[5].n = 1;
    trk[2].step[5].note[0] = 69;
    trk[2].p[P_SLEN] = 32;
    trk[1].step[3].time = ST_NOTE;
    trk[1].step[3].n = 1;
    trk[1].step[3].note[0] = 45;
    panic_req = 0;
    rc = world_apply(2, 4);                              /* LIFT, HEAVY */
    CHECK(rc == WE_OK && wrt.scene == 2 && wrt.var == 4, "scene C, variation HEAVY: %d", rc);
    CHECK(!panic_req, "no release at a scene change");
    CHECK(notes_are(&trk[2].step[5], 1, 69, 0, 0) && trk[2].p[P_SLEN] == 32, "keys loop kept");
    CHECK(trk[2].p[P_AMODE] == 1, "PULSE's arp group kept");
    CHECK(trk[0].preset == 10 && trk[0].p[P_E5] == dark->e[5] && trk[0].p[P_ATK] == dark->env[0],
          "HEAVY: pad DARK STR");
    CHECK(trk[0].p[P_E4] == 80 && trk[0].p[P_REV] == 50, "scene pad.CUT 80 over the World's rev 50");
    CHECK(trk[0].p[P_CHOR] == 20 && trk[1].p[P_CHOR] == 20 && trk[2].p[P_CHOR] == 20, "scene *.chor");
    CHECK(song.g[G_DRLVL] == 110 && song.g[G_DUCK] == 30, "scene drums.level, g.duck");
    CHECK(TDRUM->p[P_E0] == 6 && trk[1].p[P_E6] == 40, "HEAVY: kit 909, bass.DRV");
    CHECK(trk[0].p[P_SLEN] == 8 && trk[0].p[P_SDIV] == 1, "pad_stab: 8 steps at 1/8");
    CHECK(notes_are(&trk[0].step[0], 3, 60, 64, 67) && trk[0].step[0].flags == SF_ACCENT, "pad_stab C triad !");
    CHECK(notes_are(&trk[0].step[4], 3, 62, 65, 69) && trk[0].step[4].lvl == (LV_SOFT | LV_SOFT << 2 | LV_SOFT << 4) &&
          trk[0].step[6].time == ST_TIE, "pad_stab Dm triad _, tie");
    CHECK(notes_are(&trk[1].step[1], 1, 33, 0, 0) && notes_are(&trk[1].step[12], 1, 40, 0, 0), "bass_drive");
    CHECK(dstep_has(&TDRUM->dstep[12], 4) && dstep_rat(&TDRUM->dstep[12], 4) == 1 &&
          dstep_lvl(&TDRUM->dstep[15], 2) == LV_GHOST, "dr_busy: hat x2, ghost snare");
    CHECK(sane(), "every value in range");
    rc = world_apply(1, 0);                              /* back to MAIN, ORIGINAL */
    CHECK(rc == WE_OK && notes_are(&trk[1].step[3], 1, 45, 0, 0), "the edit in bass_main came back (write-back)");
    CHECK(trk[0].preset == 9 && TDRUM->p[P_E0] == 5 && song.g[G_DUCK] == GP[G_DUCK].def, "ORIGINAL again");
    rc = world_apply(1, 3);                              /* MAIN, PULSING: swap bass_main -> bass_drive */
    CHECK(rc == WE_OK && notes_are(&trk[1].step[1], 1, 33, 0, 0) && notes_are(&trk[1].step[12], 1, 40, 0, 0),
          "PULSING swaps bass_main for bass_drive");
    rc = world_apply(1, 1);                              /* AIRY */
    CHECK(rc == WE_OK && trk[0].p[P_REV] == 80 && trk[2].p[P_REV] == 60 && song.g[G_RSIZE] == 110, "AIRY pairs, fx");
    rc = world_apply(3, 0);                              /* BREAKDOWN: degrees, accidentals, octaves, c7 / c9 */
    CHECK(rc == WE_OK && song.g[G_RSIZE] == 115 && song.g[G_SWING] == 20, "scene D fx (rsize, swing)");
    CHECK(trk[1].p[P_SLEN] == 32 && trk[1].p[P_SDIV] == 1, "bass_walk: 16 steps at 1/8 unrolled to 32 (%d)",
          trk[1].p[P_SLEN]);
    CHECK(notes_are(&trk[1].step[0], 1, 33, 0, 0) && notes_are(&trk[1].step[2], 1, 35, 0, 0) &&
          notes_are(&trk[1].step[4], 1, 28, 0, 0) && trk[1].step[5].time == ST_TIE &&
          notes_are(&trk[1].step[6], 1, 39, 0, 0), "1 . b3 . 5, - #4 = A1 B1 E1 tie D#2");
    CHECK(notes_are(&trk[1].step[8], 1, 45, 0, 0) && notes_are(&trk[1].step[9], 1, 33, 0, 0) &&
          trk[1].step[9].flags == SF_ACCENT && notes_are(&trk[1].step[10], 2, 33, 40, 0) &&
          trk[1].step[10].lvl == (LV_SOFT | LV_SOFT << 2), "1' A1! [1 5]_");
    CHECK(notes_are(&trk[1].step[12], 1, 45, 0, 0) && trk[1].step[12].flags == SF_SLIDE &&
          notes_are(&trk[1].step[13], 1, 47, 0, 0) && trk[1].step[13].rat == 1 && notes_are(&trk[1].step[14], 1, 40, 0, 0),
          "c7~ c9*2 E2 over i: A2 B2 E2");
    CHECK(notes_are(&trk[1].step[28], 1, 48, 0, 0) && notes_are(&trk[1].step[29], 1, 52, 0, 0), "c7 c9 over iv7: C3 E3");
    CHECK(notes_are(&trk[0].step[0], 3, 57, 60, 64) && notes_are(&trk[0].step[32], 3, 62, 65, 69), "pad over i, iv7");
    for (k = 0; k < NSTEP; k++)
        CHECK(!dstep_mask(&TDRUM->dstep[k]), "drums null: an empty pattern");
    CHECK(sane(), "every value in range");
    /* busy paths */
    song.playing = 1;
    CHECK(world_apply(0, 0) == WE_BUSY && world_load(blob, n) == WE_BUSY, "refused while playing");
    song.playing = 0;
    CHECK(world_apply(4, 0) == WE_STATE && world_apply(0, 5) == WE_STATE, "scene / variation out of range");
    world_block();                                      /* (Phase 11 commits here; inert now) */
    world_unload();
    CHECK(!wrt.active, "world_unload: SLOOP's paths again");
    CHECK(!memcmp(before, proj_slot, sizeof before), "proj_slot untouched");
    printf("full World: load, stage and commit (scenes A..D, variations, pool write-back, keys loop)\n");
}

/* ----------------------------------------------------------- 3. corruption --- */
static uint32_t accepted;

static void try_blob(uint8_t *b, uint32_t n)          /* whatever passes the check must load and play safely */
{
    wb_ctx_t c;
    uint32_t s, v;
    if (wb_check(b, n, &c) != WE_OK)
        return;
    accepted++;
    sloop_state();
    if (world_load(b, n) != WE_OK) {
        CHECK(0, "wb_check passed, world_load failed");
        return;
    }
    for (s = 0; s < WF_NSCENE; s++)
        for (v = 0; v < wctx.cnt[WF_S_VARS]; v++) {
            int rc = world_apply(s, v);
            CHECK(rc == WE_OK, "apply %u %u: %d", s, v, rc);
            if (!sane()) {
                CHECK(0, "an accepted blob put a value out of range (scene %u, var %u)", s, v);
                return;
            }
        }
}

static void test_corruption(const uint8_t *orig, uint32_t n, const char *what)
{
    uint8_t *b = malloc(n + 16);
    wb_ctx_t c;
    uint32_t i, k, L = n, rej = 0;
    static const uint8_t MASK[3] = {0x01, 0x80, 0xFF};
    CHECK(wb_check(orig, n, &c) == WE_OK, "%s: accepted", what);
    for (i = 0; i < n; i++) {                            /* every truncation (an exact buffer: ASan sees over-reads) */
        uint8_t *t = malloc(i ? i : 1);
        memcpy(t, orig, i);
        CHECK(wb_check(t, i, &c) != WE_OK, "%s: truncated to %u accepted", what, i);
        if (i >= WF_MIN_LEN) {                           /* ... also with L and the CRC patched to match */
            t[6] = (uint8_t)i;
            t[7] = (uint8_t)(i >> 8);
            put_crc(t);
            rej += wb_check(t, i, &c) != WE_OK;
            try_blob(t, i);
        }
        free(t);
    }
    CHECK(rej == n - WF_MIN_LEN, "%s: %u of %u truncations with L patched rejected", what, rej, n - WF_MIN_LEN);
    for (i = 0; i < n; i++)                              /* every byte flipped: the CRC (or the header) */
        for (k = 0; k < 3; k++) {
            memcpy(b, orig, n);
            b[i] ^= MASK[k];
            CHECK(wb_check(b, n, &c) != WE_OK, "%s: byte %u ^ %02x accepted", what, i, MASK[k]);
        }
    for (i = WF_CRC_FROM; i < L; i++)                    /* ... and with the CRC recomputed: structure */
        for (k = 0; k < 3; k++) {
            memcpy(b, orig, n);
            b[i] ^= MASK[k];
            put_crc(b);
            try_blob(b, n);
        }
    for (i = 0; i < 20000; i++) {                        /* random corruptions, CRC recomputed */
        uint32_t m = 1 + rnd() % 4, j;
        memcpy(b, orig, n);
        for (j = 0; j < m; j++)
            b[WF_CRC_FROM + rnd() % (L - WF_CRC_FROM)] = (uint8_t)rnd();
        put_crc(b);
        try_blob(b, n);
    }
    free(b);
    printf("%s: truncations, flips (raw and CRC-recomputed), 20000 random corruptions: %u structurally valid "
           "variants played safely\n", what, accepted);
    accepted = 0;
}

/* targeted corruptions of the full World, each with its error code */
static void expect(uint8_t *b, uint32_t n, int code, const char *what)
{
    wb_ctx_t c;
    int rc;
    put_crc(b);
    rc = wb_check(b, n, &c);
    CHECK(rc == code, "%s: WORLD ERROR %d, expected %d", what, rc, code);
}

static void test_targeted(const uint8_t *orig, uint32_t n)
{
    uint8_t *b = malloc(WF_MAX_LEN + 64);
    wb_ctx_t c;
    uint32_t sc, o;
    CHECK(wb_check(orig, n, &c) == WE_OK, "full: accepted");
    sc = c.scene[0];
#define FRESH() memcpy(b, orig, n)
    FRESH(); b[0] = 'X'; expect(b, n, WE_MAGIC, "magic");
    FRESH(); b[4] = 2; expect(b, n, WE_VERSION, "version 2");
    FRESH(); b[5] = 4; expect(b, n, WE_FLAGS, "unknown flag");
    FRESH(); b[6] = (uint8_t)(WF_MAX_LEN + 1); b[7] = (uint8_t)((WF_MAX_LEN + 1) >> 8);
    memset(b + n, 0, WF_MAX_LEN + 1 - n); expect(b, WF_MAX_LEN + 1, WE_SIZE, "L = 3841");
    FRESH(); b[6] = (uint8_t)(n + 1); b[7] = (uint8_t)((n + 1) >> 8); expect(b, n, WE_SIZE, "L beyond the buffer");
    FRESH(); b[16] = 0; expect(b, n, WE_HEADER, "nsec 0");
    FRESH(); b[17] = 1; expect(b, n, WE_HEADER, "reserved byte");
    FRESH(); b[WF_HDR + 2] ^= 1; expect(b, n, WE_TABLE, "a section length off by one");
    FRESH(); b[WF_HDR + 4] = b[WF_HDR]; expect(b, n, WE_SECTION, "two sections of one type");
    FRESH(); b[WF_HDR] = WF_S_OVERRIDES; expect(b, n, WE_SECTION, "a reserved section type (OVERRIDES)");
    FRESH(); b[sc + 12] = c.cnt[WF_S_PROGS]; expect(b, n, WE_INDEX, "scene progression out of range");
    FRESH(); b[sc + 16] = WF_MAX_PAT - 1; expect(b, n, WE_INDEX, "scene pattern out of range");
    FRESH(); b[sc + 19] = b[c.scene[1] + 17]; expect(b, n, WE_INDEX, "a synth pattern as a drum BEAT");
    FRESH(); b[sc + 18] = b[c.scene[1] + 17]; expect(b, n, WE_SCENE, "a scene pattern on the keys track");
    FRESH(); b[sc + 14] = 3; expect(b, n, WE_SCENE, "transition 3 bars");
    FRESH(); b[sc + 13] = 7; expect(b, n, WE_INDEX, "energy table out of range");
    FRESH(); b[c.trk[0] + 2] = ENGINES[0]->npresets; expect(b, n, WE_TRACK, "preset out of range");
    FRESH(); b[c.trk[0] + 1] = NENGINES; expect(b, n, WE_TRACK, "engine out of range");
    FRESH(); b[c.trk[3] + 2] = DRUM_KITS; expect(b, n, WE_TRACK, "kit out of range");
    FRESH(); b[c.trk[0] + WF_TRACK_HDR] = P_ROOT; expect(b, n, WE_PARAM, "a pair on P_ROOT");
    FRESH(); b[c.trk[0] + WF_TRACK_HDR] = P_COUNT; expect(b, n, WE_PARAM, "a parameter id past P_COUNT");
    FRESH(); b[c.off[WF_S_GLOBALS]] = G_BPM; expect(b, n, WE_PARAM, "a global outside the whitelist");
    FRESH(); b[c.off[WF_S_META] + WF_NAME_LEN + WF_CAT_LEN + WF_BLURB_LEN] = 30; expect(b, n, WE_META, "bpm 30");
    FRESH(); b[c.off[WF_S_META]] = 'a'; expect(b, n, WE_STRING, "a lowercase name");
    FRESH(); b[c.off[WF_S_META] + WF_NAME_LEN - 1] = 'A'; expect(b, n, WE_STRING, "a name without its NUL");
    FRESH(); b[c.off[WF_S_KEYS]] = TRK_DRUM; expect(b, n, WE_KEYS, "keys on the drum track");
    FRESH(); b[c.off[WF_S_KEYS] + 2] |= 2; expect(b, n, WE_KEYS, "melody mask outside the scale");
    FRESH(); b[c.off[WF_S_DEFAULTS] + 1] = c.cnt[WF_S_VARS]; expect(b, n, WE_DEFAULTS, "default variation");
    FRESH(); b[c.off[WF_S_DEFAULTS] + 2] = 251; expect(b, n, WE_DEFAULTS, "macro default above 1");
    FRESH(); b[c.off[WF_S_MAPS] + 3] = WF_CURVE_CUSTOM + c.cnt[WF_S_CURVES]; expect(b, n, WE_INDEX, "custom curve");
    FRESH(); b[c.off[WF_S_MAPS] + 1] = WF_K_GLOBAL << 5 | 1; expect(b, n, WE_MAP, "a global target with a track mask");
    FRESH(); b[c.off[WF_S_MAPS] + 0] = WF_NCTL; expect(b, n, WE_MAP, "control 16");
    FRESH(); b[c.off[WF_S_RULES] + 1] = WF_UNIT; expect(b, n, WE_RULE, "rule threshold 1.0");
    FRESH(); b[c.off[WF_S_CURVES] + 8] = 254; expect(b, n, WE_CURVE, "a curve not ending at 1");
    FRESH(); b[c.off[WF_S_GUARD] + WF_G_POLY] = 5; expect(b, n, WE_GUARD, "max_poly 5");
    FRESH(); b[c.off[WF_S_GUARD] + WF_G_RESERVED] = 0; expect(b, n, WE_GUARD, "a reserved GUARD byte");
    FRESH(); b[c.off[WF_S_ENERGY] + WF_ENERGY_HDR] = 1; expect(b, n, WE_ENERGY, "the first band not at 0");
    FRESH(); b[c.var[0] + 11] = 1; expect(b, n, WE_VAR, "ORIGINAL with an energy bias");
    o = c.pat[0];                                        /* pad_main@dark: [A3 C4 E4]: NOTE, nf 3, notes */
    FRESH(); b[o + WF_PAT_HDR + 2] = 200; expect(b, n, WE_NOTE, "a note above 127");
    FRESH(); b[o + 1] = 65; expect(b, n, WE_PATTERN, "pattern length 65");
    FRESH(); b[o] = 6; expect(b, n, WE_PATTERN, "division 6");
    FRESH(); b[o + WF_PAT_HDR + 1] = 5; expect(b, n, WE_PATTERN, "5 notes in a step");
    FRESH(); b[o + WF_PAT_HDR] = (uint8_t)(WF_R_NOTE << 6 | 63); expect(b, n, WE_PATTERN, "a cursor past the end");
    o = c.pat[1];                                        /* dr_min (scene A's MINIMAL) */
    FRESH(); b[o] = WF_PAT_DRUM | 1; expect(b, n, WE_PATTERN, "a drum pattern at 1/8");
    FRESH(); b[o + WF_PAT_HDR + 1 + 0] |= 64; expect(b, n, WE_PATTERN, "a lane byte with bit 6");
#undef FRESH
    free(b);
    printf("targeted corruptions: each refused with its error code\n");
}

static void test_minimal(const uint8_t *blob, uint32_t n)
{
    uint32_t t, k;
    int rc;
    sloop_state();
    rc = world_start(blob, n);
    CHECK(rc == WE_OK, "minimal: world_start %d", rc);
    CHECK(song.g[G_BPM] == 100 && trk[0].p[P_ROOT] == 0 && trk[0].p[P_SCALE] == 1, "minimal: 100 BPM, C MAJ");
    CHECK(wrt.scene == 1 && trk[2].eng_req == 1 && trk[2].preset == 0, "minimal: scene B (main), keys RHODES");
    CHECK(trk[2].p[P_SLEN] == 16 && trk[2].p[P_AMODE] == 0, "minimal: a 1-bar keys loop, PULSE OFF");
    CHECK(TDRUM->p[P_E0] == 5, "minimal: kit 808");
    for (t = 0; t < NTRK; t++)
        for (k = 0; k < NSTEP; k++)
            CHECK(t == TRK_DRUM ? !dstep_mask(&trk[t].dstep[k]) : trk[t].step[k].time == ST_REST,
                  "minimal: no patterns, every track empty");
    CHECK(sane(), "minimal: in range");
    printf("minimal World: load, stage, commit\n");
}

int main(int argc, char **argv)
{
    uint32_t nm, nf;
    uint8_t *mini, *full;
    const uint8_t *fb;
    uint32_t fn;
    wb_ctx_t c;
    if (argc != 3) {
        printf("usage: world_test MINIMAL.wblob FULL.wblob\n");
        return 2;
    }
    mini = read_file(argv[1], &nm);
    full = read_file(argv[2], &nf);
    CHECK(wb_check(mini, nm, &c) == WE_OK && wb_check(full, nf, &c) == WE_OK, "both blobs pass wb_check");
    CHECK(wb_check(0, 0, &c) == WE_SIZE && wb_check(full, 10, &c) == WE_SIZE, "no blob, a short one");
    CHECK(world_factory(WORLD_NFACTORY, &fb, &fn) == WE_STATE, "factory index bound");
    world_boot();
    CHECK(wrt.factory_ok == (WORLD_NFACTORY >= 32 ? 0xFFFFFFFFu : (1u << WORLD_NFACTORY) - 1u) && !wrt.active,
          "world_boot: the factory Worlds pass and nothing becomes active");
    test_presets();
    test_minimal(mini, nm);
    test_full(full, nf);
    test_targeted(full, nf);
    test_corruption(mini, nm, "minimal");
    test_corruption(full, nf, "full");
    printf(fails ? "WORLD TEST FAILED (%d)\n" : "world test: all checks passed\n", fails);
    return fails != 0;
}
