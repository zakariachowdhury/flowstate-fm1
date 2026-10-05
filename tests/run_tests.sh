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

# the real-time simulator (host/sim, needs SDL2) and the World host tools. First the demo (NEON RAIN) headless,
# 6 s through the null sink in real time (no window, no sound): PLAY, scene C asked for mid-bar and committed
# exactly on the next bar, mutes, tempo and filter, the audio heard and then silenced (the script's
# expectations), exactly 6 s played, a clean exit. Then --fast twice: the same bytes each time and, when real
# time had no underrun, the same bytes as real time (nothing dropped or doubled). Then FLOWSTATE STUDIO, --fast:
# a variation on the bar, a scene on the bar, another World chosen while one plays and switched on the bar, a
# mute and back, a level, a macro, STOP with no note left held or sounding, a SLOOP project; the Studio and
# ADVANCED drawn (--shot). Then sloop-render: every factory World clean (build/renders/worlds), and a scene
# sequence whose changes land on the bars
SIM_SCRIPT='0.95 expect world = NEON_RAIN; 1 play; 2.5 expect playing = 1; 2.5 expect scene = B; 2.5 expect rms > -35
    2.6 scene C; 2.7 expect next = C; 4.30 expect scene = B; 4.40 expect scene = C
    4.45 mute 4; 4.5 expect mute4 = 1; 4.55 mute 4; 4.6 expect mute4 = 0
    4.6 bpm +2; 4.6 filter -20; 4.65 expect bpm = 74; 4.65 expect filter = -20
    4.7 mute 1 on; 4.7 mute 2 on; 4.7 mute 3 on; 4.7 mute 4 on; 5.95 expect rms < -45'
STUDIO_SCRIPT='0.95 expect world = NEON_RAIN; 0.95 expect var = ORIGINAL; 1 play
    1.5 var DREAMY; 1.6 expect varnext = DREAMY; 4.30 expect var = ORIGINAL; 4.40 expect var = DREAMY
    4.5 scene C; 7.60 expect scene = B; 7.70 expect scene = C
    7.8 world MIDNIGHT_DRIVE; 7.9 expect browse = MIDNIGHT_DRIVE; 8.0 confirm; 8.1 expect pending = MIDNIGHT_DRIVE
    10.95 expect world = NEON_RAIN; 11.05 expect world = MIDNIGHT_DRIVE; 11.05 expect bpm = 100; 11.05 expect playing = 1
    11.05 expect scene = B; 11.3 expect rms > -40
    11.3 mute 2; 11.4 expect mute2 = 1; 11.5 mute 2; 11.6 expect mute2 = 0
    11.7 level 1 90; 11.8 expect level1 = 90; 11.9 macro COLOR +10; 12.0 expect macro1 = 60
    12.1 stop; 12.15 expect gated = 0; 14.4 expect voices = 0; 14.4 expect rms < -60
    14.5 world GROOVE; 14.5 confirm; 14.6 expect world = GROOVE; 14.6 expect bpm = 120'
