/* SPDX-License-Identifier: GPL-3.0-only */
/* The Studio's model of what plays (sim.h studio_t), on the firmware thread: what the FLOWSTATE STUDIO view
 * draws and what its controls do. Its entries:
 *
 *   Worlds         the factory Musical Worlds (world.c, sorted by category as the firmware keeps them); the
 *                  default is NEON RAIN, found by its id (UI spec: the first boot)
 *   a World file   --world PATH (.world.json or .wblob): the main thread compiles it, and again whenever the file
 *                  changes (every 500 ms it looks): the World reloads at once, playing or not (design 2.8)
 *   SLOOP projects the projects in examples/projects (or --worlds DIR), as in Phase 4: loading one stores four
 *                  scenes of it in sections A..D (A its pad and keys, B all, C all with louder drums and more
 *                  echo, D all but the drums) and plays B
 *
 * A World's scenes, variations, roles and keys come from the World (host_world); its scenes and variations
 * change on the next bar while playing (world.c world_request). Choosing another entry highlights it while the
 * current one plays on; confirming loads it, at once while stopped, on the next bar while playing (design D10):
 * a World through world_switch (on the bar the transport stops and the new World starts again), a project here.
 * The macros are positions that turn SLOOP's KNOB 1..4 until Phase 7; the keys are SLOOP's until Smart Keys
 * (Phase 6: wrt.keys_on). */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sim.h"

static const char *const SCENE_NAME[4] = {"INTRO", "MAIN", "LIFT", "BREAKDOWN"};   /* (a project's sections) */
#define LIFT_DRUMS 16                            /* C LIFT: drum level + */
#define LIFT_ECHO 24                             /* C LIFT: each synth track's delay send + */
#define NEON_RAIN "0x4eee4454"                   /* the first-boot World (its id: the index is by category) */

static struct {
    int n;
    studio_world_t w[STUDIO_WORLDS];
    int fidx[STUDIO_WORLDS];                     /* SK_WORLD: the factory index */
    char path[STUDIO_WORLDS][512];               /* SK_FILE, SK_PROJECT */
    char role[STUDIO_WORLDS][HOST_NTRK][8];      /* SK_PROJECT: from the sounds' names */
    int world, browse, pending, phase, was_playing, last_beat, want_file;
    uint32_t target;                             /* the World id a switch waits for */
    int macro[4];
    char title[16], msg[64], shown[64];
    uint64_t msg_at;                             /* (a message shows for 4 s of audio) */
} S = {.world = -1, .browse = -1, .pending = -1};

/* ---- names */
int studio_names_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)(*a == '_' ? ' ' : *a)) != toupper((unsigned char)(*b == '_' ? ' ' : *b)))
            return 0;
    return *a == *b;
}
static int has(const char *s, const char *w) { return strstr(s, w) != NULL; }
static void role_of(const host_state_t *st, char role[HOST_NTRK][8])   /* a project's tracks */
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
static void say(const char *m) { snprintf(S.msg, sizeof S.msg, "%s", m); }

/* ---- the entries */
static int add(int kind, const char *name, const char *cat, const char *key, int bpm)
{
    studio_world_t *w;
    if (S.n == STUDIO_WORLDS)
        return -1;
    w = &S.w[S.n];
    snprintf(w->name, sizeof w->name, "%s", name);
    snprintf(w->category, sizeof w->category, "%s", cat);
    snprintf(w->key, sizeof w->key, "%s", key);
    w->bpm = bpm;
    w->kind = kind;
    return S.n++;
}
static void add_projects(const sim_opts_t *o)
{
    char dir[1024], files[STUDIO_WORLDS][256];
    DIR *d;
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
        char path[1300], name[16], key[12];
        int k;
        snprintf(path, sizeof path, "%s/%s", dir, files[i]);
        if (host_project_info(path, &st) < 0)
            continue;
        stem(files[i], name, sizeof name);
        snprintf(key, sizeof key, "%s %s", st.t[0].root, st.t[0].scale);
        if ((k = add(SK_PROJECT, name, st.t[HOST_NTRK - 1].sound, key, st.bpm)) < 0)
            break;
        snprintf(S.path[k], sizeof S.path[k], "%s", path);
        role_of(&st, S.role[k]);
    }
}
static int entry_of_world(uint32_t id)           /* the entry of the World loaded (by id) */
{
    int i;
    host_world_entry_t e;
    for (i = 0; i < S.n; i++)
        if (S.w[i].kind == SK_WORLD && !host_world_factory(S.fidx[i], &e) && e.id == id)
            return i;
    for (i = 0; i < S.n; i++)
        if (S.w[i].kind == SK_FILE)
            return i;
    return -1;
}

