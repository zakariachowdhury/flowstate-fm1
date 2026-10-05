/* SPDX-License-Identifier: GPL-3.0-only */
/* The PLAY session in flash and the parked SLOOP project (docs/design/play-mode-architecture.md 10.1, 10.4;
 * decisions D12, D13). Main loop.
 *
 *   OBJ_WSESSION (storage.c, 0xF9000 / 0xFA000 in the Flowstate region): the PLAYSTATE (world_rt.h wplay_t): the
 *   mode, the World, scene, variation, the 16 controls, PULSE, BEAT, octave, tempo, whether PLAY was ever pressed.
 *   Saved as SLOOP's autosave is (project.c): stopped, nothing sounding, 2.5 s without input, 20 s after the last
 *   save, only when it changed. Phase 10 appends the keys loop, Phase 14 the overrides and user Worlds.
 *
 *   wsession_tick   project.c autosave_tick (H18), every pass: the session when it changed; while a World is active
 *                   SLOOP's autosave waits (the fence: the user's SLOOP project in flash stays as it is)
 *   wsession_boot   main.c felucca_init (H22), after autosave_resume: no session (a first boot) or a PLAY / ADVANCED
 *                   one: that World, stopped (ui_play.c play_boot; a first boot: NEON RAIN, the FIRST screen). A
 *                   SLOOP session: SLOOP as it booted
 *   wpark_save      SLOOP -> a World: the working project into autosave_buf (project.c: SLOOP's autosave is fenced
 *                   meanwhile, so it stays), with what a project_t does not hold (octave, solo, song mode, the
 *                   section playing, user preset marks). Not in flash yet: written first at the next quiet moment
 *   wpark_restore   LEAVE WORLD: it back, exactly (proj_apply, the autosave_resume path)
 * Never the project slots (the sections A..D): only an explicit TOOLS > SAVE in ADVANCED writes one. */

static wplay_t wss;                                /* the session: as read, then the last World's state (RAM) */
static uint32_t wss_hash, wss_ms, wss_checked;
static uint8_t wss_sloop;                          /* (the host: boot SLOOP whatever the session says) */
static uint8_t wpark_dirty;                        /* the parked project is not in flash yet */
static struct {
    int8_t octave, live;
    uint8_t solo, arr, user[NTRK], ok;
} wpark;

static uint32_t wss_world(void) { return wss.world ? wss.world : PL_FIRST_WORLD; }

static void wpark_save(void)
{
    uint32_t k;
    proj_capture(&autosave_buf);
    wpark.octave = song.octave;
    wpark.live = live_sec;
    wpark.solo = song.solo;
    wpark.arr = arrangement_enabled;
    for (k = 0; k < NTRK; k++)
        wpark.user[k] = trk[k].user;
    wpark.ok = 1;
    arrangement_enabled = 0;                       /* (no song mode over a World's scenes, H26) */
    song.solo = 0;
    wpark_dirty = autosave_buf.sum != autosave_hash;
    undo.valid = 0;                                /* (SLOOP's undo holds a SLOOP pattern) */
}

static void wpark_restore(void)
{
    const project_t *p = &autosave_buf;
    uint32_t k;
    if (!wpark.ok || !proj_ok(p))
        return;
    fm1_irq_off();                                 /* the audio ISR must not see half a project */
    proj_apply(p, 1);
    song.sel = (uint8_t)(p->sel < NTRK ? p->sel : 0u);
    song.octave = wpark.octave;
    song.solo = wpark.solo;
    arrangement_enabled = wpark.arr;
    live_sec = wpark.live;
    for (k = 0; k < NTRK; k++)
        trk[k].user = wpark.user[k];
    fm1_irq_on();
    wpark.ok = 0;
    wpark_dirty = 0;                               /* (SLOOP's own autosave keeps it from now on) */
    undo.valid = 0;
    sync_reload = 1;
}

