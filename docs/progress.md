<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Progress log: the Musical World firmware

One entry per phase of the plan, newest last. Each entry lists what was done, the key decisions, how it was
verified, the risks, the commits, and what comes next.

Main references:
- The architecture of SLOOP as forked: [architecture-audit.md](architecture-audit.md)
- The design that the later phases follow: [design/play-mode-architecture.md](design/play-mode-architecture.md)

---

## Phase 0: Architecture audit (2026-10-04)

**Done.** A read-only audit of SLOOP 2.1 (`e421e43`), recorded in [architecture-audit.md](architecture-audit.md).
It covers:
- the code layers, the dependency map and the audio, sequencer and UI paths;
- which code compiles on a host and which is hardware-bound;
- flash and RAM budgets;
- insertion points for Smart Keys, macros and guardrails;
- the simulator recommendation, licensing, 15 ranked risks, and the answers to all 31 questions.

**Verified.**
- The firmware builds.
- `tests/run_tests.sh` passes all 24 groups.
- The app image matches the published 2.1 apart from the build-date stamp.

**Key findings.**
- **Voices are keyed by pitch.** Two sounding notes of the same pitch cut each other off, which must be fixed before
  Smart Keys.
- **Sections A–D are the user's own project slots.**
- **Flash:** 75 KB free.
- **RAM pool:** nearly full.
- **CPU baseline:** stale.

**Commit:** `622ad2e`.

## Phase 1: Mac development environment (2026-10-05)

**Done.**
- `tools/setup-macos.sh`: one idempotent setup command.
- Pinned and SHA-256-checked downloads: the JieLi toolchain (`tools/get_toolchain.sh`) and the three AC79 SDK files
  (`tools/fetch_sdk.py`).
- The build container image is pinned by digest.
- `tools/build.py --gen-only` and `tests/run_tests.sh --host-only`: development without Docker.
- `SOURCE_DATE_EPOCH` gives reproducible builds.
- The CPU baseline was rebaselined: all 54 presets are checked again.
- [macos-development.md](macos-development.md).

**Verified.**
- A date-pinned build is byte-identical to the published `sloop-2.1.fwsc` (SHA-256 `acf2c754…`).
- The full test suite and `--host-only` both pass.
- A fresh setup run worked end to end.

**Behaviour change:** none.

**Commits:** `3f22947` to `c111286`.

## Phase 2: Host audio renderer (2026-10-05)

**Done.**

- **`host/`: the one shared host platform layer.**
  - `hal_host.h` stands in for the hardware and reuses the real `fm1_input.h`, `fm1_adc.h` and `fm1_flash.h`, so
    the key map, LEDs and flash write windows are the device's.
  - `core.c` is the host unity root, with the firmware sources included in `felucca.c` order.
  - `host.h` is a small API.
  - `Makefile` builds into `build/host-bin/`.
- **Flash.** A 1 MiB NOR image, optionally file-backed, runs the real `storage.c`, `project.c` and `upreset.c`.
- **The 64-bit hazard at `eng_sample.c:89`** is handled on the host side. The flash image is a static array placed
  above `SMP_DATA`, with an abort if that ever fails. The firmware is untouched.
- **`sloop-render`.** Renders a SLOOP project (FUN4; FUN1–3 are converted) or a song of up to 4 sections to WAV
  through the real transport. Options: `--bars`, `--seconds`, `--tail`, `--analyze`, `--check`, `--dump`, `--screen`.
  It runs about 200× realtime and the output is deterministic.
- **`sloop-examples`** builds three example projects with the real firmware API into `examples/projects/`:
  - **ambient:** 70 BPM, D major
  - **groove:** lo-fi house, 120 BPM, A minor
  - **cinematic:** 72 BPM, C minor

**Design.** [design/play-mode-architecture.md](design/play-mode-architecture.md) covers Phases 3–18. Its 16 decisions
(D1–D16) include:
- macros as a sparse overlay applied and restored inside the audio interrupt;
- the `FELUCCA_WORLD` flag, with SLOOP mode bit-identical;
- JSON World sources compiled to `FWD1` blobs of about 2 KB;
- World scenes kept separate from the user's project slots;
- white keys on the scale and black keys on the chord tones;
- a pitch reference count;
- user Worlds stored in the unused flash region.

