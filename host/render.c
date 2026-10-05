/* SPDX-License-Identifier: GPL-3.0-only */
/* sloop-render: a SLOOP project -> WAV through the real firmware (host/core.c). The project is loaded as
 * the FM-1 loads one, PLAY goes through the transport, the sequencer, engines, FX and mixer render at the
 * project's tempo; then STOP and the release tail. 44.1 kHz stereo 16-bit: the raw mix (Q15, master at
 * unity, as the host tests render; the device's DAC plays it 6 dB lower).
 *
 *   sloop-render [options] INPUT.fun4 OUT.wav
 *   sloop-render [options] --song A.fun4,B.fun4[,C.fun4[,D.fun4]] [--order A:4,B:8,..] [INPUT.fun4] OUT.wav
 *   sloop-render [options] --world NAME|FILE [--scene A..D] [--var NAME] [--world-sequence] OUT.wav
 *
 *   --bars N       play N bars of 4/4 (default: until every track's pattern has played twice)
 *   --seconds S    play S seconds instead
 *   --tail S       after STOP, S seconds of release and effect tails (default 4)
 *   --song LIST    up to four projects as the sections A..D, played in song mode (the arranger). The
 *                  tempo and the global FX are the working project's (as on the device): INPUT, else the
 *                  first section of the order, loaded as SAVE + its key loads one while stopped
 *   --order LIST   the song: section:bars, .. (A..D or 1..4, 1..64 bars; default: each section once, until
 *                  its patterns have played twice). --bars / --seconds cut it short
 *   --analyze      peak, RMS, DC offset, full-scale samples, longest silence, a hash of the audio
 *   --check        --analyze, and exit 1 if the audio is silent, peaks at or above -0.1 dBFS, reaches
 *                  full scale or has a DC offset
 *   --dump         the project: tempo, swing, and per track engine, preset, pattern, mix
 *   --screen PPM   also run the main loop (UI, autosave) between the audio, a pass every 22 blocks as on
 *                  the device, and write the screen at the end of play (240 x 240 PPM). The audio is the same
 *   --world W      a Musical World (world.c) instead of a project: a factory World by name or id ("NEON RAIN",
 *                  0x4eee4454), a .wblob, or a .world.json (compiled with tools/worldc.py); its default scene and
 *                  variation unless --scene / --var say otherwise (default 8 bars)
 *   --scene A..D, --var NAME|N   the World's scene and variation (NAME as the World calls it, N from 1)
 *   --world-sequence  scenes A, B, C, D, --bars each (default 4), changed while playing on the bar, as a
 *                  player asks for them (world_request: the commit lands on the next bar)
 *   --ctl LIST     the World's macros (macro.c): COLOR=0.2,ENERGY=0.9 (0..1, or 0..100 above 1; home 0.5); any
 *                  of COLOR MOTION SPACE ENERGY and the later controls (SOFT .. FREEZE). Set before PLAY, at once
 *   --sweep CTL    CTL from 0 to 1 over the bars played (as a hand would turn it: smoothed), the others as --ctl
 *                  or the World's defaults
 * The same input always gives the same bytes. Any format SLOOP reads works (FUN1..3 are converted). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "host.h"

static void usage(void)
{
    fprintf(stderr, "usage: sloop-render [--bars N | --seconds S] [--tail S] [--analyze | --check] [--dump]\n"
                    "                    [--screen OUT.ppm] INPUT.fun4 OUT.wav\n"
                    "       sloop-render [options] --song A.fun4,B.fun4,.. [--order A:4,B:8,..] [INPUT.fun4] OUT.wav\n"
                    "       sloop-render [options] --world NAME|FILE [--scene A..D] [--var NAME] [--world-sequence]\n"
                    "                    [--ctl COLOR=0.2,ENERGY=0.9] [--sweep CTL] OUT.wav\n");
    exit(2);
}

/* ---- the audio, in memory */
static int16_t *pcm;
static size_t pcm_n, pcm_cap;                    /* frames */
static int ui_on;                                /* --screen: the main loop runs too */
static uint32_t ui_blocks;
static void render(uint32_t frames)              /* whole blocks; with the main loop: a pass every 22 of them */
{
    uint32_t f;
    for (f = 0; f < frames; f += HOST_BLOCK) {
        if (pcm_n + HOST_BLOCK > pcm_cap) {
            pcm_cap = pcm_cap ? 2 * pcm_cap : 1u << 20;
            if (!(pcm = realloc(pcm, pcm_cap * 4))) {
                fprintf(stderr, "sloop-render: out of memory\n");
                exit(1);
            }
        }
        if (ui_on && ui_blocks++ % HOST_FRAME_BLOCKS == 0)
            host_ui_frame();
        else
            host_world_service();                /* (a World switch, a stage applied: the main loop's part) */
        host_audio(pcm + 2 * pcm_n, HOST_BLOCK);
        pcm_n += HOST_BLOCK;
    }
}
static uint32_t blocks_of(double frames) { return (uint32_t)ceil(frames / HOST_BLOCK); }

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void put16(FILE *f, uint32_t v) { uint8_t b[2] = {v, v >> 8}; fwrite(b, 1, 2, f); }
static int wav_write(const char *path)
{
    FILE *f = fopen(path, "wb");
    size_t i;
    if (!f)
        return -1;
    fwrite("RIFF", 1, 4, f);
    put32(f, (uint32_t)(36 + pcm_n * 4));
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);                                 /* PCM */
    put16(f, 2);
    put32(f, HOST_FS);
    put32(f, HOST_FS * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, (uint32_t)(pcm_n * 4));
    for (i = 0; i < pcm_n * 2; i++)
        put16(f, (uint16_t)pcm[i]);              /* little-endian on any host */
    return fclose(f) ? -1 : 0;
}