sim_test() {
    sim=build/host-bin/flowstate-sim
    $sim --demo --headless 6 --script "$SIM_SCRIPT" --wav "$OUT/sim-rt.wav" > "$OUT/sim-rt.txt" ||
        { cat "$OUT/sim-rt.txt"; return 1; }
    grep '^expect' "$OUT/sim-rt.txt"
    [ "$(grep -c '^expect: .*: ok' "$OUT/sim-rt.txt")" -eq 12 ] || { echo "not every expectation ran"; return 1; }
    grep -q 'the sink played 264600 (6.000 s)' "$OUT/sim-rt.txt" || { cat "$OUT/sim-rt.txt"; return 1; }
    n=$(sed -n 's/.*rendered \([0-9]*\) frames.*/\1/p' "$OUT/sim-rt.txt")
    [ "$n" -ge 264600 ] && [ "$n" -le $((264600 + 736)) ] || { echo "rendered $n frames for 264600 played"; return 1; }
    [ "$(wc -c < "$OUT/sim-rt.wav" | tr -d ' ')" -eq $((44 + 264600 * 4)) ] || { echo "sim-rt.wav: wrong size"; return 1; }
    $sim --demo --headless 6 --fast --script "$SIM_SCRIPT" --wav "$OUT/sim-fast.wav" > "$OUT/sim-fast.txt" || return 1
    $sim --demo --headless 6 --fast --script "$SIM_SCRIPT" --wav "$OUT/sim-fast2.wav" --advanced \
        --shot "$OUT/sim-advanced.bmp" > /dev/null || return 1
    cmp "$OUT/sim-fast.wav" "$OUT/sim-fast2.wav" || return 1
    if grep -q 'underruns 0, 0 frames missing' "$OUT/sim-rt.txt"; then
        cmp "$OUT/sim-rt.wav" "$OUT/sim-fast.wav" || return 1
        echo "simulator: 6 s played in real time, no underrun, the same bytes as --fast (twice)"
    else
        grep 'underruns' "$OUT/sim-rt.txt"
        echo "simulator: underruns in real time (a busy machine?): not compared with --fast; --fast twice the same bytes"
    fi
    $sim --demo --headless 15 --fast --script "$STUDIO_SCRIPT" --shot "$OUT/sim-studio.bmp" > "$OUT/sim-studio.txt" ||
        { cat "$OUT/sim-studio.txt"; return 1; }
    grep '^expect' "$OUT/sim-studio.txt"
    [ "$(grep -c '^expect: .*: ok' "$OUT/sim-studio.txt")" -eq 24 ] || { echo "not every Studio expectation ran"; return 1; }
    for f in sim-studio sim-advanced; do                  # 1000 x 872, 24 bits: drawn, not compared
        [ "$(wc -c < "$OUT/$f.bmp" | tr -d ' ')" -eq $((54 + 3000 * 872)) ] || { echo "$f.bmp: not drawn"; return 1; }
    done
    echo "simulator: FLOWSTATE STUDIO: variation and scene on the bar, a World switched on the bar, no note left" \
         "after STOP, a SLOOP project; $OUT/sim-studio.bmp, $OUT/sim-advanced.bmp"
    mkdir -p build/renders/worlds
    for w in FROZEN_LAKE NEON_RAIN DUSTY_CAFE MIDNIGHT_DRIVE; do
        id=$(echo $w | tr 'A-Z' 'a-z')
        build/host-bin/sloop-render --world "$(echo $w | tr _ ' ')" --bars 4 --check \
            "build/renders/worlds/$id-studio.wav" > "$OUT/render-$id.txt" || { cat "$OUT/render-$id.txt"; return 1; }
        echo "render: $(grep '^rendered' "$OUT/render-$id.txt" | sed 's/^rendered //'); $(grep 'peak' "$OUT/render-$id.txt" | tr -s ' ')"
    done
    build/host-bin/sloop-render --world "NEON RAIN" --world-sequence --bars 2 --check "$OUT/render-sequence.wav" \
        > "$OUT/render-sequence.txt" || { cat "$OUT/render-sequence.txt"; return 1; }
    [ "$(grep -c '^scene [BCD] .* from [246]\.000 bars' "$OUT/render-sequence.txt")" -eq 3 ] ||
        { cat "$OUT/render-sequence.txt"; echo "the scenes did not change on the bars"; return 1; }
    echo "render: NEON RAIN A B C D, 2 bars each: each scene from its bar ($(grep -c '^scene' "$OUT/render-sequence.txt") changes)"
}
if command -v sdl2-config >/dev/null 2>&1; then
    run "simulator and Worlds: real time headless, scenes and variations on the bar, a World switch, renders" sim_test
else
    echo "== skip simulator (no SDL2: brew install sdl2; only build/host-bin/flowstate-sim needs it)"
