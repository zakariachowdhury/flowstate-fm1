/* SPDX-License-Identifier: GPL-3.0-only */
/* The audio FIFO and the sinks that drain it.
 *
 * FIFO: int16 stereo frames, single producer (the firmware thread) and single consumer (the sink), lock-free
 * (ring.h). The firmware thread keeps it filled to a target; the sink takes what the device asks for.
 *
 * Sinks: the SDL audio callback (the window, or --device; SDL_AUDIODRIVER chooses the driver: coreaudio,
 * disk, dummy), or the null sink, a thread that takes a buffer every buffer's worth of wall time (or as fast
 * as the firmware renders, --fast). Both go through sink_pull, which never touches the firmware: it copies,
 * counts and times. Before the FIFO first reaches its target the sink plays silence and counts nothing
 * (start-up is not an underrun); after that a short FIFO is an underrun, padded with silence.
 *
 * SDL 2 on macOS plays through an AudioQueue with a fixed number of buffers of obtained.samples frames
 * (SDL_coreaudio.m prepare_audioqueue: 2, or 2 * ceil(15 ms / buffer) for buffers under 15 ms), all of them
 * in flight: that queue sits between the callback and the device, and it is part of the latency estimate.
 * The CoreAudio HAL's own share (IO buffer, device and stream latency, safety offset) is read from the
 * default output device when this is built for macOS. */
#include <SDL.h>
#include <math.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <CoreAudio/CoreAudio.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#include "sim.h"
#include "ring.h"

/* ------------------------------------------------ time --- */
double sim_now(void)
{
    static double k;
    if (!k)
        k = 1.0 / (double)SDL_GetPerformanceFrequency();
    return (double)SDL_GetPerformanceCounter() * k;
}
void sim_sleep_us(int us)
{
    struct timespec ts = {us / 1000000, (long)(us % 1000000) * 1000L};
    nanosleep(&ts, NULL);
}
/* the calling thread as an audio thread: on macOS the Mach time-constraint policy CoreAudio's own IO thread
 * has (a default or QoS thread's 2 ms sleep overshoots by 1 ms on average and 7 ms at worst: timer leeway;
 * this one by ~20 us). Elsewhere SDL's high priority. */
void sim_realtime_thread(double period_ms, double compute_ms)
{
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    thread_time_constraint_policy_data_t p;
    double k;
    mach_timebase_info(&tb);
    k = 1e6 * tb.denom / tb.numer;               /* ms -> absolute time units */
    p.period = (uint32_t)(period_ms * k);
    p.computation = (uint32_t)(compute_ms * k);
    p.constraint = (uint32_t)(period_ms * k);
    p.preemptible = 1;
    if (thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p,
                          THREAD_TIME_CONSTRAINT_POLICY_COUNT) != KERN_SUCCESS)
        fprintf(stderr, "flowstate-sim: no real-time policy for this thread: wake-ups may be late\n");
#else
    (void)period_ms, (void)compute_ms;
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif
}

/* ------------------------------------------------ the FIFO --- */
static struct {
    int16_t buf[2u * SIM_FIFO_CAP];
    uint32_t w __attribute__((aligned(CACHE_LINE)));   /* frames pushed (the firmware thread) */
    uint32_t r __attribute__((aligned(CACHE_LINE)));   /* frames taken (the sink) */
    uint32_t target __attribute__((aligned(CACHE_LINE)));
} fifo;

void fifo_reset(void) { fifo.w = fifo.r = 0; }
uint32_t fifo_fill(void) { return AT_LOAD(&fifo.w) - AT_LOAD(&fifo.r); }
void fifo_set_target(uint32_t frames) { AT_STORE(&fifo.target, frames); }
uint32_t fifo_push(const int16_t *src, uint32_t n)
{
    uint32_t w = fifo.w, room = SIM_FIFO_CAP - (w - AT_LOAD(&fifo.r)), i = w & (SIM_FIFO_CAP - 1u), k;
    n = n < room ? n : room;
    k = SIM_FIFO_CAP - i < n ? SIM_FIFO_CAP - i : n;
    memcpy(fifo.buf + 2u * i, src, 4u * k);
    memcpy(fifo.buf, src + 2u * k, 4u * (n - k));
    AT_STORE(&fifo.w, w + n);
    return n;
}
static uint32_t fifo_pull(int16_t *dst, uint32_t n)
{
    uint32_t r = fifo.r, avail = AT_LOAD(&fifo.w) - r, i = r & (SIM_FIFO_CAP - 1u), k;
    n = n < avail ? n : avail;
    k = SIM_FIFO_CAP - i < n ? SIM_FIFO_CAP - i : n;
    memcpy(dst, fifo.buf + 2u * i, 4u * k);
    memcpy(dst + 2u * k, fifo.buf, 4u * (n - k));
    AT_STORE(&fifo.r, r + n);
    return n;
}