/* ---- --analyze / --check */
static double dbfs(double v) { return v > 0 ? 20.0 * log10(v / 32768.0) : -INFINITY; }
static int analyze(const char *path, int check)
{
    int32_t peak[2] = {0, 0};
    double sum[2] = {0, 0}, sum2 = 0, dc[2];
    size_t i, full = 0, near = 0, run = 0, best = 0, best_at = 0;
    uint64_t h = 0xCBF29CE484222325ull;          /* FNV-1a 64 over the samples (little-endian) */
    int fails = 0, c;
    for (i = 0; i < pcm_n; i++) {
        int quiet = 1;
        for (c = 0; c < 2; c++) {
            int32_t v = pcm[2 * i + c], a = v < 0 ? -v : v;
            uint16_t u = (uint16_t)v;
            peak[c] = a > peak[c] ? a : peak[c];
            sum[c] += v;
            sum2 += (double)v * v;
            full += v >= 32767 || v <= -32767;
            near += a >= 32393;                  /* within 0.1 dB of full scale */
            quiet &= a <= 2;
            h = (h ^ (u & 0xFFu)) * 0x100000001B3ull;
            h = (h ^ (u >> 8)) * 0x100000001B3ull;
        }
        run = quiet ? run + 1 : 0;
        if (run > best) {
            best = run;
            best_at = i + 1 - run;
        }
    }
    for (c = 0; c < 2; c++)
        dc[c] = pcm_n ? sum[c] / pcm_n / 32768.0 : 0;
    {
        int32_t pk = peak[0] > peak[1] ? peak[0] : peak[1];
        double rms = pcm_n ? sqrt(sum2 / (2.0 * pcm_n)) : 0;
        printf("analyze: %s, %.2f s\n", path, (double)pcm_n / HOST_FS);
        printf("  peak     %7.2f dBFS (L %.2f, R %.2f)\n", dbfs(pk), dbfs(peak[0]), dbfs(peak[1]));
        printf("  rms      %7.2f dBFS\n", dbfs(rms));
        printf("  dc       L %+.6f, R %+.6f of full scale\n", dc[0], dc[1]);
        printf("  clipping %zu samples at full scale, %zu within 0.1 dB of it\n", full, near);
        if (best)
            printf("  silence  longest run %.3f s (|sample| <= 2 on both sides), from %.2f s\n",
                   (double)best / HOST_FS, (double)best_at / HOST_FS);
        else
            printf("  silence  none (no frame with |sample| <= 2 on both sides)\n");
        printf("  hash     fnv1a64 %016llx\n", (unsigned long long)h);
        if (!check)
            return 0;
        if (pk < 33) {                           /* -60 dBFS */
            printf("check: FAIL: silent (peak %.1f dBFS)\n", dbfs(pk));
            fails++;
        }
        if (pk >= 32393 || full) {
            printf("check: FAIL: peak %.2f dBFS, %zu samples at full scale (want < -0.1 dBFS, none)\n", dbfs(pk), full);
            fails++;
        }
        if (fabs(dc[0]) > 0.001 || fabs(dc[1]) > 0.001) {
            printf("check: FAIL: DC offset %+.6f / %+.6f (want within 0.001 of full scale)\n", dc[0], dc[1]);
            fails++;
        }
        if (!fails)
            printf("check: ok\n");
    }
    return fails;
}