It was aligned with the owner's UI spec, now saved as [spec/flowstate-ui-spec.pdf](spec/flowstate-ui-spec.pdf)
with a text copy alongside (design §16). The product is **Flowstate**, and the demo Worlds are Neon Rain,
Midnight Drive, Frozen Lake and Dusty Cafe.

**Verified.**
- The firmware package is still byte-identical to the published 2.1 (`SOURCE_DATE_EPOCH`).
- `./tests/run_tests.sh` passes all 25 groups (new: the host renderer group). The 83 golden renders are unchanged.
- `--host-only` passes.
- The three examples render clean: peaks between −3.2 and −4.1 dBFS, no full-scale samples, DC below 0.0005,
  identical bytes on a second render.

**Risks / open.**
- The existing tests still use their own stubs, not `hal_host.h`. Migrating them is optional and must keep the
  golden hashes.
- On the host there is no CPU model (no voice shedding, `cpu_q8` stays 0) and no MIDI or editor connection.

**Next.** Phase 3: real-time host audio. SDL2, a firmware thread feeding a lock-free FIFO, keyboard commands, and
measurements of latency, underruns, CPU and timing.

## Phase 3: Real-time host audio (2026-10-05)

**Done.** `build/host-bin/flowstate-sim` (`host/sim/`) runs the SLOOP core in real time on macOS through SDL2.

**Threads.**
- One firmware thread runs the core in lockstep, as the device's single core does: audio blocks, plus a main-loop
  pass every 22 blocks. It writes into a lock-free FIFO.
- The SDL audio callback only drains the FIFO.
- The main thread polls input and draws from a double-buffered snapshot.
- The firmware and sink threads use the macOS real-time thread policy, so they wake up about 20 µs late instead of
  1–7 ms.

**Controls.** The FM-1 panel is mapped to the computer keyboard by key position. Option-key commands give PLAY/STOP,
scene A–D on the next bar, mute tracks 1–4, filter and tempo. K1–K4 stand in for the macros until Phase 7.

**Options:** `--project`, `--demo`, `--flash` (persists the NOR image), `--buffer`, `--fifo`, `--headless`, `--fast`,
`--mute-output`, `--script` (timed commands and expectations), `--stats`, `--wav`, `--screen`, `--shot`.

**Measured (M1 Pro, built-in speakers at 48 kHz).**
- 0 underruns over 60 s at the default 672-frame buffer.
- Latency the simulator controls: about 32 ms. SDL2 keeps at least 30 ms queued in its AudioQueue, so 20–25 ms is
  not reachable with SDL. The speakers add 28 ms.
- CPU: about 1.4 % of realtime.
- The speaker clock drifts 5–11 ppm.

The full table is in [simulator.md](simulator.md).

**Verified** on the commit alone, in a clean worktree:
- A date-pinned build is byte-identical to the published 2.1.
- `./tests/run_tests.sh` passes all 26 groups. The new simulator group checks scripted PLAY, scene on the bar, mutes,
  tempo, filter, audio, the exact frame count and a clean exit, and that the real-time output is bit-identical to
  `--fast`.

**Risks / open.**
- The latency floor of SDL2 on macOS. Writing directly to CoreAudio's output unit could reach about 10 ms plus the
  device, if that is ever needed.
- On the host there is still no CPU model and no MIDI.

**Commits:** `7b21d70` to `51a95f4`.

**Next.** Phase 4: the FLOWSTATE STUDIO simulator UI (UI spec §12). Phase 5 (the World format) is already being
built in parallel.

## Phase 4: Basic simulator UI (2026-10-05)

**Done.** `flowstate-sim` opens on a **FLOWSTATE STUDIO** view laid out as UI spec §12
(screenshot: [images/studio.png](images/studio.png)). It contains:
- **The World card and chooser.** You highlight a World and load it, on the next bar while playing (design D10).
- **Scene buttons A–D**, with `NEXT BAR` / `CHANGES NEXT BAR` feedback.
- **The four macro knobs** (COLOR, MOTION, SPACE, ENERGY) with the spec's end words.
- **Performance buttons:** PLAY, REC, PULSE, BEAT and FX, with their LEDs.
- **Track strips:** mute, level and sound.
- **The keys line** and a 27-key keyboard.
- **The live 240×240 device screen.**
- **A developer inspector** (Option+I).

