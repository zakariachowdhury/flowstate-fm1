/* SPDX-License-Identifier: GPL-3.0-only */
/* ADVANCED edits and user Worlds on the host (Phase 14: world.c world_capture / world_encode, world_store.c, ui_play.c's
 * SAVE list and MY WORLDS; design 10.2, 10.3): the whole firmware through host/core.c (FELUCCA_WORLD 1, the real UI,
 * flash and audio), the panel driven as a player drives it. Each scenario runs in its own process:
 *   edit      ADVANCED edits (levels, a send, a global, another engine, a step, a length) -> PLAY: the overrides, the
 *             World plays them; a variation and back, a scene and back: the same instrument exactly; an edit made in
 *             another scene keeps the first one; immediate transitions (SAVE + key 5); more than 64 edits
 *   save      SAVE AS USER WORLD through the SAVE list: NEON RAIN 2, then NEON RAIN 3; the writes only in the slots' sectors,
 *             offset 0xF00 left erased, the payload under 3,584 B; the user World loaded; reboot (the flash image): MY
 *             WORLDS lists both, the session's World is the user World (slot + id), it loads into the same state
 *             and renders bit-identically; a damaged slot (storage CRC) and an invalid blob (FWD1) not listed, a
 *             session pointing at one falls back to NEON RAIN
 *   slots     10 slots, the 11th MY WORLDS FULL (nothing written); SAVE over a user World; DELETE (asks first); DELETE
 *             on a factory World; a World over the payload cap: WORLD TOO BIG (nothing written)
 *   reset     RESET WORLD (asks first): the factory World exactly (the state, and a render against a fresh boot's);
 *             a user World: as last saved
 *   leave     LEAVE WORLD with unsaved edits asks first (LEAVE WORLD? NOT SAVED), again leaves; the SLOOP project
 *             parked before PLAY MODE bit-identical; without edits it leaves at once
 *   build/host/userworld_test OUTDIR [scenario] */
#include <stdint.h>
#include <sys/wait.h>
#include "../host/core.c"

static const char *outdir = "build/host";
static int fails;
static void check(int ok, const char *what)
{
    uint32_t n = 0;
    printf("uworld: ");
    for (; *what; what++, n++)
        if ((uint8_t)*what == 0xB7)
            printf("\xc2\xb7");
        else
            putchar(*what);
    printf("%*s %s\n", n < 96 ? (int)(96 - n) : 0, "", ok ? "ok" : "FAIL");
    fails += !ok;
}

static void frame(void)
{
    static int16_t pcm[2 * HOST_BLOCK * HOST_FRAME_BLOCKS];
    host_audio(pcm, HOST_BLOCK * HOST_FRAME_BLOCKS);
    host_ui_frame();
}
static void frames(uint32_t n)
{
    while (n--)
        frame();
}
static void wait_ms(uint32_t ms)
{
    uint32_t t0 = fm1_ms;
    while (fm1_ms - t0 < ms)
        frame();
}
static void until_ms(uint32_t t)                 /* (two processes reach a render with the same blocks behind them) */
{
    while (fm1_ms < t)
        frame();
}
static void press(uint32_t b) { host_button(b, 1); frame(); }
static void release(uint32_t b) { host_button(b, 0); frame(); }
static void tap(uint32_t b) { press(b); release(b); }
static void wait_bar(void)
{
    uint32_t b = clk_beat >> 2;
    while ((clk_beat >> 2) == b && song.playing)
        frame();
    frames(2);
}
static void boot(int device, const char *flash)
{
    host_boot_device(device);
    if (host_boot(flash)) {
        printf("uworld: cannot boot\n");
        exit(1);
    }
    frames(2);
}
static void adv_enter(void)                      /* EDIT held 2 s, then tapped: ADVANCED */
{
    press(B_EDIT);
    wait_ms(2100);
    release(B_EDIT);
    tap(B_EDIT);
    frames(2);
}
static void adv_exit(void)                       /* EDIT held 2 s untouched: PLAY */
{
    press(B_EDIT);
    wait_ms(2100);
    release(B_EDIT);
    frames(2);
}
static void save_row(uint32_t row)               /* the SAVE list, its row, SAVE (twice for RESET and DELETE) */
{
    tap(B_SAVE);                                 /* (the list opens on its first row) */
    host_turn(EN_PRESET, (int32_t)row);
    frame();
    tap(B_SAVE);
    if (row >= 2u)
        tap(B_SAVE);
    frames(2);
}
static int toast(const char *s) { return pl.toast_t && !strcmp(pl.toast, s); }
static int fidx_neon(void) { return world_factory_find(PL_FIRST_WORLD); }
static uint32_t render_hash(uint32_t blocks)
{
    static int16_t pcm[2 * HOST_BLOCK];
    uint32_t h = 2166136261u, i, k;
    host_play();
    for (i = 0; i < blocks; i++) {
        host_audio(pcm, HOST_BLOCK);
        if (i % HOST_FRAME_BLOCKS == 0)
            host_ui_frame();
        for (k = 0; k < 2u * HOST_BLOCK; k++)
            h = (h ^ (uint16_t)pcm[k]) * 16777619u;
    }
    return h;
}

