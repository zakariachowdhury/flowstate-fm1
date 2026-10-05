# Musical guardrails

PLAY MODE lets a beginner turn any knob and press any key without reaching an obviously bad state. The Musical
Guardrail Engine sits between the player's actions and SLOOP's core: it keeps notes in key and in range, sounds
inside safe ranges, and arrangement changes on musical boundaries. It does this without making everything bland:
the World's author decides the ranges, and the guard only bounds what the player can reach from there.

This page covers:
- what the guardrails guarantee;
- the rules and limits;
- the GUARD section for World authors;
- how to run the sweeps that check all of it;
- the results of the full sweep.

The design is [design/play-mode-architecture.md §6](design/play-mode-architecture.md#6-musical-guardrail-engine)
(as built: §6.5). The code is mainly in:
- `firmware/src/guard.c`;
- `firmware/src/guard_limits.h`;
- the read-site clamps in `fx.c` and `voice.c`;
- loop follow in `seq.c`.

---

## 1. What is guaranteed

Everything below holds only while a World plays. With no World, every hook takes SLOOP's own path, and both
regression builds match the same 83 goldens. The H6 read-site clamps are the one exception: they are active in SLOOP
too, but they change nothing for any value the editor can set.

| Bad state | What prevents it | Checked by |
| --- | --- | --- |
| Off-key notes, unintended dissonance | Smart Keys map every key into the World's scale and chord (Phase 6). The user loop on the keys track follows the chord (H12): an avoid note plays the nearest chord tone. | `smartkeys_test`, `guard_test` (loop follow over every chord of every World), the sweep's player |
| Notes out of a sensible register | The keys track's range is an octave at least. Notes are folded by octaves into it. | `smartkeys_test`, `guard_test` |
| Too many voices | The keys track's polyphony is capped (`max_poly`). The 8-voice budget is enforced. A UNISON part plays at most `max_unison` voices. | `smartkeys_test`, `guard_test`, the sweep (voice count) |
| Unstable held notes | A held key keeps the note it sounded, whatever happens to the chord, octave or scene (Phase 6). | `smartkeys_test` |
| Runaway feedback | Delay feedback ≤ 120 (0.84 per repeat) and reverb size ≤ 127 (comb gain 0.957). These hold at the point of reading (H6) and in every macro slot. The World's `max_dfdbk` / `max_rsize` cap them further. A default combination shortens the echo in a big room. | `guard_test` (garbage values render as the clamped ones, and the tail falls), the sweep (tail under −60 dBFS 6 s after STOP) |
| Excessive resonance | Resonance (each engine's RESO) ≤ 110 in a slot, and the World's `max_reso` (default 110). A default combination caps resonance at 90 when the drive is high. Every engine's EDIT value is clamped to its descriptor while the engine renders (H6). | `guard_test`, the model cross-check |
| Clipping, extreme output | A track's level ≤ 120, and the World's `max_level` (default 116). The master DC blocker, peak limiter and tanh knee are unchanged. | The sweep: no full-scale sample, peak ≤ −0.3 dBFS, limiter input ≤ 4 × `LIM_T` |
| Integer garbage or overflow | The H6 clamps keep every bus coefficient, DIST, DUST and engine value inside the range its DSP was written for. | The sweep: no jump of more than 0.9 of full scale between two samples, DC ≤ 0.001 over the whole render |
| Invalid parameters | Every effective value stays inside its descriptor, its hard limit and its slot's GUARD range. A base the World put past a limit stays where it is: the macro cannot push it further. | `guard_test` (random writers), `macro_test`, the sweep (the overlay's live table read every 4 blocks) |
| Chaotic over-modulation | Macros move the World's chosen parameters within its chosen amounts. "100 % is never all-max". At most 48 targets. `~bright` / `~shape` ≤ ±64 steps. | `worldc check`, `macro_test` |
| Muddy, unusable combinations | The sound combinations: a big room with a long echo, drive into resonance, a distorted part through DUST, sends into a huge room or a long echo. The World's cross-macro rules (high SPACE + high ENERGY clears the bass, the drums' reverb and the echo). | `guard_test`, the model cross-check |
| Accidental silence | The sweep catches it. The factory Worlds were fixed where it found it (§6). The Smart Keys track is never muted by ENERGY. | The sweep: heard (RMS > −45 dBFS, no silence over 2 s) whenever the ENERGY band holds a layer besides the keys |
| Extreme volume jumps | ENERGY's gain compensation (data, Phase 7). The factory Worlds were tuned. | The sweep: a half turn of one macro moves the loudness by at most 9 LU (12 LU with no player) |
| CPU overload | The 8-voice budget and shedding (`audio.c`). GUARD's caps on UNISON voices, GRAIN density and the number of distorted tracks. The runtime CPU guard: over the ceiling, the costly slots return to their base. | `guard_test`, the sweep (mean ≤ 2,300 host instructions a sample, every DMA half ≤ 2,700) |
| Ugly section-switch timing | A scene or variation commits on the next bar. ENERGY layers change on the bar (or every second bar). Lanes and masks change on the beat (or the bar). At least `min_band_bars` pass between band changes. Fills play on phrase ends. | `guard_test` (scene commits), `macro_test` (ENERGY timing) |
| Unusable envelope lengths | Envelope times are World data. A macro may move them only within its mapping, inside the descriptor. The sweep's tail check catches a release that never ends. | `worldc check`, the sweep |

