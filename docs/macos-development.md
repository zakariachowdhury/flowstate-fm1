<!-- SPDX-License-Identifier: GPL-3.0-only -->
# SLOOP development on macOS

How to build and test SLOOP on a Mac, Apple silicon first. For Windows (WSL) and the general
build notes, see [BUILDING.md](../BUILDING.md).

## What you need

| | Why |
| --- | --- |
| macOS on Apple silicon (Intel Macs also work) | |
| Xcode command-line tools | `cc` for the host tests, `git` |
| Python 3.9 or newer with Pillow | the generators (`tools/gen_*.py`) and the package tools |
| Docker Desktop, with Rosetta on Apple silicon | runs the JieLi toolchain (below) |
| Node.js 18 or newer (optional) | the web page tests |
| SDL2 (optional, `brew install sdl2`) | the real-time simulator, [simulator.md](simulator.md) |
| About 300 MB of disk and network access once | toolchain, SDK files, container image |

The JieLi toolchain (clang 4.0.1 for the pi32v2 core of the FM-1's AC79 chip) exists only as
Linux x86-64 binaries. On a Mac, `tools/build.py` runs each compiler and linker call in a
`linux/amd64` Debian container, and Rosetta translates it. The host tests compile with Apple's
`cc` and run natively, so they need no Docker.

## One-command setup

```
tools/setup-macos.sh
```

It checks each item below and prints `ok`, or `FAIL` with the command that fixes it. It is safe to
run again: anything already present and matching its pin is left alone, and a second run takes a
few seconds. Steps:

1. macOS and the CPU type, the Xcode command-line tools, and Rosetta (on Apple silicon). If Rosetta
   is missing, the script prints `softwareupdate --install-rosetta` and stops. It never accepts
   Apple's licence for you.
2. Python ≥ 3.9 with Pillow, plus numpy on Python ≥ 3.12. Anything missing is reported with
   `python3 -m pip install -r tools/requirements-dev.txt`. The script installs it only when you pass
   `--install-python-deps`.
3. Node.js (optional): a warning if it is missing.
4. Docker: the script checks that it is installed and running, and starts Docker Desktop if needed
   (waiting up to 2 minutes). It pulls the pinned image if that is not present yet.
5. The toolchain: `tools/get_toolchain.sh`. The script then runs `clang --version` in the container
   with the source tree mounted, which also checks Docker's file sharing.
6. The SDK files: `tools/fetch_sdk.py`.
7. The generators: `python3 tools/build.py --gen-only`, as a final smoke test.

Options and environment:

| | |
| --- | --- |
| `--install-python-deps` | run the pip command above when a package is missing |
| `--no-docker` | host-only work: skip Rosetta, Docker, the toolchain and the SDK |
| `JIELI_TOOLCHAIN` | use this toolchain directory (checked, not downloaded) |
| `JIELI_HOME` | where `get_toolchain.sh` installs (default `~/.jieli`) |
| `AC79_SDK` | where `fetch_sdk.py` puts the SDK files (default `~/fw-AC79_AIoT_SDK`) |
| `PYTHON` | the Python to use (default `python3`), as for `build.sh` |

The script prints the `export` lines you need when a location is not the default.

## What gets installed where

**JieLi toolchain**: `tools/get_toolchain.sh [DEST]` (DEST defaults to `$JIELI_HOME`, else `~/.jieli`).

| | |
| --- | --- |
| URL | `https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20250805.1.tar.xz` (what `https://pkgman.jieliapp.com/s/linux-toolchain` redirected to in October 2026) |
| Size | 26,009,040 bytes |
| SHA-256 | `f686586bcfb45e0f0bb27fd2b39c7a7f313cb4f0e88a66a14da621ffa8225958` |
| Unpacks to | `DEST/jieli-linux-toolchains-20250324.1/` (136 MB). The 20250805.1 tarball contains the 20250324.1 toolchain; its newest file is dated 2025-03-24 |
| Links | `DEST/toolchain` → that directory (the default `JIELI_TOOLCHAIN`) |
| Marker | `DEST/toolchain.sha256`: the tarball installed there |

The script downloads with `curl -fL --retry 3`, checks the SHA-256 and refuses on a mismatch.
Nothing is downloaded when the marker names the pinned tarball. An install made by the earlier,
unpinned script (no marker) is accepted when its files match the pinned tree. That check hashes
every file and link target, and the result must be `3008c63d…` (`PIN_TREE` in the script).
`JIELI_TOOLCHAIN_URL` and `JIELI_TOOLCHAIN_SHA256` replace the pin, and you must set both.
`file://` URLs work.

**AC79 SDK files**: `tools/fetch_sdk.py [--sdk DIR]` (DIR defaults to `$AC79_SDK`, else
`~/fw-AC79_AIoT_SDK`). The files come from gitee tag `AC79NN_SDK_V1.2.1_2023-12-13` (Apache-2.0)
and land at their SDK paths, so a full checkout and this partial one are interchangeable. The hashes
are `SDK_SHA256` in `tools/build.py`. A file already present with the right hash is kept. A
download is written, atomically, only when its hash matches.

| File (under `cpu/wl82/tools/`) | Bytes | SHA-256 |
| --- | --- | --- |
| `uboot.boot` | 14,536 | `4e3b4c220dc96641cb5a723f41e68ce41d5261ae9434bb33fbd7f2c59976ded4` |
| `cfg_tool.bin` | 383 | `276579954f076886a6a7694f65dc71c034a63a2c204b76749065c0ac7b010d1b` |
| `cfg/eq_cfg_hw.bin` | 655 | `41167491bffed4651750719c973d2758adeb9021a5670d02d6a53c85ed80ea7d` |

**Container image**: `debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251`.
This digest is a multi-arch index; the build runs it as `linux/amd64` (about 75 MB). The pin is
`DOCKER_IMAGE` in `tools/build.py`, and `JIELI_DOCKER_IMAGE` overrides it.

**Python packages**: `tools/requirements-dev.txt` lists minimum versions:

- Pillow (required)
- numpy (Python ≥ 3.12 only: `tools/sampleio.py` uses it there for a faster pitch detection, with the
  same results)
- mido and python-rtmidi (optional: only `tools/fm1_install.py` and `tools/fm1_sample_upload.py`
  use them)

## Build

```
./build.sh
```

The build takes about 10 seconds on Apple silicon. It writes to `build/`:

- `felucca.bin` (the app)
- `loader/ota.bin` (the update loader)
- `felucca.fwsc` (the installable package)
- intermediates: `felucca.elf`, `felucca.dis`, the generated headers in `gen/`

`./build.sh --release 0.9-beta` makes a release build with identity `FM-1_909`; the package is
`build/felucca-0.9-beta.fwsc`. The build flags (`FELUCCA_FLASH`, …) are in BUILDING.md.

**Reproducible builds.** The only part of the image that changes from day to day is the build date
on the ABOUT page. Normally that is the compiler's `__DATE__`, which is UTC because the container
runs in UTC. `SOURCE_DATE_EPOCH=<Unix seconds>` takes the date from that UTC time instead, so the
same sources always give the same bytes. The published `docs/firmware/sloop-2.1.fwsc` was built
on 4 October 2026. As long as `firmware/` is unchanged from 2.1, this rebuilds it byte for byte:

```
SOURCE_DATE_EPOCH=$(date -j -u -f %Y-%m-%dT%H:%M:%S 2026-10-04T12:00:00 +%s) ./build.sh
cmp build/felucca.fwsc docs/firmware/sloop-2.1.fwsc && echo identical
```

(On Linux: `date -u -d 2026-10-04T12:00:00 +%s`.) Both files have SHA-256 `acf2c754…`. This was
checked with a toolchain and SDK files freshly installed by the scripts above, and with Python
3.11.5 and Pillow 10.4.0. Python 3.9.6 with Pillow 11.3.0 generates identical `build/gen` headers.

## Tests

```
./tests/run_tests.sh               # all 32 groups; run ./build.sh first (about 2 minutes)
./tests/run_tests.sh --host-only   # no ./build.sh, toolchain, Docker or SDK needed
```

`--host-only` creates `build/gen` with `tools/build.py --gen-only` if it is missing. It skips the
three groups that need the target build, marked "skip" below. The web tests run only when `node` is
installed, the simulator test only when `sdl2-config` is (both runs).

| # | Group | Source | `--host-only` |
| --- | --- | --- | --- |
| 1 | flash storage: A/B, torn writes | `tests/storage_test.c` | runs |
| 2 | application USB recovery and boot-loop guard | `tests/recovery_test.c` | runs |
| 3 | song order, timing, repeats, missing scenes | `tests/arranger_test.c` | runs |
| 4 | song audio: four tracks, scene transition, stop | `tests/song_audio_test.c` | runs |
| 5 | song screen: commands, load, display bounds | `tests/song_ui_test.c` | runs |
| 6 | drum lanes, kit audio, metronome, record arm, free take | `tests/studio_drums_test.c` | runs |
| 7 | sequencer 2.0: drift, ratchets, roll, erase / undo, chords, mute / solo | `tests/seq2_test.c` | runs |
| 8 | synthesised drum kits: every kit × sound bounded, audible, levels, cost | `tests/drumkit_test.c` | runs |
| 9 | punch-in FX: 16 effects, bounded, dry after release | `tests/punch_test.c` | runs |
| 10 | live UI: pages, layers, holds, drums, REC, fuzz | `tests/ui_pages_test.c` | runs |
| 11 | soak: `SOAK_MIN` simulated minutes of random live use | `tests/soak_test.c` | runs |
| 12 | user presets: UP_PUT parser, bank round trip, versions | `tests/upreset_test.c` | runs |
| 13 | TRS MIDI parser | `tests/midi_uart_test.c` | runs |
| 14 | M-UPGRADE entry, against `build/felucca.fwsc` | `tests/ota_test.c` | skip |
| 15 | update loader: another app → this build | `tests/ldr_test.c` | skip |
| 16 | scales: white-key mapping, note lifecycle | `tests/scale_test.c` | runs |
| 17 | DSP render (ANALOG preset 0) | `tests/hostsim.c` | runs |
| 18 | TRACKS: 4-track pattern, live recording, voice budget, cost | `tests/hostsim.c` | runs |
| 19 | project formats (FUN1–3 → FUN4), capture / apply, autosave | `tests/project_test.c` | runs |
| 20 | SLICER: clicks, timing, sync, STUT, cost | `tests/slicer_test.c` | runs |
| 21 | regression: golden renders, health, voices, CPU budget | `tests/regress.c` | runs |
| 22 | regression: target cost of the render loops in `build/felucca.dis` | `tests/target_budget.py` | skip |
| 23 | installer CLI against a simulated FM-1 (no mido needed) | `tests/install_test.py` | runs |
| 24 | web pages: editor protocol, samples, packages, update protocol | `web/test_web.mjs` | runs (with Node) |
| 25 | host renderer: the example projects, 16 bars each, clean and the same bytes twice; `examples/projects/` as `host/examples.c` makes them | `host/render.c`, `host/examples.c` | runs |
| 26 | simulator and Worlds: 6 s of NEON RAIN headless in real time (a scene committed exactly on the next bar, mutes, audio, frames), the same bytes as `--fast`; the Studio (a variation and a scene on the bar, a World switch on the bar, no note left after STOP, a SLOOP project, both views drawn); every factory World rendered clean by `sloop-render --world`, and a scene sequence on the bars | `host/sim/`, `host/render.c` | runs (with SDL2) |
| 27 | unity order: `host/core.c` includes `felucca.c`'s firmware files in its order (hardware-only files apart) | `tests/unity_order_test.py` | runs |
| 28 | worlds: the World compiler (sloop-params.json fresh, schema, round trip, notation, errors, size limits) | `tests/worldc_test.py` | runs |
| 29 | worlds: the FWD1 parser, load, stage, commit; truncations, byte flips, 20,000 corruptions (ASan/UBSan) | `tests/world_test.c` | runs |
| 30 | worlds: the World modules never name `proj_slot` (design D6) | `firmware/src/world*` | runs |
| 31 | regression with `FELUCCA_WORLD=1`: the same golden renders | `tests/regress_world.c` | runs |
| 32 | worlds: every factory World × scene × variation rendered clean, ENERGY, macros, loudness | `tests/world_render.c` | runs |

Environment:

| | |
| --- | --- |
| `GOLDEN_UPDATE=1` | rewrite the render hashes in `tests/golden.txt` after an intended change of the sound; review the diff and commit it with the change |
| `BUDGET_UPDATE=1` | rewrite `tests/cpu_baseline.txt` and, in a full run, `tests/target_budget.txt` after an intended change of the cost |
| `VERBOSE=1` | the regression suite prints every render |
| `SOAK_MIN=n` | length of the soak test in simulated minutes (default 10) |
| `JOBS=n` | regression renders run at once (default 8) |
| `CC` | host compiler (default `cc`) |

## Fast host-only loop

For work on the DSP, sequencer, UI or storage code you need neither Docker nor the toolchain:

```
python3 tools/build.py --gen-only          # build/gen: fonts, icons, tables, samples, kits, logo
SOAK_MIN=1 ./tests/run_tests.sh --host-only
```

`--host-only` creates `build/gen` only when it is missing. After a change to a generator or its
assets, run `--gen-only` again. To run one test, compile it the way `tests/run_tests.sh` does, for
example:

```
mkdir -p build/host
cc -O2 -w -Ibuild/gen -Ifirmware/src -o build/host/seq2_test tests/seq2_test.c -lm && build/host/seq2_test
```

Run the full `./build.sh && ./tests/run_tests.sh` before you commit. Only the full run checks the
target cost and the update path.

## Rendering

`host/` runs the real firmware on the Mac: the engines, voices, drums, effects, mixer, sequencer, arranger,
projects, flash storage and UI, compiled from `firmware/src` as `src/felucca.c` builds them. Only the
hardware is replaced, by `host/hal_host.h`: time follows the audio rendered, the LCD is a framebuffer and
the flash a 1 MiB NOR image. Programs use the small API in `host/host.h`. Nothing in `host/` synthesises
or sequences anything itself.

```
make -C host           # build/host-bin/sloop-render and sloop-examples (makes build/gen if it is missing)
```

**`sloop-render`** turns a SLOOP project (`.fun4`, the format SAVE writes; FUN1–3 are converted) into a
44.1 kHz stereo 16-bit WAV. It loads the project as the FM-1 does, presses PLAY, plays at the project's
tempo, presses STOP, and records the release tail. The WAV is the raw mix with the master at unity, as
the host tests render it; the device's DAC plays it 6 dB lower. The same input always gives the same bytes.

```
sloop-render [--bars N | --seconds S] [--tail S] [--analyze | --check] [--dump] [--screen OUT.ppm] INPUT.fun4 OUT.wav
sloop-render [options] --song A.fun4,B.fun4[,C.fun4,D.fun4] [--order A:4,B:8,..] [INPUT.fun4] OUT.wav
sloop-render [options] --world NAME|FILE [--scene A..D] [--var NAME|N] [--world-sequence] OUT.wav
```

| | |
| --- | --- |
| `--bars N`, `--seconds S` | how long to play. The default is until every track's pattern has played twice |
| `--tail S` | seconds recorded after STOP (default 4) |
| `--song`, `--order` | up to four projects become the sections A–D, and the arranger plays the order in song mode, as on the device. Tempo and global FX come from INPUT, or else from the first section played. The default order plays each section once, until its patterns have played twice |
| `--analyze` | peak and RMS level, DC offset, samples at full scale, longest silence, and a hash of the audio |
| `--check` | `--analyze`, then exit 1 if the render is silent, peaks at or above −0.1 dBFS, reaches full scale or has a DC offset |
| `--dump` | tempo, swing, and each track's engine, preset, pattern length and mix |
| `--screen OUT.ppm` | also runs the main loop between audio blocks and saves the screen at the end of play. The audio does not change |
| `--world`, `--scene`, `--var` | a Musical World instead of a project: a factory World by name or id, a `.wblob`, or a `.world.json` (compiled with `tools/worldc.py`); its default scene and variation unless given (default 8 bars). [simulator.md](simulator.md#musical-worlds-on-the-host) |
| `--world-sequence` | the World's scenes A, B, C, D, `--bars` each (default 4), each asked for while playing and committed on the bar |

```
mkdir -p build/renders && make -C host
build/host-bin/sloop-render --dump --analyze examples/projects/cinematic.fun4 build/renders/cinematic.wav
build/host-bin/sloop-render --song examples/projects/ambient.fun4,examples/projects/cinematic.fun4 \
    --order A:8,B:8,A:4 build/renders/song.wav
build/host-bin/sloop-render --world "NEON RAIN" --var DREAMY --check build/renders/neon_rain-dreamy.wav
```

A render runs at about 200 times realtime: 16 bars of `examples/projects/ambient.fun4` (59 s) take 0.3 s.

**`flowstate-sim`** plays the same firmware in real time through the Mac's audio output (it needs SDL2). Its
window is FLOWSTATE STUDIO (the UI spec's simulator: the Musical Worlds, their scenes and variations, macros,
tracks, keys) with the FM-1 panel a Tab away. `build/host-bin/flowstate-sim` starts it with NEON RAIN; Option+Space
plays. `--world PATH.world.json` reloads a World whenever its file changes. Its options, keys, threads, latency
measurements and known differences from the device are in [simulator.md](simulator.md).

**`sloop-examples [DIR]`** writes the three example projects in `examples/projects/` (or DIR):
`ambient.fun4`, `groove.fun4` and `cinematic.fun4`. It builds them with the firmware's own functions
(TOOLS > NEW, then engines and presets chosen by name, parameters, steps), so each file loads on an FM-1
like any saved project. `host/examples.c` describes the music. After changing it, run
`build/host-bin/sloop-examples` and commit the new `.fun4` files. Test group 25 fails while they differ.

## Troubleshooting

- **`build.sh: Docker is not running`.** Start Docker Desktop, or run `tools/setup-macos.sh`, which
  starts it. Other Docker engines may work, but only Docker Desktop has been tested.
- **`mounts denied` or "the source tree is not visible in the container".** Docker Desktop >
  Settings > Resources > File sharing must include the source tree and the toolchain directory. The
  defaults (`/Users`, `/Volumes`, `/private`, `/tmp`, `/var/folders`) cover the usual places.
- **Rosetta.** Install it with `softwareupdate --install-rosetta`. In Docker Desktop, turn on
  Settings > General > "Use Rosetta for x86_64/amd64 emulation on Apple Silicon". Without it, amd64
  emulation falls back to QEMU, which is slower, and `setup-macos.sh` warns. `exec format error`
  means the image is not running as `linux/amd64`. Check `JIELI_DOCKER_IMAGE`.
- **JieLi server unreachable** (`jl-update.oss-cn-shenzhen.aliyuncs.com`). `get_toolchain.sh`
  retries 3 times. With a copy of the same tarball from elsewhere, run
  `JIELI_TOOLCHAIN_URL=file:///path/to/jieli-linux-toolchains-20250805.1.tar.xz tools/get_toolchain.sh`.
  The SHA-256 is still checked.
- **Toolchain SHA-256 mismatch.** The file at the URL has changed. Do not override the pin
  blindly: a different compiler changes the image and the target cost baselines. To use a new
  release on purpose, set `JIELI_TOOLCHAIN_URL` and `JIELI_TOOLCHAIN_SHA256`, then rebaseline with
  `BUDGET_UPDATE=1`.
- **gitee unreachable.** `fetch_sdk.py` retries, then prints a fallback: a shallow, sparse clone of
  the tag (`git clone --depth 1 --filter=blob:none --sparse --branch AC79NN_SDK_V1.2.1_2023-12-13
  …`, then `git sparse-checkout set cpu/wl82/tools`). Copying the three files from any checkout of
  the tag also works, as long as the hashes match.
- **`warning: SDK … differs from AC79NN_SDK_V1.2.1`.** `AC79_SDK` points at another SDK version.
  The build still works, but the package will not match the reference. Run
  `tools/fetch_sdk.py --sdk ~/sloop-sdk` and set `AC79_SDK=~/sloop-sdk`.
- **`build.sh: python3 has no Pillow`.** Run `python3 -m pip install -r tools/requirements-dev.txt`.
  Homebrew's Python refuses this with `externally-managed-environment`. In that case, use a virtual
  environment outside the tree (`.venv` is not in `.gitignore`):
  `python3 -m venv ~/.venvs/sloop && . ~/.venvs/sloop/bin/activate`, then pip install. The tests
  call `python3`, so activate the venv rather than only setting `PYTHON`.
- **numpy on Python ≥ 3.12.** Without numpy, `tools/sampleio.py` falls back to pure Python, which
  gives the same results more slowly. Install numpy from `tools/requirements-dev.txt`.
- **`./build.sh: permission denied`.** Checkouts older than commit `3f22947` lack the executable
  bit. Run `sh build.sh`, `sh tests/run_tests.sh` and `sh tools/setup-macos.sh`, or update the
  checkout.

## See also

- [BUILDING.md](../BUILDING.md): build options, Windows, samples, installing on the FM-1
- [docs/architecture-audit.md](architecture-audit.md): the code architecture. §17 is the
  verification log; §18 is the plan this setup implements.
