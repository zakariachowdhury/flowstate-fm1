/* SPDX-License-Identifier: GPL-3.0-only */
/* Scripts: timed commands for the firmware thread (--script), for tests and demonstrations.
 *
 *   SCRIPT  steps separated by ';' or new lines; '#' starts a comment
 *   STEP    [TIME] COMMAND     TIME: seconds of audio since power-on (the logo takes 0.93 s), or +S after the
 *                              step before; no TIME: the same moment as the step before
 *   play | stop | playstop
 *   scene A..D                 SAVE + key 1..4: the section, from the next bar while playing
 *   store A..D                 SAVE + key 5..8: the working project into the section
 *   mute N [on|off]            track 1..4 (4 = drums); toggles without on / off
 *   level N V | level N +S     the track's level (0..127), or a step
 *   bpm | swing | filter | dust | duck  V | +S | -S
 *   master V                   the MASTER pot, 0..1023
 *   key K [down|up]            K: 1..27 or F3..G5; without down / up a tap (held for one main-loop pass)
 *   button NAME [down|up]      FX SCL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+
 *   rec | undo                 REC / EDIT tapped: PLAY REC (arm, take, close on the bar, overdub), UNDO
 *   turn ENC STEPS             SELECT ALGO PRESET K1..K4 MASTER (+ = clockwise)
 *   world NAME|N|next|prev     choose a World or a project (the current one plays on); confirm loads it (at once
 *   confirm | cancel           while stopped, on the next bar while playing); cancel forgets the choice. A name
 *                              with spaces takes '_': NEON_RAIN
 *   var NAME|N|next|prev       the World's variation, on the next bar while playing
 *   macro NAME|N V|+S|-S       COLOR MOTION SPACE ENERGY (1..4), 0..100
 *   select N                   the track the keys play (1..4)
 *   print                      the state on stdout
 *   expect FIELD OP VALUE      playing scene next bpm filter mute1..4 level1..4 macro1..4 sel voices gated rms peak
 *                              time master rec (0 empty, 1 armed, 2 recording, 3 loop, 4 overdub) loop (the keys
 *                              loop's notes) (numbers; A..D or - for a scene); world browse pending var varnext (a
 *                              name or -; voices: synth voices sounding, gated: still held, 0 after STOP);
 *                              = != < <= > >= (names: = !=). A failure: exit status 1
 *   quit */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "sim.h"

static const char *const FIELD[F_NF] = {"playing", "scene", "next", "bpm", "filter", "mute", "level", "rms",
                                        "peak", "time", "master", "macro", "sel", "voices", "gated", "rec", "loop",
                                        "world", "browse", "pending", "var", "varnext"};
const char *cmd_field_name(int f) { return f >= 0 && f < F_NF ? FIELD[f] : "?"; }
const char *cmd_scene_name(int s)
{
    static const char *const N[5] = {"-", "A", "B", "C", "D"};
    return s >= 0 && s < 4 ? N[s + 1] : N[0];
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return 0;
    return *a == *b;
}
static int num(const char *s, int32_t *v)
{
    char *e;
    long x;
    if (!s)
        return 0;
    x = strtol(s, &e, 10);
    if (e == s || *e)
        return 0;
    *v = (int32_t)x;
    return 1;
}
static int scene_of(const char *s)
{
    if (!s || !s[0] || s[1])
        return -2;
    return *s == '-' ? -1 : toupper((unsigned char)*s) >= 'A' && toupper((unsigned char)*s) <= 'D' ?
           toupper((unsigned char)*s) - 'A' : -2;
}
static int key_of(const char *s)                 /* 1..27, or F3..G5 (C4 = 60 = key 7) -> 0..26; -1 */
{
    static const int8_t PC[7] = {9, 11, 0, 2, 4, 5, 7};
    int32_t v;
    int n;
    if (num(s, &v))
        return v >= 1 && v <= SIM_KEYS ? v - 1 : -1;
    if (!s || toupper((unsigned char)s[0]) < 'A' || toupper((unsigned char)s[0]) > 'G')
        return -1;
    n = PC[toupper((unsigned char)*s++) - 'A'];
    if (*s == '#')
        n++, s++;
    else if (*s == 'b')
        n--, s++;
    if (*s < '0' || *s > '8' || s[1])
        return -1;
    n += 12 * (*s - '0' + 1) - 53;
    return n >= 0 && n < SIM_KEYS ? n : -1;
}