Tab switches to the full FM-1 panel (ADVANCED).

All of it runs from one studio model (`host/sim/studio.c`). Until the World runtime is wired in, that model fills
it with stand-ins:
- the example projects act as Worlds, and SLOOP sections as scenes;
- the macro knobs drive KNOB 1–4.

Features that arrive later carry honest labels: VAR shows "PHASE 12", SMART MELODY shows "PHASE 6".

**Verified** on the commit alone, in a clean worktree:
- A date-pinned build is byte-identical to the published 2.1.
- `./tests/run_tests.sh` passes all 26 groups. The simulator group now also scripts the Studio: a World chosen while
  another plays loads on its next bar; then scene, mute, level and macro; the Studio and ADVANCED views are drawn.

**Commits:** `5e31230` to `aaf8663`.

**Next.** Phase 5: Musical World data format. The core (format, compiler, firmware parser) is already built; the
four demo Worlds are being composed.

## Phase 5: Musical World data format (2026-10-05)

**Done.**

- **Format.** A normative byte-level spec, [design/fwd1-format.md](design/fwd1-format.md), and
  `firmware/src/world_fmt.h`, which holds the same constants. The compiler parses that header, so the format has one
  source of truth.
- **Compiler.** `tools/worldc.py`, commands `compile` / `decompile` / `check` / `names`. It reads World JSON
  ([worlds.md](worlds.md)): sounds by engine and preset name, parameters by label, drum lane strings, melodic
  degrees, chord tokens resolved against each progression, roman-numeral progressions, scenes, variations, macros,
  cross-macro rules, ENERGY tables, Smart Keys, guard and defaults.
- **Name resolution.** All names come from the firmware's own tables, via a committed
  `worlds/schema/sloop-params.json` generated by `tools/dump_params.c`.
- **Build.** `tools/gen_worlds.py` embeds `worlds/factory/*.world.json` into `build/gen/felucca_worlds.h`.
- **Firmware: `world.c` / `world_rt.h`, behind `FELUCCA_WORLD`.**
  - A bounds-checked blob parser.
  - A RAM pattern pool.
  - Stage (base → variation → scene → overrides) and commit while stopped.
  - It never touches the user's project slots.
  - Hooks, inert with no World active: `world_block`, the autosave fence, `world_boot`.
  - `preset_fill()` was factored out of `apply_preset_to`, bit-identically.
- **The four demo Worlds** of UI spec §11:

  | World | Category | Key | Tempo | Size |
  | --- | --- | --- | --- | --- |
  | NEON RAIN | cinematic | D minor | 72 BPM | 1,356 B |
  | MIDNIGHT DRIVE | synthwave | A minor | 100 BPM | 1,786 B |
  | FROZEN LAKE | ambient | E major | 66 BPM | 1,297 B |
  | DUSTY CAFE | lo-fi, swung | F major | 82 BPM | 1,428 B |

  Each has 4 scenes, 4–5 variations, ENERGY tables, macro mappings and the three cross-macro rules. All material is
  original.

**Verified.**
- `./tests/run_tests.sh` passes all 31 groups. New:
  - worldc unit tests;
  - FWD1 parser tests, including every truncation, every byte flip and 20,000 random corruptions under ASan/UBSan;
  - "World modules never name `proj_slot`";
  - the regression suite rebuilt with `FELUCCA_WORLD=1`, matching the **same** 83 goldens;
  - every factory World × scene × variation rendered through the real firmware (`tests/world_render.c`): clean
    (peak ≤ −3 dBFS), every note in the scale, registers respected, ENERGY monotonic, macro ranges safe, default
    loudness within 0.9 LU.
- `--host-only` passes.

**Sizes.**
- Firmware image: 516,452 B (+10.2 KB, including 5.9 KB of World data); about 65 KB of flash is left.
- `.bss`: +16 B today. About 11 KB more once the Phase 9 UI calls the whole World API.

**Notes / risks.**
- Until Phase 11, scenes change only while stopped, and ENERGY bands are only emulated in the test renderer.
- The factory index is sorted by category, so Phase 9 must pick NEON RAIN for first boot by id (`0x4eee4454`).
- UBSan flags a pre-existing signed left shift in `eng_analog.c:82`. It is harmless on the target and was left
  untouched.
