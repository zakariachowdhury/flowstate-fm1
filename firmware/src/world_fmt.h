/* SPDX-License-Identifier: GPL-3.0-only */
/* FWD1, the compiled Musical World: every constant of the format, in one place.
 *
 * Normative description: docs/design/fwd1-format.md. The firmware (world.c) includes this header; the compiler
 * (tools/worldc.py) reads it with a small parser, so the two sides cannot drift. Rules for this file, so that the
 * parser keeps up:
 *   - only #define, one name per line (a backslash continues it); values are integer expressions
 *     (decimal, hex, u suffix, | << + - * and parentheses, other WF_ names), "strings", {brace, lists} of those,
 *     or comma lists of SLOOP identifiers (P_*, G_*) that worldc resolves through worlds/schema/sloop-params.json;
 *   - names are WF_* (format), WE_* (error codes);
 *   - no hex literal of five or more digits (build.py's register-window check reads firmware/src).
 * Little-endian, byte-aligned; the parser never casts the blob to a struct. */
#ifndef WORLD_FMT_H
#define WORLD_FMT_H

/* ------------------------------------------------------------------ header --- */
#define WF_MAGIC "FWD1"
#define WF_VERSION 1                 /* a parser rejects a higher version */
#define WF_HDR 20                    /* magic, version, flags, L, world_id, CRC, nsec, 3 reserved */
#define WF_SECENT 4                  /* section table entry: u8 type, u8 count, u16 len */
#define WF_MIN_LEN 24                /* header + one table entry */
#define WF_MAX_LEN 3840              /* the storage payload of a user World */
#define WF_SOFT_LEN 2048             /* worldc warns above this */
#define WF_FACTORY_LEN 3072          /* worldc refuses a factory World above this */
#define WF_MAX_SEC 16                /* nsec: 1..16 */
#define WF_CRC_AT 12                 /* CRC-32 (zlib) over bytes [0, 12) then [16, L): all but the CRC field */
#define WF_CRC_FROM 16
#define WF_F_USER 1                  /* flags b0: a user World (Phase 14) */
#define WF_F_SESSION 2               /* flags b1: a session delta (Phase 9) */
#define WF_F_KNOWN 3
#define WF_FNV_BASIS 2166136261u     /* world_id = FNV-1a-32 over the bytes of the source id */
#define WF_FNV_PRIME 16777619u

/* ---------------------------------------------------------------- sections --- */
#define WF_S_META 1
#define WF_S_TRACKS 2
#define WF_S_GLOBALS 3
#define WF_S_PATTERNS 4
#define WF_S_PROGS 5
#define WF_S_SCENES 6
#define WF_S_VARS 7
#define WF_S_MAPS 8
#define WF_S_CURVES 9
#define WF_S_RULES 10
#define WF_S_ENERGY 11
#define WF_S_GUARD 12
#define WF_S_KEYS 13
#define WF_S_DEFAULTS 14
#define WF_S_OVERRIDES 15            /* reserved: user Worlds and sessions (Phases 9, 14); rejected by this parser */
#define WF_S_PATCHES 16              /* reserved (session) */
#define WF_S_PLAYSTATE 17            /* reserved (session) */
#define WF_S_LAST 14                 /* the highest type this parser accepts */
#define WF_S_REQUIRED ((1u << WF_S_META) | (1u << WF_S_TRACKS) | (1u << WF_S_PROGS) | (1u << WF_S_SCENES) | \
                       (1u << WF_S_VARS) | (1u << WF_S_KEYS) | (1u << WF_S_DEFAULTS))
#define WF_S_NAMES "- META TRACKS GLOBALS PATTERNS PROGS SCENES VARS MAPS CURVES RULES ENERGY GUARD KEYS DEFAULTS"

