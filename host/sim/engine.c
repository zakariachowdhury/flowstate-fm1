/* SPDX-License-Identifier: GPL-3.0-only */
/* The firmware thread: the FM-1's single core. Everything that touches the firmware (host.h) happens here,
 * in the device's lockstep: a main-loop pass (host_ui_frame) every HOST_FRAME_BLOCKS audio blocks
 * (host_audio, HOST_BLOCK frames), as tests/ui_pages_test.c and sloop-render --screen run them. Blocks are
 * rendered one at a time while the FIFO is under its target, so the FIFO, not the pass, sets the latency.
 *
 * Input: commands from the main thread (the panel, the keyboard) arrive through the command ring and take
 * effect before the next block, so the audio interrupt's keyboard_block sees a key within a block. A key or
 * button that went down is held until a main-loop pass has seen it held: a tap shorter than one pass
 * reaches the UI as a press and the next pass as the release (the device's matrix scan sees every press).
 * Script steps run at their exact audio time. When the firmware busy-waits inside a pass (calibration,
 * fm1_delay_ms) or during the boot logo, host_set_yield's callback keeps the audio and the inputs going.
 *
 * Output: each block, lowered 6 dB as the device's DAC gets it (audio.c OUT_SHIFT: Q15 << 7 into 24 bits),
 * goes into the FIFO; after each pass the screen, the LEDs, the state and the timing go out as a snapshot. */
#include <SDL.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "sim.h"
#include "ring.h"

/* ------------------------------------------------ the command ring (main thread -> here) --- */
#define CMDQ 1024u
static struct {
    sim_cmd_t q[CMDQ];
    uint32_t w __attribute__((aligned(CACHE_LINE)));
    uint32_t r __attribute__((aligned(CACHE_LINE)));
} cq;
void engine_send(const sim_cmd_t *c)
{
    uint32_t w = cq.w;
    if (w - AT_LOAD(&cq.r) >= CMDQ)
        return;                                  /* (1024 commands between two blocks: never) */
    cq.q[w % CMDQ] = *c;
    AT_STORE(&cq.w, w + 1u);
}

/* a compiled World file (main thread -> here): one slot, taken before the next block */
static struct {
    uint8_t *b;
    uint32_t n;
    int reload;
    int full;                                    /* (the hand-over: set by the main thread, cleared here) */
} mbox;
void engine_send_blob(uint8_t *b, uint32_t n, int reload)
{
    if (AT_LOAD(&mbox.full)) {                   /* (the last one not taken yet: this one is newer) */
        free(b);
        return;
    }
    mbox.b = b;
    mbox.n = n;
    mbox.reload = reload;
    AT_STORE(&mbox.full, 1);
}

/* ------------------------------------------------ the snapshot: two slots, a seqlock each --- */
static struct {
    seqlock_t lock;
    sim_snap_t s;
} snap[2];
static uint32_t snap_latest, snap_count;
uint32_t engine_snapshot(sim_snap_t *s)
{
    uint32_t n = AT_LOAD(&snap_count), k = AT_LOAD(&snap_latest);
    if (n)
        seq_read(&snap[k].lock, s, &snap[k].s, sizeof *s);
    return n;
}

/* ------------------------------------------------ state (this thread only, unless marked) --- */
#define METER_N 689                              /* blocks: 0.5 s, for the script's rms / peak */
static struct {
    sim_opts_t o;
    sim_step_t *script;
    int nscript, next;
    SDL_Thread *th;
    int quit, done, failures, booted;            /* (quit, done, booted, failures: read by the main thread) */
    uint32_t phys_notes, phys_btns;              /* held by the user (panel, keyboard, scripts) */
    uint32_t rose_notes, rose_btns;              /* went down since the last pass: held until a pass saw them */
    uint32_t cur_notes, cur_btns;                /* what the firmware has been told */
    int master_adc;
    uint64_t blocks;                             /* since the boot (the pass schedule) */
    uint32_t yields, passes, target;
    int64_t sleeps;
    /* meter: the output's last 0.5 s */
    double msq[METER_N];
    int32_t mpk[METER_N];
    double msq_sum;
    uint32_t mi, mn;
    /* the stats window (1 s of audio) and the whole run */
    double half_t, cpu_sum, cpu_max, blk_sum, blk_max, ui_sum, ui_max, pass_last, pi_sum, pi_sq, pi_min, pi_max;
    double w_sq;
    int32_t w_pk;
    uint32_t half_blocks, halves, nblk, nui, npi;
    uint64_t next_window;
    sim_fw_stats_t fw;
    /* --flash: the image goes to its file a second after the firmware last wrote, while stopped */
    uint32_t flash_seen;
    double flash_t, cpu_t;
    int flash_dirty;
} E;

