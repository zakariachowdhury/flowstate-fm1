/* SPDX-License-Identifier: GPL-3.0-only */
/* The Studio's model of what plays (sim.h studio_t), on the firmware thread: what the FLOWSTATE STUDIO view
 * draws and what its controls do. Until the Musical Worlds are in (Phase 5) it is filled from SLOOP, and says
 * so (studio_t.standin, keys_smart, macro_live, nvar):
 *
 *   Worlds     stand-ins: the projects in examples/projects (or --worlds DIR), named after their file; the
 *              category is the drum kit's name, the tempo and key the project's
 *   scenes     SLOOP's sections A..D, stored from the World when it loads: A INTRO its harmony tracks (pad,
 *              chords, keys, texture; else the first synth track), B MAIN all four, C LIFT all four with the
 *              drums 16 louder and the synth tracks' echo sends 24 higher, D BREAKDOWN all but the drums. A
 *              World starts on B. A scene changes on the next bar while playing (SAVE + key) and brings its
 *              own mutes, as on the device
 *   roles      from the sound's name: PAD, BASS, LEAD, KEYS (else by the track's place); the drum track DRUMS
 *   variations none yet (Phase 12)
 *   macros     positions 0..100 that turn SLOOP's KNOB 1..4 by the same steps (Phase 7: the World's macros)
 *   keys       SLOOP's keys on the selected track: its key, scale and snap (Phase 6: SMART MELODY)
 *
 * Choosing a World highlights it while the current one plays on; confirming loads it, at once while stopped,
 * on the next bar while playing, and it plays on from there (design D10). From Phase 5 the same functions read
 * the World runtime instead (its list, scene and variation names, roles); the views do not change. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sim.h"

static const char *const SCENE_NAME[4] = {"INTRO", "MAIN", "LIFT", "BREAKDOWN"};
#define LIFT_DRUMS 16                            /* C LIFT: drum level + */
#define LIFT_ECHO 24                             /* C LIFT: each synth track's delay send + */

static struct {
    int n;
    char path[STUDIO_WORLDS][512];
    studio_world_t w[STUDIO_WORLDS];
    char role[STUDIO_WORLDS][HOST_NTRK][8];
    int world, browse, pending, phase, was_playing, last_beat;
    int macro[4];
    char title[16];                              /* a project that is no World */
} S = {.world = -1, .browse = -1, .pending = -1};

/* ---- roles, names */
static int has(const char *s, const char *w) { return strstr(s, w) != NULL; }
static void role_of(const host_state_t *st, char role[HOST_NTRK][8])
{
    static const char *const BY_PLACE[3] = {"PAD", "BASS", "LEAD"};
    int k;
    for (k = 0; k < HOST_NTRK; k++) {
        const char *s = st->t[k].sound ? st->t[k].sound : "";
        const char *r = k == HOST_NTRK - 1 ? "DRUMS" :
                        has(s, "PAD") || has(s, "STR") || has(s, "ATMOS") || has(s, "CHOIR") ? "PAD" :
                        has(s, "BASS") || has(s, "SUB") ? "BASS" :
                        has(s, "LEAD") || has(s, " LD") || has(s, "FLUTE") || has(s, "SOLO") ? "LEAD" :
                        has(s, "KEYS") || has(s, "PIANO") || has(s, "EP") || has(s, "VIBES") || has(s, "BELL") ||
                        has(s, "ORGAN") ? "KEYS" : BY_PLACE[k];
        snprintf(role[k], 8, "%s", r);
    }
}
static int harmony(const char *role) { return !strcmp(role, "PAD") || !strcmp(role, "KEYS"); }
static void stem(const char *path, char *out, int n)   /* "x/groove.fun4" -> "GROOVE" */
{
    const char *b = strrchr(path, '/');
    int i;
    b = b ? b + 1 : path;
    for (i = 0; i < n - 1 && b[i] && b[i] != '.'; i++)
        out[i] = (char)toupper((unsigned char)b[i]);
    out[i] = 0;
}
static int by_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* ---- the stand-in Worlds */
void studio_init(const sim_opts_t *o)
{
    char dir[1024], files[STUDIO_WORLDS][512];
    DIR *d = NULL;
    struct dirent *e;
    int i, n = 0;
    if (o->worlds) {
        snprintf(dir, sizeof dir, "%s", o->worlds);
        d = opendir(dir);
    } else {                                     /* next to build/host-bin, else from here */
        snprintf(dir, sizeof dir, "%s../../examples/projects", o->base ? o->base : "");
        if (!(d = opendir(dir)))
            d = opendir(strcpy(dir, "examples/projects"));
    }
    for (i = 0; i < 4; i++)
        S.macro[i] = 50;
    if (!d) {
        if (o->worlds)
            fprintf(stderr, "flowstate-sim: --worlds %s: cannot read the directory\n", o->worlds);
        return;
    }
    while ((e = readdir(d)) && n < STUDIO_WORLDS) {
        size_t l = strlen(e->d_name);
        if (l > 5 && !strcmp(e->d_name + l - 5, ".fun4"))
            snprintf(files[n++], sizeof files[0], "%s", e->d_name);
    }
    closedir(d);
    qsort(files, (size_t)n, sizeof files[0], by_name);
    for (i = 0; i < n; i++) {
        host_state_t st;
        studio_world_t *w = &S.w[S.n];
        snprintf(S.path[S.n], sizeof S.path[0], "%s/%s", dir, files[i]);
        if (host_project_info(S.path[S.n], &st) < 0)
            continue;
        stem(files[i], w->name, sizeof w->name);
        snprintf(w->category, sizeof w->category, "%s", st.t[HOST_NTRK - 1].sound);
        snprintf(w->key, sizeof w->key, "%s %s", st.t[0].root, st.t[0].scale);
        w->bpm = st.bpm;
        role_of(&st, S.role[S.n]);
        S.n++;
    }
}
static int find(const char *name)
{
    int i;
    for (i = 0; i < S.n; i++)
        if (!strcasecmp(S.w[i].name, name))
            return i;
    return -1;
}

