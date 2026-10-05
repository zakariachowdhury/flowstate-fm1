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
# 6 s through the null sink in real time (no window, no sound): PLAY, scene C asked for mid-bar and still waiting
# after the next bar line (its transition is 2 bars, Phase 11), mutes, tempo and filter, the audio heard and then silenced (the script's
# expectations), exactly 6 s played, a clean exit. Then --fast twice: the same bytes each time and, when real
# time had no underrun, the same bytes as real time (nothing dropped or doubled). Then FLOWSTATE STUDIO, --fast:
# a variation on the bar, a scene on its 2-bar line, another World chosen while one plays and switched on the bar
# without a stop (Phase 11), a
# mute and back, a level, a macro, STOP with no note left held or sounding, a SLOOP project; the Studio and
# ADVANCED drawn (--shot). Then sloop-render: every factory World clean (build/renders/worlds), and a scene
# sequence whose changes land on the bars
SIM_SCRIPT='0.95 expect world = NEON_RAIN; 1 play; 2.5 expect playing = 1; 2.5 expect scene = B; 2.5 expect rms > -35
    2.6 scene C; 2.7 expect next = C; 4.30 expect scene = B; 4.40 expect next = C
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
        python3 tools/worldc.py compile worlds/test/full.world.json -o "$OUT/world-full.wblob" >/dev/null 2>&1 &&
        python3 tools/worldc.py compile worlds/test/guard.world.json -o "$OUT/world-guard.wblob" >/dev/null 2>&1 ||
        { echo "worldc cannot compile worlds/test"; return 1; }
    $CC -g -w -fsanitize=address,undefined -fno-sanitize-recover=undefined -Ibuild/gen -Ifirmware/src \
        -o "$OUT/world_test" tests/world_test.c -lm || return 1
    "$OUT/world_test" "$OUT/world-minimal.wblob" "$OUT/world-full.wblob" "$OUT/world-guard.wblob"
}
run "worlds: FWD1 check, load, stage, commit; truncations, flips, 20000 corruptions (ASan/UBSan); proj_slot untouched" \
    world_test
run "worlds: the World modules never name proj_slot (design D6)" sh -c '! grep -n proj_slot firmware/src/world*'
$CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/regress_world" tests/regress_world.c -lm
run "regression with FELUCCA_WORLD=1 (world.c built in, no World loaded): the same golden renders" \
    env -u GOLDEN_UPDATE -u BUDGET_UPDATE "$OUT/regress_world" tests/golden.txt tests/cpu_baseline.txt

# the factory Worlds (worlds/factory, docs/worlds.md "Factory Worlds") through the real firmware with
# tests/world_render.c (wb_check, world_load, world_apply, PLAY; the macros at the World's defaults and its ENERGY
# band through arrange.c, Phase 7): each compiles within the factory limit (3,072 B; above 2,048 B a warning); every
# scene x variation, 4 bars and a 6 s tail, is heard, peaks at most -1 dBFS, has no full-scale sample and no DC,
# leaves no voice and no echo above -60 dBFS after STOP, stays in the voice budget with no held note stolen, keeps
# its notes in the scale and their registers; also with no ENERGY arrangement (--raw: every pattern plays) and with
# a player's phrase on the Smart Keys; ENERGY bands only add notes and hits; no macro mapping leaves its range or
# saturates at 100 %; the Worlds sit within 3 LU of each other at their defaults; every variation (at its own macro
# defaults) within 3 LU of ORIGINAL in each scene
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
        echo "$id: $n B; $(grep -c '^  [A-D] .* ok' "$OUT/$id.txt") renders clean; at the defaults $lufs LUFS;" \
             "variations at most $(sed -n 's/^variations: .*(the furthest \(.*\) LU).*/\1/p' "$OUT/$id.txt" | head -1) LU from ORIGINAL"
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

