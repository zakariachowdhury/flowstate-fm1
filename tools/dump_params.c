/* SPDX-License-Identifier: GPL-3.0-only */
/* SLOOP's name tables as JSON, for tools/worldc.py: the track and global parameter descriptors (TP, GP), the
 * engines (ENGINES[]: EDIT labels, ranges, value names, factory preset names and the track parameters each preset
 * starts a World's track from, the engine roles of world_fmt.h),
 * the drum kits and lanes, the scales and the step divisions. Built from the firmware's own sources (as
 * tests/hostsim.c includes them), so a World is compiled against exactly what the firmware has.
 *   cc -O1 -w -Ibuild/gen -Ifirmware/src -o build/host/dump_params tools/dump_params.c -lm
 *   build/host/dump_params > worlds/schema/sloop-params.json
 * The JSON is committed (the build needs no host compiler); tests/run_tests.sh regenerates it and fails when
 * it differs from the committed copy. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define __attribute__(x)
#define memset felucca_memset
#define memcpy felucca_memcpy
#define memcmp felucca_memcmp
#include "felucca_tables.h"
#include "../firmware/src/libc.c"
#undef memset
#undef memcpy
#undef memcmp
static struct { volatile uint32_t notes, buttons; } fm1_in;
static void fm1_irq_off(void) {}
static void fm1_irq_on(void) {}
#include "../firmware/src/core.h"
#include "../firmware/src/engines.c"
#include "../firmware/src/drums.c"
#include "../firmware/src/params.c"
#include "../firmware/src/voice.c"
#include "../firmware/src/slicer.c"
#include "../firmware/src/fx.c"
static void fm1_delay_ms(uint32_t ms) { (void)ms; }
#include "../firmware/src/usb.c"
#include "../firmware/src/midi_uart.c"
#include "../firmware/src/seq.c"
#include "../firmware/src/world_fmt.h"

/* the parameter ids by their enum names (worldc lowercases them: "level", "ld_flt", "rev", ...) */
#define N(x) [P_##x] = #x
static const char *const P_ENUM[P_COUNT] = {
    N(LEVEL), N(ATK), N(DEC), N(SUS), N(REL), N(ED_FLT), N(ED_PIT), N(ED_SHP), N(ED_FX),
    N(LRATE), N(LWAVE), N(LPHASE), N(LFADE), N(LD_PIT), N(LD_FLT), N(LD_SHP), N(LD_AMP),
    N(AMODE), N(ARATE), N(AOCT), N(AGATE), N(ASWING), N(APROB), N(AHOLD), N(AORDER),
    N(ROOT), N(SCALE), N(QUANT), N(TRANS), N(SLEN), N(SDIV), N(SSWING), N(SGATE),
    N(DIST), N(CHOR), N(DLY), N(REV), N(VOICE), N(GLIDE), N(PAN), N(MUTE),
    N(GLMODE), N(PRIO), N(ALLOC), N(DETUNE), N(SLCR), N(SLPAT), N(SLRATE), N(SLDEPTH), N(CHORD),
    N(E0), N(E1), N(E2), N(E3), N(E4), N(E5), N(E6), N(E7)};
#undef N
#define N(x) [G_##x] = #x
static const char *const G_ENUM[G_COUNT] = {
    N(BPM), N(SWING), N(CLOCK), N(TUNE), N(DTIME), N(DFDBK), N(DCOLOR), N(DMIX),
    N(RSIZE), N(RDAMP), N(CRATE), N(CDEPTH), N(MIDI), N(SYNC), N(ROUTE), N(INFO),
    N(SLOT), N(NAME), N(LOAD), N(SAVE), N(ENGSEL), N(ENGGO), N(CLRSEQ), N(INITSND),
    N(DRCH), N(DRLVL), N(DRREV), N(DUST), N(DUCK), N(FILT), N(ROLL), N(NEWPRJ)};
#undef N
static const char *const FMT[] = {"INT", "PCT", "BIPCT", "TIME", "LFOHZ", "CUTOFF", "DB", "SEMI", "ENUM", "BPM",
                                  "NOTE", "ONOFF", "OCT", "STEPS", "SWING", "FILT"};