/* one command (words w[0..n-1]) -> c; 0 = ok */
static int parse(char **w, int n, sim_cmd_t *c)
{
    static const char *const GLOB[HOST_NG] = {"bpm", "swing", "filter", "dust", "duck"};
    static const char *const ENC[SIM_NENC] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4", "MASTER"};
    const char *a = n > 1 ? w[1] : NULL, *b = n > 2 ? w[2] : NULL;
    int32_t v;
    int i;
    memset(c, 0, sizeof *c);
    if (ieq(w[0], "play") && n == 1) c->op = OP_PLAY;
    else if (ieq(w[0], "stop") && n == 1) c->op = OP_STOP;
    else if (ieq(w[0], "playstop") && n == 1) c->op = OP_PLAYSTOP;
    else if (ieq(w[0], "print") && n == 1) c->op = OP_PRINT;
    else if (ieq(w[0], "quit") && n == 1) c->op = OP_QUIT;
    else if ((ieq(w[0], "rec") || ieq(w[0], "undo")) && n == 1) {   /* REC / EDIT tapped (PLAY REC, UNDO) */
        c->op = OP_BUTTON_TAP;
        c->a = (uint8_t)(ieq(w[0], "rec") ? HOST_B_REC : HOST_B_EDIT);
    }
    else if ((ieq(w[0], "scene") || ieq(w[0], "store")) && n == 2 && scene_of(a) >= 0) {
        c->op = ieq(w[0], "scene") ? OP_SCENE : OP_STORE;
        c->a = (uint8_t)scene_of(a);
    } else if (ieq(w[0], "mute") && (n == 2 || n == 3) && num(a, &v) && v >= 1 && v <= 4) {
        c->op = OP_MUTE;
        c->a = (uint8_t)(v - 1);
        c->v = n == 2 ? -1 : ieq(b, "on") ? 1 : ieq(b, "off") ? 0 : -2;
        if (c->v == -2)
            return -1;
    } else if (ieq(w[0], "level") && n == 3 && num(a, &v) && v >= 1 && v <= 4) {
        c->op = OP_LEVEL;
        c->a = (uint8_t)(v - 1);
        c->rel = b[0] == '+' || b[0] == '-';
        if (!num(b, &c->v))
            return -1;
    } else if (ieq(w[0], "world") && n == 2) {
        c->op = OP_WORLD;
        if (ieq(a, "next") || ieq(a, "prev")) {
            c->a = WA_STEP;
            c->v = ieq(a, "next") ? 1 : -1;
        } else if (num(a, &c->v)) {
            c->a = WA_PICK;
            c->v--;
        } else {
            c->a = WA_NAME;
            snprintf(c->s, sizeof c->s, "%s", a);
        }
    } else if (ieq(w[0], "var") && n == 2) {
        c->op = OP_VAR;
        if (ieq(a, "next") || ieq(a, "prev")) {
            c->a = WA_STEP;
            c->v = ieq(a, "next") ? 1 : -1;
        } else if (num(a, &c->v)) {
            c->a = WA_PICK;
            c->v--;
        } else {
            c->a = WA_NAME;
            snprintf(c->s, sizeof c->s, "%s", a);
        }
    } else if ((ieq(w[0], "confirm") || ieq(w[0], "cancel")) && n == 1) {
        c->op = OP_WORLD;
        c->a = ieq(w[0], "confirm") ? WA_CONFIRM : WA_CANCEL;
    } else if (ieq(w[0], "macro") && n == 3) {
        for (i = 0; i < 4 && !ieq(a, MACRO_NAME[i]); i++)
            ;
        if (i == 4 && (!num(a, &v) || v < 1 || v > 4))
            return -1;
        c->op = OP_MACRO;
        c->a = (uint8_t)(i < 4 ? i : v - 1);
        c->rel = b[0] == '+' || b[0] == '-';
        if (!num(b, &c->v))
            return -1;
    } else if (ieq(w[0], "select") && n == 2 && num(a, &v) && v >= 1 && v <= 4) {
        c->op = OP_SELECT;
        c->a = (uint8_t)(v - 1);
    } else if (ieq(w[0], "master") && n == 2 && num(a, &c->v)) {
        c->op = OP_MASTER;
    } else if ((ieq(w[0], "key") || ieq(w[0], "button")) && (n == 2 || n == 3)) {
        int k = -1, down = n == 3 && ieq(b, "down"), up = n == 3 && ieq(b, "up");
        if (n == 3 && !down && !up)
            return -1;
        if (ieq(w[0], "key")) {
            k = key_of(a);
            c->op = n == 2 ? OP_KEY_TAP : OP_KEY;
        } else {
            for (i = 0; i < HOST_NB; i++)
                if (ieq(a, host_button_name((uint32_t)i)))
                    k = i;
            c->op = n == 2 ? OP_BUTTON_TAP : OP_BUTTON;
        }
        if (k < 0)
            return -1;
        c->a = (uint8_t)k;
        c->v = down;
    } else if (ieq(w[0], "turn") && n == 3 && num(b, &c->v)) {
        for (i = 0; i < SIM_NENC && !ieq(a, ENC[i]); i++)
            ;
        if (i == SIM_NENC)
            return -1;
        c->op = OP_TURN;
        c->a = (uint8_t)i;
    } else if (ieq(w[0], "expect") && n == 4) {
        static const char *const OPS[6] = {"=", "!=", "<", "<=", ">", ">="};
        int f;
        c->op = OP_EXPECT;
        for (f = 0; f < F_NF; f++) {
            size_t l = strlen(FIELD[f]);
            int indexed = f == F_MUTE || f == F_LEVEL || f == F_MACRO;
            if (indexed && !strncmp(a, FIELD[f], l) && a[l] >= '1' && a[l] <= '4' && !a[l + 1]) {
                c->rel = (uint8_t)(a[l] - '1');
                break;
            }
            if (!indexed && ieq(a, FIELD[f]))
                break;
        }
        for (i = 0; i < 6 && strcmp(b, OPS[i]); i++)
            ;
        if (f == F_NF || i == 6)
            return -1;
        c->a = (uint8_t)f;
        c->cmp = (uint8_t)i;
        if (f >= F_WORLD) {
            if (c->cmp > CMP_NE)
                return -1;
            snprintf(c->s, sizeof c->s, "%s", w[3]);
        } else if (f == F_SCENE || f == F_NEXT) {
            if (scene_of(w[3]) < -1)
                return -1;
            c->v = scene_of(w[3]);
        } else if (!num(w[3], &c->v)) {
            return -1;
        }
    } else {
        for (i = 0; i < HOST_NG && !ieq(w[0], GLOB[i]); i++)
            ;
        if (i == HOST_NG || n != 2 || !num(a, &c->v))
            return -1;
        c->op = OP_GLOBAL;
        c->a = (uint8_t)i;
        c->rel = a[0] == '+' || a[0] == '-';
    }
    return 0;
}