/* the World just loaded (the working project) -> its four scenes in sections A..D */
static void store_scenes(int w)
{
    host_state_t st;
    int s, k, any = 0;
    host_state(&st);
    for (k = 0; k < HOST_NTRK - 1; k++)
        any |= harmony(S.role[w][k]);
    for (s = 0; s < 4; s++) {
        for (k = 0; k < HOST_NTRK; k++) {
            int drums = k == HOST_NTRK - 1, m = st.t[k].mute;
            if (s == 0)
                m = m || drums || (any ? !harmony(S.role[w][k]) : k != 0);
            else if (s == 3)
                m = m || drums;
            host_track_set((uint32_t)k, HOST_T_MUTE, m);
            host_track_set((uint32_t)k, HOST_T_LEVEL, st.t[k].level + (s == 2 && drums ? LIFT_DRUMS : 0));
            if (!drums)
                host_track_set((uint32_t)k, HOST_T_DLY, st.t[k].dly + (s == 2 ? LIFT_ECHO : 0));
        }
        host_section_store((uint32_t)s);
    }
}
static int load_now(int w)                       /* stopped: the project, its scenes, B; 0 = ok */
{
    if (host_project_load(S.path[w]) < 0)
        return -1;
    store_scenes(w);
    host_scene(1);                               /* (stopped: B is the working project) */
    S.world = w;
    S.title[0] = 0;
    return 0;
}
int studio_load(const char *name)
{
    int w = find(name);
    if (w < 0) {
        fprintf(stderr, "flowstate-sim: no World '%s' (", name);
        for (w = 0; w < S.n; w++)
            fprintf(stderr, "%s%s", w ? ", " : "", S.w[w].name);
        fprintf(stderr, ")\n");
        return -1;
    }
    return load_now(w);
}
void studio_project_loaded(const char *path)     /* a SLOOP project, no World */
{
    S.world = -1;
    stem(path, S.title, sizeof S.title);
}

/* before each block: a World confirmed while playing loads on the next bar, then plays on */
void studio_block(void)
{
    host_state_t st;
    if (S.pending < 0)
        return;
    if (S.phase == 0) {
        if (host_playing()) {
            host_state(&st);
            if (st.beat % 4 || st.beat == S.last_beat) {
                S.last_beat = st.beat;
                return;
            }
        }
        S.was_playing = host_playing();
        if (host_project_load(S.path[S.pending]) < 0) {   /* (it asks the transport to stop) */
            fprintf(stderr, "flowstate-sim: %s: cannot load\n", S.path[S.pending]);
            S.pending = -1;
            return;
        }
        S.phase = 1;
    }
    if (host_playing())                          /* the audio interrupt stops it in the next block */
        return;
    store_scenes(S.pending);
    host_scene(1);
    if (S.was_playing)
        host_play();
    S.world = S.pending;
    S.title[0] = 0;
    S.pending = -1;
    S.phase = 0;
}

