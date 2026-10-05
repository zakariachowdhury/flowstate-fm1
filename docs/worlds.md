<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Writing a Musical World

A Musical World is one JSON file. It holds sounds, patterns, harmony, four scenes, variations, macro mappings,
guardrails and defaults. `tools/worldc.py` compiles it into a small binary blob (`FWD1`, about 1–2 KB). The
firmware plays that blob in PLAY MODE.

Where to look:
- This guide covers every key and the notation.
- The binary format is in [design/fwd1-format.md](design/fwd1-format.md).
- The architecture is in [design/play-mode-architecture.md](design/play-mode-architecture.md).

---

## Contents

1. [Quick start](#1-quick-start)
2. [The file](#2-the-file)
3. [Metadata](#3-metadata)
4. [Tracks and sounds](#4-tracks-and-sounds)
5. [Global effects](#5-global-effects-fx)
6. [Patterns](#6-patterns)
7. [Progressions](#7-progressions)
8. [Scenes](#8-scenes)
9. [Variations](#9-variations)
10. [Macros, controls, curves](#10-macros-controls-curves)
11. [Rules](#11-rules)
12. [Energy](#12-energy)
13. [Smart Keys](#13-smart-keys)
14. [Guard](#14-guard)
15. [Defaults](#15-defaults)
16. [How the parts combine](#16-how-the-parts-combine)
17. [Limits](#17-limits)
18. [Gotchas](#18-gotchas)
19. [Factory Worlds](#19-factory-worlds)

---

## 1. Quick start

```sh
python3 tools/worldc.py names                      # engines, EDIT labels, presets, parameters, kits, lanes
python3 tools/worldc.py names ANALOG               # one engine
python3 tools/worldc.py check worlds/factory/neon_rain.world.json
python3 tools/worldc.py compile worlds/factory/neon_rain.world.json -o /tmp/neon.wblob
python3 tools/worldc.py decompile /tmp/neon.wblob  # what the device gets, as JSON (absolute notes)
```

**Listening to a World** (until the simulator plays Worlds): `tests/world_render.c` runs a compiled blob through
the real firmware and writes a WAV and a measurement (`tests/run_tests.sh` builds it into `build/host/`):

```sh
build/host/world_render --scene B --wav /tmp/b.wav /tmp/neon.wblob     # one scene, 8 bars, the measurement
build/host/world_render --scene all --var all /tmp/neon.wblob          # a table: loudness, voices, notes, hits
build/host/world_render --keys --sequence /tmp/neon.wav /tmp/neon.wblob  # A B C D, with a player's phrase
build/host/world_render --bands /tmp/neon.wblob                        # each ENERGY band of each scene
build/host/world_render --macros /tmp/neon.wblob                       # every mapping at 0 and 1, per scene
build/host/world_render --mute 234 /tmp/neon.wblob                     # track 1 alone (gain staging)
```

It emulates the ENERGY band at the World's default position until Phase 11 plays the bands (`--raw` plays every
pattern of the scene, as the Phase 5 firmware does). Macros are Phase 7: every render is the authored sound.

How Worlds reach the firmware:
- Factory Worlds live in `worlds/factory/<id>.world.json`.
- `tools/build.py` compiles all of them into the firmware, through `tools/gen_worlds.py` into `build/gen/felucca_worlds.h`. This also happens with `--gen-only`.
- A World with an error fails the build.
- `worlds/test/full.world.json` uses every feature. Start from it, or from the design's example in [design §2.4](design/play-mode-architecture.md#24-example-complete-short).

**Messages** name the place with a JSON path, then the problem:

```
neon_rain.world.json: error: $.tracks[0].sound.preset: "WARM PADD" is not a preset of ANALOG (presets: 808 BOOM, ...)
neon_rain.world.json: warning: $.patterns.old_bass: not used by any scene, fill or swap: not compiled
```

**Where the names come from.** Engine, preset, parameter, kit and lane names come from the firmware itself:
- `tools/dump_params.c` prints them into `worlds/schema/sloop-params.json`.
- After changing an engine or a preset, regenerate that file. The command is at the top of `tools/dump_params.c`.
- `tests/run_tests.sh` fails while it is stale.

---

## 2. The file

Every object accepts a `"notes"` string, which the compiler ignores; JSON has no comments. The one exception is
`guard.notes`, which holds note rules (see [§14](#14-guard)).

| Key | Required | What |
| --- | --- | --- |
| `format` | yes | `"flowstate-world/1"` |
| `id` | yes | `[a-z0-9_]{1,24}`. The device id is FNV-1a-32 of it, and two factory Worlds may not collide. |
| `name` | yes | up to 14 characters of `A–Z 0–9 space & ' - .` |
| `category` | yes | up to 10 characters, the same set. The World list sorts by category, then name. |
| `blurb` | no | up to 24 printable Latin-1 characters |
| `tempo` | yes | [§3](#3-metadata) |
| `key` | yes | [§3](#3-metadata) |
| `swing` | no | 0–100 (`G_SWING`), default 0 |
| `tracks` | yes | exactly 4 ([§4](#4-tracks-and-sounds)) |
| `fx` | no | [§5](#5-global-effects-fx) |
| `patterns` | no | [§6](#6-patterns) |
| `progressions` | yes | [§7](#7-progressions) |
| `scenes` | yes | A, B, C and D ([§8](#8-scenes)) |
| `variations` | no | up to 8, and the first is `ORIGINAL` ([§9](#9-variations)). Default: `{"ORIGINAL": {}}`. |
| `macros`, `controls`, `curves` | no | [§10](#10-macros-controls-curves) |
| `rules` | no | [§11](#11-rules) |
| `energy` | no | [§12](#12-energy) |
| `smart_keys` | yes | [§13](#13-smart-keys) |
| `guard` | no | [§14](#14-guard) |
| `defaults` | no | [§15](#15-defaults) |

---

## 3. Metadata

```json
"tempo": {"bpm": 96, "min": 84, "max": 108},
"key": {"root": "A", "scale": "MIN"},
"swing": 10
```

**Tempo.**
- `bpm` is 40–240.
- `min`/`max` bound the player's tempo nudge (GLO + SELECT).
- They default to ±10 % and may be at most ±20 %.

**Key.**
- `root` is `C`…`B`, with `#` or `b` (`F#`, `Bb`).
- `scale` is one of SLOOP's 16: `CHR MAJ MIN DOR MIX PEN MPEN HARM PHRY LYD LOC MEL BLUES WHOLE DIMHW DIMWH`.
- The key is written to every synth track, and no scene or variation can change it.

---

## 4. Tracks and sounds

There are four tracks:
- The first three are synth tracks.
- The fourth is the drum track. It has role `drums` and no engine.
- One synth track is the Smart Keys track (`smart_keys.track`). It plays the player's loop and never a pattern.

```json
"tracks": [
  {"name": "pad",   "role": "pad",  "sound": {"engine": "ANALOG", "preset": "WARM PAD", "params": {"CUT": 60, "rev": 50, "WAVE": "SQR"}}},
  {"name": "bass",  "role": "bass", "register": "A1", "sound": {"engine": "ANALOG", "preset": "SUB BASS"}},
  {"name": "keys",  "role": "lead", "register": "A3", "sound": {"engine": "TRIO", "preset": "SYNC LEAD", "params": {"dly": 40}}},
  {"name": "drums", "role": "drums", "sound": {"kit": "808", "params": {"level": 104, "rev": 12, "pan": -8}}}
]
```

| Key | What |
| --- | --- |
| `name` | `[a-z][a-z0-9_]{0,11}`, unique. Patterns, targets and scenes use it. `g` and `*` are taken. |
| `role` | `pad chords bass lead keys texture drums` |
| `register` | the floor of the part's home octave, as a note name (C4 = 60). Defaults: pad, chords, keys and texture C3; bass C2; lead C4. |
| `sound.engine` | `ANALOG DIGITAL PHASE LOFI SAMPLE VOICE TRIO WHEEL GRAIN` |
| `sound.preset` | a factory preset name of that engine (`worldc.py names ENGINE`) |
| `sound.kit` | drum track: `ORIGINAL DEEP TIGHT BRIGHT DUST 808 909 606 VINTAGE 80S TRAP DRILL BOOMBAP LO-FI HOUSE TECHNO MINIMAL ELECTRO JUNGLE DUBSTEP DEMBOW AFRO LATIN DISCO SYNTHWV CHIP 8BIT ARCADE GLITCH INDUSTR TRIBAL HYPER AMBIENT JAZZ`. Default 808. |
| `sound.params` | values set on top of the preset |

**Parameter names.**
- **Common parameters** use the lowercase SLOOP name: `level atk dec sus rel ed_flt ed_pit ed_shp lrate lwave lphase lfade ld_pit ld_flt ld_shp ld_amp sgate dist chor dly rev voice glide pan glmode prio alloc detune slcr slpat slrate sldepth chord quant trans sswing` and the arp group `amode … aorder`.
- **Engine parameters** use that engine's EDIT label in capitals, for example `CUT RES DRV` on ANALOG or `IDX FB` on DIGITAL. The same label can mean a different slot on another engine, so it is resolved against the track's engine. `e0`…`e7` also work.
- Values are absolute and must lie inside the parameter's range. Enum values may be given by name (`"WAVE": "SQR"`, `"lwave": "TRI"`, `"voice": "MONO"`).

**Parameters a World never sets**, because the World itself sets them:
- `root` and `scale` come from the key;
- `slen` and `sdiv` come from the pattern;
- `mute` belongs to the player and to ENERGY;
- `ed_fx` is unused.

**The drum track** has these parameters:
- `level` and `rev`. These are really the globals `g.drlvl`/`g.drrev`; worldc maps them for you.
- `pan`, `sswing`, `slcr`, `slpat`, `slrate`, `sldepth`.

**The Smart Keys track's arpeggiator** (`amode … aorder`) belongs to PULSE (`defaults.pulse`), so do not set it.

---

## 5. Global effects (`fx`)

```json
"fx": {"dtime": "1/8", "dfdbk": 50, "dmix": 80, "rsize": 96, "rdamp": 60, "dust": 3}
```

The only globals a World may set are:

```
dtime dfdbk dcolor dmix rsize rdamp crate cdepth drlvl drrev dust duck filt
```

`swing` is set at the top level, not here. Unset globals take their SLOOP defaults. `worldc.py names` lists the ranges.

---

## 6. Patterns

```json
"patterns": {
  "bass_main": {"track": "bass", "steps": "c1 . c1 . c1 . c5 . c1 . c1 . c1 . c5 ."},
  "dr_main":   {"track": "drums", "lanes": {"kick": "x...x...x...x...", "snare": "....x.......x...", "hat": "x.x.x.x.x.x.x.x."}}
}
```

| Key | What |
| --- | --- |
| `track` | the track the pattern belongs to; it only plays there |
| `div` | `1/4 1/8 1/16 1/32 8T 16T`, default `1/16`. Drums are always `1/16`. |
| `steps` | melodic notation (synth tracks) |
| `lanes` | `{lane: string}` (the drum track) |

### Drum patterns

Each lane is one string with one character per step. The length is 16, 32 or 64, and the same for every lane of
the pattern. `|` may separate bars of 16.

The lanes are:

```
kick kick2 snare clap hat open pedal rim snare2 tomlo tomhi crash ride shaker conga bell
```

SLOOP's own names also work, lowercase without spaces (`openhat`, `lowtom`, `cowbell`, …).

| Character | Step |
| --- | --- |
| `.` | nothing |
| `x` | a normal hit |
| `X` | a hard hit |
| `s` | a soft hit |
| `g` | a ghost hit |
| `2` `3` `4` | a ratchet: ×n hits in the step, normal level |

### Melodic patterns

Steps are separated by spaces. `|` separates bars; each bar then holds exactly the steps of one bar at that
`div` (16 at 1/16, 8 at 1/8, 12 at 8T, …). A pattern is 1–64 steps. Before chord tokens are unrolled, that is the
number of steps.

| Token | Meaning |
| --- | --- |
| `1` … `7` | a degree of the World's scale (a pentatonic scale has degrees 1–5). Degree 1 is the track's tonic: the lowest root at or above the register. |
| `b3` `#4` | accidentals before the degree (repeatable) |
| `5'` `1,` | octave marks after the degree: `'` up, `,` down (repeatable) |
| `c1` `c3` `c5` `c7` `c9` | root, 3rd, 5th, 7th and 9th of the chord sounding at that step. Octave marks work (`c1,`). |
| `A#3`, `Bb2`, `C4` | an absolute note (C4 = 60). Capital letter. |
| `[c1 c3 c5]` | up to 4 notes in one step (any of the above). Duplicates are merged. |
| `.` | rest |
| `-` | tie: the previous step's notes go on |
| suffix `!` | accent (`SF_ACCENT`) |
| suffix `_` | soft (every note of the step) |
| suffix `~` | slide into the next step |
| suffix `*2` `*3` `*4` | ratchet ×n (every note of the step) |

Suffixes come after the token (`c1!`, `[1 5]_`, `c9*2`) and can be combined (`c7~!`).

**Chord tokens follow the progression.** They are resolved against the progression of each scene that plays the
pattern:

- **The chord root.** It is placed in the track's register: within a fifth below to a tritone above the track's tonic. In A minor with a bass register A1, i is A1, VI is F1, III is C2 and VII is G1. The bass and pads therefore move by small steps.
- **The tones.** `c3`, `c5` and `c7` are the chord's own tones, so `c3` is the minor third on a minor chord. When the chord lacks a tone, the token gives the root an octave up: `c7` on a triad, `c3` on a power chord. On a 6th chord, `c7` is the 6th.
- **The 9th.** `c9` is the major 9th, unless that note is outside the World's scale and the minor 9th is inside it.
- **Unrolling.** A pattern with chord tokens is unrolled to the shortest repeat of the pattern against the progression: at most `lcm(pattern length, progression length in steps)`, and at most 64 steps. A 16-step bass line over a 16-beat progression at 1/16 becomes 64 steps; over `["i:4", "i:4"]` it stays 16.
- **One copy per progression.** The pattern is compiled once for each progression it meets, and identical results are shared. This counts against the pool of 16 ([§17](#17-limits)).
- **Too long.** If the repeat is longer than 64 steps (a 1/16 pattern over a 32-beat progression is 128 steps), compiling fails. Use a slower `div`, a shorter progression, or fewer chord tokens.

**Degrees do not move with the chords.** Degrees and absolute notes are fixed. Only `c*` tokens follow the
harmony.

---

## 7. Progressions

```json
"progressions": {"main": ["i:4", "VI:4", "III:4", "VII:4"], "dark": ["i:8", "iv7:8"]}
```

A progression holds 1–16 chords, written `numeral[suffix][:beats]`:
- Beats are 1–32 and default to 4.
- The total must be 4, 8, 16 or 32 beats.
- Chords change on beats only.

**Numerals.**
- `I II III IV V VI VII` are major-case and `i ii iii iv v vi vii` minor-case.
- **Numerals count degrees of the World's scale, not of the major scale.** In A minor, `VII` is G and `III` is C. In D dorian, `VII` is already C, so `bVII` is B.
- `b` or `#` before the numeral moves its root by a semitone.
- For pentatonic and other non-7-note scales, numerals count the parent scale: MPEN and BLUES count the natural minor; PEN, CHR, WHOLE and the diminished scales count the major.

**Quality** comes from the case and the suffix:

| Written | Quality | Written | Quality |
| --- | --- | --- | --- |
| `I` | major | `i` | minor |
| `I7` | dominant 7 | `i7` | minor 7 |
| `Imaj7`, `IM7` | major 7 | `iø`, `im7b5` | half-diminished |
| `i°`, `io`, `idim` | diminished | `i°7`, `io7`, `idim7` | diminished 7 |
| `I+`, `Iaug` | augmented | `I5` | power chord |
| `Isus2` | sus2 | `Isus4`, `Isus` | sus4 |
| `I6` | major 6 | `i6` | minor 6 |
| `Iadd9`, `I9` | add 9 | `iadd9`, `i9` | minor add 9 |

There are no 9th chords with a 7th: `9` means add9. A minor-major 7th (`imaj7`) is refused.

---

## 8. Scenes

```json
"scenes": {
  "A": {"name": "INTRO", "role": "intro", "progression": "dark", "energy": "e",
        "patterns": {"pad": "pad_main", "bass": null, "drums": {"GROOVE": "dr_break"}}},
  "B": {"name": "MAIN", "role": "main", "progression": "main", "energy": "e", "fill": "dr_fill",
        "patterns": {"pad": "pad_main", "bass": "bass_main", "drums": {"GROOVE": "dr_main", "BUSY": "dr_busy", "BREAK": "dr_break"}}},
  "C": {"name": "LIFT", "role": "lift", "progression": "main", "transition": 2, "params": {"pad.CUT": 80, "*.chor": 20},
        "patterns": {"pad": "pad_stab", "bass": "bass_drive", "drums": "dr_busy"}},
  "D": {"name": "BREAKDOWN", "role": "breakdown", "progression": "dark", "fx": {"rsize": 115},
        "patterns": {"pad": "pad_main", "drums": null}}
}
```

All four scenes are required.

| Key | What |
| --- | --- |
| `name` | up to 10 characters of `A–Z 0–9 space & ' - .` |
| `role` | `intro main lift breakdown` |
| `progression` | the progression's name |
| `energy` | an energy table's name ([§12](#12-energy)), or absent |
| `transition` | 1, 2 or 4 bars (default 1): how a change to this scene is quantised (Phase 11) |
| `patterns` | per track: a pattern name, or `null` (absent). A track not listed is absent too. |
| `patterns.<drums>` | a pattern name (the GROOVE), `null`, or `{BEAT: name}` for `MINIMAL GROOVE BUSY BREAK`. A BEAT without a pattern plays the GROOVE. |
| `fill` | a drum pattern, played on phrase ends when an ENERGY band allows fills (Phase 11) |
| `params` | `track.param`, `*.param` (every synth track; common parameters only) or `g.name` → value |
| `fx` | globals → value |

What a scene cannot change:
- the tempo, the key or the engines;
- the Smart Keys track's pattern;
- structural parameters: the voice mode, the arpeggiator, the slicer mode and pattern, LFO wave, enum engine parameters (only WHEEL's ROTR is allowed), swing on a track, and so on. `worldc.py names` marks them.

---

## 9. Variations

```json
"variations": {
  "ORIGINAL": {},
  "AIRY":    {"params": {"pad.rev": 80}, "fx": {"rsize": 110}, "energy_bias": -0.1},
  "PULSING": {"swap": {"bass_main": "bass_drive"}, "energy_bias": 0.1},
  "HEAVY":   {"sounds": {"pad": "DARK STR", "drums": "909"}, "params": {"bass.DRV": 40}}
}
```

Variations are ordered. The first must be `"ORIGINAL": {}`, and there may be up to 8. Names follow the scene rules.

| Key | What |
| --- | --- |
| `sounds` | per track: another preset of the same engine, or another drum kit |
| `params`, `fx` | as in scenes. A variation cannot set `swing`. |
| `swap` | `{from: to}`: wherever a scene plays synth pattern `from`, play `to` (a pattern of the same track). Drum grooves are not swapped (use the BEATs). |
| `energy_bias` | −0.25..0.25, added to the ENERGY knob (Phase 11) |

---

## 10. Macros, controls, curves

```json
"macros": {
  "COLOR":  [{"to": "*.~bright", "min": -40, "max": 28, "curve": "exp"}, {"to": "keys.@RESO", "min": -10, "max": 18}],
  "SPACE":  [{"to": "*.rev", "min": -30, "max": 45, "curve": "s"}, {"to": "g.dfdbk", "min": -30, "max": 30}],
  "ENERGY": [{"to": "*.level", "min": 4, "max": -4}, {"to": "drums.level", "min": -10, "max": 6}]
},
"controls": {"SOFT": [{"to": "keys.atk", "max": 60, "curve": "exp"}]},
"curves": {"knee": {"points": [[0, 0], [0.5, 0.2], [1, 1]]}}
```

A **mapping** moves one target by an offset from the authored value.

**The knob position.** A macro sits at 50 % (home):
- turned down, the offset runs to `min`;
- turned up, it runs to `max`;
- at home the sound is exactly what the World says.

Controls that rest at 0 (SOFT, SHORT, BODY, TAIL, DRIFT, WOBBLE, PULSE, ECHO, CRUSH, FREEZE) use only `max`. RATE and FILTER rest at 50 %. Phase 7 evaluates the mappings; this phase compiles them.

**Targets:**

| Form | Meaning |
| --- | --- |
| `pad.CUT`, `bass.rev` | a track's parameter |
| `*.level` | the parameter on every synth track (common parameters only) |
| `pad+bass.chor` | on these tracks |
| `keys.@RESO` | the engine role: `@BRIGHT @RESO @DRIVE @SHAPE @DETUNE @AIR @MOVE @BODY`. On an engine without that role the mapping does nothing, and worldc warns. `worldc.py names ENGINE` lists the roles. |
| `pad.~bright`, `pad.~shape` | the smooth brightness / timbre offset every engine has |
| `g.dfdbk` | a World global (not `dtime` or `swing`) |
| `drums.level` | the same as `g.drlvl` |

**Mapping keys:**
- `min` and `max`: −128..127 in the target's units.
- `curve`:
  - `lin` (the default), `exp` (slow start), `log` (fast start), `s` (smoothstep), `late` (nothing until halfway);
  - a name from `curves`;
  - an inline 9-value curve.
- `smooth`: `fast medium slow stepped`, default `medium`. Enum targets are always stepped.
- `saturate`: for Phase 8's "100 % is never all-max" check.

**Limits.** There are at most 40 mappings in all, counting `controls`.

**Controls** (`SOFT SHORT BODY TAIL DRIFT WOBBLE PULSE RATE FILTER ECHO CRUSH FREEZE`) replace that control's built-in mappings with yours.

**Curves.** Each curve is one of:
- 9 values 0..1 (at x = 0, 1/8, …, 1);
- `{"points": [[x, y], …]}`, from x = 0 to x = 1, interpolated;
- `{"lut": [9 integers 0..255]}`.

A curve must start at 0 and end at 1. There are at most 8.

---

## 11. Rules

```json
"rules": [
  {"if": {"SPACE": 0.8, "ENERGY": 0.8}, "then": [{"to": "bass.rev", "add": -30}, {"to": "g.dfdbk", "add": -25}]},
  {"if": {"MOTION": 0.9}, "then": [{"to": "g.cdepth", "add": -30}]}
]
```

When every named control is above its threshold, each action adds up to `add` to its target, scaled by how far
past the thresholds the controls are (design §5.4).

Limits:
- one or two conditions, each with a threshold below 1;
- one to four actions;
- up to 8 rules;
- targets as for macros.

---

## 12. Energy

```json
"energy": {"e": {"density_lanes": ["hat", "open"], "bands": [
  {"from": 0.00, "layers": ["pad", "keys"], "drums": ["kick", "hat"], "density": "x...x...x...x..."},
  {"from": 0.35, "layers": ["pad", "bass", "keys", "drums"], "bass": "x.x.x.x.x.x.x.x."},
  {"from": 0.65, "layers": "all", "drums": "all", "fills": true},
  {"from": 0.90, "layers": "all", "fills": true, "ratchets": true}]}}
```

There are up to 4 tables. Each scene names one. A table has 1–4 bands. The ENERGY knob (plus the variation's
bias) picks the band. Phase 11 makes them sound.

| Key | What |
| --- | --- |
| `from` | where the band starts: 0 for the first, then ascending |
| `layers` | the tracks heard, or `"all"`. The Smart Keys track must be in every band. |
| `drums` | the drum lanes allowed, or `"all"` (the default) |
| `density` | 16 steps of `x`/`.` applied to `density_lanes`: a hit there sounds only on an `x` |
| `<synth track>` | 16 steps of `x`/`.`: that track's notes play only on an `x` |
| `fills`, `ratchets` | allow the scene's fill, and ratchets |

---

## 13. Smart Keys

```json
"smart_keys": {"track": "keys", "mode": "melody", "melody_scale": "MPEN", "white": "scale", "black": "chord+9", "tonic": "A3", "range": ["E2", "C6"]}
```

| Key | What |
| --- | --- |
| `track` | the synth track the keys play (required). It never gets a scene pattern. |
| `mode` | `melody` (later: `chords bass drums`) |
| `melody_scale` | a scale name, or a list of semitones above the root. It must contain the root and stay inside the World scale. Default: PEN if it fits the scale, else MPEN, else the scale itself. |
| `white` | `scale` (fixed white keys, the default) or `safe` |
| `black` | `chord` (the default) or `chord+9` |
| `tonic` | the note of the C4 key. It must be the root. Default: the root nearest C4. |
| `range` | the note range of the keys; the same as `guard.notes.<track>.range` |

The Smart Keys mapping itself arrives in Phase 6 (design §4).

---

## 14. Guard

```json
"guard": {
  "notes": {"keys": {"max_poly": 4, "loop_follow": "snap", "avoid": "classic"}, "bass": {"range": ["E1", "A3"]}},
  "record": {"quantize": 0.75, "max_notes": 3},
  "sound": {"ranges": {"keys.RES": [0, 100], "g.dfdbk": [0, 90]}, "max_level": 116, "max_dfdbk": 90},
  "arrangement": {"mute_change": "bar", "density_change": "beat", "fills_every": 4, "min_band_bars": 2},
  "cpu": {"max_unison": 4, "grain_dens": 90, "max_dist_tracks": 2, "ceiling": 0.85}
}
```

Every field is optional. An unset field takes the firmware default (design §6.1). `guard.notes` is keyed by synth
track. `max_poly`, `loop_follow` and `avoid` apply to the Smart Keys track only.

`sound.ranges` keys are targets as in [§10](#10-macros-controls-curves). The other fields and their ranges:

| Field | Range |
| --- | --- |
| `max_level`, `max_reso`, `max_rsize`, `max_dist`, `max_dust` | 0–127 |
| `max_dfdbk` | 0–120 |
| `mute_change` | `bar` or `2bars` |
| `density_change` | `beat` or `bar` |
| `fills_every` | 1–32 bars |
| `min_band_bars` | 1–16 |
| `max_unison` | 1–8 |
| `grain_dens` | 0–127 |
| `max_dist_tracks` | 0–3 |
| `ceiling` | a fraction of the audio interrupt's time |

Phase 8 applies the guard.

---

## 15. Defaults

```json
"defaults": {"scene": "B", "variation": "ORIGINAL", "macros": [0.5, 0.5, 0.5, 0.6], "pulse": "SLOW", "beat": "GROOVE",
             "shape": [0, 0.2, 0, 0], "movement": [0, 0, 0, 0.5]}
```

| Key | Default |
| --- | --- |
| `scene` | the first scene with role `main`, else A |
| `variation` | the first |
| `macros` | COLOR MOTION SPACE ENERGY, 0..1, all 0.5 |
| `pulse` | `OFF SLOW PULSE DRIVE`, default OFF |
| `beat` | `MINIMAL GROOVE BUSY BREAK`, default GROOVE |
| `shape` | SOFT SHORT BODY TAIL, all 0 |
| `movement` | DRIFT WOBBLE PULSE RATE, 0, 0, 0, 0.5 |

Positions are stored in steps of 1/250.

---

## 16. How the parts combine

For each track the firmware starts from SLOOP's defaults, then applies these layers. Later steps win.

1. **The preset.** This is the factory preset, as the PRESETS knob loads it. If the variation swaps the sound, its preset is used instead.
2. **The track's `sound.params`.** These still apply after a variation's preset swap.
3. **The variation's `params`.**
4. **The scene's `params`.**
5. **(Phase 14) The player's edits from Advanced Mode.**

Then every value is clamped to its range.

The globals follow the same order: SLOOP defaults, then `swing`, `fx`, the variation's `fx` and the scene's `fx`.

**On a scene change:**
- The Smart Keys track keeps its loop and the PULSE settings.
- Each other track keeps any edits made to its outgoing pattern; they come back when that pattern returns.

---

## 17. Limits

| | Limit |
| --- | --- |
| Blob size | above 2,048 B worldc warns; above 3,072 B a factory World fails. `check` prints the size. |
| Patterns | 16 in the pool, **after** unrolling: a chord-token pattern counts once per progression it plays over, and each swap target counts. Unused patterns are not compiled, and worldc warns. |
| Pattern length | 64 steps, also after unrolling |
| Progressions | 8 used, each 1–16 chords, 4/8/16/32 beats |
| Variations | 8, ORIGINAL first |
| Mappings | 40, counting `controls` |
| Curves | 8 |
| Rules | 8, each with up to 4 actions |
| Energy | 4 tables of up to 4 bands |
| Guard | 32 sound ranges |
| Pairs | 64 per track, scene or variation |

The design's §2.4 example compiles to 930 B, and `worlds/test/full.world.json` to 1,158 B. Most of a World's
bytes are its patterns: a chord-token bass line unrolled to 64 steps is about 80–140 B. Repeated notes are cheap,
because a step that repeats the previous note costs 2 B.

---

## 18. Gotchas

- **`c1` is not `C1`.** Lowercase `c1`…`c9` are chord tokens; capital `C1` is the note C1.
- **Numerals count the World's scale.** `VII` in A minor is G major, not G#. Write `bVII` only when you mean a semitone below the scale's 7th.
- **Degrees count the World's scale too.** In a pentatonic World, degree 3 is the third note of the pentatonic (D in A MPEN), not the third of the key.
- **Chord tokens multiply patterns.** Two progressions × 3 chord-token patterns = 6 pool entries. Watch the pool count.
- **One pattern, one track.** A pattern belongs to one track. A bass line for the pad track needs its own pattern.
- **Drum level is global.** `drums.level`/`drums.rev` are `g.drlvl`/`g.drrev`. They can be macro targets, but only on their own (not `drums+pad.level`).
- **`*` means synth tracks only, and common parameters only.** Use one mapping per track for engine labels (`pad.CUT`, `bass.CUT`).
- **Structural parameters belong to the sound.** Voice mode, arpeggiator, slicer mode, LFO wave and engine enums can only be set in `tracks[].sound.params`, never in scenes, variations or macros.
- **Leave the keys track's arpeggiator alone.** PULSE owns it.
- **Preset swaps keep your params.** A variation that swaps the preset still gets the track's `sound.params` on top. If the World sets `CUT` for one preset, check that it suits the swapped one too.
- **Ties start nothing.** A tie after a rest does nothing; a tie at step 1 continues the last step across the loop.
- **Check the size.** Size grows fastest with many distinct notes per step over long unrolled patterns. `check` prints the byte count of every World.
- **A scene's value wins over the variation's.** Scene parameters are absolute and come last
  ([§16](#16-how-the-parts-combine)): a scene that sets `pad.CUT` erases the DARK variation's `pad.CUT` in that scene,
  and a scene `ed_flt` replaces the filter envelope of whatever preset a variation swapped in. Let scenes and
  variations move different parameters; brighten a scene with accents (`!`) or `ed_flt` on a track whose presets
  all share one filter envelope.
- **Levels are not linear.** A track's `level` is 0.5 dB per step (112 is 0 dB). The drum level (`drums.level`,
  `g.drlvl`) is linear. A note's velocity is linear in amplitude: an accent (127) is about +2.4 dB over a plain step
  (96), soft (72) −2.5 dB, ghost (42) −7 dB; drum hits are 100, hard 127, soft 72, ghost 42.
- **Sine basses are loud.** A sustained `SUB BASS` at level 96 alone measured −11 LUFS, louder than a whole backing
  should be. Measure each part alone (`world_render --mute`).
- **Fill toms hit like kicks.** The synthesised kits' toms peak as high as the kick: write fills with soft toms.
- **`c9` follows the scale.** On a chord whose major ninth is outside the World's scale (iii and vii in a major key,
  ii and v in a natural minor), `c9` is the minor ninth: a rootless voicing with `c9` turns harsh there. Keep such
  chords out of progressions that play 9th voicings.
- **Releases hold voices.** A released voice sounds until its envelope is 72 dB down, about 1.8 × its REL time. At
  each chord change the old chord's voices overlap the new one's, and the three synth tracks share 8 voices: keep a
  World's own patterns to about 6 voices so the player's keys never take a held note (`world_render` counts it).
- **Keep ENERGY band edges away from the defaults.** The default position plus each variation's `energy_bias` should
  sit clearly inside a band (the factory Worlds keep 0.05 or more), or a variation drops a layer by accident.
- **Long echoes need room.** A `1/4` delay at 66–72 BPM repeats every ~0.85 s; with SPACE at 100 % the feedback
  must still let the tail fall below −60 dBFS within 6 s of STOP (design 12.3). The factory Worlds keep the base
  `dfdbk` at 60 or below there and SPACE adds at most +8.
- **A swap is one pattern for one.** A pattern without chord tokens that plays over several progressions is one
  pool entry: it cannot swap to a chord-token pattern, which differs per progression (worldc refuses it). Give that
  scene its own pattern.
- **Preset transposition is for the keys.** `TRANS` (−24 on SUB BASS, −12 on GB BASS) moves what the keys play;
  pattern notes sound as written.
- **The Smart Keys reach past the top white key.** The black keys climb to chord tones above the highest white key
  (design 4.1): with the tonic D4 and MPEN the 27 keys reach C7. `range` must hold all of them.

---

## 19. Factory Worlds

The four demo Worlds of the UI spec (§11). Each starts on scene B with ORIGINAL, the macros at home and PULSE off.
Every scene × variation is rendered by `tests/run_tests.sh` ([§1](#1-quick-start)).

| World | Category | BPM | Key, Smart Keys | Tracks: 1 · 2 · 3 (Smart Keys) · drums | Scenes A · B · C · D | Variations | Blob |
| --- | --- | --- | --- | --- | --- | --- | --- |
| NEON RAIN `neon_rain` | CINEMATIC | 72 | D minor; MPEN from D4 | pad: ANALOG WARM PAD · bass: ANALOG SUB BASS · lead: ANALOG G-FUNK LD · 808 | INTRO · MAIN · LIFT · BREAKDOWN | ORIGINAL DREAMY DARK PULSING HEAVY | 1,356 B |
| MIDNIGHT DRIVE `midnight_drive` | SYNTHWAVE | 100 | A minor; MPEN from A3 | chords: ANALOG SYN BRASS · bass: TRIO FAT BASS · lead: LOFI GAME LEAD · SYNTHWV | IGNITION · CRUISE · OVERDRIVE · TUNNEL | ORIGINAL DRIVING DREAMY DARK HEAVY | 1,786 B |
| FROZEN LAKE `frozen_lake` | AMBIENT | 66 | E major; PEN from E4 | pad: ANALOG ATMOS PAD · texture: GRAIN VIBE HAZE · bell: DIGITAL MUSIC BOX · AMBIENT | FIRST ICE · STILLNESS · AURORA · DEEP ICE | ORIGINAL AIRY FLOATING DARK SPARSE | 1,297 B |
| DUSTY CAFE `dusty_cafe` | LO-FI | 82, swing 34 | F major; PEN from F4 | keys: DIGITAL RHODES · bass: SAMPLE UP BASS · piano: SAMPLE LOFI KEYS · LO-FI | STEAM · WARM CUP · SUNLIGHT · LAST CALL | ORIGINAL DREAMY AIRY DARK SPARSE | 1,428 B |

**Harmony per scene** (A · B · C · D):

| World | A | B | C | D |
| --- | --- | --- | --- | --- |
| NEON RAIN | i9 VImaj7, 2 bars each | i9 VImaj7 iv7 VII over a D pedal | VImaj7 VIIsus4–VII i9 III | VImaj7 i9 |
| MIDNIGHT DRIVE | i VII | i v VI VII | VI VII III v | VI iv |
| FROZEN LAKE | I(add9) IVmaj7, 4 bars each | Imaj7 vi7 IVmaj7 I(add9), 2 bars each | IVmaj7 Vsus4 vi7 I(add9) | vi(add9) IVmaj7, 4 bars each |
| DUSTY CAFE | IVmaj7 Imaj7 | IVmaj7 Imaj7 vi7 ii7 | ii7 V7 Imaj7 vi7 | IVmaj7 ii7 |

**ENERGY** at the default position plays: NEON RAIN A the pad, B pad + drone + heartbeat kick, rim and shaker
eighths, C everything but the open hat, with fills, D pad + drone; MIDNIGHT DRIVE A the chords, B kick, snare and
eighth hats under the pulse (sixteenths, open hats and fills from 0.75), C the same with clap, open hats and fills,
D chords + bass; FROZEN LAKE A the pad, B and C pad + texture + the soft percussion (C with fills), D the pad; DUSTY
CAFE A the Rhodes, B kick, snare, rim and eighth hats (ghost sixteenths, shaker and fills from 0.75), C everything
with fills, D Rhodes + bass. Lower ENERGY removes the drums, then the bass; the bands only ever add.

**Gain staging.** The levels follow one plan, measured with `world_render` (BS.1770 loudness):
- the default scene at about −16.5 LUFS (the four Worlds within 1 LU of each other), RMS −17.5 to −18.5 dBFS;
- A and D (without drums at the default ENERGY) 0.5–3.5 LU under B, C level with B or up to 1.5 LU above;
- each variation within about 2 LU of ORIGINAL in the same scene (a preset swap gets a `level` to match), unless
  its ENERGY bias changes the band: MIDNIGHT DRIVE's DRIVING and HEAVY bring the bass into A (+3 LU);
- the Smart Keys instrument alone, playing a beginner's phrase, 1–2 LU under the backing (the music box, which
  decays at once, 4.5 LU under, with the same peaks);
- the output limiter rarely over 1 dB (the synthwave groove, the densest, 13–18 % of the time and never over
  6 dB), every peak under −3 dBFS.
