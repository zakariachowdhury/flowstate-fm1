/* SPDX-License-Identifier: GPL-3.0-only */
/* The PLAY session in flash and the parked SLOOP project (docs/design/play-mode-architecture.md 10.1, 10.4;
 * decisions D12, D13). Main loop.
 *
 *   OBJ_WSESSION (storage.c, 0xF9000 / 0xFA000 in the Flowstate region): the PLAYSTATE (world_rt.h wplay_t): the
 *   mode, the World, scene, variation, the 16 controls, PULSE, BEAT, octave, tempo, whether PLAY was ever pressed;
 *   then the keys loop (play_rec.c): u8 steps (0: none), u8 layers, its steps (10 B each: at most 690 B in all,
 *   under ST_LOW_MAX). Saved as SLOOP's autosave is (project.c): stopped, nothing sounding, 2.5 s without input,
 *   20 s after the last save, only when it changed. The record is built on the main loop's stack (st_save reads
 *   the current copy into st_buf first). Phase 14 appends the overrides and user Worlds.
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
 * Never the project slots (the sections A..D): only an explicit TOOLS > SAVE in ADVANCED writes one.
 *
 * User Worlds (Phase 14; design 10.1, 10.3, D12): MY WORLDS. Slot k is the storage object OBJ_UWORLD0 + k (0xE5000 +
 * 0x2000 k, copies A and B), a whole FWD1 blob with flag USER (world.c world_encode: it needs no factory World) of at
 * most ST_LOW_MAX = 3,584 B, so nothing is programmed past offset 0xF00 of its sector. The factory Worlds are const in
 * the app image: nothing here writes them.
 *   wub[2]          a user World's blob in RAM while it plays (st_buf is every storage call's), and the next one: a
 *                   load reads into the one the World playing does not use (a switch lands on the bar)
 *   wus_scan        the slots' ids and names for CHOOSE WORLD (ui_play.c wus), once; save and delete keep them. A
 *                   slot that fails its CRCs or wb_check is not listed (and not loaded)
 *   wuser_read      slot k into the free buffer, checked; not while a World switch waits for its bar
 *   wuser_save      SAVE (over: into the slot of the user World loaded, with its name) or SAVE AS USER WORLD (the
 *                   first free slot; the name with the next free number: NEON RAIN 2, MIDNIGHT DRI 10). Stopped
 *                   only: an erase stalls the audio for about 50 ms (audit 4.1). The working World is then that one
 *   wuser_delete    DELETE USER WORLD: the slot of the user World loaded erased (both copies); it plays on, unsaved
 * The session refers to a user World by slot + 1 and id (wplay_t uslot, world): a slot emptied or saved over with
 * another World is not found, and the boot falls back to NEON RAIN (ui_play.c pl_find). */

static wplay_t wss;                                /* the session: as read, then the last World's state (RAM) */
static uint32_t wss_hash, wss_ms, wss_checked;
static uint8_t wss_sloop;                          /* (the host: boot SLOOP whatever the session says) */
static uint8_t wpark_dirty;                        /* the parked project is not in flash yet */
static struct {
    int8_t octave, live;
    uint8_t solo, arr, user[NTRK], ok;
} wpark;

static uint32_t wss_world(uint32_t *uslot)
{
    *uslot = wss.world ? wss.uslot : 0u;
    return wss.world ? wss.world : PL_FIRST_WORLD;
}

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
    s->uslot = (uint8_t)(wus_slot(wrt.id) + 1);
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

#define WSS_LOOP sizeof(wplay_t)                   /* the keys loop's record in the session: after the PLAYSTATE */
#define WSS_MAX (WSS_LOOP + 2u + NSTEP * sizeof(step_t))
_Static_assert(WSS_MAX <= ST_LOW_MAX, "the session fits under offset 0xF00 of its sector");
static uint32_t wss_record(uint8_t *b, const wplay_t *s)   /* the session record (the PLAYSTATE, the loop): its size */
{
    const track_t *t = &trk[wrt.keys_trk % NPART];
    uint32_t n = s->mode != WM_SLOOP && wrt.active && prec_has(t) ? trk_len(t) : 0u;
    memcpy(b, s, sizeof *s);
    b[WSS_LOOP] = (uint8_t)n;
    b[WSS_LOOP + 1u] = prec.layers;
    memcpy(b + WSS_LOOP + 2u, t->step, n * sizeof(step_t));
    return WSS_LOOP + 2u + n * (uint32_t)sizeof(step_t);
}
static uint32_t wss_now(void)                      /* the session now: its hash */
{
    uint8_t b[WSS_MAX];
    wplay_t s;
    wplay_capture(&s);
    return proj_hash(b, wss_record(b, &s));
}

static int wsession_save(void)                     /* the session now; 0 = in flash */
{
#if FELUCCA_FLASH
    uint8_t b[WSS_MAX] __attribute__((aligned(4)));
    wplay_t s;
    uint32_t h, n;
    wplay_capture(&s);
    n = wss_record(b, &s);
    h = proj_hash(b, n);
    if (h == wss_hash)
        return 0;
    if (!flash_ok || st_save(OBJ_WSESSION, b, n))
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
    if (wss_now() == wss_hash)
        return wrt.active;
    wsession_save();
    wss_ms = now;
    return 1;
#else
    return wrt.active;
#endif
}