/* item counts per section (the table's count byte) */
#define WF_NTRK 4                    /* TRACKS: always 4 (3 synth, the drum track last) */
#define WF_NSCENE 4                  /* SCENES: always 4 (A..D) */
#define WF_MAX_GLOBALS 14            /* GLOBALS: each whitelisted global at most once */
#define WF_MAX_PAT 16                /* PATTERNS: the RAM pool */
#define WF_MAX_PROG 8
#define WF_MAX_CHORDS 16
#define WF_MAX_VARS 8
#define WF_MAX_MAPS 40
#define WF_MAX_CURVES 8
#define WF_MAX_RULES 8
#define WF_MAX_ACTS 4
#define WF_MAX_ENERGY 4
#define WF_MAX_BANDS 4
#define WF_MAX_GRANGES 32            /* GUARD: target range records */
#define WF_MAX_COMBOS 8              /* GUARD: sound combination records */
#define WF_MAX_PAIRS 64              /* parameter pairs of one track, scene or variation */

/* fixed sizes (bytes) */
#define WF_NAME_LEN 15               /* META name[15]: <= 14 characters + NUL */
#define WF_CAT_LEN 11                /* META category[11] */
#define WF_BLURB_LEN 25              /* META blurb[25] */
#define WF_LABEL_LEN 11              /* scene and variation names [11] */
#define WF_META_LEN 57
#define WF_TRACK_HDR 5               /* role, engine, preset, register, npairs + 2 per pair */
#define WF_PAIR 2                    /* {u8 id, i8 value} */
#define WF_SPAIR 3                   /* scoped: {u8 scope, u8 id, i8 value} */
#define WF_PAT_HDR 4                 /* kind_div, len, u16 nbytes + data */
#define WF_PROG_HDR 1                /* nchords + 2 per chord */
#define WF_SCENE_HDR 24              /* + 3 per pair */
#define WF_VAR_HDR 15                /* name[11], bias, nsound, nswap, npairs + 2a + 2b + 3n */
#define WF_MAP_LEN 6
#define WF_CURVE_LEN 9
#define WF_RULE_HDR 4                /* + 4 per action */
#define WF_ACT_LEN 4
#define WF_ENERGY_HDR 3              /* u16 density_lanes, nbands + 12 per band */
#define WF_BAND_LEN 12
#define WF_GUARD_FIX 32              /* + 4 per range record + 9 per combination record */
#define WF_GRANGE_LEN 4
#define WF_COMBO_LEN 9
#define WF_KEYS_LEN 8
#define WF_DEFAULTS_LEN 16

#define WF_NONE 255                  /* "none" for an index byte; "unset" (the firmware default) in GUARD */

/* --------------------------------------------------------- META and strings --- */
/* name and category: FONT_L glyphs 32..95 (the parser); worldc allows only A-Z 0-9 space & ' - . */
#define WF_CH_LO 32
#define WF_CH_HI 95
#define WF_NAME_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 &'-."
/* blurb: printable Latin-1 (FONT_S): 32..126 and 160..255 */
#define WF_BPM_MIN 40
#define WF_BPM_MAX 240
#define WF_SWING_MAX 100
#define WF_TEMPO_RANGE_PCT 20        /* worldc: min >= bpm - 20 %, max <= bpm + 20 % */

/* ------------------------------------------------------------------ TRACKS --- */
#define WF_ROLE_PAD 0
#define WF_ROLE_CHORDS 1
#define WF_ROLE_BASS 2
#define WF_ROLE_LEAD 3
#define WF_ROLE_KEYS 4
#define WF_ROLE_TEXTURE 5
#define WF_ROLE_DRUMS 6
#define WF_NROLES 7
#define WF_ROLE_NAMES "pad chords bass lead keys texture drums"
#define WF_ENGINE_DRUMS 255          /* TRACKS engine byte of the drum track; its preset byte is the kit */
#define WF_TRK_DRUM 3
/* the register (MIDI note) a source omits: the floor of the part's home octave, per role */
#define WF_REG_DEFAULT {48, 48, 36, 60, 48, 48, 0}
/* parameters no pair may set (TRACKS, SCENES, VARS): the World key, the pattern shape, the user's mute */
#define WF_P_FIXED P_ROOT, P_SCALE, P_SLEN, P_SDIV, P_MUTE, P_ED_FX
/* the only track parameters of the drum track (its level and reverb are G_DRLVL / G_DRREV: GLOBALS) */
#define WF_P_DRUM P_PAN, P_SSWING, P_SLCR, P_SLPAT, P_SLRATE, P_SLDEPTH
/* structural: never in SCENES / VARS pairs, macros, rules or guard ranges (design 5.3); every F_ENUM engine
 * parameter is structural too, except the one named by WF_ENUM_OK_ENGINE / WF_ENUM_OK_PARAM (WHEEL ROTR) */