/* ------------------------------------------------ the sink side: pulls, metrics, recording --- */
static struct {
    int mute, primed;
    double t_prime, t_last;
    uint64_t consumed, limit;            /* frames played since primed; the null sink's end */
    double rs_n, rs_t, rs_tt, rs_c, rs_tc;   /* least squares of frames played against time: the drift */
    uint64_t underruns_total, missing_total;
    /* the window being measured (1 s of audio) */
    uint32_t calls, req_min, req_max, underruns, missing;
    double fill_sum, fill_min, fill_max, per_sum, per_min, per_max;
    uint32_t per_n;
    uint64_t next_window;
    seqlock_t lock;
    sim_sink_stats_t pub;                /* the last complete window */
    int16_t *rec;                        /* --wav: what the sink played */
    uint32_t rec_n, rec_cap;
} S;

static void window_reset(void)
{
    S.calls = S.underruns = S.missing = S.per_n = 0;
    S.req_min = 0xFFFFFFFFu;
    S.req_max = 0;
    S.fill_sum = S.per_sum = 0;
    S.fill_min = S.per_min = 1e30;
    S.fill_max = S.per_max = 0;
}
static void window_publish(double now)
{
    double el = now - S.t_prime;
    seq_write_begin(&S.lock);
    S.pub.windows++;
    S.pub.calls = S.calls;
    S.pub.req_min = S.req_min;
    S.pub.req_max = S.req_max;
    S.pub.fill_min = S.fill_min;
    S.pub.fill_avg = S.calls ? S.fill_sum / S.calls : 0;
    S.pub.fill_max = S.fill_max;
    S.pub.underruns = S.underruns;
    S.pub.missing = S.missing;
    S.pub.period_avg = S.per_n ? 1e3 * S.per_sum / S.per_n : 0;
    S.pub.period_min = S.per_n ? 1e3 * S.per_min : 0;
    S.pub.period_max = 1e3 * S.per_max;
    S.pub.underruns_total = S.underruns_total;
    S.pub.missing_total = S.missing_total;
    S.pub.consumed = S.consumed;
    {   /* the slope of frames played over time since primed: the pulls' bursts average out */
        double d = S.rs_n * S.rs_tt - S.rs_t * S.rs_t;
        S.pub.drift_ppm = d > 0 && el > 0 ? ((S.rs_n * S.rs_tc - S.rs_t * S.rs_c) / d / HOST_FS - 1.0) * 1e6 : 0;
    }
    seq_write_end(&S.lock);
    window_reset();
}

