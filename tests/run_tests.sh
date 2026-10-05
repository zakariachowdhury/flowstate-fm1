#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Host tests (no hardware). Run from the repo root after ./build.sh:
#   tests/run_tests.sh
#   tests/run_tests.sh --host-only   without ./build.sh (no toolchain, Docker or SDK): makes build/gen if
#                                    missing (tools/build.py --gen-only), skips the tests that need the target
#                                    build (M-UPGRADE entry, update loader, target cost of the render loops)
#
# Regression suite (tests/regress.c, tests/target_budget.py; details at the top of regress.c):
#   golden renders  every engine x preset, the drum kit, voice modes, FX sends, a 4-track mix: one hash
#                   each in tests/golden.txt. A change of the sound fails with the list of renders.
#   health          clipping, DC, peak level, voices free after the release, silence at the end.
#   CPU             instructions / sample per preset and mix (tests/cpu_baseline.txt, +25 %), ns printed;
#                   target: loop instructions of the render functions in build/felucca.dis
#                   (tests/target_budget.txt, +10 %; exact, static).
#   voices          the budget of 8, steal fades, MONO / LEGATO / UNISON keep their note, the VOICE cap,
#                   no hanging notes on any MIDI / key routing.
# After an intended change of the sound: GOLDEN_UPDATE=1 sh tests/run_tests.sh, review the diff
# of tests/golden.txt, commit it with the change. After an intended change of the cost (or a new
# compiler): BUDGET_UPDATE=1 (rewrites cpu_baseline.txt and target_budget.txt). VERBOSE=1: every render.
set -e
export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
cd "$(dirname "$0")/.."
OUT=build/host
mkdir -p "$OUT"
CC="${CC:-cc} -O1 -Wall -Wno-unused-function"
fail=0
run() { echo "== $1"; shift; "$@" || fail=1; }

host_only=0
case "$1" in
    "") ;;
    --host-only) host_only=1 ;;
    *) echo "usage: tests/run_tests.sh [--host-only]"; exit 2 ;;
esac
target() { if [ $host_only = 1 ]; then echo "== skip $1 (needs the target build)"; else run "$@"; fi; }

if [ $host_only = 1 ]; then
    [ -d build/gen ] || python3 tools/build.py --gen-only
else
    [ -f build/felucca.fwsc ] || { echo "run ./build.sh first"; exit 1; }
fi

$CC -o "$OUT/storage_test" tests/storage_test.c
run "flash storage (A/B, torn writes)" "$OUT/storage_test"

$CC -o "$OUT/recovery_test" tests/recovery_test.c
run "application USB recovery and boot-loop guard" "$OUT/recovery_test"

$CC -o "$OUT/arranger_test" tests/arranger_test.c
run "song order, timing, repeats and missing scenes" "$OUT/arranger_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/song_audio_test" tests/song_audio_test.c -lm
run "song: four simultaneous tracks, scene transition and stop" "$OUT/song_audio_test" "$OUT/song-demo.wav"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/song_ui_test" tests/song_ui_test.c -lm
run "song screen: commands, load (OCT+ twice), display bounds" "$OUT/song_ui_test" "$OUT/song-screen.ppm"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/studio_drums_test" tests/studio_drums_test.c -lm
run "drum lanes, kit audio, metronome, record arm, free take" "$OUT/studio_drums_test" "$OUT/drum-styles.wav"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/seq2_test" tests/seq2_test.c -lm
run "sequencer 2.0: no drift, ratchets, roll, erase / undo, ghost / hard, chords, mute / solo" "$OUT/seq2_test"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/drumkit_test" tests/drumkit_test.c -lm
run "synthesised drum kits: every kit x sound bounded, audible, finite, levels, cost" "$OUT/drumkit_test" "$OUT/drum-kits.wav" "$OUT/drum-kits.txt"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/punch_test" tests/punch_test.c -lm
run "punch-in FX: 16 effects, bounded, dry after release, FX-held keys" "$OUT/punch_test" "$OUT/punch-fx.wav"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/ui_pages_test" tests/ui_pages_test.c -lm
run "live UI: pages, layers (punch, steps, erase, roll, key, mix), holds, drums, REC, fuzz" "$OUT/ui_pages_test" "$OUT"

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/soak_test" tests/soak_test.c -lm
run "soak: ${SOAK_MIN:-10} minutes of random live use (bounded, no hanging voices, idle after stop)" "$OUT/soak_test" "${SOAK_MIN:-10}"

$CC -o "$OUT/upreset_test" tests/upreset_test.c
run "user presets (UP_PUT parser, bank round trip, versions)" "$OUT/upreset_test"