fi
run "unity order: host/core.c builds felucca.c's firmware files in its order" python3 tests/unity_order_test.py

# Musical Worlds (Phase 5: docs/design/fwd1-format.md, docs/worlds.md): the compiler (tools/worldc.py) and the
# firmware's FWD1 parser (firmware/src/world.c, built with FELUCCA_WORLD=1); then the regression suite again with
# FELUCCA_WORLD=1 against the same goldens (no World active: SLOOP renders bit-identically)
python3 tools/gen_worlds.py build/gen/felucca_worlds.h >/dev/null || fail=1   # (--host-only may keep an older build/gen)
run "worlds: worldc (sloop-params.json fresh, schema enums, round trip, notation, errors, size limits, gen_worlds)" \
    python3 tests/worldc_test.py
world_test() {
    python3 tools/worldc.py compile worlds/test/minimal.world.json -o "$OUT/world-minimal.wblob" >/dev/null 2>&1 &&
        python3 tools/worldc.py compile worlds/test/full.world.json -o "$OUT/world-full.wblob" >/dev/null 2>&1 ||
        { echo "worldc cannot compile worlds/test"; return 1; }
    $CC -g -w -fsanitize=address,undefined -fno-sanitize-recover=undefined -Ibuild/gen -Ifirmware/src \
        -o "$OUT/world_test" tests/world_test.c -lm || return 1
    "$OUT/world_test" "$OUT/world-minimal.wblob" "$OUT/world-full.wblob"
}
run "worlds: FWD1 check, load, stage, commit; truncations, flips, 20000 corruptions (ASan/UBSan); proj_slot untouched" \
    world_test
run "worlds: the World modules never name proj_slot (design D6)" sh -c '! grep -n proj_slot firmware/src/world*'
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/regress_world" tests/regress_world.c -lm
run "regression with FELUCCA_WORLD=1 (world.c built in, no World loaded): the same golden renders" \
    env -u GOLDEN_UPDATE -u BUDGET_UPDATE "$OUT/regress_world" tests/golden.txt tests/cpu_baseline.txt

# the factory Worlds (worlds/factory, docs/worlds.md "Factory Worlds") through the real firmware with
# tests/world_render.c (wb_check, world_load, world_apply, PLAY; the ENERGY band at the World's default emulated
# until Phase 11): each compiles within the factory limit (3,072 B; above 2,048 B a warning); every scene x
# variation, 4 bars and a 6 s tail, is heard, peaks at most -1 dBFS, has no full-scale sample and no DC, leaves no
# voice and no echo above -60 dBFS after STOP, stays in the voice budget with no held note stolen, keeps its notes
# in the scale and their registers; the same as the Phase 5 firmware plays it (--raw: no ENERGY) and with a
# player's phrase on the Smart Keys; ENERGY bands only add; no macro mapping leaves its range or saturates at
# 100 %; the Worlds sit within 3 LU of each other at their defaults
factory_worlds_test() {
    $CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/world_render" tests/world_render.c -lm || return 1
    : > "$OUT/worlds-loudness.txt"
    for f in worlds/factory/*.world.json; do
        [ -f "$f" ] || continue
        id=$(basename "$f" .world.json)
        python3 tools/worldc.py compile "$f" -o "$OUT/$id.wblob" > "$OUT/$id.txt" 2>&1 || { cat "$OUT/$id.txt"; return 1; }
        n=$(wc -c < "$OUT/$id.wblob" | tr -d ' ')
        [ "$n" -le 3072 ] || { echo "$id: $n B, above the factory limit of 3,072 B"; return 1; }
        [ "$n" -le 2048 ] || echo "$id: warning: $n B, above the 2,048 B guideline"
        r="$OUT/world_render"
        for a in "--scene all --var all" "--scene all --var all --raw" "--scene all --keys" "--bands" "--macros"; do
            # shellcheck disable=SC2086
            $r --bars 4 $a --check "$OUT/$id.wblob" >> "$OUT/$id.txt" || { cat "$OUT/$id.txt"; return 1; }
        done
        lufs=$(sed -n 's/.*at the defaults \(-*[0-9.]*\) LUFS.*/\1/p' "$OUT/$id.txt" | head -1)
        echo "$id $lufs" >> "$OUT/worlds-loudness.txt"
        echo "$id: $n B; $(grep -c '^  [A-D] .* ok' "$OUT/$id.txt") renders clean; at the defaults $lufs LUFS"
    done
    awk '{ if (NR == 1 || $2 < lo) lo = $2; if (NR == 1 || $2 > hi) hi = $2 }
         END { printf "loudness at the defaults: %.2f .. %.2f LUFS (%.2f LU apart)\n", lo, hi, hi - lo; exit (hi - lo > 3) }' \
        "$OUT/worlds-loudness.txt"
}
run "worlds: the factory Worlds, every scene x variation rendered clean (world_render), ENERGY, macros, loudness" \
    factory_worlds_test