- The compiler bug found while authoring (a variation swap whose source has no chord tokens, spanning several
  progressions) is now refused with a clear message and covered by a test.

**Commits:** `e0af0e6` to `92e4214`.

**Next.** Phase 6: Smart Keys. It starts by wiring the World runtime into the host core, renderer and Studio so the
Worlds can be heard in the simulator, then the pitch reference count, then SMART MELODY.

## Phase 6: Smart Keys (2026-10-05)

**Done.**

**Harmony.** `firmware/src/harmony.c` follows the committed progression per audio block. It names the chord in the
World's key ("Bbmaj7", "Dmadd9") and gives the chord-tone masks.

**SMART MELODY.** `firmware/src/smartkeys.c` implements design D7 and UI spec §4.
- **White keys** play the World's melody scale (pentatonic by default). The C4 key is the tonic, and they never move.
- **Black keys** play the current chord's tones, ascending in the same register.
- **Range and octaves:** notes are folded into the role range; OCT moves by octaves.
- **Note guard:** a polyphony cap.
- **MIDI in:** mapped, and remembered per incoming note.
- **PULSE with one key held:** arpeggiates chord tones.
- **Held notes** keep exactly what they sounded, whatever happens to the chord, the octave or the scene.

**Pitch reference count** (design D8; fixes audit R1). Notes of the same pitch no longer cut each other: two keys, or
a key and the sequencer.
- **Exactness:** the live-input counts, slides, ratchets and panic are handled so the counts stay exact.
- **Voice budget:** an existing SLOOP hole let a same-pitch voice reuse exceed the 8-voice budget. It is fixed in
  World mode.
- **Scope:** all of this applies only while a World is active.

**Worlds in the host tools.**
- `host/core.c` builds with `FELUCCA_WORLD 1`.
- `sloop-render --world NAME|FILE --scene --var` and `--world-sequence`.
- The Studio:
  - opens on NEON RAIN;
  - lists the factory Worlds, then the SLOOP projects;
  - uses real scene and variation names, applied on the next bar;
  - shows role-named tracks and "KEYS: SMART MELODY" with the current chord;
  - hot-reloads a World JSON while playing.
- `world_request` applies a scene/variation immediately when stopped and on the next bar while playing.
- A World switch while playing happens on the bar as stop → load → start. Seamless switching is Phase 11.

**Verified.**
- `./tests/run_tests.sh` passes all 33 groups. New:
  - `smartkeys_test`: about 20 M checks, ASan/UBSan, against an independent model of design §4.1, covering every key
    over every chord of every World. Nine deliberately injected bugs were all caught.
  - `unity_order_test`.
  - Random key-mashing over all 80 scene × variation renders: 0 notes out of key, about 60 % of them chord tones.
  - Scene, variation and World switching on the bar in the simulator.
- Both regression builds match the 83 goldens.
- `--host-only` passes.

**Sizes.**
- Image 517,992 B.
- RAM `.data` + `.bss` 64,656 B of 98,304 B. The World pattern pool (10 KB) is now reachable.

**Deviations from the design**, all inert in SLOOP mode:
- The note guard runs where the note is chosen (`key_down` / MIDI), not inside `input_on`.
- A second table, `vlive`, counts the notes the live input started.
- A range under an octave is widened to one octave.
- Smart Keys ignore the keys track's transpose and chord mode.

**Left for later.**
- SMART CHORDS / BASS / DRUMS need product decisions (target track, loop, SCL selector). Their hooks are in place.
- Key LEDs and the mode selector: Phase 9.
- Moving the note guard into `guard.c` and loop-follow: Phase 8.

**Commits:** `89a69be` to `2806141`.

**Next.** Phase 7: the performance macro engine (COLOR, MOTION, SPACE, ENERGY).

## Phase 7: Performance macro engine (2026-10-05)

**Done.**

**Macro engine** (`firmware/src/macro.c`, design §5).
- **Controls.** COLOR, MOTION, SPACE and ENERGY are positions 0–1000, taken from the World's defaults.
- **Evaluation, `macro_eval`** (main loop, on change, at most 60 Hz):
  - runs every mapping through its curve: lin, exp, log, s, late or a 9-point LUT;
  - resolves `@ROLE` against each track's engine;
  - adds the cross-macro rules, scaled by how far past their thresholds the controls are;
  - clamps to the parameter descriptors and to the hard limits in `guard_limits.h`;
  - publishes an overlay table of at most 48 slots.