$CC -o "$OUT/midi_uart_test" tests/midi_uart_test.c
run "TRS MIDI parser" "$OUT/midi_uart_test"

$CC -o "$OUT/ota_test" tests/ota_test.c
target "M-UPGRADE entry" "$OUT/ota_test" build/felucca.fwsc

if [ $host_only = 0 ]; then
    head -c 200000 build/felucca.bin > "$OUT/old_app.bin"
    python3 tools/fm1pkg_make.py "$OUT/old_app.bin" build/loader/ota.bin "$OUT/old.fwsc" >/dev/null
fi
$CC -o "$OUT/ldr_test" tests/ldr_test.c
target "update loader: other app -> this build" "$OUT/ldr_test" "$OUT/old.fwsc" build/felucca.fwsc

$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/hostsim" tests/hostsim.c -lm
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/scale_test" tests/scale_test.c -lm
run "scales: white-key mapping and note lifecycle" "$OUT/scale_test"
run "DSP render (ANALOG preset 0)" "$OUT/hostsim" 0 0 1 "$OUT/render.wav"
mkdir -p build/tracks_demo
run "TRACKS: 4-track pattern, live recording (lengths, swing), voice budget, engine switch, cost" env TRACKS=build/tracks_demo "$OUT/hostsim" 0 0 1 "$OUT/tracks.wav"
$CC -w -Ibuild/gen -Ifirmware/src -o "$OUT/project_test" tests/project_test.c -lm
run "project formats (FUN3 / FUN2 / FUN1 -> FUN4), capture / apply, autosave" "$OUT/project_test"
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/slicer_test" tests/slicer_test.c -lm
mkdir -p build/slicer_demo
run "SLICER: no clicks, timing, sync with the sequencer, STUT, cost, demos" "$OUT/slicer_test" build/slicer_demo
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/regress" tests/regress.c -lm
run "regression: golden renders, health, voices, CPU budget" "$OUT/regress" tests/golden.txt tests/cpu_baseline.txt
# SLICE (tests/slice_test.c) needs a FELUCCA_SLICE=1 build; the engine is not built by default

target "regression: target cost of the render loops" python3 tests/target_budget.py \
    build/felucca.dis tests/target_budget.txt

run "installer CLI (fm1_install.py) against a simulated FM-1" python3 tests/install_test.py

if command -v node >/dev/null 2>&1; then
    run "web pages: editor protocol, samples, packages, update protocol" node web/test_web.mjs
else
    echo "== skip web tests (no node)"
fi

# host programs (host/): the example projects through the real firmware: 16 bars each, clean (heard, under
# -0.1 dBFS, no full-scale sample, no DC), the same bytes twice; host/examples.c still makes the committed ones
make -s -j2 -C host
render_test() {
    mkdir -p "$OUT/examples"
    build/host-bin/sloop-examples "$OUT/examples" >/dev/null || return 1
    for n in ambient groove cinematic; do
        build/host-bin/sloop-render --bars 16 --check "examples/projects/$n.fun4" "$OUT/render-$n.wav" || return 1
        build/host-bin/sloop-render --bars 16 "examples/projects/$n.fun4" "$OUT/render-$n-2.wav" >/dev/null || return 1
        cmp "$OUT/render-$n.wav" "$OUT/render-$n-2.wav" || return 1
        cmp "$OUT/examples/$n.fun4" "examples/projects/$n.fun4" || { echo "examples/projects/$n.fun4 is not what host/examples.c makes"; return 1; }
        echo "render: $n: the same bytes twice; examples/projects/$n.fun4 as host/examples.c makes it"
    done
}
run "host renderer: example projects, 16 bars each, clean and deterministic (sloop-render, sloop-examples)" render_test

# the real-time simulator (host/sim, needs SDL2): the demo (the stand-in World GROOVE, starting on scene B)
# headless, 5 s through the null sink in real time (no window, no sound): PLAY, a scene switch on the next bar,
# mutes, tempo and filter, the audio heard and then silenced (the script's expectations), exactly 5 s played, a
# clean exit. Then --fast twice: the same bytes each time and, when real time had no underrun, the same bytes
# as real time (nothing dropped or doubled). Then FLOWSTATE STUDIO, --fast: choose a World while one plays,
# confirm, it loads on the bar; a scene, a mute and back, a level, a macro; the Studio and ADVANCED drawn (--shot)
SIM_SCRIPT='1 play; 2.5 expect playing = 1; 2.5 expect scene = B; 2.5 expect rms > -30
    2.6 scene A; 2.7 expect next = A; 2.7 expect scene = B
    3.2 expect scene = A; 3.2 expect mute1 = 1; 3.2 expect mute4 = 1; 3.2 expect mute2 = 0
    3.3 mute 4; 3.4 expect mute4 = 0; 3.5 mute 2 on; 3.5 mute 3 on; 3.5 mute 4 on
    3.6 bpm +2; 3.6 filter -20; 3.7 expect bpm = 122; 3.7 expect filter = -20; 4.9 expect rms < -45'