/* ---- --dump */
static void dump(const host_state_t *s)
{
    static const char *const TRK[HOST_NTRK] = {"track 1", "track 2", "track 3", "drums  "};
    int i;
    printf("tempo    %d BPM, swing %d (%.1f %%)\n", s->bpm, s->swing, 50.0 + s->swing * 0.25);
    for (i = 0; i < HOST_NTRK; i++) {
        const host_track_t *t = &s->t[i];
        printf("%s  %-8s %-11s %2d x %-4s %5.2f bars, %2d %s  level %3d  pan %+3d%s", TRK[i], t->engine, t->sound,
               t->steps, t->div, t->bars, t->notes, i == 3 ? "hits " : "notes", t->level, t->pan,
               t->mute ? "  MUTED" : "");
        if (i < 3)
            printf("  cho %3d  dly %3d  rev %3d  key %s %s\n", t->cho, t->dly, t->rev, t->root, t->scale);
        else
            printf("  rev %3d\n", t->rev);
    }
}

/* until every track with steps has played its pattern twice, in whole bars (at least one) */
static uint32_t natural_bars(const host_state_t *s)
{
    double b = 0;
    int i;
    for (i = 0; i < HOST_NTRK; i++)
        if (s->t[i].notes && !s->t[i].mute && 2 * s->t[i].bars > b)
            b = 2 * s->t[i].bars;
    return b > 0 ? (uint32_t)ceil(b - 1e-9) : 4u;
}

/* ---- --ctl / --sweep: the World's controls by name; 0 = ok */
static double ctl_pos[HOST_NCTL] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};   /* < 0: default */
static int ctl_index(const char *name, size_t n)
{
    int c;
    for (c = 0; c < HOST_NCTL; c++)
        if (strlen(host_macro_name((uint32_t)c)) == n && !strncasecmp(host_macro_name((uint32_t)c), name, n))
            return c;
    return -1;
}
static int ctl_parse(const char *list)
{
    const char *s = list;
    while (*s) {
        const char *eq = strchr(s, '='), *end = strchr(s, ',');
        char *num_end;
        double x;
        int c;
        if (!end)
            end = s + strlen(s);
        if (!eq || eq > end || (c = ctl_index(s, (size_t)(eq - s))) < 0)
            return -1;
        x = strtod(eq + 1, &num_end);
        if (num_end != end || x < 0 || x > 100)
            return -1;
        ctl_pos[c] = x > 1 ? x / 100.0 : x;
        s = *end ? end + 1 : end;
    }
    return 0;
}