- **The overlay, inside the audio ISR.** Each slot is smoothed (fast / medium / slow / stepped). The effective value
  `clamp(base + offset)` is written into `p[]`/`g[]` for the block, then the authored base is restored. The UI,
  editor and autosave never see macro values.
- **Smooth targets.** `~bright`/`~shape` reach the engines as per-voice offsets.

**ENERGY as arrangement** (`firmware/src/arrange.c`). The scene's ENERGY band, with hysteresis:
- mutes layers through SLOOP's mute fade, on the bar;
- masks drum lanes, density and synth steps, on the beat;
- plays fills on phrase ends.

The Smart Keys track is never muted.

**Host.**
- The Studio's four knobs drive the real macros.
- The Option+I inspector shows each macro's hidden mappings with live values
  ([images/inspector.png](images/inspector.png)).
- `sloop-render --ctl` and `--sweep`.

**Factory Worlds tuned.** The all-zero macro corner of three Worlds was about 22 LU under their defaults. COLOR's
minimum is now smaller and ENERGY no longer darkens, so the corner sits 2–7 LU under. The defaults are unchanged.

**Verified.** `./tests/run_tests.sh` passes all 34 groups.

`macro_test` (about 5 M checks, ASan/UBSan) covers:
- inert behaviour with no World;
- 625 macro combinations per World against an independent model;
- ranges, hard limits, and "100 % is never all-max";
- the base restored after every block;
- rules at their thresholds;
- smoothing;
- ENERGY timing and hysteresis;
- slot overflow, using a new test World.

24 injected bugs were all caught.

Every World also renders clean at every macro extreme:

| World | Worst peak | Slowest tail 6 s after STOP | ENERGY 0→1: notes | ENERGY 0→1: loudness |
| --- | --- | --- | --- | --- |
| NEON RAIN | −4.1 dBFS | −78 dBFS | 2.8× | −1.2 LU |
| MIDNIGHT DRIVE | −3.1 dBFS | −81 dBFS | 8.6× | +4.8 LU |
| FROZEN LAKE | −4.5 dBFS | −66 dBFS | 5.0× | −0.5 LU |
| DUSTY CAFE | −3.5 dBFS | −78 dBFS | 2.7× | +1.0 LU |

ENERGY makes the music fuller, not louder. Both regression builds match the 83 goldens.

**Sizes and cost.**
- Image 520,424 B (+2.4 KB).
- RAM `.data` + `.bss` 66,880 B of 98,304 B.
- The overlay costs about 1.3 instructions per moving slot per sample, at most 1.6 % of the heaviest mix.

**Deviations** (design §5.8):
- The overlay is applied before `events_block`, so mappings read at note time (such as gate) work.
- `arrange.c` (including fills) and `guard_limits.h` moved forward into this phase.

**Commits:** `9af542f` to `f1c7b8c`.

**Next.** Phase 8: the Musical Guardrail Engine. `guard.c` with soft caps, sound ranges and combinations, the note
guard moved there, read-site clamps, a CPU guard, and exhaustive macro sweeps checking clipping, runaway feedback,
silence, CPU and invalid parameters.

## Phase 8: Musical Guardrail Engine (2026-10-05)

**Done.**

**`firmware/src/guard.c`** has three areas.
- **Notes.** The note guard (range fold and polyphony cap) moved here from Smart Keys. Loop follow: an avoid note in
  the keys loop plays the nearest chord tone. The record-guard entry points that Phase 10 will call are implemented.
- **Sound.**
  - The World's GUARD soft caps and ranges apply inside each macro slot.
  - A limit on how many tracks may distort at once.
  - Six default sound combinations (for example, a big room with a long echo caps the echo), or the World's own,
    now carried in the format.
- **CPU.** UNISON and GRAIN density caps. Above the ceiling, the costly slots glide back to their base.

**Read-site clamps (H6).** The bus globals, DUST, DIST and every engine EDIT value are clamped against the hard limits
and descriptors at the point where they are read. They protect SLOOP mode too, and are no-ops for in-range values.