STUDIO_SCRIPT='0.95 expect world = GROOVE; 1 play
    1.2 world AMBIENT; 1.3 expect browse = AMBIENT; 1.3 expect world = GROOVE
    1.5 confirm; 1.6 expect pending = AMBIENT; 1.6 expect world = GROOVE
    3.05 expect world = AMBIENT; 3.05 expect bpm = 70; 3.05 expect playing = 1; 3.05 expect scene = B
    3.3 scene D; 3.4 expect next = D; 6.6 expect scene = D; 6.6 expect mute4 = 1
    6.7 mute 2; 6.8 expect mute2 = 1; 6.9 mute 2; 7.0 expect mute2 = 0
    7.1 level 1 90; 7.2 expect level1 = 90; 7.3 macro COLOR +10; 7.4 expect macro1 = 60'
sim_test() {
    sim=build/host-bin/flowstate-sim
    $sim --demo --headless 5 --script "$SIM_SCRIPT" --wav "$OUT/sim-rt.wav" > "$OUT/sim-rt.txt" ||
        { cat "$OUT/sim-rt.txt"; return 1; }
    grep '^expect' "$OUT/sim-rt.txt"
    [ "$(grep -c '^expect: .*: ok' "$OUT/sim-rt.txt")" -eq 13 ] || { echo "not every expectation ran"; return 1; }
    grep -q 'the sink played 220500 (5.000 s)' "$OUT/sim-rt.txt" || { cat "$OUT/sim-rt.txt"; return 1; }
    n=$(sed -n 's/.*rendered \([0-9]*\) frames.*/\1/p' "$OUT/sim-rt.txt")
    [ "$n" -ge 220500 ] && [ "$n" -le $((220500 + 736)) ] || { echo "rendered $n frames for 220500 played"; return 1; }
    [ "$(wc -c < "$OUT/sim-rt.wav" | tr -d ' ')" -eq $((44 + 220500 * 4)) ] || { echo "sim-rt.wav: wrong size"; return 1; }
    $sim --demo --headless 5 --fast --script "$SIM_SCRIPT" --wav "$OUT/sim-fast.wav" > "$OUT/sim-fast.txt" || return 1
    $sim --demo --headless 5 --fast --script "$SIM_SCRIPT" --wav "$OUT/sim-fast2.wav" --advanced \
        --shot "$OUT/sim-advanced.bmp" > /dev/null || return 1
    cmp "$OUT/sim-fast.wav" "$OUT/sim-fast2.wav" || return 1
    if grep -q 'underruns 0, 0 frames missing' "$OUT/sim-rt.txt"; then
        cmp "$OUT/sim-rt.wav" "$OUT/sim-fast.wav" || return 1
        echo "simulator: 5 s played in real time, no underrun, the same bytes as --fast (twice)"
    else
        grep 'underruns' "$OUT/sim-rt.txt"
        echo "simulator: underruns in real time (a busy machine?): not compared with --fast; --fast twice the same bytes"
    fi
    $sim --demo --headless 7.5 --fast --script "$STUDIO_SCRIPT" --shot "$OUT/sim-studio.bmp" > "$OUT/sim-studio.txt" ||
        { cat "$OUT/sim-studio.txt"; return 1; }
    grep '^expect' "$OUT/sim-studio.txt"
    [ "$(grep -c '^expect: .*: ok' "$OUT/sim-studio.txt")" -eq 16 ] || { echo "not every Studio expectation ran"; return 1; }
    for f in sim-studio sim-advanced; do                  # 1000 x 872, 24 bits: drawn, not compared
        [ "$(wc -c < "$OUT/$f.bmp" | tr -d ' ')" -eq $((54 + 3000 * 872)) ] || { echo "$f.bmp: not drawn"; return 1; }
    done
    echo "simulator: FLOWSTATE STUDIO: a World chosen while one plays, loaded on the bar; scene, mute, level, macro;" \
         "$OUT/sim-studio.bmp, $OUT/sim-advanced.bmp"
}
if command -v sdl2-config >/dev/null 2>&1; then
    run "simulator: real time headless (flowstate-sim): PLAY, scene on the bar, mutes, audio, frames; the Studio" sim_test
else
    echo "== skip simulator (no SDL2: brew install sdl2; only build/host-bin/flowstate-sim needs it)"
fi

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