static double thread_cpu(void)                   /* this thread's CPU time, s (not wall time) */
{
    struct timespec ts;
    return clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) ? 0 : ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void idle(void)
{
    E.sleeps++;
    if (E.o.fast)
        sched_yield();
    else
        sim_sleep_us(500);                       /* (the AudioQueue's pulls are >= 4 ms apart) */
}

/* ------------------------------------------------ input --- */
static void apply_inputs(void)
{
    uint32_t n = E.phys_notes | E.rose_notes, b = E.phys_btns | E.rose_btns, d, i;
    for (d = n ^ E.cur_notes, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            host_key(i, (int)(n >> i & 1u));
    for (d = b ^ E.cur_btns, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            host_button(i, (int)(b >> i & 1u));
    E.cur_notes = n;
    E.cur_btns = b;
}

/* ------------------------------------------------ the meter --- */
static void meter(const int16_t *pcm)
{
    double sq = 0;
    int32_t pk = 0;
    uint32_t i;
    for (i = 0; i < 2u * HOST_BLOCK; i++) {
        int32_t v = pcm[i], a = v < 0 ? -v : v;
        sq += (double)v * v;
        pk = a > pk ? a : pk;
    }
    E.msq_sum += sq - E.msq[E.mi];
    E.msq[E.mi] = sq;
    E.mpk[E.mi] = pk;
    E.mi = (E.mi + 1u) % METER_N;
    E.mn += E.mn < METER_N;
    if (!E.mi) {                                 /* (summed again each lap: no rounding drift over hours) */
        uint32_t i;
        for (E.msq_sum = 0, i = 0; i < METER_N; i++)
            E.msq_sum += E.msq[i];
    }
    E.w_sq += sq;
    E.w_pk = pk > E.w_pk ? pk : E.w_pk;
}
static double db(double v) { return v > 0 ? 20.0 * log10(v / 32768.0) : -120.0; }
static double meter_rms(void) { return E.mn ? db(sqrt(fmax(E.msq_sum, 0) / (E.mn * 2.0 * HOST_BLOCK))) : -120.0; }
static double meter_peak(void)
{
    int32_t pk = 0;
    uint32_t i;
    for (i = 0; i < E.mn; i++)
        pk = E.mpk[i] > pk ? E.mpk[i] : pk;
    return db(pk);
}

/* ------------------------------------------------ commands --- */
static double field(int f, int trk, const host_state_t *st)
{
    switch (f) {
    case F_PLAYING: return host_playing();
    case F_SCENE: return st->scene;
    case F_NEXT: return st->scene_next;
    case F_BPM: return st->bpm;
    case F_FILTER: return st->filter;
    case F_MUTE: return st->t[trk & 3].mute;
    case F_LEVEL: return st->t[trk & 3].level;
    case F_RMS: return meter_rms();
    case F_PEAK: return meter_peak();
    case F_TIME: return (double)host_frames() / HOST_FS;
    case F_MASTER: return E.master_adc;
    case F_MACRO: return studio_macro(trk);
    case F_SEL: return st->sel + 1;
    case F_VOICES: return st->voices;
    case F_GATED: return st->gated;
    default: return 0;
    }
}
static void print_state(void)
{
    host_state_t st;
    host_state(&st);
    printf("state: t=%.3f playing=%d scene=%s next=%s beat=%d bpm=%d filter=%d mute=%d%d%d%d level=%d,%d,%d,%d "
           "master=%d rms=%.1f peak=%.1f\n", (double)host_frames() / HOST_FS, host_playing(), cmd_scene_name(st.scene),
           cmd_scene_name(st.scene_next), st.beat, st.bpm, st.filter, st.t[0].mute, st.t[1].mute, st.t[2].mute,
           st.t[3].mute, st.t[0].level, st.t[1].level, st.t[2].level, st.t[3].level, E.master_adc, meter_rms(),
           meter_peak());
    {
        host_world_t w;
        host_world(&w);
        printf("       world=%s browse=%s pending=%s var=%s varnext=%s macros=%d,%d,%d,%d sel=%d voices=%d gated=%d "
               "keys=%s chord=%s\n", studio_name_field(F_WORLD), studio_name_field(F_BROWSE), studio_name_field(F_PENDING),
               studio_name_field(F_VAR), studio_name_field(F_VARNEXT), studio_macro(0), studio_macro(1), studio_macro(2),
               studio_macro(3), st.sel + 1, st.voices, st.gated, w.keys_on ? "smart" : "sloop", w.chord[0] ? w.chord : "-");
    }
    fflush(stdout);
}
static void expect(const sim_cmd_t *c)
{
    static const char *const OPS[6] = {"=", "!=", "<", "<=", ">", ">="};
    host_state_t st;
    double v, want = c->v;
    int ok;
    host_state(&st);
    if (c->a >= F_WORLD) {                       /* a World's name */
        const char *got = studio_name_field(c->a);
        ok = studio_names_eq(got, c->s) == (c->cmp == CMP_EQ);
        printf("expect: t=%.3f %s %s %s: %s (%s)\n", (double)host_frames() / HOST_FS, cmd_field_name(c->a),
               OPS[c->cmp], c->s, ok ? "ok" : "FAIL", got);
        fflush(stdout);
        if (!ok)
            AT_ADD(&E.failures, 1);
        return;
    }
    v = field(c->a, c->rel, &st);
    switch (c->cmp) {
    case CMP_EQ: ok = v == want; break;
    case CMP_NE: ok = v != want; break;
    case CMP_LT: ok = v < want; break;
    case CMP_LE: ok = v <= want; break;
    case CMP_GT: ok = v > want; break;
    default: ok = v >= want; break;
    }
    if (c->a == F_SCENE || c->a == F_NEXT)
        printf("expect: t=%.3f %s %s %s: %s (%s)\n", (double)host_frames() / HOST_FS, cmd_field_name(c->a),
               OPS[c->cmp], cmd_scene_name(c->v), ok ? "ok" : "FAIL", cmd_scene_name((int)v));
    else
        printf("expect: t=%.3f %s%s %s %d: %s (%.1f)\n", (double)host_frames() / HOST_FS, cmd_field_name(c->a),
               c->a == F_MUTE || c->a == F_LEVEL || c->a == F_MACRO ? (const char *[]){"1", "2", "3", "4"}[c->rel & 3] : "",
               OPS[c->cmp], c->v, ok ? "ok" : "FAIL", v);
    fflush(stdout);
    if (!ok)
        AT_ADD(&E.failures, 1);
}

static int is_input(int op) { return op <= OP_RELEASE_ALL; }
static void exec(const sim_cmd_t *c)
{
    host_state_t st;
    switch (c->op) {
    case OP_KEY:
    case OP_KEY_TAP:
        if (c->a < SIM_KEYS) {
            uint32_t m = 1u << c->a;
            if (c->op == OP_KEY_TAP || c->v)
                E.rose_notes |= m;
            if (c->op == OP_KEY)
                E.phys_notes = c->v ? E.phys_notes | m : E.phys_notes & ~m;
        }
        break;
    case OP_BUTTON:
    case OP_BUTTON_TAP:
        if (c->a < HOST_NB) {
            uint32_t m = 1u << c->a;
            if (c->op == OP_BUTTON_TAP || c->v)
                E.rose_btns |= m;
            if (c->op == OP_BUTTON)
                E.phys_btns = c->v ? E.phys_btns | m : E.phys_btns & ~m;
        }
        break;
    case OP_TURN:
        if (c->a == SIM_MASTER) {
            E.master_adc = E.master_adc + c->v * 16;
            E.master_adc = E.master_adc < 0 ? 0 : E.master_adc > 1023 ? 1023 : E.master_adc;
            host_master_pot(E.master_adc);
        } else if (c->a < HOST_NE) {
            host_turn(c->a, c->v);
        }
        break;
    case OP_RELEASE_ALL:
        E.phys_notes = E.phys_btns = 0;
        break;
    case OP_PLAY: host_play(); break;
    case OP_STOP: host_stop(); break;
    case OP_PLAYSTOP:
        if (host_playing())
            host_stop();
        else
            host_play();
        break;
    case OP_SCENE: host_scene(c->a); break;
    case OP_STORE: host_section_store(c->a); break;
    case OP_MUTE:
        host_state(&st);
        host_track_set(c->a, HOST_T_MUTE, c->v < 0 ? !st.t[c->a & 3].mute : c->v);
        break;
    case OP_LEVEL:
        host_state(&st);
        host_track_set(c->a, HOST_T_LEVEL, c->rel ? st.t[c->a & 3].level + c->v : c->v);
        break;
    case OP_GLOBAL: host_global_set(c->a, c->rel ? host_global(c->a) + c->v : c->v); break;
    case OP_MASTER:
        E.master_adc = c->v < 0 ? 0 : c->v > 1023 ? 1023 : c->v;
        host_master_pot(E.master_adc);
        break;
    case OP_MACRO:
    case OP_WORLD:
    case OP_VAR:
    case OP_SELECT: studio_exec(c); break;
    case OP_PRINT: print_state(); break;
    case OP_EXPECT: expect(c); break;
    case OP_QUIT: AT_STORE(&E.done, 1); break;
    default: break;
    }
}
/* the ring's commands; inside a busy wait (or the boot) only the panel's inputs: the rest waits its turn */
static void take_cmds(int inputs_only)
{
    uint32_t r = cq.r, w = AT_LOAD(&cq.w);
    for (; r != w; r++) {
        const sim_cmd_t *c = &cq.q[r % CMDQ];
        if (inputs_only && !is_input(c->op))
            break;
        exec(c);
    }
    AT_STORE(&cq.r, r);
}
static void run_script(void)
{
    uint64_t now = host_frames();
    while (E.next < E.nscript && (uint64_t)llround(E.script[E.next].t * HOST_FS) <= now)
        exec(&E.script[E.next++].c);
}

/* ------------------------------------------------ the snapshot out --- */
static void publish(void)
{
    uint32_t k = (AT_LOAD_RELAXED(&snap_latest) + 1u) & 1u, i;
    sim_snap_t *s = &snap[k].s;
    seq_write_begin(&snap[k].lock);
    s->frames = host_frames();
    s->passes = E.passes;
    s->booted = E.booted;
    s->master_adc = E.master_adc;
    s->fifo_target = (int)E.target;
    memcpy(s->lcd, host_lcd(), sizeof s->lcd);
    for (i = 0; i < HOST_NB; i++)
        s->led_btn[i] = (uint8_t)host_button_led(i);
    for (i = 0; i < SIM_KEYS; i++)
        s->led_key[i] = (uint8_t)host_led(14u + i);
    s->notes = E.cur_notes;
    s->buttons = E.cur_btns;
    if (E.booted) {
        host_state(&s->st);
        studio_fill(&s->studio);
        s->nparams = host_track_params((uint32_t)s->st.sel, s->params, SIM_PARAMS);
        s->nslots = host_macro_slots(s->slots, SIM_SLOTS);
        s->nmaps = host_macro_mappings(s->maps, SIM_MAPS);
        s->nrules = host_macro_rules(s->rules, SIM_RULES);
        host_energy(&s->energy);
    }
    s->fw = E.fw;
    seq_write_end(&snap[k].lock);
    AT_STORE(&snap_latest, k);
    AT_ADD(&snap_count, 1u);
}

/* ------------------------------------------------ timing --- */
static void window_close(void)
{
    sim_fw_stats_t *f = &E.fw;
    double n = (double)(E.nblk ? E.nblk : 1);
    f->windows++;
    f->cpu_avg = E.halves ? E.cpu_sum / E.halves : 0;
    f->cpu_max = E.cpu_max;
    f->block_us = 1e6 * E.blk_sum / n;
    f->block_us_max = 1e6 * E.blk_max;
    f->ui_us = E.nui ? 1e6 * E.ui_sum / E.nui : 0;
    f->ui_us_max = 1e6 * E.ui_max;
    f->pass_ms = E.npi ? 1e3 * E.pi_sum / E.npi : 0;
    f->pass_ms_min = E.npi ? 1e3 * E.pi_min : 0;
    f->pass_ms_max = 1e3 * E.pi_max;
    f->pass_ms_sd = E.npi > 1 ? 1e3 * sqrt(fmax(E.pi_sq / E.npi - (E.pi_sum / E.npi) * (E.pi_sum / E.npi), 0)) : 0;
    f->rms_db = db(sqrt(E.w_sq / (n * 2.0 * HOST_BLOCK)));
    f->peak_db = db(E.w_pk);
    f->cpu_max_all = f->cpu_max > f->cpu_max_all ? f->cpu_max : f->cpu_max_all;
    f->cpu_sum_all += f->cpu_avg;
    E.cpu_sum = E.cpu_max = E.blk_sum = E.blk_max = E.ui_sum = E.ui_max = E.pi_sum = E.pi_sq = E.pi_max = 0;
    E.pi_min = 1e30;
    E.halves = E.nblk = E.nui = E.npi = 0;
    E.w_sq = 0;
    E.w_pk = 0;
}
static void account_block(double dt)
{
    E.blk_sum += dt;
    E.blk_max = dt > E.blk_max ? dt : E.blk_max;
    E.nblk++;
    E.half_t += dt;
    if (++E.half_blocks == SIM_HALF / HOST_BLOCK) {   /* 8 blocks = 5.8 ms of audio, with its passes */
        double cpu = 100.0 * E.half_t / ((double)SIM_HALF / HOST_FS);
        E.cpu_sum += cpu;
        E.cpu_max = cpu > E.cpu_max ? cpu : E.cpu_max;
        E.halves++;
        E.half_t = 0;
        E.half_blocks = 0;
    }
    if (host_frames() >= E.next_window) {
        double c = thread_cpu();
        E.fw.thread_cpu = 100.0 * (c - E.cpu_t) / (E.nblk * (double)HOST_BLOCK / HOST_FS);
        E.cpu_t = c;
        E.next_window += HOST_FS;
        window_close();
    }
}
static void account_pass(double t0, double t1)
{
    double dt = t1 - t0;
    E.ui_sum += dt;
    E.ui_max = dt > E.ui_max ? dt : E.ui_max;
    E.nui++;
    E.half_t += dt;
    if (E.pass_last > 0) {
        double p = t0 - E.pass_last;
        E.pi_sum += p;
        E.pi_sq += p * p;
        E.pi_min = p < E.pi_min ? p : E.pi_min;
        E.pi_max = p > E.pi_max ? p : E.pi_max;
        E.npi++;
    }
    E.pass_last = t0;
}

static void flash_check(double now)
{
    uint32_t c;
    if (!E.o.flash)
        return;
    c = host_flash_changes();
    if (c != E.flash_seen) {
        E.flash_seen = c;
        E.flash_t = now;
        E.flash_dirty = 1;
    } else if (E.flash_dirty && now - E.flash_t > 1.0 && !host_playing()) {
        if (host_flash_write(E.o.flash))
            fprintf(stderr, "flowstate-sim: cannot write %s\n", E.o.flash);
        E.flash_dirty = 0;
    }
}

/* ------------------------------------------------ the lockstep --- */
static void ui_pass(void)
{
    double t0 = sim_now(), t1;
    host_ui_frame();
    t1 = sim_now();
    E.rose_notes = E.rose_btns = 0;              /* a pass has seen them held: they may go up now */
    E.passes++;
    account_pass(t0, t1);
    flash_check(t1);
    publish();
}
static void render_block(void)
{
    int16_t pcm[2 * HOST_BLOCK];
    double t0, t1;
    uint32_t i;
    take_cmds(0);
    if (AT_LOAD(&mbox.full)) {
        studio_blob(mbox.b, mbox.n, mbox.reload);
        AT_STORE(&mbox.full, 0);
    }
    run_script();
    host_world_service();                        /* (the main loop's part of a World switch: before each block) */
    studio_block();
    apply_inputs();
    if (E.blocks % HOST_FRAME_BLOCKS == 0)
        ui_pass();
    t0 = sim_now();
    host_audio(pcm, HOST_BLOCK);
    t1 = sim_now();
    for (i = 0; i < 2u * HOST_BLOCK; i++)
        pcm[i] = (int16_t)(pcm[i] >> 1);         /* the DAC's -6 dB (audio.c OUT_SHIFT) */
    meter(pcm);
    fifo_push(pcm, HOST_BLOCK);
    E.blocks++;
    account_block(t1 - t0);
}

/* the firmware waits (boot logo, fm1_delay_ms, calibration): each block it renders meanwhile */
static void on_yield(const int16_t *pcm, uint32_t frames, void *ctx)
{
    int16_t out[2 * HOST_BLOCK];
    uint32_t i;
    (void)ctx;
    for (i = 0; i < 2u * frames && i < 2u * HOST_BLOCK; i++)
        out[i] = (int16_t)(pcm[i] >> 1);
    while (!AT_LOAD(&E.quit) && fifo_fill() + frames > E.target)
        idle();
    if (!AT_LOAD(&E.quit))
        fifo_push(out, frames);
    take_cmds(1);
    apply_inputs();
    if (++E.yields % HOST_FRAME_BLOCKS == 0) {
        E.rose_notes = E.rose_btns = 0;
        publish();
    }
}

static int SDLCALL fw_thread(void *u)
{
    (void)u;
    sim_realtime_thread(1000.0 * SIM_HALF / HOST_FS, 1.5);   /* wakes within ~20 us of its sleep (audio.c) */
    host_set_yield(on_yield, NULL);
    host_boot_device(!E.o.sloop);                /* as the device boots: the session decides (PLAY MODE first) */
    if (host_boot(E.o.flash)) {
        AT_STORE(&E.failures, E.failures + 1);
        AT_STORE(&E.done, 1);
        return 1;
    }
    E.master_adc = 1023;                         /* (host_boot: the pot fully up, unity) */
    if (E.o.project && host_project_load(E.o.project) < 0) {
        fprintf(stderr, "flowstate-sim: %s: cannot load the project\n", E.o.project);
        AT_STORE(&E.failures, E.failures + 1);
        AT_STORE(&E.done, 1);
        return 1;
    }
    studio_init(&E.o);                           /* the Worlds, the World file, the projects; the first World */
    E.flash_seen = host_flash_changes();
    E.flash_dirty = E.o.flash != NULL;           /* (a new image, or the boot wrote: save it once) */
    E.flash_t = sim_now();
    E.pi_min = 1e30;
    E.next_window = (host_frames() / HOST_FS + 1u) * HOST_FS;   /* windows on whole seconds since power-on */
    E.cpu_t = thread_cpu();
    AT_STORE(&E.booted, 1);
    while (!AT_LOAD(&E.quit)) {
        if (fifo_fill() + HOST_BLOCK > E.target) {
            idle();
            continue;
        }
        render_block();
    }
    return 0;
}

int engine_start(const sim_opts_t *o, sim_step_t *script, int nscript)
{
    memset(&E, 0, sizeof E);
    E.o = *o;
    E.script = script;
    E.nscript = nscript;
    E.target = (uint32_t)o->fifo;
    fifo_set_target(E.target);
    E.th = SDL_CreateThread(fw_thread, "firmware", NULL);
    return E.th ? 0 : -1;
}
void engine_stop(void)
{
    if (!E.th)
        return;
    AT_STORE(&E.quit, 1);
    SDL_WaitThread(E.th, NULL);
    E.th = NULL;
}
int engine_done(void) { return AT_LOAD(&E.done); }
int engine_failures(void) { return AT_LOAD(&E.failures); }
int64_t engine_sleeps(void) { return E.sleeps; }
int engine_finish(const char *flash, const char *screen)   /* (the thread has stopped) */
{
    int r = 0;
    if (flash && AT_LOAD(&E.booted) && host_flash_write(flash)) {
        fprintf(stderr, "flowstate-sim: cannot write %s\n", flash);
        r = -1;
    }
    if (screen && host_lcd_ppm(screen)) {
        fprintf(stderr, "flowstate-sim: cannot write %s\n", screen);
        r = -1;
    }
    return r;
}