#define WF_P_STRUCT P_VOICE, P_ROOT, P_SCALE, P_QUANT, P_TRANS, P_SLEN, P_SDIV, P_CHORD, P_MUTE, P_SLCR, \
                    P_SLPAT, P_AMODE, P_ARATE, P_AOCT, P_AGATE, P_ASWING, P_APROB, P_AHOLD, P_AORDER, P_LWAVE, \
                    P_GLMODE, P_PRIO, P_ALLOC, P_SLRATE, P_SSWING, P_ED_FX
#define WF_ENUM_OK_ENGINE 7          /* WHEEL */
#define WF_ENUM_OK_PARAM 7           /* E7 ROTR (stepped) */

/* ----------------------------------------------------------------- GLOBALS --- */
/* the only globals a World sets (GLOBALS, scene and variation fx); G_SWING comes from META in the base */
#define WF_G_WHITELIST G_SWING, G_DTIME, G_DFDBK, G_DCOLOR, G_DMIX, G_RSIZE, G_RDAMP, G_CRATE, G_CDEPTH, \
                       G_DRLVL, G_DRREV, G_DUST, G_DUCK, G_FILT
/* whitelisted but structural: never a macro, rule or guard target */
#define WF_G_STRUCT G_SWING, G_DTIME
/* never in a variation (a variation keeps the groove) */
#define WF_G_NOVAR G_SWING

/* ---------------------------------------------------------------- PATTERNS --- */
#define WF_PAT_DRUM 128              /* kind_div b7: a drum pattern; b0-2: div (N_DIV); b3-6: 0 */
#define WF_PAT_DIVMASK 7
#define WF_NDIV 6
#define WF_DIV_16 2                  /* 1/16: the drum track, always */
#define WF_MAX_LEN_STEPS 64
/* synth record stream: ctl = kind << 6 | arg */
#define WF_R_NOTE 0                  /* skip arg REST steps, then a NOTE step: nf, note[n], [lvl rat] [vel] [micro] */
#define WF_R_TIE 1                   /* arg (1..63) TIE steps */
#define WF_R_REST 2                  /* arg (1..63) REST steps */
#define WF_R_END 3                   /* arg 0: the rest of the pattern is REST; the last byte */
#define WF_NF_N 7                    /* nf b0-2: notes 0..4 (0 = the notes of the previous NOTE) */
#define WF_NF_ACCENT 8               /* b3: SF_ACCENT */
#define WF_NF_SLIDE 16               /* b4: SF_SLIDE */
#define WF_NF_LVLRAT 32              /* b5: two bytes follow: lvl, rat (2 bits per note) */
#define WF_NF_VEL 64                 /* b6: one byte: vel (1..127) */
#define WF_NF_MICRO 128              /* b7: one byte: micro-timing 1..7 (step flags b2-4; Phase 10) */
/* drum lane record: u8 lane | haslvl << 4 | hasrat << 5, bitmap len / 8 bytes (step i: byte i >> 3, bit i & 7),
 * then 2 bits per hit (in step order, LSB first) for the levels, then for the ratchets, if flagged */
#define WF_DL_LANE 15
#define WF_DL_LVL 16
#define WF_DL_RAT 32
#define WF_LANE_IDS "kick kick2 snare clap hat open pedal rim snare2 tomlo tomhi crash ride shaker conga bell"
#define WF_NLANES 16

