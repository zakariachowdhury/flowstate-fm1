/* SPDX-License-Identifier: GPL-3.0-only */
/* Musical Worlds: the runtime state the main loop and the audio ISR share (design 1.6). Included at the end of
 * core.h when FELUCCA_WORLD is 1, so every consumer of core.h sees it. Types and state only: the code is in
 * world.c (included after the UI, see felucca.c). With wrt.active == 0 every hook takes SLOOP's own path. */
#include "world_fmt.h"

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
} wstage_t;
static wstage_t wst;

/* the pattern pool: the World's patterns decoded (world_load), written back when a track leaves one */
typedef union {
    step_t step[NSTEP];
    dstep_t dstep[NSTEP];
} wpat_t;
static wpat_t wpool[WF_MAX_PAT];

static void world_block(void);   /* seq.c events_block (H15): instead of live_block while a World is active */
static void wsession_tick(void); /* project.c autosave_tick (H18): the PLAY session instead of SLOOP's autosave */

/* H2, the pitch reference count (design 4.4), audio ISR only, while wrt.refcount: per synth part and pitch, how
 * many holders sound it (voice.c trk_note_on / trk_note_off), and how many of them the live input started
 * (seq.c input_on / input_off). trk_all_off clears both */
static uint8_t vref[NPART][128], vlive[NPART][128];