# The performance macros and ENERGY as arrangement (Phase 7: firmware/src/macro.c, arrange.c, guard_limits.h; design
# 5): tests/macro_test.c under ASan/UBSan on the factory Worlds and the test Worlds (extreme: mappings far past every
# range): no World, nothing moves; 625 positions of the four macros per World against an independent model (curves,
# roles, sums, rules, ranges, hard limits, 100 % never all-max), every base restored after every block, a scene
# committed inside the overlay; rules at their thresholds; smoothing per class; ENERGY bands on their beats and bars,
# with hysteresis, and what the sequencer plays obeying them; ~bright reaching the engines. Then its cost (an -O2
# build: the overlay's instructions per sample). Then every factory World at its macros' extremes (world_render
# --extremes: each macro at 0 and 1, ENERGY 0 / 0.5 / 1, all at 0, all at 1): clean, the tails gone within 6 s, ENERGY
# denser but not louder (gain compensation), no extreme far quieter than the defaults; and sloop-render's --ctl
# (the World's defaults given explicitly: the same bytes; others: other bytes) and --sweep
macros_test() {
    for w in minimal full extreme; do
        python3 tools/worldc.py compile worlds/test/$w.world.json -o "$OUT/mt-$w.wblob" >/dev/null 2>&1 ||
            { echo "worldc cannot compile worlds/test/$w.world.json"; return 1; }
    done
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ibuild/gen \
        -Ifirmware/src -o "$OUT/macro_test" tests/macro_test.c -lm || return 1
    "$OUT/macro_test" "$OUT/mt-minimal.wblob" "$OUT/mt-full.wblob" "$OUT/mt-extreme.wblob" || return 1
    $CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/macro_cost" tests/macro_test.c -lm || return 1
    "$OUT/macro_cost" --cost || return 1
    $CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/world_render" tests/world_render.c -lm || return 1
    for f in worlds/factory/*.world.json; do
        [ -f "$f" ] || continue
        id=$(basename "$f" .world.json)
        python3 tools/worldc.py compile "$f" -o "$OUT/mt-$id.wblob" >/dev/null 2>&1 || return 1
        "$OUT/world_render" --extremes --bars 4 --check "$OUT/mt-$id.wblob" > "$OUT/extremes-$id.txt" ||
            { cat "$OUT/extremes-$id.txt"; return 1; }
        sed -n 's/^extremes: //p' "$OUT/extremes-$id.txt"
    done
    r=build/host-bin/sloop-render
    $r --world "NEON RAIN" --bars 2 --analyze "$OUT/ctl-default.wav" > "$OUT/ctl-default.txt" || return 1
    $r --world "NEON RAIN" --bars 2 --ctl COLOR=0.5,MOTION=50,SPACE=0.5,ENERGY=0.552 --analyze "$OUT/ctl-same.wav" \
        > "$OUT/ctl-same.txt" || return 1
    $r --world "NEON RAIN" --bars 2 --ctl COLOR=0.2,ENERGY=0.9 --check "$OUT/ctl-moved.wav" > "$OUT/ctl-moved.txt" ||
        { cat "$OUT/ctl-moved.txt"; return 1; }
    cmp -s "$OUT/ctl-default.wav" "$OUT/ctl-same.wav" ||
        { echo "sloop-render --ctl at the World's defaults is not the default render"; return 1; }
    ! cmp -s "$OUT/ctl-default.wav" "$OUT/ctl-moved.wav" || { echo "sloop-render --ctl changed nothing"; return 1; }
    $r --world "MIDNIGHT DRIVE" --bars 4 --sweep ENERGY --check "$OUT/sweep-energy.wav" > "$OUT/sweep-energy.txt" ||
        { cat "$OUT/sweep-energy.txt"; return 1; }
    echo "render: --ctl at the defaults: the same bytes; COLOR=0.2,ENERGY=0.9: other bytes, clean; --sweep ENERGY: clean"
}
run "macros: model, limits, restore, rules, smoothing, ENERGY bands (ASan/UBSan); cost; extremes per World; --ctl" \
    macros_test

# The Musical Guardrail Engine (Phase 8: firmware/src/guard.c, the read-site clamps H6 in fx.c and voice.c, loop
# follow H12 in seq.c; docs/guardrails.md): tests/guard_test.c under ASan/UBSan (inert with no World; H6 a no-op in
# range and the clamped sound out of it; ranges, folds, polyphony, loop follow over every chord, the record guard;
# the World's soft caps and ranges; the sound combinations; the distorted tracks; random writers; the CPU guard;
# scene commits on the bar). Then tools/worldc.py's model of the macros and the guard against the C engine: the
# same slot tables (ranges, targets, effective values) for every scene x variation at 105 macro positions, on the
# factory Worlds and the test Worlds guard, full and extreme; `worldc check` on the factory Worlds. Then the sweeps
# (tests/guard_sweep.c): each detector against a signal made to fail it, and the factory Worlds over the macro space
# (quick: every scene on the 3^4 grid and 8 random points, and the corners of controls 4..15 with the macros at 100 %;
# SWEEP=full: every scene x variation, with and without a
# player, the edges and 300 random points per World, then the injected bugs of tests/guard_mutants.py)
guard_tests() {
    for w in minimal full extreme guard; do
        python3 tools/worldc.py compile worlds/test/$w.world.json -o "$OUT/gt-$w.wblob" >/dev/null 2>&1 ||
            { echo "worldc cannot compile worlds/test/$w.world.json"; return 1; }
    done
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ibuild/gen \
        -Ifirmware/src -o "$OUT/guard_test" tests/guard_test.c -lm || return 1
    "$OUT/guard_test" "$OUT/gt-minimal.wblob" "$OUT/gt-full.wblob" "$OUT/gt-extreme.wblob" "$OUT/gt-guard.wblob" ||
        return 1
    $CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/guard_sweep" tests/guard_sweep.c -lm || return 1
    for f in worlds/factory/*.world.json; do
        [ -f "$f" ] || continue
        id=$(basename "$f" .world.json)
        python3 tools/worldc.py compile "$f" -o "$OUT/gt-$id.wblob" >/dev/null 2>&1 || return 1
    done
    n=0
    for b in "$OUT"/gt-*.wblob; do
        [ "$b" = "$OUT/gt-minimal.wblob" ] && continue
        "$OUT/guard_sweep" --dump-slots "$b" > "$OUT/slots-c.txt" || return 1
        python3 tools/worldc.py model "$b" > "$OUT/slots-py.txt" || return 1
        cmp -s "$OUT/slots-c.txt" "$OUT/slots-py.txt" ||
            { echo "$b: tools/worldc.py model differs from the C engine:"; diff "$OUT/slots-c.txt" "$OUT/slots-py.txt" | head; return 1; }
        n=$((n + $(grep -c '^@' "$OUT/slots-c.txt")))
    done
    echo "model: tools/worldc.py and the C engine give the same $n slot tables (every scene x variation, 105 positions)"
    python3 tools/worldc.py check worlds/factory/*.world.json || return 1
    "$OUT/guard_sweep" --selftest || return 1
    # shellcheck disable=SC2046
    "$OUT/guard_sweep" $(ls "$OUT"/gt-*.wblob | grep -v -e gt-minimal -e gt-full -e gt-extreme -e gt-guard) || return 1
    if [ "${SWEEP:-}" = full ]; then
        python3 tests/guard_mutants.py || return 1
    fi
}
run "guardrails: rules, H6, notes, CPU (ASan/UBSan); the worldc model = the C engine; sweeps over the macro space${SWEEP:+ ($SWEEP)}" \
    guard_tests

# PLAY MODE (Phase 9: firmware/src/ui_play.c, world_store.c; design 8, 10.4): tests/ui_play_test.c on the whole
# firmware (host/core.c, FELUCCA_WORLD 1: the real UI, flash and audio) under ASan/UBSan, each scenario in its own
# process: a first boot into PLAY MODE (NEON RAIN, FIRST); every screen with its texts (the text hook) and the control
# map; CHOOSE WORLD switching on the next bar while playing and at once while stopped, HOME and the timeout cancelling;
# scenes and variations on the bar; the overlays' timeouts; EDIT held 2 s untouched -> the dialog -> ADVANCED and back;
# H17 / H26; the LEDs; the redraw cost of each change (pixels sent to the panel); LEAVE WORLD bringing the SLOOP project
# back bit-identically (and its render); the session across a reboot of the flash image; 20,000 frames of random input
# with audio (PLAY_FUZZ=n: another length); LIVE FX through the panel (FREEZE, a white key over it, FX let go, ADVANCED;
# SOUND SHAPE and MOVEMENT kept; Phase 13). The screens: $OUT/play-*.ppm
ui_play_test() {
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ihost -Ibuild/host-obj \
        -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/ui_play_test" tests/ui_play_test.c -lm || return 1
    "$OUT/ui_play_test" "$OUT" > "$OUT/ui_play.txt" 2>&1 || { cat "$OUT/ui_play.txt"; return 1; }
    grep -a 'cost\|fuzz:' "$OUT/ui_play.txt"
    echo "PLAY MODE: $(grep -ac ' ok$' "$OUT/ui_play.txt") checks passed; the screens in $OUT/play-*.ppm"
}
run "PLAY MODE: screens, control map, World / scene / variation on the bar, ADVANCED, LEAVE WORLD, session, fuzz" \
    ui_play_test

# PLAY REC (Phase 10: firmware/src/play_rec.c; design 9.1, D14; UI spec 7): tests/play_rec_test.c on the whole firmware
# (host/core.c, FELUCCA_WORLD 1) under ASan/UBSan, block by block, each scenario in its own process: a humanised phrase
# (up to +-0.45 of a step) at the World's quantise, at 0 and at 1 (nearest steps, micro-timing = the leftover x
# (1 - quantize), in key as it sounded, the take closed on the bar line, playback at step + micro-timing within a
# block, each note once a pass); the record guard (duplicates, max_poly, max_notes, out of key, lengths); REC inside a
# bar's first step; UNDO / REDO through 4 levels and the ring's wrap, CLEAR; a take across a scene and chord change, the
# loop's phase, loop follow; the loop across a reboot, cleared by a World switch; ARMED, STOP, ADVANCED (SLOOP's
# sequencer, FUN4); a fuzz of REC / keys / EDIT / scenes / PLAY with audio (REC_FUZZ=n frames). With SDL2, a phrase
# recorded headlessly in the simulator (--script: rec, keys, rec, overdub, undo)
REC_SCRIPT='1 play; 3 rec; 3.5 expect rec = 2; 4 key C4; 4.5 key E4; 5 key G4; 6.5 key A4; 7 rec; 10 expect rec = 3
    10 expect loop >= 3; 11 rec; 11.2 expect rec = 4; 11.5 key D5; 12 rec; 12.5 expect loop >= 5; 13 undo
    13.5 expect loop >= 3; 13.5 expect loop < 5; 14 stop; 16 expect gated = 0; 17 quit'
play_rec_test() {
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ihost -Ibuild/host-obj \
        -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/play_rec_test" tests/play_rec_test.c -lm || return 1
    "$OUT/play_rec_test" "$OUT" > "$OUT/play_rec.txt" 2>&1 || { cat "$OUT/play_rec.txt"; return 1; }
    grep -a 'micro-timing =\|fuzz:' "$OUT/play_rec.txt"
    echo "PLAY REC: $(grep -ac ' ok$' "$OUT/play_rec.txt") checks passed"
    if command -v sdl2-config >/dev/null 2>&1; then
        build/host-bin/flowstate-sim --demo --headless 18 --fast --mute-output --script "$REC_SCRIPT" \
            > "$OUT/sim-rec.txt" || { cat "$OUT/sim-rec.txt"; return 1; }
        [ "$(grep -c '^expect: .*: ok' "$OUT/sim-rec.txt")" -eq 8 ] ||
            { cat "$OUT/sim-rec.txt"; echo "not every REC expectation ran"; return 1; }
        echo "simulator: a phrase recorded headlessly (rec, keys, rec: a loop; rec: overdub; undo; stop): 8 expectations"
    fi
}
run "PLAY REC: the take, gentle quantise and micro-timing, record guard, layers, undo ring, scenes, session, fuzz" \
    play_rec_test

# Scene transitions and World switches (Phase 11: firmware/src/world.c wreq_block / world_commit, arrange.c fills
# and BEAT masks, macro.c commit glides, fx.c the delay's crossfade and the level / pan / delay-mix ramps; design 7,
# 5.7, 9.2; UI spec 6): tests/scene_test.c on the whole firmware (host/core.c, FELUCCA_WORLD 1) under ASan/UBSan,
# block by block, each scenario in its own process: every factory World playing with a keys loop and a held key
# through A->B, B->C, C->D, D->A and 12 random requests (some replaced on their way): each commit on the first block
# of its transition's bar line (1, 2, 4 bars or the phrase, from the section start; a variation the next line), the
# last request the one that lands, a scene from step 0 once, a variation phase-locked, the held key sounding on, the
# keys loop in phase, no sample step at a commit beyond the render's largest elsewhere, the wet bus never 6 dB down in
# the 12 ms after one, nothing left after STOP; fills on the bar before a scene's line (not without drums, muted or
# MINIMAL); 4 World switches while playing (no stop, on the next line, the new tempo and keys track, the loop cleared,
# old voices released, no silence gap, the tails going on, the macros ramping in from neutral, one replaced on its
# way); BEAT masks on the bar; ADVANCED's immediate commits; a "phrase" transition and the SCENES footer; the commit
# glides; the click detector against unsmoothed jumps on a lone note (level, pan, delay mix; a delay time) and
# smoothed ones. Variations (Phase 12): every variation of every factory World against ORIGINAL while playing, A / B / A /
# B on the bars: the same tempo and progression, every sequencer note in the scale, the controls at the variation's
# macro defaults gliding in (one the player turned left alone), back to ORIGINAL's parameters, patterns and controls
# exactly, no click, nothing left after STOP. The renders for the owner: build/renders/worlds/<id>-scenes.wav (A, B, C,
# D on their lines while playing, a key held across two of them, then a World switch, STOP and a tail), each commit
# analysed
scene_test() {
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ihost -Ibuild/host-obj \
        -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/scene_test" tests/scene_test.c -lm || return 1
    mkdir -p build/renders/worlds
    "$OUT/scene_test" "$OUT" > "$OUT/scene.txt" 2>&1 || { cat "$OUT/scene.txt"; return 1; }
    grep -a 'commits: the largest\|switches: the largest\|a lone held note' "$OUT/scene.txt" | sed 's/^scene: //'
    grep -a '^render: ' "$OUT/scene.txt" | grep -v '^render:   '
    echo "scenes: $(grep -ac ' ok$' "$OUT/scene.txt") checks passed; the renders in build/renders/worlds/*-scenes.wav"
}
run "scenes: transitions on their lines, fills, World switches without a stop, BEAT, ADVANCED, glides, clicks, tails, variations" \
    scene_test

# LIVE FX, SOUND SHAPE and MOVEMENT (Phase 13: macro.c's built-in mappings of controls 4..15 and FREEZE, guard.c
# guard_live, guard_limits.h's Smart Keys windows, punch.c; design 9.3, 9.4; UI spec 8, 10): tests/livefx_test.c on
# tests/world_render.c's harness under ASan/UBSan, each scenario in its own process: every control 4..15 at 0 / 0.5 / 1
# on every factory World and full.world (inside descriptors, ranges and the keys windows; home = the default table;
# ECHO's feedback caps, CRUSH's gain compensation and DUST cap, FILTER's sides; a World's controls replacing the
# built-in ones; the first World's values printed); FREEZE and the white keys; a FILTER sweep; FREEZE engaged and let go
# 1000 times at random while playing (never stuck, then bit-identical to the render without it); FX let go with all
# four up (a ramp home, no click); renders: ECHO + SPACE at 1 (the tail under -60 dBFS within 10 s), CRUSH at 1 (within
# 3 LU of dry), SOUND SHAPE / MOVEMENT (with MOTION) / FILTER extremes clean. Then the cost (an -O2 build): host
# instructions a sample with everything of Phase 13 at 1, under GL_CPU_BUDGET, guard.c's estimate not under it
livefx_test() {
    b=""
    for id in neon_rain midnight_drive frozen_lake dusty_cafe; do
        python3 tools/worldc.py compile worlds/factory/$id.world.json -o "$OUT/lf-$id.wblob" >/dev/null 2>&1 || return 1
        b="$b $OUT/lf-$id.wblob"
    done
    python3 tools/worldc.py compile worlds/test/full.world.json -o "$OUT/lf-full.wblob" >/dev/null 2>&1 || return 1
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ibuild/gen \
        -Ifirmware/src -o "$OUT/livefx_test" tests/livefx_test.c -lm || return 1
    # shellcheck disable=SC2086
    "$OUT/livefx_test" $b "$OUT/lf-full.wblob" > "$OUT/livefx.txt" 2>&1 || { cat "$OUT/livefx.txt"; return 1; }
    grep -a 'FREEZE: \|release (\|extremes: \|swept\|LUFS; CRUSH' "$OUT/livefx.txt" | sed 's/^livefx: //'
    echo "LIVE FX: $(grep -ac ' ok$' "$OUT/livefx.txt") checks passed"
    $CC -O2 -w -Ibuild/gen -Ifirmware/src -o "$OUT/livefx_cost" tests/livefx_test.c -lm || return 1
    # shellcheck disable=SC2086
    "$OUT/livefx_cost" --cost $b || return 1
}
run "LIVE FX, SOUND SHAPE, MOVEMENT: bounds, FREEZE, release ramps, tails, loudness, extremes (ASan/UBSan); cost" \
    livefx_test

# ADVANCED edits and user Worlds (Phase 14: world.c world_capture / world_encode, world_store.c, ui_play.c's SAVE list
# and MY WORLDS; design 10.2, 10.3): tests/userworld_test.c on the whole firmware (host/core.c, FELUCCA_WORLD 1) under
# ASan/UBSan, each scenario in its own process: edits in ADVANCED (levels, a send, a global, another engine, a step, a
# length) captured as overrides, the World playing them; a variation and back, a scene and back: the same instrument
# exactly; an edit in another scene keeps the first; SAVE + key 5 (scene changes at once); more than 64 edits; SAVE AS
# USER WORLD (NEON RAIN 2, 3) writing only its slot's sectors, offset 0xF00 erased, under 3,584 B; a reboot of the flash
# image: MY WORLDS lists them, the session's World by slot + id, the same state and a bit-identical render; a damaged
# slot and an invalid blob left out, the session falling back to NEON RAIN; 10 slots then MY WORLDS FULL; SAVE over a
# user World; DELETE (asks first); WORLD TOO BIG; RESET WORLD (asks first) = a fresh boot's World (state and render),
# a user World as last saved; LEAVE WORLD with unsaved edits asking first, the SLOOP project bit-identical
userworld_test() {
    $CC -g -w -fsanitize=address,undefined -fno-sanitize=shift-base -fno-sanitize-recover=undefined -Ihost -Ibuild/host-obj \
        -Ibuild/gen -Ifirmware/src -Ifirmware/hal -o "$OUT/userworld_test" tests/userworld_test.c -lm || return 1
    "$OUT/userworld_test" "$OUT" > "$OUT/userworld.txt" 2>&1 || { cat "$OUT/userworld.txt"; return 1; }
    grep -a 'overrides:\|render\|slot . copy' "$OUT/userworld.txt" | sed 's/^uworld: //'
    echo "USER WORLDS: $(grep -ac ' ok$' "$OUT/userworld.txt") checks passed"
}
run "ADVANCED edits and user Worlds: capture, scenes, SAVE AS / SAVE / RESET / DELETE, slots, reboot, LEAVE (ASan/UBSan)" \
    userworld_test

# validate-world (Phase 16: tools/validate-world, tools/validate_world.py; design 12.3; docs/validation.md): tests/validate_test.py.
# The four factory Worlds validate clean from the command line (the checklist of 17 items; the quick sweep of
# tests/guard_sweep.c, built into build/host/validate on first use, plays each); every World of worlds/test/bad is broken
# in one way and fails exactly its item (worldc's and the model's messages reaching the right item; a feedback tail that
# only a render catches); the JSON report's schema and the exit status; guard_sweep's every failure keyword (and its volume
# jumps) reaching its item, from made-up output; ids, directories, --user, files that are not Worlds
run "validate-world: the factory Worlds clean (quick sweep), every bad World failing exactly its item, the JSON schema" \
    python3 tests/validate_test.py

[ $fail -eq 0 ] && echo "ALL HOST TESTS PASSED" || { echo "HOST TESTS FAILED"; exit 1; }
