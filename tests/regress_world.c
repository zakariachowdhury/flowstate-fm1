/* SPDX-License-Identifier: GPL-3.0-only */
/* The regression suite (tests/regress.c) built with FELUCCA_WORLD=1: world_rt.h in core.h, world.c compiled in,
 * and the arranger, so that the live-section path of events_block is built with its World hook (H15:
 * wrt.active ? world_block : live_block); no World loaded. It must match the same tests/golden.txt: with no
 * World active SLOOP renders bit-identically (docs/design/play-mode-architecture.md 1.3, 13).
 * Run by tests/run_tests.sh. */
#define FELUCCA_WORLD 1
#define FELUCCA_ARRANGER 1
#include <stdint.h>
static void arrangement_apply(uint32_t s);   /* arranger_scene.c (in project.c): a song section; never reached */
#include "regress.c"
#include "../firmware/src/world.c"
static void arrangement_apply(uint32_t s) { (void)s; }