/* the device wants n frames: the audio thread (SDL callback) or the null sink. Copies, counts, times */
static void sink_pull(int16_t *out, uint32_t n)
{
    double now = sim_now();
    uint32_t fill = fifo_fill(), got;
    if (!S.primed) {                             /* start-up: silence until the FIFO first holds its target */
        if (fill < AT_LOAD(&fifo.target) || fill < n) {
            memset(out, 0, 4u * n);
            return;
        }
        S.primed = 1;
        S.t_prime = S.t_last = now;
        S.next_window = HOST_FS;
        window_reset();
    }
    got = fifo_pull(out, n);
    if (got < n) {
        memset(out + 2u * got, 0, 4u * (n - got));
        S.underruns++;
        S.missing += n - got;
        S.underruns_total++;
        S.missing_total += n - got;
    }
    if (S.rec && S.rec_n < S.rec_cap) {
        uint32_t k = S.rec_cap - S.rec_n < n ? S.rec_cap - S.rec_n : n;
        memcpy(S.rec + 2u * S.rec_n, out, 4u * k);
        S.rec_n += k;
    }
    if (S.mute)
        memset(out, 0, 4u * n);
    S.calls++;
    S.req_min = n < S.req_min ? n : S.req_min;
    S.req_max = n > S.req_max ? n : S.req_max;
    S.fill_sum += fill;
    S.fill_min = fill < S.fill_min ? fill : S.fill_min;
    S.fill_max = fill > S.fill_max ? fill : S.fill_max;
    if (S.calls > 1 || S.pub.windows) {
        double p = now - S.t_last;
        S.per_sum += p;
        S.per_min = p < S.per_min ? p : S.per_min;
        S.per_max = p > S.per_max ? p : S.per_max;
        S.per_n++;
    }
    S.t_last = now;
    {
        double t = now - S.t_prime, c = (double)S.consumed;   /* (frames played before this pull: when it was due) */
        S.rs_n += 1;
        S.rs_t += t;
        S.rs_tt += t * t;
        S.rs_c += c;
        S.rs_tc += t * c;
    }
    AT_STORE_RELAXED(&S.consumed, S.consumed + n);
    if (S.consumed >= S.next_window) {
        S.next_window += HOST_FS;
        window_publish(now);
    }
}

void sink_stats(sim_sink_stats_t *s)
{
    seq_read(&S.lock, s, &S.pub, sizeof *s);
}
int sink_record(uint32_t max_frames)
{
    S.rec = malloc(4u * (size_t)max_frames);
    S.rec_cap = S.rec ? max_frames : 0;
    return S.rec ? 0 : -1;
}

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void put16(FILE *f, uint32_t v) { uint8_t b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }
int sink_write_wav(const char *path)             /* (after sink_close) */
{
    FILE *f = fopen(path, "wb");
    uint32_t i;
    if (!f)
        return -1;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36u + S.rec_n * 4u);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, HOST_FS);
    put32(f, HOST_FS * 4u);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, S.rec_n * 4u);
    for (i = 0; i < 2u * S.rec_n; i++)
        put16(f, (uint16_t)S.rec[i]);
    return fclose(f) ? -1 : 0;
}

/* ------------------------------------------------ the SDL device --- */
static SDL_AudioDeviceID adev;
static void SDLCALL sdl_callback(void *u, Uint8 *stream, int len)
{
    (void)u;
    sink_pull((int16_t *)(void *)stream, (uint32_t)len / 4u);
}