**`tools/worldc.py`** gains an integer-exact Python model of the macros and guard (`worldc model`). `worldc check` now
refuses:
- out-of-range or over-limit mappings;
- saturation at 100 % without an explicit `saturate`;
- more than 48 slots;
- Smart Keys ranges under an octave.

**World data fixes.** The dark variations of NEON RAIN, MIDNIGHT DRIVE and FROZEN LAKE went nearly silent at COLOR 0;
their cutoffs and levels are raised. FROZEN LAKE's echo tail is shorter. The ORIGINAL variations and defaults are
unchanged.

**Verified.**
- `./tests/run_tests.sh` passes all 35 groups. New: `guard_test`, `guard_sweep` (quick mode), `guard_mutants` (28
  injected bugs, all caught), and the Python model checked byte-identical to the C engine (13,020 slot tables).
- **Full sweep** (`SWEEP=full`): 14,800 points over every World × scene × the 4-D macro space. All clean:

  | Check | Result |
  | --- | --- |
  | Worst peak | −2.5 dBFS |
  | Tails 6 s after STOP | every one below −60 dBFS |
  | Silence while a layer plays | none |
  | Loudness jumps with a player | at most 7.6 LU |
  | CPU | at most 2,072 host instructions/sample; the guard never needed to engage |

- Both regression builds match the 83 goldens.

**Sizes.** Image 521,396 B (+1 KB).

**Deviations** (design §6.5):
- Sound combinations use the reserved GUARD byte 25, so existing Worlds get the six defaults.
- The H6 clamps cover more parameters than the design named.

**Left for later.**
- The PLAY REC record guard (Phase 10).
- LIVE ECHO's feedback limit (Phase 13).
- The `validate-world` wrapper (Phase 16).
- Calibrating the CPU budget against the device (Phase 17).
- Two Worlds pass the tail check by less than 1 dB, so a DSP change that lengthens tails will flag them.

**Commits:** `16cb839` to `d0785fd`.

**Next.** Phase 9: the PLAY MODE UI on the device: home, World browser, scenes, variations, macro overlay, record and
live FX screens; key LEDs; long-hold EDIT for Advanced Mode; first boot into NEON RAIN.

## Phase 9: PLAY MODE UI (2026-10-05)

**Done.** The device now boots into **PLAY MODE**. First boot opens NEON RAIN on the FIRST screen.

**Screens.** `firmware/src/ui_play.c` implements every UI spec screen with its texts:

| Screen | What it shows |
| --- | --- |
| FIRST | FLOWSTATE READY, PRESS PLAY |
| HOME | FLOWSTATE, World, category and scene, activity bars, KEYS: SMART MELODY, the four macros |
| Macro overlay | DARK–BRIGHT etc. bar |
| CHOOSE WORLD | list; PLAY confirms, on the next bar while playing |
| SCENES | CHANGES NEXT BAR |
| VARIATION | SAME WORLD · NEW FEEL |
| RECORDING, LOOP | the record states |
| PULSE, BEAT | lists |
| LIVE FX, SOUND SHAPE, MOVEMENT | four-knob pages |
| ADVANCED | ADVANCED MODE dialog |

Screenshots: `images/play-*.png`. Glyphs the fonts lack (▶ ■ ● ━ ✦ ⌁ › and the waveform) are drawn by hand.
Redraws are limited to changed bands: a knob detent costs about 4 ms of SPI, and HOME at rest costs 0.