/* text, or the name of a file holding it */
int cmd_script(const char *text, sim_step_t **steps, int *nsteps, char *err, int errlen)
{
    char *buf, *p, *line;
    double t = 0;
    int cap = 64, n = 0, lineno = 0;
    FILE *f = fopen(text, "rb");
    if (f) {
        long len;
        fseek(f, 0, SEEK_END);
        len = ftell(f);
        fseek(f, 0, SEEK_SET);
        buf = malloc((size_t)len + 1);
        if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len) {
            fclose(f);
            snprintf(err, (size_t)errlen, "cannot read %s", text);
            return -1;
        }
        buf[len] = 0;
        fclose(f);
    } else {
        buf = strdup(text);
    }
    *steps = malloc(sizeof **steps * (size_t)cap);
    for (p = buf; p && *p;) {
        char *w[8], *e, *hash;
        int nw = 0;
        line = p;
        p = line + strcspn(line, ";\n");
        if (*p)
            *p++ = 0;
        lineno++;
        if ((hash = strchr(line, '#')))
            *hash = 0;
        for (e = strtok(line, " \t\r"); e && nw < 8; e = strtok(NULL, " \t\r"))
            w[nw++] = e;
        if (!nw)
            continue;
        if (isdigit((unsigned char)w[0][0]) || w[0][0] == '+' || w[0][0] == '.') {
            double x = strtod(w[0] + (w[0][0] == '+'), &e);
            if (*e) {
                snprintf(err, (size_t)errlen, "step %d: bad time '%s'", lineno, w[0]);
                return -1;
            }
            t = w[0][0] == '+' ? t + x : x;
            memmove(w, w + 1, sizeof w[0] * (size_t)--nw);
        }
        if (!nw || parse(w, nw, &(*steps)[n].c)) {
            snprintf(err, (size_t)errlen, "step %d: cannot read '%s'", lineno, nw ? w[0] : "");
            return -1;
        }
        (*steps)[n++].t = t;
        if (n == cap)
            *steps = realloc(*steps, sizeof **steps * (size_t)(cap *= 2));
    }
    free(buf);
    {   /* in time order (stable: steps at one time keep theirs) */
        int i, j;
        for (i = 1; i < n; i++)
            for (j = i; j > 0 && (*steps)[j - 1].t > (*steps)[j].t; j--) {
                sim_step_t x = (*steps)[j];
                (*steps)[j] = (*steps)[j - 1];
                (*steps)[j - 1] = x;
            }
    }
    *nsteps = n;
    return 0;
}
