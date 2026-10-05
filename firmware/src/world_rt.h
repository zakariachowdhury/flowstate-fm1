/* SPDX-License-Identifier: GPL-3.0-only */
/* Musical Worlds: the runtime state the main loop and the audio ISR share (design 1.6). Included by core.h when
 * FELUCCA_WORLD is 1 (before trk_silent, which reads wrt.mute), so every consumer of core.h sees it. Types, state and
 * prototypes only: the code is in harmony.c, smartkeys.c, macro.c, arrange.c (after seq.c) and world.c (after the UI;
 * see felucca.c). With wrt.active == 0 every hook takes SLOOP's own path. */
#include "world_fmt.h"
#include "guard_limits.h"

typedef struct {
    volatile uint8_t active;     /* a World drives the tracks (PLAY, ADV_WORLD); 0 = SLOOP. Written with IRQs off,
                                  * together with the first commit */
    uint8_t loaded;              /* world_load succeeded: wctx and wpool hold that World */
    uint8_t keys_trk;            /* the Smart Keys track (KEYS trk): plays the user loop, never a scene pattern */
    uint8_t refcount;            /* Phase 6 (H2): per-pitch voice reference counts on */
    uint8_t keys_on;             /* Phase 6: Smart Keys map the keys */
    volatile uint8_t mute;       /* Phase 11 (H1): tracks the ENERGY band silences, bit per track */
    uint8_t scene, var;          /* the committed scene and variation */
    uint8_t prog, energy, fill;  /* their progression, energy table and fill (pool index), WF_NONE = none */
    uint8_t beat;                /* the BEAT (WF_BEAT_*): which scene drum pattern plays */
    uint8_t cur_pat[NTRK];       /* the pool entry each track's steps came from (written back on a change) */
    uint32_t id;                 /* world_id of the loaded World */
    uint32_t factory_ok;         /* world_boot: bit per factory World that passed wb_check */
} wrt_t;
static wrt_t wrt;

/* an ENERGY table as the stage carries it and the commit makes it sound (arrange.c, design 5.7): the scene's bands,
 * the variation's bias and GUARD's arrangement timing */
typedef struct {
    uint8_t n;                   /* bands, 0 = no table: everything plays */
    int8_t bias;                 /* the variation's energy_bias, 1/250 */
    uint8_t mute_bars;           /* layers change on every 1st or 2nd bar (GUARD mute_change) */
    uint8_t dens_bar;            /* lanes, density and play masks change on: 0 the beat, 1 the bar */
    uint8_t min_bars;            /* bars between two band changes */
    uint8_t phrase;              /* fills: the last bar of every phrase of this many bars */
    uint8_t fill;                /* the scene's fill (a drum pattern of the pool), WF_NONE = none */
    uint16_t dlanes;             /* the lanes the density masks apply to */
    uint16_t from[WF_MAX_BANDS]; /* 0..1000 */
    uint8_t flags[WF_MAX_BANDS]; /* WF_B_LAYERS | WF_B_FILLS | WF_B_RATCHETS */
    uint16_t lanes[WF_MAX_BANDS], dens[WF_MAX_BANDS], play[WF_MAX_BANDS][NPART];
} wetab_t;

/* the stage: the next state, built by the main loop (world_stage), applied by world_commit (the ISR on a
 * boundary, Phase 11; or the main loop with IRQs off while stopped). FREE -> READY (main) -> APPLIED (commit)
 * -> FREE (main); at most one is pending */
enum { WST_FREE, WST_READY, WST_APPLIED };
typedef struct {
    volatile uint8_t st;         /* WST_* */
    uint8_t sw;                  /* the commit switches the World: release everything, clear the keys loop, tempo */
    uint8_t scene, var;
    uint8_t prog, energy, fill;
    uint8_t pat[NTRK];           /* pool entry per track, WF_NONE = an empty pattern (the keys track: its loop stays) */
    uint8_t eng[NTRK], preset[NTRK];
    int16_t bpm;
    int16_t p[NTRK][P_COUNT];
    int16_t g[G_COUNT];          /* only the WF_G_WHITELIST entries are applied */
    wetab_t et;                  /* the scene's ENERGY table (arrange.c) */
} wstage_t;
static wstage_t wst;

/* the pattern pool: the World's patterns decoded (world_load), written back when a track leaves one */
typedef union {
    step_t step[NSTEP];
    dstep_t dstep[NSTEP];
} wpat_t;
static wpat_t wpool[WF_MAX_PAT];

