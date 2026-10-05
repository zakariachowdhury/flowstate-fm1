/* SPDX-License-Identifier: GPL-3.0-only */
/* PLAY MODE's hard limits (docs/design/play-mode-architecture.md 6.3): what no World, macro, rule or control can push
 * an effective value past, whatever the World's data says. The macro overlay (macro.c) clamps every slot to its
 * descriptor and to these; a limit never moves an authored value (a base already past one stays where the World put
 * it, and a macro cannot push it further). Phase 8's guard.c adds the World's own soft caps and ranges (GUARD) inside
 * them, and the read-site clamps of H6 (fx.c).
 *
 * Rules for this file, so that tools/worldc.py can read it as it reads world_fmt.h (Phase 8: the Python reference
 * model of the macros takes its limits from here): only #define GL_NAME integer, one per line, and comments. */
#ifndef GUARD_LIMITS_H
#define GUARD_LIMITS_H

#define GL_DFDBK_MAX 120             /* G_DFDBK: delay feedback 120 * 230 / 32768 = 0.842 < 1 (fx.c fx_buses) */
#define GL_RSIZE_MAX 127             /* G_RSIZE: reverb comb gain (25000 + 127 * 50) / 32768 = 0.957 < 1 (155: 1.0) */
#define GL_LEVEL_MAX 120             /* P_LEVEL of a track in a World: +4 dB over 0 dB (112), 0.5 dB a step */
#define GL_RESO_MAX 110              /* the engines' resonance (role RESO: ANALOG RES, VOICE Q, TRIO RES), 0..127 */
#define GL_ECHO_DFDBK 96             /* LIVE ECHO's feedback (Phase 13): 0.67 */
#define GL_VMOD_MAX 64               /* ~bright, ~shape: at most +-64 steps of the engine's cutoff / shape (Q8: << 8) */

#endif