static const uint8_t ENG_ROLE[][WF_NEROLES] = WF_ENG_ROLE;
_Static_assert(sizeof ENG_ROLE / sizeof ENG_ROLE[0] == NENGINES, "world_fmt.h WF_ENG_ROLE: one row per engine");
_Static_assert(DRUM_LANES == WF_NLANES && NTRK == WF_NTRK && TRK_DRUM == WF_TRK_DRUM, "world_fmt.h matches core.h");
_Static_assert(NSCALES == 16 && sizeof N_DIV / sizeof N_DIV[0] == WF_NDIV, "16 scales, 6 divisions");

static void str(const char *s)                       /* a JSON string (Latin-1 bytes as \u00XX) */
{
    putchar('"');
    for (; s && *s; s++) {
        unsigned c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            printf("\\%c", c);
        else if (c < 32 || c > 126)
            printf("\\u%04x", c);
        else
            putchar((int)c);
    }
    putchar('"');
}

static void lower(const char *s)
{
    char b[16];
    unsigned i;
    for (i = 0; s[i] && i < sizeof b - 1u; i++)
        b[i] = (char)(s[i] >= 'A' && s[i] <= 'Z' ? s[i] - 'A' + 'a' : s[i]);
    b[i] = 0;
    str(b);
}

static void desc(const param_desc_t *d)
{
    int32_t v;
    printf("\"label\": ");
    str(d->label);
    printf(", \"fmt\": \"%s\", \"min\": %d, \"max\": %d, \"def\": %d", d->fmt < sizeof FMT / sizeof FMT[0] ? FMT[d->fmt] : "?",
           d->min, d->max, d->def);
    if (d->fmt == F_ENUM && d->names) {
        printf(", \"names\": [");
        for (v = d->min; v <= d->max; v++) {
            if (v > d->min)
                printf(", ");
            str(d->names[v]);
        }
        printf("]");
    }
    if (d->unit) {
        printf(", \"unit\": ");
        str(d->unit);
    }
}

static void lanes_of(const char *ids)                 /* WF_LANE_IDS: space-separated, DRUM_LANES of them */
{
    char b[16];
    uint32_t l = 0, k;
    while (*ids && l < DRUM_LANES) {
        for (k = 0; ids[k] && ids[k] != ' ' && k < sizeof b - 1u; k++)
            b[k] = ids[k];
        b[k] = 0;
        ids += k;
        while (*ids == ' ')
            ids++;
        printf("    {\"index\": %u, \"id\": ", l);
        str(b);
        printf(", \"name\": ");
        str(LANE_NAME[l]);
        printf(", \"short\": ");
        str(LANE_SHORT[l]);
        printf(", \"note\": %u}%s\n", LANE_NOTE[l], l + 1u < DRUM_LANES ? "," : "");
        l++;
    }
    if (l != DRUM_LANES || *ids) {
        fprintf(stderr, "dump_params: WF_LANE_IDS has %u ids, the drum track %u lanes\n", l, DRUM_LANES);
        exit(1);
    }
}

