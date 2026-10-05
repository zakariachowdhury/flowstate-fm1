/* SPDX-License-Identifier: GPL-3.0-only */
/* sloop-examples: three example projects, made with the firmware's own calls (host/core.c is included):
 * TOOLS > NEW (project_new), each part's engine and preset by name from the engine tables (set_engine_of,
 * apply_preset_to: what the PRESETS knob does), the drum kit by name, parameters inside their ranges, the
 * steps into the patterns, and the project as SAVE stores it (proj_capture, format FUN4).
 *   sloop-examples [DIR]      DIR/ambient.fun4, groove.fun4, cinematic.fun4 (default examples/projects)
 *
 * ambient    70 BPM, D major. A slow ATMOS PAD (ANALOG) in airy three-note voicings, Dmaj9 Bm9 Gmaj9 Aadd9,
 *            two bars each, under a sparse VIBES line (SAMPLE), a sine sub on the roots, a few soft hits of
 *            the AMBIENT kit; long releases, a big reverb, quarter-note echoes. 8-bar cycle.
 * groove     120 BPM, A minor, swung 16ths. Lo-fi house: HOUSE kit four to the floor with offbeat open
 *            hats and a shaker, an FM BASS on the offbeats, LOFI KEYS (SAMPLE) stabs on Dm9 G13 Cmaj9 Am9,
 *            a LOFI FLUTE answer every other four bars; DUST on the master, the kick ducking the parts.
 * cinematic  72 BPM, C minor, the "Neon Rain" mood. WARM PAD (ANALOG) on Cm(add9) Ab Eb Bb, a SUB BASS
 *            pedal on C that falls to Bb, a G-FUNK LD phrase that glides between its notes, a sparse
 *            half-time SYNTHWV beat with a rain of ghost shakers; long echoes, a big reverb.
 * All notes are the projects' own (no melody is borrowed). */
#include "core.c"

static const char *now;                          /* the project being made (messages) */
static void die(const char *what, const char *name)
{
    fprintf(stderr, "sloop-examples: %s: %s '%s'\n", now, what, name);
    exit(1);
}

/* ---- sounds */
static void sound(uint32_t k, const char *engine, const char *preset)
{
    uint32_t e, i;
    for (e = 0; e < NENGINES && !str_eq(ENGINES[e]->name, engine); e++)
        ;
    if (e == NENGINES)
        die("no engine", engine);
    for (i = 0; i < ENGINES[e]->npresets && !str_eq(ENGINES[e]->presets[i].name, preset); i++)
        ;
    if (i == ENGINES[e]->npresets)
        die("no preset", preset);
    set_engine_of(&trk[k], e);
    apply_preset_to(&trk[k], i);
}
static void kit(const char *name)
{
    uint32_t k;
    for (k = 0; k < DRUM_KITS && !str_eq(DRUM_KIT_NAMES[k], name); k++)
        ;
    if (k == DRUM_KITS)
        die("no drum kit", name);
    TDRUM->p[P_E0] = (int16_t)k;
}

/* ---- parameters, checked against their descriptors (an authoring error stops here) */
static int value_of(const param_desc_t *d, const char *name)   /* an F_ENUM value by its name */
{
    int32_t v;
    for (v = d->min; d->names && v <= d->max; v++)
        if (str_eq(d->names[v], name))
            return v;
    die("no value", name);
    return 0;
}
static void set(uint32_t k, uint32_t id, int32_t v)
{
    const param_desc_t *d = track_desc(&trk[k], id);
    if (v < d->min || v > d->max)
        die("out of range", d->label);
    trk[k].p[id] = (int16_t)v;
}
static void set_n(uint32_t k, uint32_t id, const char *name) { set(k, id, value_of(track_desc(&trk[k], id), name)); }
static void gset(uint32_t id, int32_t v)
{
    if (v < GP[id].min || v > GP[id].max)
        die("out of range", GP[id].label);
    song.g[id] = (int16_t)v;
}
static void gset_n(uint32_t id, const char *name) { gset(id, value_of(&GP[id], name)); }
static void key(const char *root, const char *scale)   /* the parts' key (SCL + key), keys snap into it */
{
    uint32_t k, r;
    for (r = 0; r < 12u && !str_eq(N_NOTE[r], root); r++)
        ;
    if (r == 12u)
        die("no root", root);
    for (k = 0; k < NPART; k++) {
        set(k, P_ROOT, (int32_t)r);
        set_n(k, P_SCALE, scale);
        set_n(k, P_QUANT, "SNAP");
    }
}