/* ------------------------------------------------------------------- PROGS --- */
/* chord: u8 root_pc << 4 | quality (root: semitones above the World key, 0..11), u8 beats (1..32) */
#define WF_Q_MAJ 0
#define WF_Q_MIN 1
#define WF_Q_DIM 2
#define WF_Q_AUG 3
#define WF_Q_SUS2 4
#define WF_Q_SUS4 5
#define WF_Q_MAJ7 6
#define WF_Q_MIN7 7
#define WF_Q_DOM7 8
#define WF_Q_M7B5 9
#define WF_Q_MAJ6 10
#define WF_Q_MIN6 11
#define WF_Q_ADD9 12
#define WF_Q_MADD9 13
#define WF_Q_POW5 14
#define WF_Q_DIM7 15
#define WF_NQUAL 16
#define WF_QUAL_NAMES "MAJ MIN DIM AUG SUS2 SUS4 MAJ7 MIN7 DOM7 M7B5 MAJ6 MIN6 ADD9 MADD9 POW5 DIM7"
/* 12-bit tone masks (bit i: i semitones above the root; the 9th of ADD9 / MADD9 as bit 2) */
#define WF_QUAL_MASK {0x091, 0x089, 0x049, 0x111, 0x085, 0x0A1, 0x891, 0x489, 0x491, 0x449, 0x291, 0x289, \
                      0x095, 0x08D, 0x081, 0x249}
/* chord tokens c3 c5 c7: semitones above the chord root; 12 = the root an octave up (a tone the chord lacks).
 * c1 is 0; c9 is 14 on ADD9 / MADD9, otherwise the scale's 9th (see fwd1-format.md, PROGS) */
#define WF_QUAL_C3 {4, 3, 3, 4, 2, 5, 4, 3, 4, 3, 4, 3, 4, 3, 12, 3}
#define WF_QUAL_C5 {7, 7, 6, 8, 7, 7, 7, 7, 7, 6, 7, 7, 7, 7, 7, 6}
#define WF_QUAL_C7 {12, 12, 12, 12, 12, 12, 11, 10, 10, 10, 9, 9, 12, 12, 12, 9}
#define WF_PROG_BEATS {4, 8, 16, 32}  /* the allowed totals */
#define WF_CHORD_BEATS_MAX 32

/* ------------------------------------------------------------------ SCENES --- */
/* name[11] role prog energy transition fill pat[3] beat[4] npairs, then {scope, id, v} x npairs */
#define WF_SROLE_NAMES "intro main lift breakdown"
#define WF_NSROLES 4
#define WF_TRANSITIONS {1, 2, 4}      /* bars */
#define WF_TRANS_PHRASE 0            /* transition "phrase": the playing progression's length (Phase 11) */
#define WF_BEAT_NAMES "MINIMAL GROOVE BUSY BREAK"
#define WF_BEAT_MINIMAL 0
#define WF_BEAT_GROOVE 1
#define WF_BEAT_BUSY 2
#define WF_BEAT_BREAK 3
#define WF_NBEATS 4
#define WF_SCOPE_G 4                 /* a scoped pair's scope: 0..3 a track (id: P_*), 4 the globals (id: G_*) */
#define WF_SCOPE_CTL 5               /* VARS only (Phase 12): a macro default, id 0..3 (COLOR .. ENERGY), u8 value 0..250 */
#define WF_SCENE_NAMES "A B C D"

/* -------------------------------------------------------------------- VARS --- */
#define WF_BIAS_MAX 63               /* energy_bias, i8, units of 1/250 (worldc: |bias| <= 0.25) */