/* the instrument a World makes: every track's parameters, sound and steps, the globals, the World's state */
typedef struct {
    int16_t p[NTRK][P_COUNT];
    uint8_t eng[NTRK], pre[NTRK];
    step_t st[NTRK][NSTEP];
    int16_t g[G_COUNT];
    uint8_t scene, var, beat, prog, loaded;
    uint32_t id;
    uint16_t pos[WF_NCTL];
    uint8_t ovn, ovr[WF_MAX_OVR + NPART][WF_SPAIR], pl[WF_MAX_PAT][2];
    wpat_t pool[WF_MAX_PAT];
} snap_t;
static void snap(snap_t *s)
{
    static const uint8_t G[] = {WF_G_WHITELIST, G_BPM};
    uint32_t t, i;
    memset(s, 0, sizeof *s);
    for (t = 0; t < NTRK; t++) {
        memcpy(s->p[t], trk[t].p, sizeof s->p[t]);
        s->eng[t] = trk[t].eng_req;
        s->pre[t] = trk[t].preset;
        memcpy(s->st[t], trk[t].step, sizeof s->st[t]);
    }
    for (i = 0; i < sizeof G; i++)
        s->g[G[i]] = song.g[G[i]];
    s->scene = wrt.scene;
    s->var = wrt.var;
    s->beat = wrt.beat;
    s->prog = wrt.prog;
    s->loaded = wrt.loaded;
    s->id = wrt.id;
    for (i = 0; i < WF_NCTL; i++)
        s->pos[i] = (uint16_t)macro_pos(i);
    s->ovn = wovr.n;
    memcpy(s->ovr, wovr.r, WF_SPAIR * wovr.n);
    memcpy(s->pl, wpl, sizeof wpl);
    memcpy(s->pool, wpool, sizeof wpool);
}
static int same(const snap_t *a, const snap_t *b, int with_id) /* 1: the same; else prints what differs */
{
    uint32_t t, i;
    int ok = 1;
    for (t = 0; t < NTRK; t++)
        for (i = 0; i < P_COUNT; i++)
            if (a->p[t][i] != b->p[t][i]) {
                printf("uworld:   track %u param %u: %d / %d\n", t, i, a->p[t][i], b->p[t][i]);
                ok = 0;
            }
    if (memcmp(a->eng, b->eng, sizeof a->eng) || memcmp(a->pre, b->pre, sizeof a->pre))
        printf("uworld:   engines / presets differ\n"), ok = 0;
    if (memcmp(a->st, b->st, sizeof a->st))
        printf("uworld:   steps differ\n"), ok = 0;
    if (memcmp(a->g, b->g, sizeof a->g))
        printf("uworld:   globals differ\n"), ok = 0;
    if (a->scene != b->scene || a->var != b->var || a->beat != b->beat || a->prog != b->prog || (with_id && a->id != b->id))
        printf("uworld:   scene / variation / BEAT / World differ\n"), ok = 0;
    if (memcmp(a->pos, b->pos, sizeof a->pos))
        printf("uworld:   control positions differ\n"), ok = 0;
    if (a->ovn != b->ovn || memcmp(a->ovr, b->ovr, sizeof a->ovr) || memcmp(a->pl, b->pl, sizeof a->pl))
        printf("uworld:   overrides / lengths differ\n"), ok = 0;
    if (memcmp(a->pool, b->pool, sizeof a->pool))
        printf("uworld:   pool differs\n"), ok = 0;
    return ok;
}