/* ---- patterns */
static int note_of(const char **s)               /* "C4" = 60 (the FM-1's middle C), "F#3", "Bb1"; -1 = none */
{
    static const int8_t PC[7] = {9, 11, 0, 2, 4, 5, 7};   /* A .. G */
    const char *p = *s;
    int n;
    if (*p < 'A' || *p > 'G')
        return -1;
    n = PC[*p++ - 'A'];
    if (*p == '#')
        n++, p++;
    else if (*p == 'b')
        n--, p++;
    if (*p < '0' || *p > '8')
        return -1;
    n += 12 * (*p++ - '0' + 1);
    *s = p;
    return n;
}
/* a synth part's pattern, one token per step ('|' between bars is ignored): a note, or a chord of up
 * to four joined by '+'; '-' TIE (what sounds goes on); '.' REST. A suffix '!' accents the step, '\''
 * plays it soft, '~' slides into the next step (legato, with the part's glide). Sets the length. */
static void steps(uint32_t k, const char *pat)
{
    track_t *t = &trk[k];
    const char *s = pat;
    uint32_t i = 0, j;
    steps_clear(t);
    for (;;) {
        step_t *st;
        while (*s == ' ' || *s == '|')
            s++;
        if (!*s)
            break;
        if (i == NSTEP)
            die("more than 64 steps", pat);
        st = &t->step[i++];
        memset(st, 0, sizeof *st);
        if (*s == '.' || *s == '-') {
            st->time = *s++ == '.' ? ST_REST : ST_TIE;
        } else {
            st->time = ST_NOTE;
            st->vel = 100;                       /* (the keys' velocity) */
            for (;;) {
                int n = note_of(&s);
                if (n < 0 || st->n == 4u)
                    die("bad note", s);
                st->note[st->n++] = (uint8_t)n;
                if (*s != '+')
                    break;
                s++;
            }
        }
        for (;; s++) {
            if (*s == '!')
                st->flags |= SF_ACCENT;
            else if (*s == '~')
                st->flags |= SF_SLIDE;
            else if (*s == '\'')
                for (j = 0; j < 4u; j++)
                    st->lvl |= (uint8_t)(LV_SOFT << (2u * j));
            else
                break;
        }
        if (*s && *s != ' ' && *s != '|')
            die("bad step", s);
    }
    set(k, P_SLEN, (int32_t)i);
}
/* one lane of the drum track, one character per step: '.' none, 'x' a hit, 'X' hard, 's' soft, 'g' ghost,
 * '2'..'4' that many hits in the step (ratchet). Every lane of a project has the same length. */
static uint32_t drum_len;                       /* the project's first lane set it */
static void lane(const char *name, const char *pat)
{
    uint32_t l, i = 0;
    for (l = 0; l < DRUM_LANES && !str_eq(LANE_NAME[l], name); l++)
        ;
    if (l == DRUM_LANES)
        die("no drum lane", name);
    for (; *pat; pat++) {
        dstep_t *d;
        if (*pat == ' ' || *pat == '|')
            continue;
        if (i == NSTEP)
            die("more than 64 steps", name);
        d = &TDRUM->dstep[i++];
        switch (*pat) {
        case '.': break;
        case 'x': dstep_set(d, l, LV_NORM, 0); break;
        case 'X': dstep_set(d, l, LV_HARD, 0); break;
        case 's': dstep_set(d, l, LV_SOFT, 0); break;
        case 'g': dstep_set(d, l, LV_GHOST, 0); break;
        case '2': case '3': case '4': dstep_set(d, l, LV_NORM, (uint32_t)(*pat - '1')); break;
        default: die("bad hit", pat);
        }
    }
    if (drum_len && i != drum_len)
        die("a lane of another length", name);
    drum_len = i;
    set(TRK_DRUM, P_SLEN, (int32_t)i);
}

static void begin(const char *name)
{
    now = name;
    drum_len = 0;
    project_new();                               /* TOOLS > NEW: default sounds, empty patterns, 90 BPM */
    steps_clear(TDRUM);
}
static void save(const char *dir, const char *name)
{
    char path[1024];
    project_t *p = &host_proj;
    FILE *f;
    snprintf(path, sizeof path, "%s/%s.fun4", dir, name);
    proj_capture(p);                             /* (what SAVE stores into a slot) */
    if (!(f = fopen(path, "wb")) || fwrite(p, 1, sizeof *p, f) != sizeof *p || fclose(f)) {
        fprintf(stderr, "sloop-examples: cannot write %s\n", path);
        exit(1);
    }
    printf("wrote %s (%zu bytes)\n", path, sizeof *p);
}