#ifdef __APPLE__
/* the default output device: its rate, and what the HAL adds below the AudioQueue (frames at that rate) */
static UInt32 ca_u32(AudioObjectID o, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = {sel, scope, 0};
    UInt32 v = 0, sz = sizeof v;
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, &v) ? 0 : v;
}
static void coreaudio_info(sim_dev_t *d)
{
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, 0};
    AudioDeviceID dev = 0;
    AudioStreamID st[8];
    Float64 rate = 0;
    UInt32 sz = sizeof dev, out = kAudioDevicePropertyScopeOutput;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &sz, &dev) || !dev)
        return;
    a.mSelector = kAudioDevicePropertyNominalSampleRate;
    sz = sizeof rate;
    if (!AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, &rate))
        d->native_rate = d->hal_rate = (int)rate;
    d->hal_io = (int)ca_u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    d->hal_device = (int)ca_u32(dev, kAudioDevicePropertyLatency, out);
    d->hal_safety = (int)ca_u32(dev, kAudioDevicePropertySafetyOffset, out);
    a.mSelector = kAudioDevicePropertyStreams;
    a.mScope = out;
    sz = sizeof st;
    if (!AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, st) && sz >= sizeof st[0])
        d->hal_stream = (int)ca_u32(st[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    d->hal_frames = d->hal_io + d->hal_device + d->hal_safety + d->hal_stream;
}
#endif

/* ------------------------------------------------ the null sink --- */
static struct {
    SDL_Thread *th;
    int stop, finished, fast, period;
} N;
static int SDLCALL null_thread(void *u)
{
    int16_t *buf = malloc(4u * (size_t)N.period);
    double t0 = sim_now();
    uint64_t sched = 0;                          /* frames the schedule has played */
    (void)u;
    sim_realtime_thread(1000.0 * N.period / HOST_FS, 0.5);   /* (as a device's IO thread) */
    while (buf && !AT_LOAD(&N.stop)) {
        uint32_t n = (uint32_t)N.period;
        if (S.primed && S.limit && S.consumed + n > S.limit)
            n = (uint32_t)(S.limit - S.consumed);
        if (S.primed && S.limit && !n) {
            AT_STORE(&N.finished, 1);
            break;
        }
        if (N.fast) {                            /* as fast as the firmware renders: wait for it, never short */
            while (!AT_LOAD(&N.stop) && fifo_fill() < (S.primed ? n : AT_LOAD(&fifo.target)))
                sched_yield();
        } else {                                 /* a device clocked by the wall: a buffer every buffer's time */
            double due = t0 + (double)(sched += n) / HOST_FS, now = sim_now();
            if (due > now)
                sim_sleep_us((int)((due - now) * 1e6));
        }
        sink_pull(buf, n);
    }
    free(buf);
    return 0;
}

int sink_open(const sim_opts_t *o, sim_dev_t *d)
{
    SDL_AudioSpec want, have;
    memset(d, 0, sizeof *d);
    S.mute = o->mute_output;
    N.period = o->buffer;
    d->freq = HOST_FS;
    d->samples = o->buffer;
    d->queue_frames = o->buffer;
    snprintf(d->driver, sizeof d->driver, "null");
    if (o->headless > 0 && !o->device)
        return 0;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "flowstate-sim: no audio (%s): the null sink plays instead\n", SDL_GetError());
        return 0;
    }
    SDL_zero(want);
    want.freq = HOST_FS;                         /* the firmware's rate; SDL / CoreAudio convert if they must */
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)o->buffer;
    want.callback = sdl_callback;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!adev) {
        fprintf(stderr, "flowstate-sim: cannot open the audio device (%s): the null sink plays instead\n",
                SDL_GetError());
        return 0;
    }
    d->open = 1;
    d->freq = have.freq;
    d->samples = have.samples;
    N.period = have.samples;
    snprintf(d->driver, sizeof d->driver, "%s", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");
    d->queue_frames = have.samples;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    {
        char *name = NULL;
        SDL_AudioSpec ds;
        if (SDL_GetDefaultAudioInfo(&name, &ds, 0) == 0) {
            snprintf(d->name, sizeof d->name, "%s", name ? name : "");
            d->native_rate = ds.freq;
            SDL_free(name);
        }
    }
#endif
    if (!strcmp(d->driver, "coreaudio")) {
        SDL_version v;
        SDL_GetVersion(&v);
        if (v.major == 2 && SDL_VERSIONNUM(v.major, v.minor, v.patch) < SDL_VERSIONNUM(2, 32, 50)) {
            double ms = 1000.0 * have.samples / have.freq;   /* SDL 2's AudioQueue (not sdl2-compat) */
            d->queue_bufs = ms < 15.0 ? 2 * (int)ceil(15.0 / ms) : 2;
            d->queue_frames = (d->queue_bufs - 1) * have.samples;   /* ahead of the one the callback fills */
        }
#ifdef __APPLE__
        coreaudio_info(d);
#endif
    }
    return 0;
}

void sink_start(double seconds, int fast)
{
    S.limit = seconds > 0 ? (uint64_t)llround(seconds * HOST_FS) : 0;
    N.fast = fast;
    if (adev)
        SDL_PauseAudioDevice(adev, 0);
    else
        N.th = SDL_CreateThread(null_thread, "null sink", NULL);
}
int sink_finished(void)
{
    return AT_LOAD(&N.finished) || (adev && S.limit && AT_LOAD(&S.consumed) >= S.limit);
}
void sink_close(void)
{
    if (adev) {
        SDL_CloseAudioDevice(adev);              /* (waits for the callback to return) */
        adev = 0;
    }
    if (N.th) {
        AT_STORE(&N.stop, 1);
        SDL_WaitThread(N.th, NULL);
        N.th = NULL;
    }
    S.pub.consumed = S.consumed;                 /* the totals, to the last pull */
    S.pub.underruns_total = S.underruns_total;
    S.pub.missing_total = S.missing_total;
}