/* ---- the edits a test makes in ADVANCED (as SLOOP's pages make them: trk[] and song.g directly) */
static uint32_t e2;                              /* track 1's other engine */
static int16_t lv0;                              /* track 0's level as edited */
static void edits(int engine)
{
    uint32_t c = wrt.cur_pat[2];
    lv0 = (int16_t)(trk[0].p[P_LEVEL] > 60 ? trk[0].p[P_LEVEL] - 13 : trk[0].p[P_LEVEL] + 13);
    trk[0].p[P_LEVEL] = lv0;
    trk[0].p[P_REV] = (int16_t)(trk[0].p[P_REV] > 100 ? 20 : trk[0].p[P_REV] + 21);
    song.g[G_DMIX] = (int16_t)(song.g[G_DMIX] == 30 ? 31 : 30);
    trk[2].step[5].time = ST_NOTE;               /* a step written */
    trk[2].step[5].n = 2;
    trk[2].step[5].note[0] = 50;
    trk[2].step[5].note[1] = 57;
    trk[2].step[5].vel = 99;
    trk[2].step[5].lvl = 5;
    trk[TRK_DRUM].dstep[3].on[0] |= 1u << 2;     /* a drum hit */
    if (c < WF_MAX_PAT && trk[2].p[P_SLEN] >= 32)
        trk[2].p[P_SLEN] = 24;                   /* a length */
    if (engine) {
        e2 = (trk[1].engine + 1u) % NENGINES;
        fm1_irq_off();
        trk[1].eng_req = (uint8_t)e2;            /* another engine, its preset 1 */
        preset_fill(trk[1].p, ENGINES[e2], ENGINES[e2]->npresets > 1u ? 1u : 0u);
        trk[1].preset = (uint8_t)(ENGINES[e2]->npresets > 1u ? 1u : 0u);
        fm1_irq_on();
    }
}

