<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Hardware calibration (Phase 17)

What only the device can tell us, and how to turn the owner's numbers into constants. Run it after (or during) the
session of [hardware-test-plan.md](hardware-test-plan.md); the plan's T7 collects the data this page fits.

Tools: the console command `flow` (`firmware/src/console.c`) and `tools/fm1_monitor.py`, which logs it to CSV, fits it
(`--fit`), reads flash (`--dump-flash`) and sends read-only commands (`--cmd`). The monitor's tests need no device:
`python3 tests/fm1_monitor_test.py`.

## 1. The CPU guard, in the terms of the data

| Name | Now | Where | Meaning |
| --- | --- | --- | --- |
| `cpu_q8` | measured | `audio.c` | The audio interrupt's time over one DMA half (256 samples at 44.1 kHz = 5,805 µs), 256 = 100 %, smoothed over 16 halves. `flow`: `cpu_pct`, `cpu_q8`. |
| `est` | model | `guard.c` `guard_cpu_est` | What the guard thinks the sounding voices and stages cost, in **host instructions a sample** (`GU_VCOST`, `GU_COST_*`, fitted on the host sweeps). `flow`: `est`, and 0 outside a World. |
| `GL_CPU_FULL` | 2,700 | `guard_limits.h` | The host instructions a sample that the guard takes as 100 % of the interrupt: `load = max(cpu_q8, est × 256 / GL_CPU_FULL)`. A placeholder. |
| `GL_CPU_BUDGET` | 2,300 | `guard_limits.h` | The most a World may cost on the host anywhere in its macro space (`guard_sweep`, `validate-world`). It is 85 % of `GL_CPU_FULL`. |
| ceiling | 85 % | World `guard.cpu.ceiling` | Over it for `GL_CPU_HOLD` (8) halves: the guard holds (DIST, DUST, GRAIN DENS, SLICER back to base, UNISON at 2 voices). Under the release level (80 %) for `GL_CPU_RELEASE` (345 halves, 2 s): it lets go. |
| shed | 85 % of a half | `audio.c` | A half over 4,934 µs sheds a voice (`shed` in `flow`). |
| `late` | measured | `audio.c` | Halves the DMA outran: an audible dropout. Must stay 0. |

Two things follow. The guard acts on whichever is larger, so a `GL_CPU_FULL` that is **too high** only makes the
estimate blind (the measured `cpu_q8` still protects, after 8 halves); one that is **too low** makes the estimate cry
wolf and hold a World that has room. And the factory Worlds' worst host corner is 2,072: it stays under the ceiling on
the device only if `GL_CPU_FULL` is truly at least 2,072 / 0.85 = **2,440**. If the fit says less, the Worlds, not the
constants, have to give.

## 2. Procedure

### 2.1 Record (about 40 minutes)

Stopped, then playing, with the monitor running the whole time and a tag typed before each item (`NR-B-all100`):

```
python3 tools/fm1_monitor.py --out ~/fm1-session/csv/cal.csv --tag idle
```

- [ ] **Idle:** a World loaded, stopped, 20 s (`idle`). Then each World playing its default scene, untouched, 30 s.
- [ ] **The grid, heaviest World first.** The host sweep ranks them NEON RAIN (mean 1,612, costliest half 2,055),
  FROZEN LAKE (1,585 / 2,023), MIDNIGHT DRIVE (1,343 / 1,675), DUSTY CAFE (1,120 / 1,657): `tools/validate-world
  neon_rain midnight_drive frozen_lake dusty_cafe` prints it again. For each World, each scene A to D, one corner at a
  time, **at least one whole phrase (8 to 16 bars) per corner** so every pattern step is heard:

  | Corner | Tag suffix |
  | --- | --- |
  | COLOR, MOTION, SPACE, ENERGY all 0 | `all0` |
  | the World's defaults (after a load) | `def` |
  | all four at 100 | `all100` |
  | ENERGY 100 alone, then ENERGY 100 + SPACE 100 | `e100`, `e100s100` |
  | MOTION 100 alone | `m100` |
  | hold FX: FILTER at 0 and at 100, CRUSH 100 (DUST), ECHO 100, FREEZE 100 | `fx-filter`, `fx-crush`, `fx-echo`, `fx-freeze` |
  | hold FX and press white keys (punch effects) | `fx-punch` |
  | SOUND SHAPE and MOVEMENT all at 100 | `shape100`, `move100` |
  | PULSE DRIVE, BEAT BUSY | `pulse-beat` |
  | four fingers on the keys, black and white, held | `keys4` |
  | a recorded loop, overdubbed | `rec` |