/* ------------------------------------------------- MAPS, RULES, GUARD targets --- */
/* the target byte: kind << 5 | track mask (bit per track 0..3; 0 for a global) */
#define WF_K_PARAM 0                 /* id: P_* (a multi-track mask: common parameters only) */
#define WF_K_ROLE 1                  /* id: engine role (WF_EROLE_*), synth tracks only */
#define WF_K_BRIGHT 2                /* vmod cutoff offset (Q8 / 256), synth tracks; id 0 */
#define WF_K_SHAPE 3                 /* vmod shape offset, synth tracks; id 0 */
#define WF_K_GLOBAL 4                /* id: G_* (whitelist, not WF_G_STRUCT); mask 0 */
#define WF_NKINDS 5
#define WF_TMASK 31
#define WF_TMASK_SYNTH 7
#define WF_CTL_NAMES "COLOR MOTION SPACE ENERGY SOFT SHORT BODY TAIL DRIFT WOBBLE PULSE RATE FILTER ECHO CRUSH FREEZE"
#define WF_NCTL 16
#define WF_CTL_HOME {500, 500, 500, 500, 0, 0, 0, 0, 0, 0, 0, 500, 500, 0, 0, 0}
#define WF_EROLE_NAMES "BRIGHT RESO DRIVE SHAPE DETUNE AIR MOVE BODY"
#define WF_NEROLES 8
#define WF_EROLE_RESO 1
#define WF_EROLE_DRIVE 2
#define WF_EROLE_DETUNE 4
#define WF_EROLE_BODY 7
/* engine roles: per engine (ENGINES[] order) the EDIT slot (0..7 = P_E0..P_E7) of each role, WF_NONE = none */
#define WF_ENG_ROLE {{4, 5, 6, 2, 1, 3, WF_NONE, WF_NONE},              /* ANALOG  CUT RES DRV MIX DTN NOIS */ \
                     {4, WF_NONE, 6, 5, WF_NONE, WF_NONE, WF_NONE, WF_NONE},   /* DIGITAL IDX FB MDEC */ \
                     {2, WF_NONE, WF_NONE, 3, 4, WF_NONE, WF_NONE, 6},          /* PHASE   DCW ENV DTN SUB */ \
                     {7, WF_NONE, 3, 2, WF_NONE, WF_NONE, 5, WF_NONE},          /* LOFI    TONE CRSH DUTY VIB */ \
                     {4, WF_NONE, 6, WF_NONE, WF_NONE, WF_NONE, WF_NONE, WF_NONE},   /* SAMPLE CUT DRV */ \
                     {4, 6, WF_NONE, 0, WF_NONE, 5, 7, WF_NONE},                /* VOICE   BUZZ Q VOWL BRTH RAND */ \
                     {5, 6, WF_NONE, 7, 3, WF_NONE, WF_NONE, WF_NONE},          /* TRIO    CUT RES PW DTN */ \
                     {3, WF_NONE, 6, 5, WF_NONE, WF_NONE, 7, 1},                /* WHEEL   TOP DRV CLICK ROTR SUB */ \
                     {7, WF_NONE, WF_NONE, 1, 6, WF_NONE, 5, 2}}                /* GRAIN   TONE POS RAND SPRD SIZE */
/* MAPS: u8 ctl, u8 target, u8 id, u8 class << 6 | curve, i8 min, i8 max */
#define WF_CURVE_NAMES "lin exp log s late"
#define WF_NBUILTIN_CURVES 5
#define WF_CURVE_CUSTOM 8            /* curve 8 + k: CURVES record k */
#define WF_CURVE_MASK 63
#define WF_CLASS_NAMES "fast medium slow stepped"
#define WF_CLASS_DEFAULT 1           /* medium; a stepped (enum) target is always class 3 */
#define WF_CLASS_STEPPED 3
/* The built-in mappings of controls 4..15 (Phase 13; design 9.3, 9.4), MAPS records: a World's `controls` that name a
 * control replace all of that control's records. The track mask WF_TMASK_KEYS means the Smart Keys track (built-in
 * records only); a negative offset is written 256 - n. SOUND SHAPE moves the keys track's envelope and MOVEMENT its
 * LFO (guard_limits.h GL_K*: the windows that keep them playable); LIVE FX the master (DJ filter, delay bus, DUST).
 * FREEZE has no record: macro.c asks the punch engine for its loop (WF_FREEZE_ON) */