void studio_exec(const sim_cmd_t *c)
{
    host_state_t st;
    switch (c->op) {
    case OP_MACRO:
        if (c->a < 4) {
            int v = c->rel ? S.macro[c->a] + c->v : c->v;
            v = v < 0 ? 0 : v > 100 ? 100 : v;
            if (v != S.macro[c->a])
                host_turn(HOST_EN_K1 + c->a, v - S.macro[c->a]);   /* (stand-in: KNOB 1..4) */
            S.macro[c->a] = v;
        }
        break;
    case OP_SELECT:
        host_track_select(c->a);
        break;
    case OP_WORLD:
        if (!S.n)
            break;
        switch (c->a) {
        case WA_STEP:
            S.browse = ((S.browse >= 0 ? S.browse : S.world >= 0 ? S.world : 0) + c->v % S.n + S.n) % S.n;
            break;
        case WA_PICK:
            if (c->v >= 0 && c->v < S.n)
                S.browse = c->v;
            break;
        case WA_NAME:
            if (find(c->s) >= 0)
                S.browse = find(c->s);
            break;
        case WA_CONFIRM:
            if (S.browse >= 0 && S.browse != S.world && S.pending < 0) {
                S.pending = S.browse;
                S.phase = 0;
                host_state(&st);
                S.last_beat = st.beat;
            }
            S.browse = -1;
            break;
        default:                                 /* WA_CANCEL: the choice, or a World waiting for its bar */
            S.browse = -1;
            if (S.phase == 0)
                S.pending = -1;
            break;
        }
        break;
    default:
        break;
    }
}

const char *studio_name_field(int f)
{
    int w = f == F_WORLD ? S.world : f == F_BROWSE ? S.browse : S.pending;
    return w >= 0 && w < S.n ? S.w[w].name : "-";
}
int studio_macro(int k) { return S.macro[k & 3]; }

void studio_fill(studio_t *m)
{
    host_state_t st;
    int k;
    host_state(&st);
    memset(m, 0, sizeof *m);
    m->nworlds = S.n;
    memcpy(m->w, S.w, sizeof m->w);
    m->world = S.world;
    m->browse = S.browse;
    m->pending = S.pending;
    m->standin = 1;
    if (S.world >= 0) {
        snprintf(m->title, sizeof m->title, "%s", S.w[S.world].name);
        snprintf(m->category, sizeof m->category, "%s", S.w[S.world].category);
        for (k = 0; k < 4; k++)
            snprintf(m->scene_name[k], sizeof m->scene_name[k], "%s", SCENE_NAME[k]);
        memcpy(m->role, S.role[S.world], sizeof m->role);
    } else {
        snprintf(m->title, sizeof m->title, "%s", S.title[0] ? S.title : "SLOOP");
        snprintf(m->category, sizeof m->category, "PROJECT");
        role_of(&st, m->role);
    }
    snprintf(m->key, sizeof m->key, "%s %s", st.t[0].root, st.t[0].scale);
    m->scenes = st.sections;
    m->scene = st.scene;
    m->scene_next = st.scene_next;
    m->nvar = 0;                                 /* (Phase 12) */
    memcpy(m->macro, S.macro, sizeof m->macro);
    m->macro_live = 0;                           /* (Phase 7) */
    for (k = 0; k < HOST_NTRK; k++) {
        snprintf(m->sound[k], sizeof m->sound[k], "%s", st.t[k].sound ? st.t[k].sound : "");
        m->mute[k] = st.t[k].mute;
        m->level[k] = st.t[k].level;
    }
    m->sel = st.sel;
    m->keys_smart = 0;                           /* (Phase 6) */
    if (st.sel == HOST_NTRK - 1)
        snprintf(m->keys, sizeof m->keys, "DRUM PADS \xB7 %s", st.t[st.sel].sound);
    else
        snprintf(m->keys, sizeof m->keys, "%s %s \xB7 %s", st.t[st.sel].root, st.t[st.sel].scale, st.t[st.sel].quant);
    m->bpm = st.bpm;
    m->playing = st.playing;
}