- [ ] **Beyond the factory range.** The fit must not extrapolate to 85 %, and the Worlds stop at about 2,100. In
  ADVANCED (a World is still active, so `est` is computed) load the CPU up on purpose, tag `adv-stress`: the costliest
  engines on all three synth tracks with 4 to 8 voices held (the heaviest `GU_VCOST` entries, `guard.c`), DIST on every
  track, GRAIN DENS up, SLICER on, a dense drum pattern. Aim for `est` 2,300 to 2,800. Stop raising it when `late`
  moves or `cpu_pct` passes 95: that is the limit found, not a fault.
- [ ] **Anchor in SLOOP mode** (LEAVE WORLD): play the densest SLOOP project you have, tag `sloop-*`. `est` is 0
  there; the rows only show the loads SLOOP 2.1 is known to carry.
- [ ] Stop the monitor (Ctrl-C). Keep the CSV and its `.log` (resets, crash records).

### 2.2 Fit

```
python3 tools/fm1_monitor.py --fit ~/fm1-session/csv/cal.csv            # ceiling 85 %, budget 10 % under it
```

It fits `cpu_q8 = a × est + b` over the rows with a World sounding, then:

- the estimate at which the device reaches the ceiling, `est_c = (0.85 × 256 − b) / a`;
- **`GL_CPU_FULL = est_c / 0.85`** (so `est × 256 / GL_CPU_FULL` equals the measured load at the ceiling, where it matters);
- **`GL_CPU_BUDGET = 0.90 × est_c`** (`--margin` changes the 10 %: a validated World then stays under the 80 % release
  level, the guard never flaps on it);
- the guard's load against the measured one over the top rows, and a table by tag: mean and peak `cpu_q8`, peak `est`,
  `max_us`, guard rows. The heaviest corner of the heaviest World is the top line.

Reading it:

| Result | Means |
| --- | --- |
| R² ≥ 0.9, `est` from about 600 (idle) to over 2,000 | the model is usable: take the proposal |
| intercept `b` over about 15 (6 %) | the interrupt carries fixed work the model does not know (timer scan, USB): normal up to that, tell the owner above |
| R² < 0.9, or one World 15 % off the line | the per-voice costs are wrong for some engine: 2.4 |
| `a` ≤ 0 or no spread | not enough varied data: record more |
| any tagged corner with peak `cpu_pct` ≥ 85, `shed` > 0 or `late` > 0 | a real overload: lighten that World or lower the budget, whatever the fit says |

Device time per estimate unit, for the record: `22,676 ns × (cpu_q8 / 256) / est` (22.7 µs is one sample at 100 %).

### 2.3 Apply

- [ ] `firmware/src/guard_limits.h`: `GL_CPU_FULL` and `GL_CPU_BUDGET`, and the comment ("Phase 17 measures" becomes
  the date, the build and the fit).