/* ---- --world: a Musical World through world.c (the World's own tempo, scenes and variations) */
static int render_world(const char *world, const char *wscene, const char *wvar, int seq, double bars, double seconds,
                        double tail, int do_analyze, int do_check, int do_dump, int sweep, const char *out)
{
    host_world_t w;
    host_state_t st;
    char msg[512];
    int rc, i, scene = -1, var = -1, next = 1, asked = 0;
    uint32_t b, play_blocks;
    double bar;
    i = host_world_factory_find(world);
    rc = i >= 0 ? host_world_load(i) : host_world_load_file(world, msg, sizeof msg);
    if (rc < 0) {
        fprintf(stderr, "sloop-render: %s: %s\n", world, i >= 0 ? host_world_error(rc) : msg);
        return 1;
    }
    host_world(&w);
    if (wscene) {
        scene = wscene[0] >= 'a' && wscene[0] <= 'd' ? wscene[0] - 'a' : wscene[0] - 'A';
        if (scene < 0 || scene > 3 || wscene[1])
            usage();
    }
    if (wvar) {
        for (i = 0; i < w.nvar && strcasecmp(w.var_name[i], wvar); i++)
            ;
        var = i < w.nvar ? i : atoi(wvar) - 1;
        if (var < 0 || var >= w.nvar) {
            fprintf(stderr, "sloop-render: %s has no variation '%s' (", w.name, wvar);
            for (i = 0; i < w.nvar; i++)
                fprintf(stderr, "%s%s", i ? ", " : "", w.var_name[i]);
            fprintf(stderr, ")\n");
            return 1;
        }
    }
    if (seq)
        scene = 0;
    if ((scene >= 0 || var >= 0) && (rc = host_world_request(scene, var)) < 0) {
        fprintf(stderr, "sloop-render: %s: %s\n", w.name, host_world_error(rc));
        return 1;
    }
    host_world(&w);
    host_state(&st);
    if (do_dump) {
        printf("world    %s (%s), %d BPM, \"%s\", id 0x%08x\n", w.name, w.category, w.bpm, w.blurb, (unsigned)w.id);
        printf("scenes   ");
        for (i = 0; i < 4; i++)
            printf("%s%c %s", i ? ", " : "", 'A' + i, w.scene_name[i]);
        printf("\nvars     ");
        for (i = 0; i < w.nvar; i++)
            printf("%s%s", i ? ", " : "", w.var_name[i]);
        printf("\nplaying  scene %c %s, variation %s; keys on track %d\n", 'A' + w.scene, w.scene_name[w.scene],
               w.var_name[w.var], w.keys_track + 1);
        for (i = 0; i < HOST_NTRK; i++)
            printf("track %d  %-8s %-8s %-11s %2d x %-4s level %3d\n", i + 1, w.role[i], st.t[i].engine, st.t[i].sound,
                   st.t[i].steps, st.t[i].div, st.t[i].level);
    }
    bar = 4.0 * 60.0 * HOST_FS / st.bpm;
    bars = bars ? bars : seq ? 4 : seconds ? 0 : 8;
    play_blocks = seconds ? blocks_of(seconds * HOST_FS) : blocks_of((seq ? 4 : 1) * bars * bar);
    for (i = 0; i < HOST_NCTL; i++)              /* --ctl, and --sweep's start */
        if (ctl_pos[i] >= 0 || i == sweep) {
            host_macro_set((uint32_t)i, i == sweep ? 0 : (int32_t)(ctl_pos[i] * 1000 + 0.5));
            do_dump |= 2;                        /* (the positions are printed) */
        }
    {
        int16_t pre[2 * HOST_BLOCK];
        host_audio(pre, HOST_BLOCK);             /* (the World's table taken) */
        if (host_macro_snap() < 0) {
            fprintf(stderr, "sloop-render: the macros did not take their positions\n");
            return 1;
        }
    }
    if (do_dump) {
        printf("macros  ");
        for (i = 0; i < 4; i++)
            printf(" %s %.2f%s", host_macro_name((uint32_t)i), host_macro((uint32_t)i) / 1000.0, i == sweep ? " (sweep 0..1)" : "");
        printf("\n");
    }
    host_play();
    for (b = 0; b < play_blocks; b++) {
        if (sweep >= 0)                          /* 0 .. 1 over the bars played */
            host_macro_set((uint32_t)sweep, (int32_t)((uint64_t)1000u * b / (play_blocks > 1 ? play_blocks - 1 : 1)));
        render(HOST_BLOCK);
        if (!seq)
            continue;
        host_state(&st);
        host_world(&w);
        if (next < 4 && !asked && st.beat >= 4 * ((int)bars - 1) + 1) {   /* in a scene's last bar: the next */
            host_world_request(next, -1);
            asked = 1;
        } else if (asked && w.scene == next) {   /* it came, on the bar */
            printf("scene %c %s from %.3f bars\n", 'A' + next, w.scene_name[next],
                   (double)(pcm_n - HOST_BLOCK * (b < 1 ? 0 : 1)) / bar);
            next++;
            asked = 0;
        }
    }
    host_stop();
    render(blocks_of(tail * HOST_FS) * HOST_BLOCK);
    if (wav_write(out)) {
        fprintf(stderr, "sloop-render: cannot write %s\n", out);
        return 1;
    }
    host_world(&w);
    printf("rendered %s: %s, scene %c %s, variation %s, %.2f s at %d BPM + %.2f s tail\n", out, w.name, 'A' + w.scene,
           w.scene_name[w.scene], w.var_name[w.var], (double)play_blocks * HOST_BLOCK / HOST_FS, st.bpm,
           blocks_of(tail * HOST_FS) * (double)HOST_BLOCK / HOST_FS);
    return do_analyze && analyze(out, do_check) ? 1 : 0;
}