#define WF_TMASK_KEYS 16
#define WF_CTL_LIVE 12               /* FILTER ECHO CRUSH FREEZE: momentary (FX held), never saved in the session */
#define WF_CTL_ECHO 13
#define WF_CTL_CRUSH 14
#define WF_CTL_FREEZE 15
#define WF_FREEZE_ON 250             /* FREEZE: off under 25 %, then a loop of 1 beat, 1/2 beat from 50 %, 1/4 from 75 % */
#define WF_CTL_BUILTIN { \
    4, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_ATK, 1 << 6 | 1, 0, 70,                   /* SOFT    atk +70 exp */ \
    5, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_DEC, 1 << 6, 0, 256 - 40,                /* SHORT   dec -40 */ \
    5, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_REL, 1 << 6, 0, 256 - 50,                /*         rel -50 */ \
    5, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_SUS, 1 << 6, 0, 256 - 60,                /*         sus -60 */ \
    6, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_SUS, 1 << 6, 0, 40,                      /* BODY    sus +40 */ \
    6, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_DEC, 1 << 6, 0, 30,                      /*         dec +30 */ \
    6, WF_K_ROLE << 5 | WF_TMASK_KEYS, WF_EROLE_BODY, 1 << 6, 0, 30,               /*         @BODY +30 */ \
    7, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_REL, 1 << 6, 0, 56,                      /* TAIL    rel +56 */ \
    7, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_REV, 1 << 6 | 3, 0, 35,                  /*         rev +35 s */ \
    8, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_LD_PIT, 2 << 6 | 4, 0, 2,                /* DRIFT   ld_pit +2 late */ \
    8, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_LD_FLT, 2 << 6, 0, 10,                   /*         ld_flt +10 */ \
    8, WF_K_ROLE << 5 | WF_TMASK_KEYS, WF_EROLE_DETUNE, 2 << 6, 0, 15,             /*         @DETUNE +15 */ \
    9, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_LD_FLT, 2 << 6, 0, 45,                   /* WOBBLE  ld_flt +45 */ \
    10, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_LD_AMP, 2 << 6, 0, 70,                  /* PULSE   ld_amp +70 */ \
    11, WF_K_PARAM << 5 | WF_TMASK_KEYS, P_LRATE, 2 << 6, 256 - 24, 24,            /* RATE    lrate -24..+24 */ \
    12, WF_K_GLOBAL << 5, G_FILT, 1 << 6, 256 - 64, 63,                            /* FILTER  filt -64..+63 */ \
    13, WF_K_GLOBAL << 5, G_DMIX, 1 << 6, 0, 40,                                   /* ECHO    dmix +40 */ \
    13, WF_K_PARAM << 5 | WF_TMASK_SYNTH, P_DLY, 1 << 6, 0, 60,                    /*         *.dly +60 */ \
    13, WF_K_GLOBAL << 5, G_DFDBK, 1 << 6, 0, 30,                                  /*         dfdbk +30 */ \
    14, WF_K_GLOBAL << 5, G_DUST, 1 << 6, 0, 90,                                   /* CRUSH   dust +90 */ \
    14, WF_K_PARAM << 5 | WF_TMASK_SYNTH, P_LEVEL, 1 << 6, 0, 256 - 5,             /*         *.level -5 */ \
    14, WF_K_GLOBAL << 5, G_DRLVL, 1 << 6, 0, 256 - 5}                             /*         drums -5 */
/* the guard's combinations while LIVE FX play, whatever the World's (WF_COMBO_DEFAULT's records): ECHO up, a big room
 * and a long echo shorten the echo, and its feedback stays at most GL_ECHO_DFDBK; CRUSH up, DUST stays moderate on a
 * distorted part */
#define WF_COMBO_LIVE { \
    WF_K_GLOBAL << 5, G_RSIZE, 104, WF_K_GLOBAL << 5, G_DFDBK, 72, WF_K_GLOBAL << 5, G_DFDBK, 72, \
    WF_K_GLOBAL << 5, G_DFDBK, 96, WF_K_GLOBAL << 5, G_DFDBK, 96, WF_K_GLOBAL << 5, G_DFDBK, 96, \
    WF_K_PARAM << 5 | 7, P_DIST, 80, WF_K_GLOBAL << 5, G_DUST, 64, WF_K_GLOBAL << 5, G_DUST, 64}
/* RULES: u8 a << 4 | b, u8 ta, u8 tb (0..249, units of 1/250), u8 nact, then {target, id, i8 add, u8 0} x nact;
 * a one-condition rule repeats it (b = a, tb = ta) */
#define WF_THRESH_MAX 249
#define WF_UNIT 250                  /* positions and thresholds in the blob: 0..250 = 0..1 */

/* ------------------------------------------------------------------ ENERGY --- */
/* u16 density_lanes, u8 nbands, then per band: u8 from (0..250, ascending, the first 0),
 * u8 layers (b0-3: tracks) | fills << 4 | ratchets << 5, u16 lanes, u16 dens_steps, u16 play[3] */
#define WF_B_LAYERS 15
#define WF_B_FILLS 16
#define WF_B_RATCHETS 32