/* ---- a SLOOP project as four scenes in sections A..D (Phase 4) */
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
static void project_now(int w)                   /* stopped: the project, its scenes, B */
{
    host_world_unload();                         /* (SLOOP's paths again) */
    if (host_project_load(S.path[w]) < 0) {
        say("CANNOT LOAD THE PROJECT");
        return;
    }
    store_scenes(w);
    host_scene(1);
    S.world = w;
    S.title[0] = 0;
}

/* ---- loading an entry: now while stopped, else on the next bar */
static void load(int w)
{
    host_state_t st;
    int rc;
    S.pending = -1;
    switch (S.w[w].kind) {
    case SK_WORLD:
        rc = host_world_load(S.fidx[w]);
        if (rc < 0) {
            snprintf(S.msg, sizeof S.msg, "WORLD ERROR %s", host_world_error(rc));
        } else if (rc == 0) {
            S.world = w;
        } else {
            S.pending = w;                       /* world.c switches on the bar */
            S.phase = 2;
        }
        break;
    case SK_FILE:
        S.want_file++;                           /* (the main thread compiles it: studio_blob) */
        S.pending = w;
        S.phase = 3;
        break;
    default:
        S.pending = w;
        S.phase = 0;
        host_state(&st);
        S.last_beat = st.beat;
        studio_block();                          /* (stopped: at once) */
        break;
    }
}

void studio_init(const sim_opts_t *o)
{
    int i, k;
    for (i = 0; i < 4; i++)
        S.macro[i] = 50;
    for (i = 0; i < host_world_factory_count(); i++) {
        host_world_entry_t e;
        if (host_world_factory(i, &e) || (k = add(SK_WORLD, e.name, e.category, "", e.bpm)) < 0)
            continue;
        S.fidx[k] = i;
    }
    if (o->world_blob)
        k = add(SK_FILE, "WORLD FILE", "AUTHORING", "", 0), snprintf(S.path[k], sizeof S.path[k], "%s", o->world);
    add_projects(o);
    /* the first World: --world (a name or the file), --project (no World), --sloop (none), else NEON RAIN */
    if (o->world_blob) {
        studio_blob(o->world_blob, o->world_blob_n, 0);
    } else if (o->world) {
        int f = host_world_factory_find(o->world);
        for (i = 0; i < S.n && !(S.w[i].kind == SK_PROJECT && studio_names_eq(S.w[i].name, o->world)); i++)
            ;
        if (f >= 0)
            host_world_load(f);                  /* (stopped: at once) */
        else if (i < S.n)
            project_now(i);
        else
            fprintf(stderr, "flowstate-sim: --world %s: no such World or project\n", o->world);
    } else if (o->project) {
        stem(o->project, S.title, sizeof S.title);
    } else if (!o->sloop) {
        host_world_load(host_world_factory_find(NEON_RAIN));
    }
    {
        host_world_t w;
        host_world(&w);
        if (w.loaded && S.world < 0)
            S.world = entry_of_world(w.id);
    }
}