/* ---- scenarios */
static void sc_edit(void)
{
    snap_t s1, s2;
    uint32_t i, bar;
    boot(1, NULL);
    check(wrt.mode == WM_PLAY && wrt.id == PL_FIRST_WORLD && wovr.n == 0, "a first boot: PLAY MODE, NEON RAIN, no edits");
    adv_enter();
    check(wrt.mode == WM_ADV, "EDIT held 2 s, tapped: ADVANCED");
    edits(1);
    frames(4);
    adv_exit();
    check(wrt.mode == WM_PLAY && toast("EDITS KEPT \xB7 SAVE TO KEEP"), "EDIT held 2 s: PLAY, EDITS KEPT \xB7 SAVE TO KEEP");
    check(trk[0].p[P_LEVEL] == lv0 && trk[1].eng_req == e2 && world_dirty(), "the World plays the edits; unsaved");
    {
        int snd = 0, lvl = 0, dmix = 0;
        for (i = 0; i < wovr.n; i++) {
            snd |= wovr.r[i][0] == (WF_OVR_SOUND | 1u) && wovr.r[i][1] == e2;
            lvl |= wovr.r[i][0] == 0 && wovr.r[i][1] == P_LEVEL && (int8_t)wovr.r[i][2] == lv0;
            dmix |= wovr.r[i][0] == WF_SCOPE_G && wovr.r[i][1] == G_DMIX;
        }
        printf("uworld: %u overrides:", wovr.n);
        for (i = 0; i < wovr.n; i++)
            printf(" %u.%u=%d", wovr.r[i][0], wovr.r[i][1], (int8_t)wovr.r[i][2]);
        printf(" (e2 %u, track 1 engine %u, DMIX %d)\n", e2, trk[1].eng_req, song.g[G_DMIX]);
        check(snd && lvl && dmix && wovr.n <= WF_MAX_OVR + NPART, "captured: track 1's sound, track 0's LEVEL, DMIX");
    }
    snap(&s1);
    check(world_request(wrt.scene, 2) == 0 && wrt.var == 2 && trk[0].p[P_LEVEL] == lv0 && trk[1].eng_req == e2,
          "another variation (stopped: at once): the edits stay");
    world_request(wrt.scene, 0);
    snap(&s2);
    check(same(&s1, &s2, 1), "ORIGINAL again: the same instrument exactly (parameters, sounds, steps, globals, pool)");
    world_request(1, 0);
    check(wrt.scene == 1 && trk[0].p[P_LEVEL] == lv0 && trk[1].eng_req == e2 && song.g[G_DMIX] == s1.g[G_DMIX],
          "scene B: the edits stay");
    adv_enter();
    trk[2].p[P_PAN] = -20;                       /* an edit in scene B */
    adv_exit();
    world_request(0, 0);
    check(trk[2].p[P_PAN] == -20 && trk[0].p[P_LEVEL] == lv0 && !memcmp(trk[2].step, s1.st[2], sizeof s1.st[2]),
          "scene A again: B's edit and A's (the step, the level) both kept");
    /* playing: a scene on its bar keeps them too */
    host_play();
    frames(10);
    world_request(2, 1);
    wait_bar();
    wait_bar();
    check(wrt.scene == 2 && wrt.var == 1 && trk[0].p[P_LEVEL] == lv0 && trk[2].p[P_PAN] == -20, "playing: C, PULSING on "
          "their bars, the edits stay");
    /* ADVANCED: SAVE + key 5, scenes at once */
    adv_enter();
    play_scene_key(4);
    check(wnow && !strcmp(ui.msg, "SCENES: AT ONCE"), "ADVANCED, SAVE + key 5: SCENES: AT ONCE");
    wait_bar();
    frames(3);
    bar = clk_beat >> 2;
    play_scene_key(1);
    frame();
    check(wrt.scene == 1 && (clk_beat >> 2) == bar, "a scene asked for: there at the next block, not on the bar");
    play_scene_key(4);
    play_scene_key(3);
    frame();
    check(!wnow && wrt.scene == 1, "SAVE + key 5 again: on their bar (D waits)");
    for (i = 0; i < 16u && wrt.scene != 3; i++)
        wait_bar();
    check(wrt.scene == 3 && !(clk_beat & 3u), "D on its bar line");
    adv_exit();
    check(!wnow || wrt.mode == WM_PLAY, "PLAY: transitions on the bar whatever ADVANCED said");
    host_stop();
    wait_ms(3000);
    /* too many edits */
    adv_enter();
    for (i = 0; i < 84u; i++)
        trk[i % 3u].p[P_LEVEL + i / 3u + 1u] ^= 1;   /* (P_ATK .. P_TRANS of the synth tracks: 84 values) */
    adv_exit();
    check(toast("TOO MANY EDITS") && wovr.n == WF_MAX_OVR + NPART, "84 edits more: TOO MANY EDITS (the table full)");
}