static void world_block(void);   /* seq.c events_block (H15): instead of live_block while a World is active */
static void world_fx_pre(void);  /* fx.c mix_block (H4, after events_block): the macro overlay in (macro.c) */
static void world_fx_post(void); /* fx.c mix_block (H5, after djf_process): the base values back */
static void arr_block(void);     /* seq.c events_block (H16): the ENERGY band (arrange.c) */
static uint32_t arr_dskip(uint32_t idx);                       /* H13: drum lanes the band leaves out at step idx */
static const dstep_t *arr_dstep(const dstep_t *s, uint32_t idx);   /* H14: the fill's step on a fill bar, else s */
static int arr_plays(uint32_t t, uint32_t idx);                /* H14: the band lets synth track t's step idx play */
static void wsession_tick(void); /* project.c autosave_tick (H18): the PLAY session instead of SLOOP's autosave */
static int guard_follows(const track_t *t);    /* H12 (seq.c): the keys loop follows the chord (guard.c) */
static uint32_t guard_loop_note(uint32_t n);   /* H12: a loop note over the sounding chord */

/* the Musical Guardrail Engine's state the earlier files read (guard.c, design 6) */
typedef struct {
    uint8_t unison;              /* GUARD max_unison: the voices of a UNISON part in a World (voice.c trk_nvoice) */
    uint8_t ceil;                /* GUARD cpu ceiling, 1/256 of the audio interrupt's time */
    volatile uint8_t hold;       /* the CPU guard holds: the costly slots at their base, a UNISON part at 2 voices */
} wguard_t;
static wguard_t wg;

/* H2, the pitch reference count (design 4.4), audio ISR only, while wrt.refcount: per synth part and pitch, how
 * many holders sound it (voice.c trk_note_on / trk_note_off), and how many of them the live input started
 * (seq.c input_on / input_off). trk_all_off clears both */
static uint8_t vref[NPART][128], vlive[NPART][128];

/* ---- the macro overlay (macro.c, design 5.4, 5.5): the target table the main loop evaluates and publishes, the
 * audio ISR applies between H4 and H5. Two tables: the ISR applies ovb[ov_live]; the main loop writes the other one,
 * only once the ISR has taken the last publication (ov_seen == ov_pub), then counts ov_pub up */
#define OV_MAX 48                /* slots: one per (track, parameter), global or vmod part */
enum { OV_P, OV_G, OV_VCUT, OV_VSHP };   /* a track parameter, a global, ~bright, ~shape (H3) */
typedef struct {
    int16_t *ptr;                /* OV_P: &trk[t].p[id], OV_G: &song.g[id]; 0 for a vmod offset */
    int32_t tgt;                 /* the offset to reach, Q8 steps of the parameter (vmod: Q8 cutoff / shape) */
    int16_t lo, hi;              /* the effective value's range: the descriptor and guard_limits.h */
    uint8_t kind;                /* OV_* */
    uint8_t cls;                 /* smoothing: 0 fast, 1 medium, 2 slow, 3 stepped (WF_CLASS_NAMES) */
    uint8_t part;                /* OV_VCUT / OV_VSHP: the synth part; else the track (WF_NONE: a global) */
    uint8_t from;                /* the same slot in the table before (its smoothing goes on), WF_NONE = new */
} ov_slot_t;
typedef struct {
    uint8_t n;
    uint8_t snap;                /* 1: the ISR starts each slot at its target (a World loaded: no ramp in) */
    ov_slot_t s[OV_MAX];
} ov_tab_t;
static ov_tab_t ovb[2];
static volatile uint8_t ov_pub, ov_seen;   /* tables published (main), taken (ISR) */
static volatile uint8_t ov_live;           /* the table the ISR applies */
static volatile uint32_t ov_blk;           /* audio blocks with a World active (macro_service's pace) */
static int32_t ov_cur[2][OV_MAX];          /* ISR: each slot's offset now, Q8 (per table: carried by from) */
static int16_t ov_saved[OV_MAX];           /* ISR: the base values while the overlay is in (H4 .. H5) */
static int32_t wvm_cut[NPART], wvm_shape[NPART];   /* H3 (voice.c): ~bright / ~shape offsets of each part, Q8 */

/* ---- ENERGY as arrangement (arrange.c, design 5.7): the committed table, the band, and the masks the sequencer's
 * hooks read (H1 wrt.mute, H13, H14). Audio ISR, except req (the main loop's ENERGY position) */
typedef struct {
    wetab_t et;                  /* the committed table */
    volatile uint16_t req;       /* the ENERGY control, 0..1000 (macro.c; the ISR adds the variation's bias) */
    uint8_t sel;                 /* the band at the position, with hysteresis */
    uint8_t tgt;                 /* the band of the change under way */
    uint8_t mb, db;              /* the band the layers (wrt.mute) and the masks follow now */
    uint8_t bars;                /* bars since the last change began (255: long ago) */
    uint8_t fill_now;            /* this bar plays the scene's fill */
    uint32_t beat;               /* clk_beat when last seen (0xFFFFFFFF: a new clock) */
    uint16_t lanes, dens;        /* band db: the drum lanes allowed, the density steps (on et.dlanes) */
    uint16_t play[NPART];        /* band db: the steps of each synth track that play */
    uint8_t ratchets, fills;     /* band db: drum ratchets allowed; the fill on phrase ends */
} warr_t;
static warr_t arr;