/* the World file, compiled by the main thread: loaded, or reloaded at once when it is the World playing */
void studio_blob(uint8_t *b, uint32_t n, int reload)
{
    int f, rc;
    for (f = 0; f < S.n && S.w[f].kind != SK_FILE; f++)
        ;
    if (f == S.n) {
        free(b);
        return;
    }
    if (reload && S.world != f) {
        free(b);
        say("THE FILE CHANGED: LOAD IT TO HEAR IT");
        return;
    }
    rc = reload ? host_world_reload_blob(b, n) : host_world_load_blob(b, n);
    if (rc < 0) {
        snprintf(S.msg, sizeof S.msg, "WORLD ERROR %s: THE OLD WORLD PLAYS ON", host_world_error(rc));
        if (S.pending == f)
            S.pending = -1;
        return;
    }
    {
        host_world_t w;
        host_world(&w);
        snprintf(S.w[f].name, sizeof S.w[f].name, "%s", w.name);   /* (now that it is known) */
        snprintf(S.w[f].category, sizeof S.w[f].category, "%s", w.category);
        S.w[f].bpm = w.bpm;
    }
    if (reload) {
        say("RELOADED");
    } else if (rc == 0) {
        S.world = f;
        S.pending = -1;
    } else {
        S.pending = f;
        S.phase = 2;
    }
}

