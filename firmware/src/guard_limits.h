/* SPDX-License-Identifier: GPL-3.0-only */
/* PLAY MODE's hard limits (docs/design/play-mode-architecture.md 6.3, docs/guardrails.md): what no World, macro, rule
 * or control can push an effective value past, whatever the World's data says. The macro overlay (macro.c) clamps
 * every slot to its descriptor and to these; a limit never moves an authored value (a base already past one stays
 * where the World put it, and a macro cannot push it further). guard.c adds the World's own soft caps, ranges and
 * combinations (GUARD) inside them; fx.c and voice.c clamp the stability-critical values where they are read (H6,
 * in SLOOP too: a no-op for every value inside its descriptor).
 *
 * Rules for this file, so that tools/worldc.py can read it as it reads world_fmt.h (its reference model of the
 * macros and the guard takes its limits from here): only #define GL_NAME integer, one per line, and comments. */
#ifndef GUARD_LIMITS_H
#define GUARD_LIMITS_H

#define GL_DFDBK_MAX 120             /* G_DFDBK: delay feedback 120 * 230 / 32768 = 0.842 < 1 (fx.c fx_buses) */
#define GL_RSIZE_MAX 127             /* G_RSIZE: reverb comb gain (25000 + 127 * 50) / 32768 = 0.957 < 1 (155: 1.0) */
#define GL_LEVEL_MAX 120             /* P_LEVEL of a track in a World: +4 dB over 0 dB (112), 0.5 dB a step */
#define GL_RESO_MAX 110              /* the engines' resonance (role RESO: ANALOG RES, VOICE Q, TRIO RES), 0..127 */
#define GL_ECHO_DFDBK 96             /* LIVE ECHO's feedback (Phase 13): 0.67 */
#define GL_VMOD_MAX 64               /* ~bright, ~shape: at most +-64 steps of the engine's cutoff / shape (Q8: << 8) */
#define GL_CPU_FULL 2700             /* the CPU guard: host instructions a sample taken as the whole audio interrupt's
                                      * time on the device (so GL_CPU_BUDGET is the 85 % ceiling); Phase 17 measures */
#define GL_CPU_BUDGET 2300           /* the most a World may cost anywhere in its macro space (tests/guard_sweep.c) */
#define GL_CPU_HOLD 8                /* DMA halves (8 blocks each) over the ceiling before the guard holds */
#define GL_CPU_RELEASE 345           /* halves (2 s) under the release level before it lets go */

#endif