static void sc_save(void)
{
    char img[600], ref[600];
    int st;
    pid_t pid;
    snap_t a, b;
    uint32_t ha = 0, hb;
    snprintf(img, sizeof img, "%s/uworld.bin", outdir);
    snprintf(ref, sizeof ref, "%s/uworld-ref.bin", outdir);
    fflush(stdout);
    if (!(pid = fork())) {
        FILE *f;
        uint32_t c0, s, k, ok;
        boot(1, NULL);
        adv_enter();
        edits(0);
        adv_exit();
        {
            static step_t lp[16];                /* a keys loop: two notes in a bar */
            for (k = 0; k < 16u; k++)
                lp[k].time = ST_REST;
            lp[0].time = lp[8].time = ST_NOTE;
            lp[0].n = lp[8].n = 1;
            lp[0].note[0] = 62;
            lp[8].note[0] = 65;
            lp[8].flags = 2u << 2;               /* (micro-timing) */
            prec_load(lp, 16, 1);
        }
        check(prec.st == PR_LOOP && world_dirty(), "edits and a keys loop: unsaved");
        host_nor_lo = 0xFFFFFFFFu;
        host_nor_hi = 0;
        c0 = host_flash_changes();
        save_row(0);
        check(toast("SAVED: NEON RAIN 2") && wus[0].id == wrt.id && !world_dirty() && wus_slot(wrt.id) == 0,
              "SAVE AS USER WORLD: SAVED: NEON RAIN 2, slot 1, the World playing is it");
        check(host_flash_changes() != c0 && host_nor_lo >= 0xE5000u && host_nor_hi < 0xE7000u,
              "the writes: slot 1's sectors only (0xE5000..0xE6FFF): no factory World, nothing else");
        save_row(0);
        check(toast("SAVED: NEON RAIN 3") && wus[1].id == wrt.id, "SAVE AS again: NEON RAIN 3, slot 2");
        for (ok = 1, k = 0; k < 2u; k++)
            for (s = 0; s < 2u; s++) {
                uint32_t o = 0xE5000u + k * 0x2000u + s * 0x1000u, i;
                st_hdr_t h;
                memcpy(&h, host_nor + o, sizeof h);
                for (i = 0xF00u; i < 0x1000u; i++)
                    ok &= host_nor[o + i] == 0xFF;
                if (h.magic == ST_MAGIC)
                    ok &= h.len <= ST_LOW_MAX, printf("uworld: slot %u copy %u: %u B\n", k + 1u, s, h.len);
            }
        check(ok, "both slots: under 3,584 B, offset 0xF00..0xFFF of each sector erased");
        check(host_world_user_load(0) == 0 && wrt.id == wus[0].id && prec.st == PR_LOOP, "NEON RAIN 2 loaded (stopped: at once),"
              " its loop with it");
        snap(&a);
        wait_ms(26000);                          /* (quiet: the session refers to it now) */
        until_ms(60000);
        ha = render_hash(3000);
        host_flash_write(img);
        f = fopen(ref, "wb");
        if (f) {
            fwrite(&a, sizeof a, 1, f);
            fwrite(&ha, 4, 1, f);
            fclose(f);
        }
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
    {
        FILE *f = fopen(ref, "rb");
        if (!f || fread(&a, sizeof a, 1, f) != 1 || fread(&ha, 4, 1, f) != 1)
            check(0, "the reference");
        if (f)
            fclose(f);
    }
    fflush(stdout);
    if (!(pid = fork())) {                       /* the reboot */
        host_world_entry_t e0, e1;
        boot(1, img);
        check(!host_world_user(0, &e0) && !host_world_user(1, &e1) && !strcmp(e0.name, "NEON RAIN 2") &&
              !strcmp(e1.name, "NEON RAIN 3") && !strcmp(e0.category, "MY WORLDS") && host_world_user(2, &e1) < 0,
              "reboot: MY WORLDS lists NEON RAIN 2 and 3");
        check(wrt.mode == WM_PLAY && wrt.id == e0.id && !strcmp(world_name(), "NEON RAIN 2"),
              "reboot: the session's World is NEON RAIN 2 (slot + id)");
        check(pl_nrows() == WORLD_NFACTORY + 3u && pl_row_slot(WORLD_NFACTORY + 1u) == 0 && pl_world_index() == WORLD_NFACTORY + 1u,
              "CHOOSE WORLD: the factory Worlds, MY WORLDS, its two; NEON RAIN 2 the one playing");
        check(host_world_user_load(0) == 0, "NEON RAIN 2 loaded again");
        snap(&b);
        check(same(&a, &b, 1), "after the reboot: the same instrument as before it (parameters, steps, loop, pool, "
              "overrides, positions)");
        until_ms(60000);
        hb = render_hash(3000);
        check(ha == hb, "and it renders bit-identically (3000 blocks)");
        printf("uworld: render %08x / %08x\n", ha, hb);
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
    fflush(stdout);
    if (!(pid = fork())) {                       /* slot 1 damaged (both copies), slot 3 a valid object, not a World */
        static uint8_t junk[200];
        uint32_t s;
        host_boot_device(1);
        if (host_boot(img))
            _exit(1);
        for (s = 0; s < 2u; s++)
            host_nor[0xE5000u + s * 0x1000u + ST_PAYLOAD_OFF + 40u] ^= 0x10;
        memset(junk, 0x46, sizeof junk);
        st_save(OBJ_UWORLD0 + 2u, junk, sizeof junk);
        host_flash_write(img);
        _exit(0);
    }
    waitpid(pid, &st, 0);
    fflush(stdout);
    if (!(pid = fork())) {
        host_world_entry_t e;
        boot(1, img);
        check(wrt.mode == WM_PLAY && wrt.id == PL_FIRST_WORLD && toast("WORLD NOT FOUND"),
              "slot 1 damaged: the session's World not found, NEON RAIN");
        check(host_world_user(0, &e) < 0 && host_world_user(2, &e) < 0 && !host_world_user(1, &e) &&
              pl_nrows() == WORLD_NFACTORY + 2u, "MY WORLDS: the damaged slot and the invalid blob left out");
        check(host_world_user_load(0) < 0 && host_world_user_load(2) < 0 && wrt.id == PL_FIRST_WORLD,
              "loading either: refused, NEON RAIN plays on");
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    fails += !WIFEXITED(st) || WEXITSTATUS(st);
}

static void sc_slots(void)
{
    uint32_t i, c0, p;
    boot(1, NULL);
    for (i = 0; i < WF_USER_SLOTS; i++)
        save_row(0);
    check(!strcmp(wus[9].name, "NEON RAIN 11") && pl_nrows() == WORLD_NFACTORY + 11u, "10 SAVE AS: NEON RAIN 2 .. 11");
    c0 = host_flash_changes();
    save_row(0);
    check(toast("MY WORLDS FULL") && host_flash_changes() == c0, "the 11th: MY WORLDS FULL, nothing written");
    adv_enter();
    trk[0].p[P_LEVEL] = 55;
    adv_exit();
    host_nor_lo = 0xFFFFFFFFu;
    host_nor_hi = 0;
    save_row(1);
    check(toast("SAVED: NEON RAIN 11") && host_nor_lo >= 0xE5000u + 9u * 0x2000u && host_nor_hi < 0xE5000u + 10u * 0x2000u,
          "SAVE on NEON RAIN 11: its slot (10) again, nothing else written");
    host_world_load(fidx_neon());
    check(host_world_user_load(9) == 0 && trk[0].p[P_LEVEL] == 55, "NEON RAIN 11 loaded: the edit saved over it");
    tap(B_SAVE);
    host_turn(EN_PRESET, 3);
    frame();
    tap(B_SAVE);
    check(pl.scr == PS_SAVE && pl.ask == 3u && wus[9].id, "DELETE USER WORLD: SAVE asks again first");
    tap(B_SAVE);
    frames(2);
    check(toast("DELETED") && !wus[9].id && host_nor[0xE5000u + 9u * 0x2000u] == 0xFF &&
          host_nor[0xE6000u + 9u * 0x2000u] == 0xFF && wrt.active, "SAVE again: DELETED, both copies erased, it plays on");
    /* over the cap: every synth pattern 64 steps of 4 notes with all the extras, the keys loop too */
    host_world_load(fidx_neon());
    {
        static step_t dense[NSTEP];
        for (i = 0; i < NSTEP; i++) {
            step_t *x = &dense[i];
            x->time = ST_NOTE, x->n = 4, x->vel = 90, x->lvl = 0x55, x->rat = 1, x->flags = 3u << 2;
            x->note[0] = 40, x->note[1] = 50, x->note[2] = 60, x->note[3] = 70;
        }
        for (p = 0; p < wctx.cnt[WF_S_PATTERNS]; p++)
            if (!(wctx.b[wctx.pat[p]] & WF_PAT_DRUM)) {
                memcpy(wpool[p].step, dense, sizeof dense);
                wpl[p][0] = 64;
            }
        for (p = 0; p < NPART; p++)
            if (p != wrt.keys_trk) {
                memcpy(trk[p].step, dense, sizeof dense);
                trk[p].p[P_SLEN] = 64;
            }
        prec_load(dense, NSTEP, 1);
    }
    c0 = host_flash_changes();
    save_row(0);
    check(toast("WORLD TOO BIG") && host_flash_changes() == c0, "dense patterns (over 3,584 B): WORLD TOO BIG, nothing written");
    host_world_load(fidx_neon());
    save_row(0);
    check(toast("SAVED: NEON RAIN 11") && wus[9].id == wrt.id, "a slot free again: SAVE AS NEON RAIN 11");
    host_world_load(fidx_neon());
    save_row(3);
    check(toast("NOT A USER WORLD") && wus[9].id, "DELETE on a factory World: NOT A USER WORLD");
}

static void sc_reset(void)
{
    snap_t f, r, u;
    boot(1, NULL);
    snap(&f);
    adv_enter();
    edits(1);
    adv_exit();
    check(wovr.n && world_dirty(), "edits (another engine too)");
    tap(B_SAVE);
    host_turn(EN_PRESET, 2);
    frame();
    tap(B_SAVE);
    check(pl.scr == PS_SAVE && pl.ask == 2u && wovr.n, "RESET WORLD: SAVE asks again first");
    host_turn(EN_PRESET, 1);
    frame();
    check(pl.ask == 0, "a knob turned: not armed any more");
    host_turn(EN_PRESET, -1);
    frame();
    tap(B_SAVE);
    tap(B_SAVE);
    frames(8);
    snap(&r);
    check(toast("WORLD RESET") && !wovr.n && !world_dirty() && same(&f, &r, 1),
          "SAVE again: WORLD RESET: the factory World exactly as a fresh boot has it");
    /* a user World: back to its last save */
    adv_enter();
    trk[0].p[P_LEVEL] = 66;
    adv_exit();
    save_row(0);
    snap(&u);
    adv_enter();
    edits(1);
    adv_exit();
    save_row(2);
    frames(8);
    snap(&r);
    check(toast("WORLD RESET") && !strcmp(world_name(), "NEON RAIN 2") && same(&u, &r, 1),
          "a user World, RESET WORLD: as last saved");
}
static uint32_t reset_hash[2];
static void sc_reset_render(int edited)          /* the render after RESET against a fresh boot's (same blocks before) */
{
    char path[600];
    FILE *f;
    uint32_t h;
    boot(1, NULL);
    if (edited) {
        adv_enter();
        edits(0);
        adv_exit();
        save_row(2);
        check(toast("WORLD RESET") && !wovr.n, "edits, then RESET WORLD");
    }
    until_ms(30000);
    h = render_hash(3000);
    snprintf(path, sizeof path, "%s/uworld-reset%d.bin", outdir, edited);
    if ((f = fopen(path, "wb"))) {
        fwrite(&h, 4, 1, f);
        fclose(f);
    }
}
static void reset_compare(void)
{
    char path[600];
    FILE *f;
    int k;
    for (k = 0; k < 2; k++) {
        snprintf(path, sizeof path, "%s/uworld-reset%d.bin", outdir, k);
        reset_hash[k] = (uint32_t)k;
        if ((f = fopen(path, "rb"))) {
            if (fread(&reset_hash[k], 4, 1, f) != 1)
                reset_hash[k] = (uint32_t)k;
            fclose(f);
        }
    }
    printf("uworld: reset render %08x / %08x\n", reset_hash[0], reset_hash[1]);
    check(reset_hash[0] == reset_hash[1], "RESET WORLD after edits renders bit-identically to a fresh boot's NEON RAIN");
}

static void sc_leave(void)
{
    static project_t p0, p1;
    boot(0, NULL);
    if (host_project_load("examples/projects/groove.fun4") < 0) {
        check(0, "examples/projects/groove.fun4");
        return;
    }
    frames(10);
    proj_capture(&p0);
    check(play_from_sloop() == 0 && wrt.mode == WM_PLAY, "SLOOP -> PLAY MODE (the project parked)");
    adv_enter();
    edits(1);
    adv_exit();
    check(trk[0].p[P_LEVEL] == lv0 && wrt.mode == WM_PLAY, "ADVANCED edits, exit: the World plays them");
    press(B_HOME);
    wait_ms(800);
    release(B_HOME);
    tap(B_OCTUP);                                /* LEAVE WORLD (first in PLAY) */
    check(ui.menu && wleave_ask && wrt.mode == WM_PLAY, "LEAVE WORLD with unsaved edits: LEAVE WORLD? NOT SAVED (asks)");
    tap(B_OCTUP);
    frames(4);
    proj_capture(&p1);
    check(wrt.mode == WM_SLOOP && !memcmp(&p0, &p1, sizeof p0), "again: SLOOP, the project bit-identical");
    play_from_sloop();
    press(B_HOME);
    wait_ms(800);
    release(B_HOME);
    tap(B_OCTUP);
    frames(4);
    proj_capture(&p1);
    check(wrt.mode == WM_SLOOP && !wleave_ask && !memcmp(&p0, &p1, sizeof p0), "no edits: LEAVE WORLD at once, the "
          "project bit-identical");
}

static int run(const char *name, void (*fn)(void))
{
    pid_t pid;
    int st;
    fflush(stdout);
    if (!(pid = fork())) {
        fn();
        fflush(stdout);
        _exit(fails != 0);
    }
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("uworld: scenario %s FAILED%s\n", name, WIFEXITED(st) ? "" : " (crashed)");
        return 1;
    }
    return 0;
}
static void sc_reset_render0(void) { sc_reset_render(0); }
static void sc_reset_render1(void) { sc_reset_render(1); }

int main(int argc, char **argv)
{
    int bad = 0;
    const char *only = argc > 2 ? argv[2] : "";
    if (argc > 1)
        outdir = argv[1];
    if (!*only || !strcmp(only, "edit"))
        bad += run("edit", sc_edit);
    if (!*only || !strcmp(only, "save"))
        bad += run("save", sc_save);
    if (!*only || !strcmp(only, "slots"))
        bad += run("slots", sc_slots);
    if (!*only || !strcmp(only, "reset")) {
        bad += run("reset", sc_reset);
        bad += run("reset-fresh", sc_reset_render0);
        bad += run("reset-edited", sc_reset_render1);
        reset_compare();
        bad += fails != 0;
    }
    if (!*only || !strcmp(only, "leave"))
        bad += run("leave", sc_leave);
    printf(bad ? "USER WORLD TEST FAILED\n" : "user world test: all checks passed\n");
    return bad != 0;
}