# Smart Keys and harmony (Phase 6: firmware/src/harmony.c, smartkeys.c, the pitch reference count in voice.c; design
# 3, 4): tests/smartkeys_test.c under ASan/UBSan (without the shift-base check: SLOOP's DSP has always shifted
# negative values left, fx.c dc_block and eng_analog.c, harmless on the target) on the factory Worlds and the test
# Worlds: every key x chord x progression x OCT against a model of design 4.1, the chord clock, held notes, same-pitch
# notes (R1), octaves, polyphony, MIDI in, SLOOP's paths, a fuzz (SK_FUZZ / SK_SEED: a longer one). Then a player
# mashing the keys live over every scene x variation of every factory World (world_render --mash): every note in key
# and in range, nothing left after STOP; the demo of the default scene of NEON RAIN and DUSTY CAFE as WAVs
smartkeys_test() {
    for w in minimal full; do
        python3 tools/worldc.py compile worlds/test/$w.world.json -o "$OUT/sk-$w.wblob" >/dev/null 2>&1 ||
            { echo "worldc cannot compile worlds/test/$w.world.json"; return 1; }
    done
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ibuild/gen \
        -Ifirmware/src -o "$OUT/smartkeys_test" tests/smartkeys_test.c -lm || return 1
    "$OUT/smartkeys_test" "$OUT/sk-minimal.wblob" "$OUT/sk-full.wblob" || return 1
    $CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/world_render" tests/world_render.c -lm || return 1
    for f in worlds/factory/*.world.json; do
        [ -f "$f" ] || continue
        id=$(basename "$f" .world.json)
        python3 tools/worldc.py compile "$f" -o "$OUT/sk-$id.wblob" >/dev/null 2>&1 || return 1
        "$OUT/world_render" --mash --scene all --var all --bars 4 --check "$OUT/sk-$id.wblob" > "$OUT/mash-$id.txt" ||
            { cat "$OUT/mash-$id.txt"; return 1; }
        echo "mash: $id: $(tail -1 "$OUT/mash-$id.txt" | sed 's/;.*//')"
    done
    for id in neon_rain dusty_cafe; do
        "$OUT/world_render" --mash --bars 8 --check --wav "$OUT/mash-$id.wav" "$OUT/sk-$id.wblob" > "$OUT/mash-$id-demo.txt" ||
            { cat "$OUT/mash-$id-demo.txt"; return 1; }
        echo "mash demo: $OUT/mash-$id.wav:$(grep '^  keys .*mashed' "$OUT/mash-$id-demo.txt" | sed 's/^  keys//')"
    done
}
run "smart keys: maps, chord clock, held notes, same pitch, octaves, polyphony, MIDI, fuzz (ASan/UBSan); key mashing" \
    smartkeys_test

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