/* before each block: a World switch finished (world.c did it on the bar), a project waiting for its bar */
void studio_block(void)
{
    host_state_t st;
    if (S.pending < 0)
        return;
    if (S.phase == 2) {                          /* a World: world.c stops, loads and starts on the bar */
        host_world_t w;
        host_world(&w);
        if (w.pending != 2) {
            S.world = entry_of_world(w.id);
            S.pending = -1;
        }
        return;
    }
    if (S.phase == 3)                            /* (a World file: waits for the main thread) */
        return;
    if (S.phase == 0) {                          /* a project: on the next bar, as Phase 4 */
        if (host_playing()) {
            host_state(&st);
            if (st.beat % 4 || st.beat == S.last_beat) {
                S.last_beat = st.beat;
                return;
            }
        }
        S.was_playing = host_playing();
        host_world_unload();
        if (host_project_load(S.path[S.pending]) < 0) {   /* (it asks the transport to stop) */
            say("CANNOT LOAD THE PROJECT");
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
    host_world_t w;
    int i;
    switch (c->op) {
    case OP_MACRO:
        if (c->a < 4) {
            int v = c->rel ? S.macro[c->a] + c->v : c->v;
            v = v < 0 ? 0 : v > 100 ? 100 : v;
            if (v != S.macro[c->a])
                host_turn(HOST_EN_K1 + c->a, v - S.macro[c->a]);   /* (until Phase 7: KNOB 1..4) */
            S.macro[c->a] = v;
        }
        break;
    case OP_SELECT:
        host_track_select(c->a);
        break;
    case OP_VAR:                                 /* a World's variation, on the next bar while playing */
        host_world(&w);
        if (!w.active || !w.nvar)
            break;
        i = c->a == WA_STEP ? ((w.pending == 1 ? w.pending_var : w.var) + c->v % w.nvar + w.nvar) % w.nvar :
            c->a == WA_PICK ? c->v : -1;
        if (c->a == WA_NAME)
            for (i = w.nvar - 1; i >= 0 && !studio_names_eq(w.var_name[i], c->s); i--)
                ;
        if (i >= 0 && i < w.nvar && (i = host_world_request(-1, i)) < 0)
            snprintf(S.msg, sizeof S.msg, "WORLD ERROR %s", host_world_error(i));
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
            for (i = 0; i < S.n && !studio_names_eq(S.w[i].name, c->s); i++)
                ;
            if (i < S.n)
                S.browse = i;
            break;
        case WA_CONFIRM:
            if (S.browse >= 0 && (S.browse != S.world || S.w[S.browse].kind == SK_FILE) && S.pending < 0)
                load(S.browse);
            S.browse = -1;
            break;
        default:                                 /* WA_CANCEL: the choice, or a project waiting for its bar */
            S.browse = -1;
            if (S.pending >= 0 && S.phase == 0)
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
    static host_world_t w;
    int e = f == F_WORLD ? S.world : f == F_BROWSE ? S.browse : S.pending;
    if (f == F_VAR || f == F_VARNEXT) {
        host_world(&w);
        e = f == F_VAR ? (w.active ? w.var : -1) : w.pending == 1 && w.pending_var != w.var ? w.pending_var : -1;
        return e >= 0 && e < w.nvar ? w.var_name[e] : "-";
    }
    return e >= 0 && e < S.n ? S.w[e].name : "-";
}
int studio_macro(int k) { return S.macro[k & 3]; }

void studio_fill(studio_t *m)
{
    host_state_t st;
    host_world_t w;
    int k, sel;
    host_state(&st);
    host_world(&w);
    memset(m, 0, sizeof *m);
    m->nworlds = S.n;
    memcpy(m->w, S.w, sizeof m->w);
    m->world = S.world;
    m->browse = S.browse;
    m->pending = S.pending;
    m->want_file = S.want_file;
    m->kind = S.world >= 0 ? S.w[S.world].kind : SK_PROJECT;
    if (strcmp(S.msg, S.shown)) {                /* a new message */
        snprintf(S.shown, sizeof S.shown, "%s", S.msg);
        S.msg_at = host_frames();
    }
    if (S.msg[0] && host_frames() - S.msg_at > 4u * HOST_FS)
        S.msg[0] = S.shown[0] = 0;
    snprintf(m->msg, sizeof m->msg, "%s", S.msg);
    m->scene = st.scene;
    m->scene_next = st.scene_next;
    m->var = m->var_next = -1;
    m->keys_track = -1;
    if (w.active) {                              /* a World */
        snprintf(m->title, sizeof m->title, "%s", w.name);
        snprintf(m->category, sizeof m->category, "%s", w.category);
        snprintf(m->blurb, sizeof m->blurb, "%s", w.blurb);
        for (k = 0; k < 4; k++)
            snprintf(m->scene_name[k], sizeof m->scene_name[k], "%s", w.scene_name[k]);
        m->scenes = 0xF;
        m->nvar = w.nvar;
        m->var = w.var;
        m->var_next = w.pending == 1 && w.pending_var != w.var ? w.pending_var : -1;
        memcpy(m->var_name, w.var_name, sizeof m->var_name);
        memcpy(m->role, w.role, sizeof m->role);
        m->keys_track = w.keys_track;
        m->keys_smart = w.keys_on;
        snprintf(m->chord, sizeof m->chord, "%s", w.chord);
    } else {                                     /* a SLOOP project (or plain SLOOP) */
        snprintf(m->title, sizeof m->title, "%s", S.world >= 0 ? S.w[S.world].name : S.title[0] ? S.title : "SLOOP");
        snprintf(m->category, sizeof m->category, "SLOOP PROJECT");
        if (S.world >= 0 && S.w[S.world].kind == SK_PROJECT) {
            for (k = 0; k < 4; k++)
                snprintf(m->scene_name[k], sizeof m->scene_name[k], "%s", SCENE_NAME[k]);
            memcpy(m->role, S.role[S.world], sizeof m->role);
        } else {
            role_of(&st, m->role);
        }
        m->scenes = st.sections;
    }
    snprintf(m->key, sizeof m->key, "%s %s", st.t[0].root, st.t[0].scale);
    memcpy(m->macro, S.macro, sizeof m->macro);
    m->macro_live = 0;                           /* (Phase 7) */
    for (k = 0; k < HOST_NTRK; k++) {
        snprintf(m->sound[k], sizeof m->sound[k], "%s", st.t[k].sound ? st.t[k].sound : "");
        m->mute[k] = st.t[k].mute;
        m->level[k] = st.t[k].level;
    }
    m->sel = sel = st.sel;
    if (m->keys_smart)
        snprintf(m->keys, sizeof m->keys, "SMART MELODY");
    else if (sel == HOST_NTRK - 1)
        snprintf(m->keys, sizeof m->keys, "DRUM PADS \xB7 %s", st.t[sel].sound);
    else
        snprintf(m->keys, sizeof m->keys, "%s %s \xB7 %s", st.t[sel].root, st.t[sel].scale, st.t[sel].quant);
    m->bpm = st.bpm;
    m->playing = st.playing;
}
