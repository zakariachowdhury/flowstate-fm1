# Validating a World

`tools/validate-world` is the one command that says whether a World is safe to ship. It runs the compiler's checks,
the model of the macros and the guard, the budgets, and then plays the World through the real firmware over many macro
combinations. It exits non-zero when a World breaks a hard limit.

```sh
./tools/validate-world worlds/factory/neon_rain.world.json   # a file (about 6 s)
./tools/validate-world neon_rain                              # a factory id (neon-rain works too)
./tools/validate-world worlds/factory/*.world.json            # several Worlds (about 5 s each)
./tools/validate-world worlds/factory                         # a directory: every *.world.json in it
./tools/validate-world --full neon_rain                       # the full macro sweep (about 40 s)
./tools/validate-world --static my.world.json                 # no sweep: the checks of a second
./tools/validate-world --json my.world.json                   # a machine-readable report
```

This page covers:
- the checklist and what each item means;
- quick, full and static runs;
- the numbers behind the budgets;
- the JSON report;
- how to fix common failures;
- the corpus of broken Worlds that tests the checker.

The design is [design/play-mode-architecture.md §12.3](design/play-mode-architecture.md#123-validate-world). The
rules and limits being checked are in [guardrails.md](guardrails.md); the World format is in [worlds.md](worlds.md).

---

## 1. The checklist

One line per item. `✓` passed, `✗` failed, `⚠` passed with warnings, `–` not run (the reason follows). A pass carries
the numbers that matter; a failure carries the first finding and how many more there are (`-v` prints them all).

```
neon_rain  NEON RAIN  1,365 B  (worlds/factory/neon_rain.world.json)
  ✓ metadata
  ✓ presets
  ✓ engines
  ✓ patterns
  ✓ scenes
  ✓ variation
  ✓ scale
  ✓ harmony
  ✓ Smart Keys
  ✓ macro ranges
  ✓ Guardrails                audible; loudness steps 4.5 LU with a player, 0.0 without (limits 9 / 12)
  ✓ CPU budget                mean 1,612 of 2,300, costliest half 2,097 of 2,700 instr/sample
  ✓ RAM budget                pool 12 of 16 patterns; targets 23 (36 with every control up) of 48
  ✓ flash budget              1,365 B of 3,072 B (factory World); factory set 5,924 B of 13,460 B
  ✓ no clipping               peak at most -3.4 dBFS (limit -0.3) over 372 renders
  ✓ no invalid feedback       worst tail -61.4 dBFS after STOP (limit -60)
  ✓ no invalid parameter IDs  11,827,253 effective values inside their descriptors
  passed: 17 clean
```

A color is used only when stdout is a terminal (and `NO_COLOR` is unset). The exit status is 0 when nothing failed,
1 when any item failed, 2 for a usage error (a World that does not exist).

**The order of the work** is not the order of the lines. First the compiler and the model run (metadata to macro
ranges, Guardrails, RAM and flash budgets). If any of those failed, the sweep does not run: the four render items
show `–`, and you fix the cheap findings first. Otherwise the World is rendered and the render items are filled.

### 1.1 What each item checks

The first group comes from `tools/worldc.py` (the compiler, whose messages carry a JSON path) and is mapped by the
path of the message. The checks themselves are [worlds.md](worlds.md)'s rules.

| Item | Checks | If it fails, fix |
| --- | --- | --- |
| `metadata` | JSON that parses, a JSON object, `format`, `id` (`a-z0-9_`, up to 24), `name`, `category` and `blurb` (charset, length), `tempo` (40–240, `min`/`max` within ±20 %), `swing`, keys the format does not know | the field the message names |
| `presets` | each synth track's `preset` exists for its engine; the drum `kit` exists; the values of the track's `params` are inside their descriptors; the global `fx` values | spell the preset as `worldc.py names ENGINE` prints it |
| `engines` | exactly four tracks with unique names, the fourth the drums; each engine exists; a register is a note name | the `tracks` list |
| `patterns` | lanes, step tokens, lengths (16, 32 or 64 drum steps; 1–64 melodic, also after unrolling), the drum track's 1/16 | the pattern the message names |
| `scenes` | scenes A, B, C and D, each complete; the progressions, patterns and ENERGY tables they name exist; `transition`, `fill`; `defaults` (scene, PULSE, BEAT) | add the missing scene; fix the name |
| `variation` | at most 8, `ORIGINAL` first and empty; `sounds`, `params`, `swap`, `macros`, `energy_bias`; a variation cannot change the tempo, key, scale, progressions, swing, drum grooves or Smart Keys | move the change to a scene, or drop it |
| `scale` | the key's root and scale exist; the Smart Keys melody scale holds the root and stays inside the World scale | choose a melody scale that is a subset (`PEN`, `MPEN`) |
| `harmony` | progressions parse (roman numerals, suffixes); 1–16 chords; 4, 8, 16 or 32 beats in all; at most 8 used | make the beats add up |
| `Smart Keys` | the track exists and is a synth track; the tonic is the key's root; the range is an octave at least; the Smart Keys track is in every ENERGY band | widen the range; put `keys` in each band's `layers` |
| `macro ranges` | every mapping, control, curve and rule is well formed (at most 40 mappings); the model: for every scene × variation, a mapping at 0 % and at 100 % keeps its parameter inside the descriptor's range, and a mapping that reaches the maximum at 100 % says `"saturate": true` ("100 % is never all-max") | make the mapping smaller, or `saturate` if it is meant |
| `Guardrails` | the `guard` section is well formed (ranges, combinations, caps); a mapping, or an authored value, past a hard limit of `guard_limits.h` (delay feedback 120, reverb size 127, a track's level 120, resonance 110). After the sweep: the World is audible (RMS over -45 dBFS, no silence over 2 s while a layer besides the keys is up), and a half turn of one macro moves the loudness by at most 9 LU with a player, 12 LU without | lower the base or the mapping; fix the cutoff or level that falls silent |

The render items come from `tests/guard_sweep.c`. Each failure message names the scene, the variation and the macro
positions (COLOR / MOTION / SPACE / ENERGY, in %), worst case first.

| Item | Checks | If it fails, fix |
| --- | --- | --- |
| `CPU budget` | the mean over any render at most 2,300 host instructions a sample (`GL_CPU_BUDGET`); no DMA half but one over 2,700 (`GL_CPU_FULL`); at most 8 voices; the CPU guard's estimate not more than 15 % under the measured mean | fewer distorted tracks, a lower GRAIN `DENS`, less UNISON; see §3 |
| `RAM budget` | at most 16 patterns in the pool after unrolling chord tokens; at most 48 overlay targets moving at once, with the macros anywhere on the 3⁴ grid and again with every control 4..15 at 100 % | share patterns; fewer `*.param` mappings |
| `flash budget` | the compiled blob at most 3,072 B (factory) or 3,840 B (`--user`); above 2,048 B is a warning; and, for a World in `worlds/factory`, the factory set against its share of the app slot (§3) | shorter patterns, `.` and `-` instead of notes, fewer variations |
| `no clipping` | no sample at full scale, peak at most -0.3 dBFS, no jump of more than 0.9 of full scale between two samples, DC at most 0.001, the master limiter's input at most 4 × `LIM_T` | lower `level`s, the drums' level, or a macro's `max` |
| `no invalid feedback` | after STOP the tail falls under -60 dBFS within 6 s (10 s with LIVE ECHO at 100 %), with no voice left | lower `fx.dfdbk`, `fx.rsize`, the sends, or the SPACE mapping's top |
| `no invalid parameter IDs` | every parameter name a track, mapping, rule, variation or guard range uses exists for the engine it is applied to, and is not structural; in the sweep, every effective value the overlay writes (read every 4 blocks) stays inside its descriptor, hard limit and range | the name the message gives; `worldc.py names ENGINE` lists what exists |

`RAM budget` and `flash budget` read their limits from `world_fmt.h` and the others from `guard_limits.h` when the
tool starts: no number here is copied by hand. The sweep's own limits (-0.3 dBFS, -60 dBFS, 9 and 12 LU) are read from
the `#define`s of `tests/guard_sweep.c`.

**Where a message goes.** A message about a parameter name that does not exist is reported under `no invalid parameter
IDs` wherever it is written (a track's `params`, a macro, a variation). A mapping that is past a hard limit is
reported once, under `Guardrails`, and not also as out of range or saturating. A scene that names a progression or
pattern that exists but has errors of its own is not reported again: the definition is. Messages that no item owns
go to `metadata`.

**What `–` means.** An item passes only when everything it covers ran. If the compiler stops early (a schema error,
or a bad track), the items it never reached show `–` ("not checked") rather than `✓`.

---

## 2. Quick, full and static

| Mode | What runs | Time for one World |
| --- | --- | --- |
| `--static` | the compiler, the model and the budgets. No audio; the four render items and the render half of Guardrails show `–` | under a second |
| default (quick) | the above, then `guard_sweep`'s quick sample, 372 renders of 2 bars and a 6 s tail | 4–6 s on 10 cores |
| `--full` | the above, then `guard_sweep --full`, then the quick pass again | 30–40 s on 10 cores |

Every render has a player: a recorded phrase on the Smart Keys track, so the keys' cost and the loop following the
chords are in every measurement.

**Quick** plays, for each scene on its ORIGINAL variation, the 3⁴ grid of the four macros (COLOR, MOTION, SPACE and
ENERGY each at 0, 50 and 100 %); 8 seeded random points (any variation, half with no player); and the corners of
controls 4..15 with the macros at 100 %: SOUND SHAPE and MOVEMENT all up, and LIVE FX at their ends (FILTER low and
high, ECHO and CRUSH at 100 %). That is the multiple-macro-combination test in a few seconds.

**Full** plays every scene × every variation over the grid (with a player and without), the grid's edge points, and
300 seeded random points, near the corners and with no player: 3,700 renders. `guard_sweep --full` does not visit
controls 4..15, so `validate-world --full` also runs the quick pass, whose corners do. The report then counts both
(4,072 renders for NEON RAIN). Run `--full` before a World ships; run quick while authoring.

`--seed N` changes the random points. `--jobs N` limits the parallel renders (default: every core). The sweep binary
is built on first use from `tests/guard_sweep.c`, as `tests/run_tests.sh` builds it, into `build/host/validate/`, and
rebuilt when a firmware or test source is newer. It needs a C compiler (`CC`, default `cc`) and `build/gen` (made by
`python3 tools/build.py --gen-only` when missing). If it cannot be built, the render items fail with the compiler's
message rather than pass.

The CPU numbers are host instructions a sample, counted by the kernel, and only macOS provides the counter
(`proc_pid_rusage`). Elsewhere the CPU item is `⚠` ("not measured"); the other checks run.

---

## 3. The budgets

| Budget | Limit | Where it comes from |
| --- | --- | --- |
| CPU, mean | 2,300 host instr/sample | `GL_CPU_BUDGET`: 1.23 × SLOOP's heaviest reference mix |
| CPU, one DMA half | 2,700 | `GL_CPU_FULL`, a placeholder until Phase 17 calibrates on the device |
| Voices | 8 | the voice budget |
| Pattern pool | 16 | `WF_MAX_PAT` |
| Overlay targets | 48 | `OV_MAX` (`world_rt.h`) |
| Blob, factory | 3,072 B (2,048 B guideline) | `WF_FACTORY_LEN`, `WF_SOFT_LEN` |
| Blob, user | 3,840 B | `WF_MAX_LEN` |
| Factory set | 50 % of the app-slot space the code leaves | below |

**The factory set.** The factory Worlds live in the app slot with the code. The room for Worlds is the slot minus the
code:

```
room   = APP_SLOT - (size of build/felucca.bin - sum of the factory blobs)
budget = 50 % of room
```

`APP_SLOT` (0x8DFBC) is read from `tools/fm1pkg_make.py`, which `tools/build.py` uses; the image size from
`build/felucca.bin`, which is built from the same factory Worlds. The sum is over all of `worlds/factory`, and the
result is part of `flash budget` for each factory World validated. Today the four Worlds take 5,924 B of a 13,460 B
budget. The other half is for the code that Phases 14 to 17 still add. If `build/felucca.bin` is absent (no build
yet), the flash line is `⚠` and says so. The check assumes the image was built from the current factory Worlds:
rebuild after adding one.

The share is `FACTORY_SHARE` in `tools/validate_world.py`. The design's plan for 30 factory Worlds (about 45–60 KB,
[§11.1](design/play-mode-architecture.md#111-flash-75316-b-free-in-the-app-slot)) does not fit today's budget, and the
check will say so as the Worlds are added. Phase 18 replaces the PERC one-shots first, which frees at least 50 KB of
room (about 38 KB of budget at this share): revisit the share then.

**Targets at once.** A macro slot is a target a macro or control moves; the firmware's overlay holds 48. `worldc
check` counts them with the four macros on the grid and the controls at the World's defaults. `validate-world` also
counts them with every control 4..15 at 100 %, because the built-in mappings of Phase 13 add slots on the Smart Keys
track, and a World with 40 slots of its own would silently lose some when SOUND SHAPE is turned up. The factory
Worlds peak at 21–24 targets, 32–36 with every control up.

---

## 4. The JSON report

`--json` prints one JSON document to stdout, and nothing else there. The schema is `validate-world/1`.

```jsonc
{
  "schema": "validate-world/1",
  "mode": "quick",                       // "static", "quick" or "full"
  "ok": true,                            // no item of any World failed
  "seconds": 6.9,
  "worlds": [
    {
      "file": "worlds/factory/neon_rain.world.json",
      "id": "neon_rain", "name": "NEON RAIN", "kind": "factory",     // "user" with --user
      "ok": true,
      "bytes": 1365,                     // null when it does not compile
      "items": [
        {"item": "metadata", "status": "pass", "summary": "", "info": "", "errors": [], "warnings": [], "data": {}},
        {"item": "CPU budget", "status": "pass", "summary": "mean 1,612 of 2,300, costliest ...",
         "info": "mean 1,612 of 2,300, costliest ...", "errors": [], "warnings": [],
         "data": {"cpu_mean_max": 1612.0, "cpu_half_max": 2097.0, "cpu_estimate_max": 2168, "voices_max": 8,
                  "budget": 2300, "limit": 2700}}
      ],
      "sweep": {
        "mode": "quick", "points": 372, "failed_points": 0,
        "worst_peak_dbfs": -3.4, "worst_tail_dbfs": -61.4, "loudness_lufs": [-26.9, -11.9],
        "cpu_mean_max": 1612.0, "cpu_half_max": 2097.0, "cpu_estimate_max": 2168, "voices_max": 8,
        "values_checked": 11827253, "max_jump_lu": {"player": 4.5, "alone": 0.0},
        "limits": {"peak_dbfs": -0.3, "tail_dbfs": -60.0, "jump_lu": 9.0, "jump_lu_alone": 12.0}
      }
    }
  ]
}
```

- `items` always has the 17 items, in the order above. `status` is `pass`, `fail`, `warn` or `skip`.
- `errors` and `warnings` hold every message, in full. `summary` is the one line the terminal prints. `info` is what a
  passing item reports (for a skipped item, why it was not run). `data` has the item's numbers (budget items, CPU,
  RAM and flash).
- `sweep` is `null` when the sweep did not run (`--static`, or a World that failed an earlier item).
- A World's `ok` is false when any of its items failed. Warnings never fail a World.
- `worlds` follows the order given on the command line.

---

## 5. Fixing common failures

| You see | Why | Fix |
| --- | --- | --- |
| `✗ metadata  $.name: "..." does not match` | the name has lowercase or punctuation outside `A-Z 0-9 space & ' - .` | upper-case it |
| `✗ presets  "X" is not a preset of ANALOG` | a typo, or a preset of another engine | `python3 tools/worldc.py names ANALOG` |
| `✗ scenes  $.scenes: missing "D"` | every World has all four scenes | add the scene (it may play nothing) |
| `✗ variation  ... it cannot change the tempo` | a variation tried `tempo`, `key`, `swing`, `beat`, `patterns` or `smart_keys` | move it to a scene; a variation keeps the World's groove |
| `✗ scale  the melody scale holds the tonic and stays inside` | `melody_scale` has notes the World scale lacks | `PEN`, `MPEN`, or a subset in semitones that includes 0 |
| `✗ harmony  5 beats in all` | a progression's beats are not 4, 8, 16 or 32 | adjust the `:n` counts |
| `✗ Smart Keys  under an octave` | the keys' `range` spans less than 12 semitones | widen it; the range is also `guard.notes.<track>.range` |
| `✗ macro ranges  base 60 +100 = 160: outside 0..127` | a mapping's offset takes the value past its descriptor | reduce `max` (or `min`); the firmware would clamp it |
| `✗ macro ranges  127 at 100 %: the parameter's maximum` | 100 % of a macro would be "everything at its maximum" | reduce the top by one, or `"saturate": true` if intended |
| `✗ Guardrails  past the hard limit 120` | a mapping or an authored value beyond `GL_DFDBK_MAX`, `GL_RSIZE_MAX`, `GL_LEVEL_MAX` or `GL_RESO_MAX` | lower it |
| `✗ Guardrails  silent(rms ...)` | at some macro position only a quiet layer plays (a dark pad with the cutoff closed) | raise that track's level or cutoff, or keep ENERGY from muting everything else |
| `✗ Guardrails  loudness moves 13.5 LU` | a half turn of one macro changes the level too much | check the mapping's gain compensation (`*.level` in ENERGY) and cutoff floor |
| `✗ CPU budget  cpu(mean ...)` | too many costly voices or effects at the corner it names | drop a UNISON part, lower GRAIN `DENS`, distort fewer tracks, or let a macro move them less |
| `✗ RAM budget  17 patterns after unrolling` | a chord-token pattern counts once for each progression it plays over, and each swap target counts | use a faster `div`, share patterns between scenes, drop a variation's swap |
| `✗ RAM budget  57 targets moving at once` | `*.param` expands to every synth track | name the tracks that matter; combine mappings that share a target |
| `✗ flash budget  the blob is 3,297 B` | patterns are most of a World | shorter patterns; repeated notes cost 2 B; `-` ties; fewer variations |
| `✗ flash budget  factory set` | the factory Worlds together use more than their share of the app slot | trim Worlds, or free space in the firmware first (§3) |
| `✗ no clipping  clip(peak ...)` | the mix reaches full scale at a macro corner | lower `level`s, the drums' level, or the ENERGY mapping's top |
| `✗ no invalid feedback  feedback(tail -45 dBFS ...)` | the echo and room ring on after STOP | lower `fx.dfdbk` and `fx.rsize`; at slow tempos a 1/4 echo is the longest; or lower the SPACE mapping |
| `✗ no invalid parameter IDs  "NOPE" is neither a common parameter` | a typo, or a parameter of another engine | `python3 tools/worldc.py names ENGINE` |

The tail check has the least room in the factory set: FROZEN LAKE and NEON RAIN pass by 2 dB and 1.4 dB, and `--full`
takes NEON RAIN to -60.2 dBFS. A change to the DSP that lengthens tails will show here first.

---

## 6. The corpus of broken Worlds, and the tests

`worlds/test/bad/*.world.json` are Worlds that each break one rule, so the checker is tested on failures as well as
passes. Each was derived from `worlds/test/minimal.world.json` and says in its `notes` what is wrong.

| File | Fails |
| --- | --- |
| `name_charset` | `metadata`: a name with lowercase and `!` |
| `unknown_preset` | `presets`: a preset ANALOG does not have |
| `unknown_engine` | `engines`: an engine that does not exist |
| `pattern_bad_step` | `patterns`: a drum lane with a `?` |
| `missing_scene` | `scenes`: no scene D |
| `variation_sets_tempo` | `variation`: a variation that changes the tempo |
| `melody_scale_off_key` | `scale`: a BLUES melody over C major |
| `progression_beats` | `harmony`: a progression of 5 beats |
| `keys_range_octave` | `Smart Keys`: a range under an octave |
| `macro_saturates` | `macro ranges`: SPACE takes `pad.rev` to its maximum without `saturate` |
| `macro_out_of_range` | `macro ranges`: `pad.rev` driven to 160 |
| `feedback_past_hard_limit` | `Guardrails`: a SPACE mapping takes the delay feedback to 130 |
| `level_past_hard_limit` | `Guardrails`: a bass level authored at 125 |
| `guard_range_inverted` | `Guardrails`: a GUARD range with low above high |
| `unknown_parameter` | `no invalid parameter IDs`: a macro target no track has |
| `pattern_pool_overflow` | `RAM budget`: 17 patterns for a pool of 16 |
| `too_many_targets` | `RAM budget`: 57 targets at once |
| `blob_too_large` | `flash budget`: a 3,297 B factory World |
| `feedback_runaway` | `no invalid feedback`: the one World that passes every static item and fails only the render: the echo at the hard limit in a 127 room |

`tests/validate_test.py` (a group of `tests/run_tests.sh`, about 20 s) checks that:
- the four factory Worlds pass all 17 items from the command line, with a quick sweep each;
- every file above fails exactly the item in its row and no other, and the fixtures and the table are the same set;
- the JSON schema is stable (keys, the item order, the status words) and the exit status follows the failures;
- `guard_sweep`'s every failure keyword (clipping, garbage, DC, limiter, feedback, silence, parameters, voices, CPU, a
  crashed render, and a volume jump) reaches its item, from made-up output, so the detectors that no fixture trips
  are covered too;
- ids, directories, `--user`, and files that are not Worlds.

A bad World that fails a static item is not swept, so the corpus runs in a few seconds apart from `feedback_runaway`.

---

## 7. What it does not cover

[Design §12.3](design/play-mode-architecture.md#123-validate-world) lists more than this tool runs. These checks exist
elsewhere in `tests/run_tests.sh` and are run on the factory and test Worlds, not on the World you hand to the tool:
- the decompile → compile round trip (`tests/worldc_test.py`);
- the 10,000-case corruption fuzz of the firmware parser under ASan/UBSan (`tests/world_test.c`);
- every Smart Keys note in the scale and inside the role range for every chord, and no hanging notes
  (`tests/smartkeys_test.c`, `world_render --mash`);
- `world_render --extremes`' ENERGY loudness rule.

`worldc check` stays the quick static check, with the same model; `validate-world` wraps it (it uses the same compiler
and model, in the same process) and adds the budgets and the sweep. The Phase 15 authoring tool runs `validate-world
--json` (static first, then quick or full) as a background job and shows the items and the budgets:
[authoring.md §5](authoring.md#5-validating).