static void wsession_boot(void)
{
    int ok = 0, n = 0;
    memset(&wss, 0, sizeof wss);
#if FELUCCA_FLASH
    if (flash_ok) {
        n = st_load(OBJ_WSESSION, st_buf, ST_LOW_MAX);   /* (the whole record, where st_save built it) */
        memcpy(&wss, st_buf, n < (int)sizeof wss ? (n > 0 ? (uint32_t)n : 0u) : sizeof wss);
        ok = n >= 12 && wss.magic == WP_MAGIC && wss.mode <= WM_ADV;
        if (ok && (uint32_t)n < sizeof wss)        /* (an older, shorter record: the rest at its defaults) */
            memset((uint8_t *)&wss + n, 0, sizeof wss - (uint32_t)n);
        if (!ok)
            memset(&wss, 0, sizeof wss);
        wss_hash = ok ? proj_hash(st_buf, (uint32_t)n) : 0u;
    }
#endif
    if (!ok)
        wss.mode = WM_PLAY;                        /* no session (a first boot, or a damaged one): PLAY MODE */
    if (wss_sloop || wss.mode == WM_SLOOP) {       /* SLOOP, as it booted */
        wss_hash = wss_now();                      /* (nothing to save until the mode changes) */
        return;
    }
    play_boot(&wss, ok);
    if (ok && wrt.active && wrt.id == wss.world && (n = st_load(OBJ_WSESSION, st_buf, ST_LOW_MAX)) >= (int)WSS_LOOP + 2 &&
        (uint32_t)n >= WSS_LOOP + 2u + st_buf[WSS_LOOP] * (uint32_t)sizeof(step_t))   /* (a user World read st_buf) */
        prec_load((const step_t *)(st_buf + WSS_LOOP + 2u), st_buf[WSS_LOOP], st_buf[WSS_LOOP + 1u]);
    wpark_dirty = wpark_dirty && autosave_hash;    /* (a first boot parks the power-on project: nothing to keep) */
}

/* ---- user Worlds (above) */
static uint8_t wub[2][ST_LOW_MAX] __attribute__((aligned(4)));
static uint8_t *wub_free(void) { return wctx.b == wub[0] ? wub[1] : wub[0]; }

static void wus_scan(void)
{
    uint32_t k;
    wb_ctx_t c;
    wus_ok = 1;
    wus_gen++;
    for (k = 0; k < WF_USER_SLOTS; k++) {
        int n = flash_ok ? st_load(OBJ_UWORLD0 + k, st_buf, ST_LOW_MAX) : -1;
        wus[k].id = 0;
        if (n > 0 && wb_check(st_buf, (uint32_t)n, &c) == WE_OK && (st_buf[5] & WF_F_USER)) {
            wus[k].id = wb_u32(st_buf + 8);
            str_cpy(wus[k].name, (const char *)st_buf + c.off[WF_S_META], WF_NAME_LEN);
        }
    }
}

static int wuser_read(uint32_t k, const uint8_t **b, uint32_t *n)
{
    uint8_t *d = wub_free();
    wb_ctx_t c;
    int len;
    if (wreq.sw)
        return WE_BUSY;                                /* (the other buffer may be the World on its way) */
    len = flash_ok && k < WF_USER_SLOTS ? st_load(OBJ_UWORLD0 + k, d, ST_LOW_MAX) : -1;
    if (len <= 0 || wb_check(d, (uint32_t)len, &c) != WE_OK || !(d[5] & WF_F_USER))
        return WE_STATE;
    *b = d;
    *n = (uint32_t)len;
    return WE_OK;
}

static int wuser_save(int over)
{
    char nm[WF_NAME_LEN];
    uint8_t *d = wub_free();
    int k = over ? wus_slot(wrt.id) : -1;
    uint32_t i, num, len, cut;
    if (song.playing || transport_req || wreq.sw || !wrt.loaded)
        return WU_STOP;
    world_capture();                                   /* (the steps playing into the pool) */
    if (k < 0) {
        for (k = 0; k < WF_USER_SLOTS && wus[k].id; k++)
            ;
        if (k == WF_USER_SLOTS)
            return WU_FULL;
        str_cpy(nm, world_name(), sizeof nm);
        cut = str_len(nm);
        while (wus_slot(wrt.id) >= 0 && cut && nm[cut - 1u] >= '0' && nm[cut - 1u] <= '9')
            cut--;                                     /* (a user World's own number goes: NEON RAIN 2 -> 3) */
        if (cut > 1u && cut < str_len(nm) && nm[cut - 1u] == ' ')
            nm[cut - 1u] = 0;
        cut = str_len(nm);
        for (num = 2; ; num++) {                       /* the next number no user World has */
            char dg[4];
            fmt_int(dg, (int32_t)num);
            nm[cut < WF_NAME_LEN - 2u - str_len(dg) ? cut : WF_NAME_LEN - 2u - str_len(dg)] = 0;
            pl_append(nm, sizeof nm, " ");
            pl_append(nm, sizeof nm, dg);
            for (i = 0; i < WF_USER_SLOTS && !(wus[i].id && str_eq(wus[i].name, nm)); i++)
                ;
            if (i == WF_USER_SLOTS)
                break;
        }
    } else {
        str_cpy(nm, wus[k].name, sizeof nm);
    }
    if (!(len = world_encode(d, ST_LOW_MAX, nm, pl_pulse())))
        return WU_BIG;
    if (!flash_ok || st_save(OBJ_UWORLD0 + (uint32_t)k, d, len) || world_adopt(d, len))
        return WU_FAIL;
    wus[k].id = wrt.id;
    str_cpy(wus[k].name, nm, WF_NAME_LEN);
    wus_gen++;
    return WU_OK;
}

static int wuser_delete(void)
{
    int k = wus_slot(wrt.id);
    if (k < 0)
        return WU_NONE;
    if (song.playing || transport_req)
        return WU_STOP;
    if (!flash_ok || st_erase(st_sector(OBJ_UWORLD0 + (uint32_t)k, 0)) || st_erase(st_sector(OBJ_UWORLD0 + (uint32_t)k, 1)))
        return WU_FAIL;
    wus[k].id = 0;
    wus_gen++;
    return WU_OK;
}