- [ ] Numbers that repeat them: `tests/validate_test.py:84` (`<= 2300`, `<= 2700`), `docs/guardrails.md` (§2.1 table and
  note, the rows on CPU in §1, §6 and §7), `docs/validation.md` (§3 table and the examples), `docs/design/play-mode-architecture.md`
  (the CPU guard row, §14's budget list). `tools/validate_world.py` and `tests/guard_sweep.c` read the header.
- [ ] If the new budget is under the factory Worlds' worst corner (2,072), choose on the device's numbers: the
  measured peak of that corner under 80 % means the Worlds are fine and the budget is a margin you may relax to the
  corner; over 80 % means lighten the World (Phase 18's library follows the rule from the start).
- [ ] `./tests/run_tests.sh` (guard_test, guard_sweep, validate_test), `./build.sh`, install, and repeat 2.1's grid on
  the four Worlds: the peak `cpu_pct` under the ceiling, `guard` 0, `shed` 0, `late` 0.

### 2.4 If the estimate itself is off

`GU_VCOST` (per voice, per engine) and `GU_COST_*` were fitted on the host's instruction counts, and the device's cost
per engine need not be proportional. Per engine, in ADVANCED: one synth track on that engine, hold 1, 2, 4, 8 notes
(`voices` in the CSV), tag `eng-NAME`. The slope of `cpu_q8` over `voices`, divided by `a`, is that engine's cost in
estimate units; replace the `GU_VCOST` entry when it differs by more than 20 %. `tests/target_budget.py` counts the
pi32v2 instructions of each engine's render loop and is a second opinion on the ratios.

### 2.5 Exercise the guard on purpose (optional, a scratch build)

Set `guard.cpu.ceiling` to 0.30 in a copy of `worlds/factory/neon_rain.world.json`, build, install. Play scene B with
ENERGY and SPACE at 100: `guard` goes to 1 after about 8 halves (50 ms), DIST, DUST and GRAIN DENS return to their
base, a UNISON part drops to 2 voices, and `guard` returns to 0 about 2 s after `cpu_pct` falls under 28 %. Install
the real build again afterwards.

## 3. Other hardware-only items

### 3.1 Display timings

The PLAY screens redraw by bands over 12 MHz SPI; the figures below are inferred, not measured.

| Item | Expected | How |
| --- | --- | --- |
| A knob detent's bar | about 4 ms of SPI | 240 fps phone video of the screen, one detent of K1: frames from the knob to the new bar; at most 2 frames at 60 fps |
| A full-screen redraw | about 77 ms | the black frames on entering ADVANCED (and SLOOP): about 18 frames at 240 fps |
| HOME at rest | 0 redraws: no flicker | 30 s of video |
| Tearing | none (no TE line) | the activity bars on HOME at 15 fps, a fast K1 spin: no torn frame |
| UI frame rate | main-loop dependent | `--cmd dbg` twice, 10 s apart: `ui_frames` (hex) difference / 10 |
| Black flash on a PLAY overlay change | none | CHOOSE WORLD, SCENES, pages: no full black frame |

### 3.2 The loader and the Flowstate region

The loader writes only `0x004000–0x092FFF`. After a successful pass it also scans `0x093000–0xFBFFF`: at offset 0xF00 of
every 4 KiB sector it reads 80 bytes and, if they look like an update record, erases the sector
(`firmware/loader/ldr_core.c ldr_records_drop`); the SPL treats such a sector as an update request at boot. Flowstate
programs nothing at or above 0xF00 of a sector in `0xE5000–0xFBFFF` (user Worlds at most 3,584 B, the session), so those
bytes stay erased and never look like a record. To confirm on the device (stopped, nothing playing):

```
python3 tools/fm1_monitor.py --dump-flash 0xE5000 0x17000 ~/fm1-session/flash-before.bin    # 0xE5000..0xFBFFF
# install the same package again (web installer, or the rescue path): the plan's sections 2 and 3
python3 tools/fm1_monitor.py --dump-flash 0xE5000 0x17000 ~/fm1-session/flash-after.bin
cmp ~/fm1-session/flash-before.bin ~/fm1-session/flash-after.bin && echo identical
python3 -c "import sys;d=open(sys.argv[1],'rb').read();print([hex(0xE5000+i) for i in range(0,len(d),4096) if d[i+0xF00:i+0x1000]!=b'\xff'*256] or 'offset 0xF00 erased in every sector')" ~/fm1-session/flash-after.bin
```

- [ ] Before the first dump make the region non-trivial: two user Worlds saved (T8), the session saved (stopped 25 s).
- [ ] `cmp`: identical. (The OTA staging area `0xE0000–0xE4FFF` below it may change with an install:
  `--dump-flash 0xE0000 0x5000` before and after if you want to see it.)
- [ ] The second command: every sector of the region has 0xF00–0xFFF erased.
- [ ] The user Worlds still list and load after the install and after ten power cycles (the SPL's boot scan).
- [ ] The same dump pair around a **rescue** install (OCT− at power-on) and around a **different** package (a rebuild
  whose app is larger or smaller).
- [ ] Any difference: stop, photograph, keep both dumps and report; do not save more Worlds.

### 3.3 Key to sound latency

Expected: key scan debounce 3.3 ms, then the key is taken at the next block (0.7 ms), rendered in a half (5.8 ms) and
played from the DMA half after (another 5.8 ms): 8 to 16 ms. Measure with a two-channel recording: the line out on one
channel, a small mic or contact pickup against the key on the other; hit a white key hard, in a World whose Smart Keys
sound has a fast attack (SOFT at 0); measure click to onset in a DAW over 20 hits. Pass at 20 ms or less; record the
mean and the spread. Repeat with PULSE DRIVE (an arp starts on the grid, not on the press).

### 3.4 Also record

- [ ] **Level against the simulator.** The simulator and the device both leave 6 dB of headroom (`OUT_SHIFT`); the
  renders of `sloop-render` are 6 dB hotter. Device peak and RMS for the plan's comparison sheet (section 17), the
  ratio per World, and whether the ratio is constant. A constant offset is the codec's analog gain; a ratio that
  varies by World is a finding.
- [ ] **Pitch.** A tuner on the C4 white key of NEON RAIN (tonic D): within 5 cents, the same at a high and a low
  octave (the 44.1 kHz clock).
- [ ] **Noise floor** at MASTER full, stopped, 10 s recorded: RMS dBFS, and audible or not on headphones.
- [ ] **Console cost.** `cpu_pct` of an idle World with the monitor polling at 250 ms, and with it stopped, 20 s each: the
  difference should be under 1 point.

## 4. Bring back

`cal.csv` and its `.log`, the `--fit` text, the two flash dumps and `cmp` result, the video of the display items, the
latency recording, the filled-in plan, and the build (`git rev-parse --short HEAD`, `shasum -a 256 build/felucca.fwsc`).
From them: the new `guard_limits.h` values, the table updates of 2.3, and a Phase 17 entry in `progress.md`.