int main(int argc, char **argv)
{
    const char *pos[2] = {0, 0}, *song = 0, *order = 0, *screen = 0, *world = 0, *wscene = 0, *wvar = 0;
    int wseq = 0, sweep = -1, ctl = 0;
    char *files[4] = {0, 0, 0, 0}, *list = 0;
    double bars = 0, seconds = 0, tail = 4;
    int do_analyze = 0, do_check = 0, do_dump = 0, npos = 0, nfiles = 0, i, fmt;
    uint8_t scene[16], nbars[16];
    uint32_t n_order = 0, b, play_blocks;
    host_state_t st;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--bars") && i + 1 < argc)
            bars = atof(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
        else if (!strcmp(a, "--tail") && i + 1 < argc)
            tail = atof(argv[++i]);
        else if (!strcmp(a, "--song") && i + 1 < argc)
            song = argv[++i];
        else if (!strcmp(a, "--order") && i + 1 < argc)
            order = argv[++i];
        else if (!strcmp(a, "--analyze"))
            do_analyze = 1;
        else if (!strcmp(a, "--check"))
            do_analyze = do_check = 1;
        else if (!strcmp(a, "--dump"))
            do_dump = 1;
        else if (!strcmp(a, "--screen") && i + 1 < argc)
            screen = argv[++i];
        else if (!strcmp(a, "--world") && i + 1 < argc)
            world = argv[++i];
        else if (!strcmp(a, "--scene") && i + 1 < argc)
            wscene = argv[++i];
        else if (!strcmp(a, "--var") && i + 1 < argc)
            wvar = argv[++i];
        else if (!strcmp(a, "--world-sequence"))
            wseq = 1;
        else if (!strcmp(a, "--ctl") && i + 1 < argc) {
            if (ctl_parse(argv[++i])) {
                fprintf(stderr, "sloop-render: --ctl %s: NAME=X,.. with NAME one of COLOR MOTION SPACE ENERGY (or "
                        "SOFT .. FREEZE) and X 0..1 or 0..100\n", argv[i]);
                return 2;
            }
            ctl = 1;
        } else if (!strcmp(a, "--sweep") && i + 1 < argc) {
            if ((sweep = ctl_index(argv[i + 1], strlen(argv[i + 1]))) < 0) {
                fprintf(stderr, "sloop-render: --sweep %s: one of COLOR MOTION SPACE ENERGY (or SOFT .. FREEZE)\n",
                        argv[i + 1]);
                return 2;
            }
            i++;
        }
        else if (a[0] == '-' && a[1])
            usage();
        else if (npos < 2)
            pos[npos++] = a;
        else
            usage();
    }
    if (world) {
        if (song || npos != 1 || (wseq && (wscene || seconds)) || bars < 0 || tail < 0 || seconds < 0 || (bars && seconds))
            usage();
        if (host_boot(NULL))
            return 1;
        host_master_volume(4096);
        return render_world(world, wscene, wvar, wseq, bars, seconds, tail, do_analyze, do_check, do_dump, sweep, pos[0]);
    }
    if (ctl || sweep >= 0) {
        fprintf(stderr, "sloop-render: --ctl and --sweep move a World's macros: with --world\n");
        return 2;
    }
    if (bars < 0 || seconds < 0 || tail < 0 || (bars && seconds) || (song ? npos < 1 : npos != 2) || (order && !song))
        usage();
    if (host_boot(NULL))
        return 1;
    host_master_volume(4096);                    /* unity: the raw mix */

    if (song) {                                  /* the sections A..D, the working project, the chain */
        char *tok;
        list = strdup(song);
        for (tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
            if (nfiles == 4) {
                fprintf(stderr, "sloop-render: --song takes four sections at most\n");
                return 2;
            }
            files[nfiles] = tok;
            if ((fmt = host_section_load((uint32_t)nfiles, tok)) < 0) {
                fprintf(stderr, "sloop-render: %s: %s\n", tok, fmt == -1 ? "cannot read" : "not a SLOOP project");
                return 1;
            }
            nfiles++;
        }
        if (!nfiles)
            usage();
        if (order) {
            char *o = strdup(order);
            for (tok = strtok(o, ","); tok; tok = strtok(NULL, ",")) {
                int s = tok[0] >= 'a' && tok[0] <= 'd' ? tok[0] - 'a' : tok[0] >= 'A' && tok[0] <= 'D' ? tok[0] - 'A' :
                        tok[0] >= '1' && tok[0] <= '4' ? tok[0] - '1' : -1;
                int n = tok[1] == ':' ? atoi(tok + 2) : -1;
                if (s < 0 || s >= nfiles || n < 1 || n > 64 || n_order == 16) {
                    fprintf(stderr, "sloop-render: --order: bad entry '%s' (section A..%c with a project, 1..64 bars, "
                            "16 entries at most)\n", tok, 'A' + nfiles - 1);
                    return 2;
                }
                scene[n_order] = (uint8_t)s;
                nbars[n_order++] = (uint8_t)n;
            }
            free(o);
        } else {
            for (i = 0; i < nfiles; i++) {
                host_section_apply((uint32_t)i);   /* (only to read its patterns: stopped, nothing sounds) */
                host_state(&st);
                b = natural_bars(&st);
                scene[n_order] = (uint8_t)i;
                nbars[n_order++] = (uint8_t)(b > 64 ? 64 : b);
            }
        }
        if (npos == 2 && (fmt = host_project_load(pos[0])) < 0) {
            fprintf(stderr, "sloop-render: %s: %s\n", pos[0], fmt == -1 ? "cannot read" : "not a SLOOP project");
            return 1;
        }
        if (npos == 1)
            host_section_apply(scene[0]);        /* the first section played is the working project */
        if (host_song_set(scene, nbars, n_order)) {
            fprintf(stderr, "sloop-render: the song is not valid\n");
            return 1;
        }
        host_song_mode(1);
    } else if ((fmt = host_project_load(pos[0])) < 0) {
        fprintf(stderr, "sloop-render: %s: %s\n", pos[0], fmt == -1 ? "cannot read" : "not a SLOOP project");
        return 1;
    }
    host_state(&st);
    if (do_dump) {
        if (!song)
            printf("project  %s (FUN%d)\n", pos[0], fmt);
        dump(&st);
        if (song) {
            printf("song     ");
            for (i = 0; i < (int)n_order; i++)
                printf("%s%c:%d (%s)", i ? ", " : "", 'A' + scene[i], nbars[i], files[scene[i]]);
            printf("\n");
        }
    }

    /* the length to play: whole blocks (the sequencer acts at block starts: no step of the next bar sneaks in) */
    {
        double bar = 4.0 * 60.0 * HOST_FS / st.bpm;
        if (seconds)
            play_blocks = blocks_of(seconds * HOST_FS);
        else if (bars)
            play_blocks = blocks_of(bars * bar);
        else if (song) {
            for (b = 0, i = 0; i < (int)n_order; i++)
                b += nbars[i];
            play_blocks = blocks_of(b * bar) + 1u;   /* (the arranger stops it at the end) */
        } else
            play_blocks = blocks_of(natural_bars(&st) * bar);
    }
    {
        int16_t pre[2 * HOST_BLOCK];
        host_audio(pre, HOST_BLOCK);             /* the load settles (stop, release, engines), not recorded */
    }
    host_play();
    ui_on = screen != 0;
    for (b = 0; b < play_blocks; b++) {
        render(HOST_BLOCK);
        if (song && !host_playing())
            break;                               /* the song's end */
    }
    if (screen && host_lcd_ppm(screen)) {
        fprintf(stderr, "sloop-render: cannot write %s\n", screen);
        return 1;
    }
    host_stop();
    render(blocks_of(tail * HOST_FS) * HOST_BLOCK);
    if (wav_write(pos[npos - 1])) {
        fprintf(stderr, "sloop-render: cannot write %s\n", pos[npos - 1]);
        return 1;
    }
    {
        double s = (double)pcm_n / HOST_FS;
        printf("rendered %s: %.2f s (%.2f bars at %d BPM + %.2f s tail)\n", pos[npos - 1], s,
               (s - blocks_of(tail * HOST_FS) * (double)HOST_BLOCK / HOST_FS) * st.bpm / 240.0, st.bpm,
               blocks_of(tail * HOST_FS) * (double)HOST_BLOCK / HOST_FS);
    }
    free(list);
    return do_analyze && analyze(pos[npos - 1], do_check) ? 1 : 0;
}