static void wplay_capture(wplay_t *s)              /* the session now (SLOOP: the last World's, in mode SLOOP) */
{
    uint32_t k;
    *s = wss;
    s->magic = WP_MAGIC;
    s->size = (uint16_t)sizeof *s;
    s->mode = wrt.mode;
    if (wrt.mode == WM_SLOOP || !wrt.active)
        return;
    s->first = pl.played;
    s->world = wrt.id;
    s->scene = wrt.scene;
    s->var = wrt.var;
    s->pulse = (uint8_t)pl_pulse();
    s->beat = wrt.beat;
    s->ctl_lo = 0;
    for (k = 0; k < WF_NCTL; k++) {
        s->ctl[k] = (uint8_t)(macro_pos(k) / 4u);
        s->ctl_lo |= (macro_pos(k) & 3u) << (2u * k);
    }
    s->kmode = 0;                                  /* (SMART MELODY: the only KEYS mode yet) */
    s->octave = song.octave;
    s->bpm = (uint16_t)song.g[G_BPM];
    wss = *s;                                      /* (the last World's state: what SLOOP keeps, PLAY MODE goes back to) */
}

static int wsession_save(void)                     /* the session now; 0 = in flash */
{
#if FELUCCA_FLASH
    wplay_t s;
    uint32_t h;
    wplay_capture(&s);
    h = proj_hash(&s, sizeof s);
    if (h == wss_hash)
        return 0;
    if (!flash_ok || st_save(OBJ_WSESSION, &s, sizeof s))
        return -1;
    wss = s;
    wss_hash = h;
    return 0;
#else
    return -1;
#endif
}

static int wsession_tick(void)
{
#if FELUCCA_FLASH
    uint32_t now = fm1_ms;
    if (!flash_ok || song.playing || transport_req || ui.menu || wreq.sw || now - ui_input_ms < AUTOSAVE_IDLE ||
        now - wss_ms < AUTOSAVE_GAP || now - wss_checked < 1000u || !audio_quiet())
        return wrt.active;
    wss_checked = now;
    if (wpark_dirty && wrt.active) {               /* the parked SLOOP project first (it was not in flash yet) */
        if (st_save(OBJ_AUTOSAVE, &autosave_buf, sizeof autosave_buf) == 0) {
            autosave_hash = autosave_buf.sum;
            wpark_dirty = 0;
        }
        wss_ms = now;
        return 1;
    }
    {
        wplay_t s;
        wplay_capture(&s);
        if (proj_hash(&s, sizeof s) == wss_hash)
            return wrt.active;
    }
    wsession_save();
    wss_ms = now;
    return 1;
#else
    return wrt.active;
#endif
}

static void wsession_boot(void)
{
    int ok = 0;
    memset(&wss, 0, sizeof wss);
#if FELUCCA_FLASH
    if (flash_ok) {
        int n = st_load(OBJ_WSESSION, &wss, sizeof wss);
        ok = n >= 12 && wss.magic == WP_MAGIC && wss.mode <= WM_ADV;
        if (ok && (uint32_t)n < sizeof wss)        /* (an older, shorter record: the rest at its defaults) */
            memset((uint8_t *)&wss + n, 0, sizeof wss - (uint32_t)n);
        if (!ok)
            memset(&wss, 0, sizeof wss);
        wss_hash = ok ? proj_hash(&wss, sizeof wss) : 0u;
    }
#endif
    if (!ok)
        wss.mode = WM_PLAY;                        /* no session (a first boot, or a damaged one): PLAY MODE */
    if (wss_sloop || wss.mode == WM_SLOOP) {       /* SLOOP, as it booted */
        wplay_t s;
        wplay_capture(&s);
        wss_hash = proj_hash(&s, sizeof s);        /* (nothing to save until the mode changes) */
        return;
    }
    play_boot(&wss, ok);
    wpark_dirty = wpark_dirty && autosave_hash;    /* (a first boot parks the power-on project: nothing to keep) */
}