int main(void)
{
    uint32_t i, k;
    for (i = 0; i < P_COUNT; i++)
        if (!P_ENUM[i]) {
            fprintf(stderr, "dump_params: P id %u has no name\n", i);
            return 1;
        }
    for (i = 0; i < G_COUNT; i++)
        if (!G_ENUM[i]) {
            fprintf(stderr, "dump_params: G id %u has no name\n", i);
            return 1;
        }
    printf("{\n  \"generator\": \"tools/dump_params.c (regenerate: see the top of that file)\",\n");
    printf("  \"limits\": {\"P_COUNT\": %d, \"P_E0\": %d, \"G_COUNT\": %d, \"NENGINES\": %d, \"NTRK\": %d, "
           "\"TRK_DRUM\": %d, \"NSTEP\": %d, \"DRUM_KITS\": %u, \"DRUM_LANES\": %d},\n",
           P_COUNT, P_E0, G_COUNT, NENGINES, NTRK, TRK_DRUM, NSTEP, (unsigned)DRUM_KITS, DRUM_LANES);
    printf("  \"P\": [\n");
    for (i = 0; i < P_COUNT; i++) {
        printf("    {\"id\": %u, \"name\": ", i);
        lower(P_ENUM[i]);
        printf(", ");
        if (i < P_E0)
            desc(&TP[i]);
        else
            printf("\"label\": \"E%u\", \"fmt\": \"ENGINE\", \"min\": -128, \"max\": 127, \"def\": 0", i - P_E0);
        printf("}%s\n", i + 1u < P_COUNT ? "," : "");
    }
    printf("  ],\n  \"G\": [\n");
    for (i = 0; i < G_COUNT; i++) {
        printf("    {\"id\": %u, \"name\": ", i);
        lower(G_ENUM[i]);
        printf(", ");
        desc(&GP[i]);
        printf("}%s\n", i + 1u < G_COUNT ? "," : "");
    }
    printf("  ],\n  \"engines\": [\n");
    for (i = 0; i < NENGINES; i++) {
        const engine_t *e = ENGINES[i];
        printf("    {\"id\": %u, \"name\": ", i);
        str(e->name);
        printf(",\n     \"edit\": [\n");
        for (k = 0; k < 8u; k++) {
            printf("       {\"k\": %u, ", k);
            desc(&e->edit[k]);
            printf("}%s\n", k < 7u ? "," : "");
        }
        printf("     ],\n     \"roles\": {");
        for (k = 0; k < WF_NEROLES; k++) {
            static const char *const R[WF_NEROLES] = {"BRIGHT", "RESO", "DRIVE", "SHAPE", "DETUNE", "AIR", "MOVE", "BODY"};
            printf("%s\"%s\": ", k ? ", " : "", R[k]);
            if (ENG_ROLE[i][k] == WF_NONE)
                printf("null");
            else
                printf("%u", ENG_ROLE[i][k]);
        }
        printf("},\n     \"presets\": [");
        for (k = 0; k < e->npresets; k++) {
            printf("%s", k ? ", " : "");
            if (k && k % 6u == 0)
                printf("\n       ");
            str(e->presets[k].name);
        }
        /* each preset as a World's synth track starts from it (world.c world_stage): the track parameters at
         * their defaults, the EDIT values at the engine's, then preset_fill (worldc's model of the macros) */
        printf("],\n     \"preset_p\": [");
        for (k = 0; k < e->npresets; k++) {
            int16_t pp[P_COUNT];
            uint32_t j;
            for (j = 0; j < P_E0; j++)
                pp[j] = TP[j].def;
            for (j = 0; j < 8u; j++)
                pp[P_E0 + j] = e->edit[j].def;
            preset_fill(pp, e, k);
            printf("%s\n       [", k ? "," : "");
            for (j = 0; j < P_COUNT; j++)
                printf("%s%d", j ? ", " : "", pp[j]);
            printf("]");
        }
        printf("]}%s\n", i + 1u < NENGINES ? "," : "");
    }
    printf("  ],\n  \"drum_kits\": [");
    for (i = 0; i < DRUM_KITS; i++) {
        printf("%s", i ? ", " : "");
        str(DRUM_KIT_NAMES[i]);
    }
    printf("],\n  \"drum_default_kit\": %u,\n  \"lanes\": [\n", (unsigned)DRUM_DEFAULT_KIT);
    lanes_of(WF_LANE_IDS);
    printf("  ],\n  \"scales\": [\n");
    for (i = 0; i < NSCALES; i++) {
        printf("    {\"id\": %u, \"name\": ", i);
        str(N_SCALE[i]);
        printf(", \"mask\": %u}%s\n", SCALE_MASK[i], i + 1u < NSCALES ? "," : "");
    }
    printf("  ],\n  \"divs\": [");
    for (i = 0; i < WF_NDIV; i++) {
        printf("%s", i ? ", " : "");
        str(N_DIV[i]);
    }
    printf("],\n  \"div_den\": [");
    for (i = 0; i < WF_NDIV; i++)
        printf("%s%u", i ? ", " : "", DIV_DEN[i]);
    printf("]\n}\n");
    return 0;
}