/* ------------------------------------------------------------------- GUARD --- */
/* 32 fixed bytes, each WF_NONE (255) when unset (the firmware default applies), then {target, id, i8 lo, i8 hi} x n */
#define WF_G_RANGE 0                 /* 0..5: lo, hi (MIDI) of tracks 0..2; both set or both 255 */
#define WF_G_POLY 6                  /* max_poly of the keys track, 1..4 */
#define WF_G_FOLLOW 7                /* loop_follow: 0 off, 1 snap */
#define WF_G_AVOID 8                 /* 0 classic, 1 none, 2 strict */
#define WF_G_QUANT 9                 /* record quantise strength 0..250 */
#define WF_G_RECNOTES 10             /* notes per recorded step 1..4 */
#define WF_G_MAXLEVEL 11             /* 0..127 */
#define WF_G_MAXRESO 12              /* 0..127 */
#define WF_G_MAXDFDBK 13             /* 0..120 */
#define WF_G_MAXRSIZE 14             /* 0..127 */
#define WF_G_MAXDIST 15              /* 0..127 */
#define WF_G_MAXDUST 16              /* 0..127 */
#define WF_G_MUTECHG 17              /* layer changes every 1 or 2 bars */
#define WF_G_DENSCHG 18              /* density changes on: 0 the beat, 1 the bar */
#define WF_G_FILLS 19                /* fills every 1..32 bars */
#define WF_G_BANDBARS 20             /* bars between ENERGY band changes, 1..16 */
#define WF_G_UNISON 21               /* max unison voices 1..8 */
#define WF_G_GRAINDENS 22            /* GRAIN DENS cap 0..127 */
#define WF_G_DISTTRK 23              /* tracks with DIST > 0, 0..3 */
#define WF_G_CPU 24                  /* cpu_q8 ceiling 1..255 */
#define WF_G_COMBOS 25               /* combination records after the ranges, 0..8 (255: WF_COMBO_DEFAULT) */
#define WF_G_RESERVED 26             /* 26..31: 255 */
#define WF_FOLLOW_NAMES "off snap"
#define WF_AVOID_NAMES "classic none strict"
#define WF_DENSCHG_NAMES "beat bar"
/* fields 6..25 (WF_G_POLY .. WF_G_COMBOS): the valid range of a set field, and the firmware default of an unset one
 * (design 6.1; fills 0 = the progression's length; combos WF_NONE = WF_COMBO_DEFAULT) */
#define WF_GUARD_MIN {1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 0, 1, 0}
#define WF_GUARD_MAX {4, 1, 2, 250, 4, 127, 127, 120, 127, 127, 127, 2, 1, 32, 16, 8, 127, 3, 254, 8}
#define WF_GUARD_DEFAULT {4, 1, 0, 188, 4, 116, 110, 96, 120, 100, 100, 1, 0, 0, 1, 4, 90, 2, 217, 255}
/* a sound combination (guard.c, design 6.1): {target a, id a, i8 over a, target b, id b, i8 over b, target c, id c,
 * i8 cap}: while a's value is above "over a" and b's above "over b", c stays at or under cap (a one-condition
 * combination repeats a as b). Targets as MAPS (kind PARAM, ROLE or GLOBAL); a per-track c is judged per track,
 * with a and b on that track when they name it (else their highest). The cap comes in as the conditions rise
 * through WF_COMBO_RAMP steps past their thresholds (a condition on c itself: as soon as c is above it). The
 * firmware's own, for a World whose GUARD sets none: */
#define WF_COMBO_RAMP 16
#define WF_COMBO_DEFAULT { \
    WF_K_GLOBAL << 5, G_RSIZE, 112, WF_K_GLOBAL << 5, G_DFDBK, 80, WF_K_GLOBAL << 5, G_DFDBK, 80, \
    WF_K_ROLE << 5 | 7, WF_EROLE_DRIVE, 64, WF_K_ROLE << 5 | 7, WF_EROLE_RESO, 90, WF_K_ROLE << 5 | 7, WF_EROLE_RESO, 90, \
    WF_K_PARAM << 5 | 7, P_DIST, 80, WF_K_GLOBAL << 5, G_DUST, 64, WF_K_GLOBAL << 5, G_DUST, 64, \
    WF_K_GLOBAL << 5, G_RSIZE, 116, WF_K_PARAM << 5 | 7, P_REV, 110, WF_K_PARAM << 5 | 7, P_REV, 110, \
    WF_K_GLOBAL << 5, G_RSIZE, 116, WF_K_GLOBAL << 5, G_DRREV, 100, WF_K_GLOBAL << 5, G_DRREV, 100, \
    WF_K_GLOBAL << 5, G_DFDBK, 90, WF_K_PARAM << 5 | 7, P_DLY, 100, WF_K_PARAM << 5 | 7, P_DLY, 100}