**Controls.**
- PRESETS = World; SELECT = scene (GLO + SELECT = tempo within the World's range); ALGORITHM = variation.
- K1–K4 = the macros.
- ARP = PULSE (drives the keys track's arp); SEQ = BEAT.
- FX / ENV / LFO held = their pages.
- EDIT held 2 s untouched = the ADVANCED dialog.

**LEDs:** the beat, the REC states, pressed keys, and the current chord's tones dim.

**Modes.** PLAY, ADVANCED (the SLOOP UI over the World) and SLOOP. Entering PLAY parks the SLOOP project, and
LEAVE WORLD restores it **bit-identically** (tested).

**Persistence.** `world_store.c` saves the session (World, scene, variation, controls, PULSE/BEAT, octave, tempo,
mode) in the newly writable flash region `0xE5000–0xFBFFF`. Nothing is written past offset 0xF00 of a sector, which
the update loader scans for update records. SLOOP's autosave stays fenced while a World is active.

**Verified.**
- `./tests/run_tests.sh` passes all 36 groups. New: `ui_play_test`, 119 checks under ASan/UBSan, covering:
  - every screen and its texts;
  - the control map;
  - World, scene and variation on the bar;
  - timeouts;
  - the ADVANCED round trip;
  - LEAVE WORLD restoring bit-identically;
  - the session across a reboot;
  - LEDs;
  - a 20,000-frame random-input fuzz.
- Both regression builds match the 83 goldens, and the SLOOP UI tests pass.
- The fuzz found and fixed an out-of-bounds read in SLOOP's STEP page on the drum track.

**Sizes. Watch this.**
- The image is 548,764 B (+27.4 KB). About 8.2 KB of that is Phase 5–8 World code that is called for the first time
  now (before, the compiler dropped it as unused).
- About **32.8 KB** of the app slot is left.
- RAM `.data` + `.bss` 67,872 B of 98,304 B.
- Phase 18's 30-World library will need flash reclaimed: replacing the Hügelton drum one-shots (74 KB, required for
  licensing anyway) is the plan.

**Deviations** (design §8.6):
- Overlays use the whole middle of the screen.
- REC is a stand-in until Phase 10.
- BEAT swaps the drum pattern on the next bar, but the factory Worlds have no per-BEAT patterns yet.
- SAVE AS USER WORLD waits for Phase 14.
- The session is a plain 48-byte record rather than an FWD1 blob.

**Commits:** `6d22a20` to `6eeeddc`.

**Next.** Phase 10: simple record and overdub.

## Phase 10: Simple record / overdub (2026-10-05)

**Done.** `firmware/src/play_rec.c` follows UI spec §7.

**The flow.**
- REC while playing starts a take; REC while stopped arms, and a key or PLAY starts it.
- REC closes the take on the next bar. A REC just after a bar line closes on that line.
- The take becomes a 1-, 2- or 4-bar loop, depending on how many bars were played.
- REC then toggles overdub. REC held 1.5 s clears the loop.
- Beginners never see bars, steps or quantise settings.

**Gentle quantise.** Each note goes to its nearest step and keeps `(1 − quantize)` of its timing offset as micro-timing,
stored in spare step bits and played back within one block. For example, FROZEN LAKE (quantize 0.5) keeps about half
of a player's timing feel.

**Record guard.** Every recorded note goes through it:
- in key, recorded exactly as it sounded;
- no duplicate notes;
- a polyphony cap;
- clamped note lengths, with no stuck ties.

**Undo.** Four layers on EDIT: a tap is UNDO, EDIT + OCT+ is REDO. The layers are swapped rather than overwritten.

**The loop:**
- keeps its phase across scene changes and follows the harmony;
- is saved with the session, at most 690 B;
- is cleared when you switch World;
- appears as normal steps in Advanced Mode.

**Screens and LEDs.** The RECORDING screen (bar dots) and LOOP n screen (the loop's activity, PLAYING / PLAYING +
REC, UNDO READY), and the REC/EDIT LEDs, follow the real state.

**Verified.**
- `./tests/run_tests.sh` passes all 37 groups. New: `play_rec_test`, 90 checks under ASan/UBSan, covering:
  - timing at three quantize strengths;
  - the guards;
  - the late REC press;
  - layers and undo;
  - scene and chord changes;
  - reboot and World switch;
  - Advanced Mode;
  - a fuzz.
- `ui_play_test` now has 126 checks.
- SLOOP's own REC and free take are unchanged, and both regression builds match the 83 goldens.

**Sizes.**
- Image 552,724 B (+3.96 KB). **28.8 KB** of the app slot is left.
- RAM `.data` + `.bss` 70,480 B of 98,304 B.

**Deviations** (design §9.1.1):
- The loop length comes from the bars played.
- REDO is EDIT + OCT+.
- The loop is appended to the session record.

**Commits:** see `git log` after `a412d15`.

**Next.** Phase 11: the scene system polish. Seamless World switching while playing, held notes and tails across
transitions, 2- and 4-bar transitions, and BEAT masks.
