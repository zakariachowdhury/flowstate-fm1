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