/* ======================================================================= ambient === */
static void ambient(void)
{
    begin("ambient");
    gset(G_BPM, 70);
    gset_n(G_DTIME, "1/4");                      /* echoes on the beat, long */
    gset(G_DFDBK, 66);
    gset(G_DCOLOR, 50);
    gset(G_DMIX, 96);
    gset(G_RSIZE, 120);                          /* a large, soft room */
    gset(G_RDAMP, 70);
    gset(G_CRATE, 24);
    gset(G_CDEPTH, 76);

    sound(0, "ANALOG", "ATMOS PAD");             /* the chords: Dmaj9, Bm9, Gmaj9, Aadd9, two bars each */
    set(0, P_ATK, 100);                          /* ~1.4 s in, ~2 s out */
    set(0, P_REL, 104);
    set(0, P_LRATE, 14);                         /* a slow drift of the pulse width and the filter */
    set(0, P_LD_SHP, 22);
    set(0, P_LD_FLT, 10);
    set(0, P_LEVEL, 86);
    set(0, P_CHOR, 64);
    set(0, P_DLY, 18);
    set(0, P_REV, 104);
    set_n(0, P_SDIV, "1/4");
    steps(0, "F#3+A3+E4 - - - - - - - | F#3+A3+C#4 - - - - - - - | "
             "F#3+B3+D4 - - - - - - - | E3+A3+B3 - - - - - - -");

    sound(1, "SAMPLE", "VIBES");                 /* the line: a few notes of D major over the chords */
    set(1, P_REL, 92);
    set(1, P_LEVEL, 98);
    set(1, P_CHOR, 20);
    set(1, P_DLY, 64);
    set(1, P_REV, 96);
    set(1, P_PAN, 12);
    set_n(1, P_SDIV, "1/8");
    steps(1, ". . F#5 - - - E5 - | A4 - - - . . . . | . . D5 - - - C#5 - | F#4 - - - . . . . | "
             ". . B4 - D5 - - - | A5 - - - - - . . | . . E5 - - - C#5 - | B4 - - - . . . .");

    sound(2, "ANALOG", "SUB BASS");              /* the roots, a sine two octaves down */
    set(2, P_ATK, 70);
    set(2, P_REL, 92);
    set(2, P_LEVEL, 84);
    set(2, P_REV, 12);
    set_n(2, P_SDIV, "1/4");
    steps(2, "D2 - - - - - - - | B1 - - - - - - - | G1 - - - - - - - | A1 - - - - - - -");

    kit("AMBIENT");                              /* a heartbeat, a shaker breathing, a rim far away */
    gset(G_DRLVL, 80);
    gset(G_DRREV, 104);
    set_n(TRK_DRUM, P_SDIV, "1/16");
    lane("KICK",   "s............... | ........g.......");
    lane("SHAKER", "....g.......g... | ....g...g...g...");
    lane("RIM",    "................ | ............g...");
    lane("RIDE",   "g............... | ................");
    key("D", "MAJ");
}

/* ======================================================================== groove === */
static void groove(void)
{
    begin("groove");
    gset(G_BPM, 120);
    gset(G_SWING, 26);                           /* 56.5 %: the 16ths shuffle */
    gset_n(G_DTIME, "8T");
    gset(G_DFDBK, 40);
    gset(G_DMIX, 80);
    gset(G_RSIZE, 70);
    gset(G_RDAMP, 64);
    gset(G_DUST, 34);                            /* the lo-fi: crackle, a softer top */
    gset(G_DUCK, 40);                            /* the kick pumps the parts */

    sound(0, "DIGITAL", "FM BASS");              /* offbeat roots and fifths: D, G, C, A */
    set(0, P_SGATE, 80);
    set(0, P_LEVEL, 112);
    set_n(0, P_SDIV, "1/16");
    steps(0, ". . D2 . . . D2 . . . A2 . . C3 . . | . . G1 . . . G1 . . . D2 . . F2 . . | "
             ". . C2 . . . C2 . . . G2 . . B2 . . | . . A1 . . . A1 . . . E2 . G2 . E2 .");

    sound(1, "SAMPLE", "LOFI KEYS");             /* stabs: Dm9 G13 Cmaj9 Am9 */
    set(1, P_SGATE, 100);
    set(1, P_LEVEL, 84);
    set(1, P_REV, 40);
    set(1, P_DLY, 16);
    set(1, P_PAN, -10);
    set_n(1, P_SDIV, "1/16");
    steps(1, "F3+A3+C4+E4 . . F3+A3+C4+E4 - . F3+A3+C4+E4' . . . . . F3+A3+C4+E4 - . . | "
             "F3+B3+D4+E4 . . F3+B3+D4+E4 - . F3+B3+D4+E4' . . . . . F3+B3+D4+E4 - . . | "
             "E3+G3+B3+D4 . . E3+G3+B3+D4 - . E3+G3+B3+D4' . . . . . E3+G3+B3+D4 - . . | "
             "G3+B3+C4+E4 . . G3+B3+C4+E4 - . G3+B3+C4+E4' . . . . . G3+B3+C4+E4 - . E3+G3+B3+C4'");

    sound(2, "SAMPLE", "LOFI FLUTE");            /* the answer, every other four bars */
    set(2, P_LEVEL, 96);
    set(2, P_DLY, 44);
    set(2, P_REV, 52);
    set(2, P_PAN, 14);
    set_n(2, P_SDIV, "1/8");
    steps(2, ". . . . . . . . | . . . . . . . . | . . . . . . . . | . . . . . . . . | "
             ". . A4 - C5 - D5 - | E5 - - - D5 - C5 - | . . G4 - A4 - C5 - | B4 - - - A4 - - .");

    kit("HOUSE");
    gset(G_DRLVL, 96);
    gset(G_DRREV, 20);
    set(TRK_DRUM, P_SSWING, 8);                  /* the drums a little more */
    set_n(TRK_DRUM, P_SDIV, "1/16");
    lane("KICK",     "x...x...x...x... | x...x...x...x..g");
    lane("CLAP",     "....x.......x... | ....x.......x...");
    lane("OPEN HAT", "..x...x...x...x. | ..x...x...x...x.");
    lane("HAT",      "g.s.g.s.g.s.g.ss | g.s.g.s.g.s.g.s.");
    lane("SHAKER",   ".g.g.g.g.g.g.g.g | .g.g.g.g.g.g.g.g");
    lane("RIM",      "................ | ...........s....");
    key("A", "MIN");
}