/*  a big room and a long echo: the echo shortens           drive into resonance: the resonance stays under 90
 *  a distorted part through DUST: DUST stays moderate      a huge room: no part's reverb send above 110
 *  a huge room: the drums' reverb at most 100              a long echo: no part's echo send above 100 */
#define WF_ENG_GRAIN 8               /* the CPU guard: GRAIN's DENS (EDIT slot 3) under GUARD grain_dens */
#define WF_GRAIN_DENS 3

/* -------------------------------------------------------------------- KEYS --- */
/* trk, mode, u16 melody_mask (12 bits, relative to the key, bit 0 set, inside the World scale), white, black,
 * tonic (MIDI), loop_pat (a pool index, 255 = none) */
#define WF_KMODE_NAMES "melody chords bass drums"
#define WF_NKMODES 4                 /* only melody is played before Phase 6+ adds the others */
#define WF_WHITE_NAMES "scale safe"
#define WF_BLACK_NAMES "chord chord+9"
#define WF_NWB 2

/* ---------------------------------------------------------------- DEFAULTS --- */
/* scene, var, ctl[4] (COLOR MOTION SPACE ENERGY), pulse, beat, shape[4] (SOFT SHORT BODY TAIL),
 * move[4] (DRIFT WOBBLE PULSE RATE); positions 0..250 */
#define WF_PULSE_NAMES "OFF SLOW PULSE DRIVE"
#define WF_NPULSE 4

/* ------------------------------------------------------------- error codes --- */
/* wb_check / world_load return one of these; the PLAY UI shows "WORLD ERROR n" */
#define WE_OK 0
#define WE_SIZE 1                    /* shorter than the header, L beyond the buffer, L outside 24..3840 */
#define WE_MAGIC 2
#define WE_VERSION 3
#define WE_FLAGS 4                   /* unknown flag bits */
#define WE_CRC 5
#define WE_HEADER 6                  /* nsec outside 1..16, reserved bytes not 0 */
#define WE_TABLE 7                   /* 20 + 4 nsec + sum(len) != L */
#define WE_SECTION 8                 /* unknown or reserved type, types not ascending (each at most once) */
#define WE_MISSING 9                 /* a required section is absent */
#define WE_LENGTH 10                 /* a section's len differs from the length its counts imply */
#define WE_COUNT 11                  /* a count outside its range */
#define WE_STRING 12                 /* a name, category, blurb or label outside its charset or not NUL-padded */
#define WE_META 13                   /* tempo, key, scale or swing out of range */
#define WE_TRACK 14                  /* role, engine, preset, kit or register */
#define WE_PARAM 15                  /* a parameter or global id out of range or not allowed there */
#define WE_PATTERN 16                /* pattern header or data */
#define WE_NOTE 17                   /* a note above 127 */
#define WE_PROG 18                   /* chord count, quality, beats, beat total */
#define WE_SCENE 19
#define WE_VAR 20
#define WE_MAP 21
#define WE_CURVE 22
#define WE_RULE 23
#define WE_ENERGY 24
#define WE_GUARD 25
#define WE_KEYS 26
#define WE_DEFAULTS 27
#define WE_INDEX 28                  /* a pattern, progression, energy or curve index out of range or of the wrong kind */
#define WE_BUSY 29                   /* world_load / world_apply: the transport is running or a stage is pending */
#define WE_STATE 30                  /* no World loaded, or a scene / variation out of range */
#define WE_NAMES "OK SIZE MAGIC VERSION FLAGS CRC HEADER TABLE SECTION MISSING LENGTH COUNT STRING META TRACK PARAM " \
                 "PATTERN NOTE PROG SCENE VAR MAP CURVE RULE ENERGY GUARD KEYS DEFAULTS INDEX BUSY STATE"

#endif