---

## 2. Rules and limits

### 2.1 Hard limits (`firmware/src/guard_limits.h`)

These hold whatever a World says. `tools/worldc.py` reads the same file.

| Limit | Value | Meaning |
| --- | --- | --- |
| `GL_DFDBK_MAX` | 120 | Delay feedback: 120 × 230 / 32768 = 0.842 a repeat |
| `GL_RSIZE_MAX` | 127 | Reverb comb gain: (25000 + 127 × 50) / 32768 = 0.957 |
| `GL_LEVEL_MAX` | 120 | A track's level in a World: +4 dB |
| `GL_RESO_MAX` | 110 | Each engine's RESO role (ANALOG RES, VOICE Q, TRIO RES) |
| `GL_VMOD_MAX` | 64 | `~bright` / `~shape`: ±64 steps of cutoff or shape |
| `GL_ECHO_DFDBK` | 96 | LIVE ECHO's feedback (Phase 13) |
| `GL_CPU_FULL` | 2,700 | Host instructions a sample taken as the whole audio interrupt's time on the device |
| `GL_CPU_BUDGET` | 2,300 | The most a World may cost anywhere in its macro space (1.23 × SLOOP's heaviest reference mix, 1,877) |
| `GL_CPU_HOLD` | 8 | DMA halves over the ceiling before the CPU guard holds |
| `GL_CPU_RELEASE` | 345 | DMA halves (2 s) under the release level before it lets go |

`GL_CPU_FULL` is a calibration placeholder. Phase 17 measures `song.cpu_q8` on the device and replaces it.

### 2.2 Read-site clamps (H6, in SLOOP too)

Each value below is clamped where the DSP reads it. The stored value is left alone. A value inside its descriptor
reads unchanged, so SLOOP renders bit for bit as before.

| Where | Values |
| --- | --- |
| `fx.c fx_buses` | `G_DFDBK` (≤ `GL_DFDBK_MAX`), `G_RSIZE` (≤ `GL_RSIZE_MAX`), and `G_DCOLOR`, `G_DMIX`, `G_RDAMP`, `G_CDEPTH` through their descriptors |
| `fx.c track_dist`, `dust_process` | `P_DIST`, `G_DUST` |
| `voice.c track_render` | Every engine's EDIT values (resonance, feedback, drive, …), swapped in for the block and back |

### 2.3 Note guard

| Rule | GUARD field | Default |
| --- | --- | --- |
| Range of each synth track (the keys track's folds every key and MIDI note) | `notes.<track>.range` | C3–C6. Under an octave is widened to one. |
| Polyphony of the keys track | `max_poly` | 4 |
| Loop follow: a keys-loop note that is an avoid note over the sounding chord (or out of the scale) plays the nearest chord tone, a tie below. The loop itself never changes. | `loop_follow` | `snap` |
| Avoid notes (a scale tone a semitone above a chord tone; strict: also below) | `avoid` | `classic` |
| Record guard (PLAY REC, `play_rec.c`): a note in key (the World scale or a tone of the sounding chord, as every Smart Keys note is) is recorded as it sounded, one out of key as the nearest safe tone; folded into the range; never the same note twice in a step or half a step from itself; at most `max_notes` and `max_poly` notes a step; a held note ties on but never round onto itself; gentle quantise: the nearest step, the leftover timing scaled by 1 − `quantize` kept as micro-timing (eighths of a step) | `record.max_notes`, `record.quantize`, `max_poly` | 4, 0.75 |

### 2.4 Sound guard

Each macro slot's range is built in three steps:
1. the parameter's descriptor;
2. the hard limits;
3. the World's soft caps and target ranges.

Over the whole table, the guard then limits how many tracks distort and applies the sound combinations.

| Soft cap | GUARD field | Default |
| --- | --- | --- |
| A track's level | `max_level` | 116 |
| Resonance (each engine's RESO) | `max_reso` | 110 |
| Delay feedback | `max_dfdbk` | 96 |
| Reverb size | `max_rsize` | 120 |
| A track's DIST | `max_dist` | 100 |
| DUST | `max_dust` | 100 |
| GRAIN density | `cpu.grain_dens` | 90 |
| Tracks a macro may distort | `cpu.max_dist_tracks` | 2 |
| Any target: `[lo, hi]` | `sound.ranges` | — |

**Sound combinations.** While target *a* is above its value (and *b* above its value), target *c* stays at or under
the cap:
- The cap comes in gradually: from nothing at the thresholds to whole 16 steps past them.
- A condition on *c* itself is whole as soon as it holds.
- A per-track *c* is judged on each track. Its conditions are read on that track when they name it, else at their
  highest.
- A combination only ever lowers a macro's reach. It never moves an authored base.

The firmware's own combinations (`WF_COMBO_DEFAULT`) apply when the World sets none:

| When | Cap | Why |
| --- | --- | --- |
| `g.rsize` > 112 and `g.dfdbk` > 80 | `g.dfdbk` ≤ 80 | A big room and a long echo turn into a wash |
| `@DRIVE` > 64 (each synth track) | `@RESO` ≤ 90 | Resonance through drive screams |
| any track's `dist` > 80 and `g.dust` > 64 | `g.dust` ≤ 64 | Two saturations stacked |
| `g.rsize` > 116 | each synth track's `rev` ≤ 110 | Sends into a huge room become mud |
| `g.rsize` > 116 | `g.drrev` ≤ 100 | Drums in a huge room lose their punch |
| `g.dfdbk` > 90 | each synth track's `dly` ≤ 100 | Echo sends into a long echo pile up |

### 2.5 CPU guard

**Static caps.** GUARD's `max_unison` (default 4) caps a UNISON part's voices. `grain_dens` and `max_dist_tracks`
(§2.4) cap GRAIN density and distortion.

**At run time.** Every DMA half (256 samples), the guard takes a load: the larger of the device's measured `cpu_q8`
and an estimate from what sounds. The estimate counts:
- each voice, by engine;
- DIST, the SLICER and DUST;
- GRAIN density;
- drum voices.

The estimate was fitted on the sweeps' measurements, with a residual of 77 instructions a sample, and rounded up
about 10 %. The host has no timer, so the estimate is what makes the guard testable there.

The guard holds when the load stays over GUARD's ceiling (`cpu.ceiling`, default 0.85) for 8 halves. While it holds:
- the costly slots (DIST, DUST, GRAIN DENS, SLICER depth) glide back to their base;
- a UNISON part plays 2 voices.

It lets go after 2 s under the release level (1/16 under the ceiling: 80 % for the default). Voice shedding
(`audio.c`) remains underneath. No factory World ever makes the guard hold.

### 2.6 Arrangement timing

Arrangement timing is not new code; it was verified in this phase.
- A scene asked for while playing commits on its transition's bar line (1, 2 or 4 bars, or the phrase; Phase 11), a
  variation on the next bar.
- ENERGY bands:
  - layers change on the bar (`mute_change: 2bars`: every second bar);
  - drum lanes, density and play masks change on the beat (`density_change: bar`: the bar);
  - at least `min_band_bars` pass between changes;
  - a hysteresis of 0.02 holds a knob that rests on an edge.
- Fills play only on the last bar of a phrase (`fills_every`).

---

## 3. The GUARD section, for authors

```json
"guard": {
  "notes": {"lead": {"max_poly": 2, "loop_follow": "snap", "avoid": "classic"}, "bass": {"range": ["E1", "E3"]}},
  "record": {"quantize": 0.8, "max_notes": 3},
  "sound": {"max_level": 116, "max_dfdbk": 88, "max_reso": 100,
            "ranges": {"bass.RES": [0, 80], "g.dfdbk": [0, 88]},
            "combos": [{"when": {"g.rsize": 110, "g.dfdbk": 70}, "cap": {"g.dfdbk": 70}},
                       {"when": {"lead.@DRIVE": 40}, "cap": {"lead.@RESO": 60}}]},
  "arrangement": {"mute_change": "bar", "density_change": "beat", "fills_every": 4, "min_band_bars": 1},
  "cpu": {"max_unison": 4, "grain_dens": 90, "max_dist_tracks": 2, "ceiling": 0.85}
}
```

Notes for authors:
- Every field is optional. The format is [worlds.md §14](worlds.md#14-guard), and the bytes are
  [design/fwd1-format.md §13](design/fwd1-format.md#guard).
- `combos`: absent, the firmware's six apply; `[]` turns them off; up to 8 of your own replace them. A combination
  names parameters, `@roles` or globals; `~bright` and `~shape` are not allowed.
- An ambient World that wants its huge room should set its own combinations, or `[]`. The defaults favour clarity.

**Checking a World.** `python3 tools/worldc.py check my.world.json` compiles the World. It then runs the reference
model of the macros and the guard over every scene × variation and the 3⁴ grid of the four macros. The checker
refuses a World when:
- a mapping alone takes its parameter outside its range, or past a hard limit;
- a mapping reaches the parameter's maximum at 100 % without `"saturate": true`;
- more than 48 targets move at once;
- the Smart Keys range is under an octave.

Mappings and rules that only together run past the top of a range are warnings.

`python3 tools/worldc.py model my.world.json` prints the model's slot tables. The model is the firmware's code over
again, integer for integer. `tests/run_tests.sh` checks it against the C engine on every factory and test World.

The test Worlds `worlds/test/extreme.world.json` and `full.world.json` push past ranges on purpose, to exercise the
clamps, so `check` refuses them. The same goes for `worlds/test/guard.world.json`, which engages every rule at once.

---

## 4. Running the sweeps

```sh
./build.sh                       # or: python3 tools/build.py --gen-only
tests/run_tests.sh               # all host tests, including the quick sweep (about 10 s of it)
SWEEP=full tests/run_tests.sh    # the full sweep (about 1.5 min on 10 cores) and the injected bugs
```

You can also run the pieces by hand:

```sh
cc -O2 -w -Ibuild/gen -Ifirmware/src -o build/host/guard_sweep tests/guard_sweep.c -lm
python3 tools/worldc.py compile worlds/factory/neon_rain.world.json -o build/host/neon.wblob
build/host/guard_sweep build/host/neon.wblob             # quick
build/host/guard_sweep --full build/host/neon.wblob      # full
build/host/guard_sweep --full --var 2 --list build/host/neon.wblob   # one variation, every point's numbers
build/host/guard_sweep --selftest                        # each detector against a signal made to fail it
build/host/guard_sweep --dump-slots build/host/neon.wblob   # the C slot tables (compare: worldc.py model)
python3 tests/guard_mutants.py                           # 28 injected bugs, each must be caught
```

**The sample.** Each point is 2 bars and a 6 s tail through the real firmware, in a fork of the booted state, run in
parallel. Every render has a player: a recorded phrase on the Smart Keys track. The quick sweep:
- every scene on its ORIGINAL variation, over the 3⁴ grid (COLOR, MOTION, SPACE and ENERGY each at 0, 50 and 100 %);
- 8 seeded random points per scene (any variation, half with no player);
- 1,424 points for the four factory Worlds.

The full sweep:
- every scene × variation, over the 3⁴ grid with the player and again without;
- the grid's 8 edge points (one macro at 25 or 75 %);
- 300 seeded random points per World (half near the corners, half with no player);
- 14,800 points.

**The checks.**

| Check | Limit |
| --- | --- |
| Clipping | No sample at full scale; peak ≤ −0.3 dBFS |
| Garbage | No jump > 0.9 of full scale between two samples (the music reaches 0.69); DC ≤ 0.001 over bars and tail; limiter input ≤ 4 × `LIM_T` |
| Feedback | After STOP the tail is under −60 dBFS within 6 s, with no voice left |
| Silence | RMS > −45 dBFS and no silence over 2 s, whenever the ENERGY band holds a layer besides the keys |
| Volume jumps | Between two grid neighbours (one macro half a turn apart): ≤ 9 LU with the player, ≤ 12 LU without |
| Parameters | Every value the overlay writes (read every 4 blocks) inside its descriptor, hard limit and range; every base inside its descriptor |
| Voices | ≤ 8 synth voices |
| CPU | Mean ≤ 2,300 host instructions a sample. The costliest DMA half but one ≤ 2,700: the counter includes the kernel's instructions, and a lone spike is its; a render over either limit is measured twice more and the least kept. guard.c's estimate is at most 15 % under the measured mean. |

---

## 5. Full sweep results (2026-10-05)

`SWEEP=full`: 14,800 points (3,700 per World), all clean.

| World | Worst peak | Worst tail 6 s after STOP | Loudness | Largest jump: player / none | CPU mean / half (max) | Estimate (max) | Effective values checked |
| --- | --- | --- | --- | --- | --- | --- | --- |
| NEON RAIN | −3.22 dBFS | −60.2 dBFS | −30.0 … −10.9 LUFS | 7.2 / 11.6 LU | 1,520 / 1,961 | 2,072 | 108.8 M |
| MIDNIGHT DRIVE | −2.62 dBFS | −78.3 dBFS | −29.9 … −9.8 LUFS | 5.9 / 11.1 LU | 1,242 / 2,072 | 1,876 | 82.5 M |
| FROZEN LAKE | −2.53 dBFS | −60.8 dBFS | −31.0 … −9.8 LUFS | 7.6 / 10.8 LU | 1,436 / 2,008 | 1,876 | 128.3 M |
| DUSTY CAFE | −3.26 dBFS | −73.4 dBFS | −23.8 … −13.9 LUFS | 2.7 / 6.0 LU | 1,134 / 2,067 | 1,769 | 97.4 M |

CPU is in host instructions a sample (cc -O2, Apple M1 Pro). The heaviest render, at 1,520, is 66 % of
`GL_CPU_BUDGET` and 81 % of SLOOP's heaviest reference mix. The costliest DMA half, at 2,072, is 77 % of
`GL_CPU_FULL`; it varies by a few percent from run to run with the kernel's share. The CPU guard never engaged.
The full sweep took 87 s on 10 cores.

The rest of the guard's tests, at the same time:
- `tests/guard_test.c`: 31,964 checks;
- the model cross-check: the same 13,020 slot tables in C and Python;
- `tests/guard_mutants.py`: 28 injected bugs, all caught.

The tails at −60.2 and −60.8 dBFS are the ambient Worlds' long echoes, at COLOR and SPACE 100 % in their dreamy,
dark and floating variations. They pass, with little room: a change to the DSP that lengthens them will fail the
sweep.

---

## 6. What the sweeps found, and the fixes

The first quick sweep found one tail too long, in FROZEN LAKE's ORIGINAL. Once it was fixed, the first full sweep
failed 171 points across three Worlds, all in variations the quick sweep does not play. Each fix below is the smallest
data change that clears its failures.

| World | Found | Fix |
| --- | --- | --- |
| FROZEN LAKE | Feedback: scene D at COLOR, MOTION and SPACE 100 % still −59.7 dBFS 6 s after STOP (the delay at 1/4, 66 BPM) | SPACE → `g.dfdbk` max +6 → +2 |
| FROZEN LAKE, DARK | The same at −59.4 dBFS; and COLOR 0 closing the dark pad: 13 LU jumps with no player, down to −37.7 LUFS | `pad.CUT` 44 → 58; the variation's `dfdbk` 56 |
| NEON RAIN, DARK | COLOR 0 closing the DARK STR pad: −44 LUFS, jumps of 10.9 LU with the player and 23 LU without | `pad.CUT` 40 → 60, `lead.CUT` 80 → 92 |
| NEON RAIN, HEAVY | The same in the breakdown: −37.8 LUFS, jumps of 15.7 LU with no player | `pad.CUT` 50 → 62 |
| MIDNIGHT DRIVE, DREAMY | Silence: at ENERGY 0 only the WARM PAD chords play, 12 dB under the ORIGINAL's (−45.6 dBFS RMS); jumps of 22 LU when ENERGY brings the bass and drums | `chords.level` 100 (track 80), `chords.CUT` 64 → 72 |
| MIDNIGHT DRIVE, DARK | COLOR 0 closing the DARK STR chords: −44 LUFS, jumps of 15.6 LU with no player | `chords.CUT` 48 → 62, `chords.level` 92 → 100 |

Two failures came from the measurement, and the check was corrected rather than the Worlds:
- **DC.** In the first quick sweep, a 2-bar window measured the master DC blocker's answer to a pulse pad's first
  note as a DC offset (up to 0.0023). DC is now measured over the whole render, which starts and ends silent.
- **CPU.** A lone DMA half of 3,253 instructions (mean 1,120) did not come back when measured again: it was the
  kernel. The check now takes the costliest half but one, and measures a render over a limit again.

The ORIGINAL variations and the defaults are unchanged, apart from FROZEN LAKE's SPACE top. The Worlds still sit
within 3 LU of each other at their defaults.

---

## 7. Not covered yet

- LIVE ECHO's feedback limit (`GL_ECHO_DFDBK`) applies once LIVE FX exists (Phase 13).
- `GL_CPU_FULL` and the CPU budget need recalibrating against the device's `cpu_q8` (Phase 17).
- `tools/validate-world` (Phase 16) will wrap `worldc check` and these sweeps for a single World.