/* ===================================================================== cinematic === */
static void cinematic(void)
{
    begin("cinematic");
    gset(G_BPM, 72);
    gset_n(G_DTIME, "1/4");
    gset(G_DFDBK, 72);
    gset(G_DCOLOR, 46);
    gset(G_DMIX, 100);
    gset(G_RSIZE, 124);                          /* the big hall */
    gset(G_RDAMP, 54);
    gset(G_CRATE, 30);
    gset(G_CDEPTH, 70);

    sound(0, "ANALOG", "WARM PAD");              /* Cm(add9), Ab, Eb, Bb: two bars each, over the pedal */
    set(0, P_ATK, 92);
    set(0, P_REL, 100);
    set(0, P_LEVEL, 86);
    set(0, P_CHOR, 60);
    set(0, P_DLY, 16);
    set(0, P_REV, 92);
    set_n(0, P_SDIV, "1/4");
    steps(0, "Eb3+G3+D4 - - - - - - - | Eb3+Ab3+C4 - - - - - - - | "
             "Eb3+G3+Bb3 - - - - - - - | D3+F3+Bb3 - - - - - - -");

    sound(1, "ANALOG", "SUB BASS");              /* the pedal: C for six bars, then Bb */
    set(1, P_ATK, 76);
    set(1, P_REL, 96);
    set(1, P_LEVEL, 84);
    set_n(1, P_SDIV, "1/4");
    steps(1, "C2 - - - - - - - | - - - - - - - - | - - - - - - - - | Bb1 - - - - - - -");

    sound(2, "ANALOG", "G-FUNK LD");             /* the voice: long notes, glides, the vibrato coming in */
    set(2, P_LEVEL, 86);
    set(2, P_E4, 100);                           /* a little brighter (CUT) */
    set(2, P_CHOR, 20);
    set(2, P_DLY, 66);
    set(2, P_REV, 86);
    set(2, P_PAN, -8);
    set_n(2, P_SDIV, "1/8");
    steps(2, ". . . . . . . . | . . . . G4 - Bb4 -~ | C5 - - - - - - . | Eb5 -~ D5 - C5 - Bb4 -~ | "
             "G4 - - - - - - . | . . Bb4 - C5 -~ Eb5 - | D5 - - - - -~ C5 - | Bb4 - - - - - . .");

    kit("SYNTHWV");                              /* half time: kick, a clap on three, rain on the shaker */
    gset(G_DRLVL, 92);
    gset(G_DRREV, 72);
    set_n(TRK_DRUM, P_SDIV, "1/16");
    lane("KICK",    "X.....x......... | X.....x...x.....");
    lane("CLAP",    "........x....... | ........x.......");
    lane("SHAKER",  "g.gg.g.gg.g.gg.g | g.gg.g.gg.g.g.gg");
    lane("HAT",     "..s...s...s...s. | ..s...s...s...s.");
    lane("LOW TOM", "................ | .............s..");
    lane("HI TOM",  "................ | ..............s.");
    key("C", "MIN");
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "examples/projects";
    if (argc > 2) {
        fprintf(stderr, "usage: sloop-examples [DIR]\n");
        return 2;
    }
    if (host_boot(NULL))
        return 1;
    ambient();
    save(dir, "ambient");
    groove();
    save(dir, "groove");
    cinematic();
    save(dir, "cinematic");
    return 0;
}
