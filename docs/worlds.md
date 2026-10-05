<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Writing a Musical World

A Musical World is one JSON file. It holds sounds, patterns, harmony, four scenes, variations, macro mappings,
guardrails and defaults. `tools/worldc.py` compiles it into a small binary blob (`FWD1`, about 1–2 KB). The
firmware plays that blob in PLAY MODE.

Where to look:
- This guide covers every key and the notation.
- The authoring tool edits all of it in a local web page, checks it as you type, and plays it in the simulator:
  [authoring.md](authoring.md) (`./tools/world-author`).
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
20. [User Worlds (MY WORLDS)](#20-user-worlds-my-worlds)

---

## 1. Quick start

```sh
python3 tools/worldc.py names                      # engines, EDIT labels, presets, parameters, kits, lanes
python3 tools/worldc.py names ANALOG               # one engine
python3 tools/worldc.py check worlds/factory/neon_rain.world.json
python3 tools/worldc.py compile worlds/factory/neon_rain.world.json -o /tmp/neon.wblob
python3 tools/worldc.py decompile /tmp/neon.wblob  # what the device gets, as JSON (absolute notes)
./tools/world-author                               # the authoring tool: forms, checks, the model, the simulator
```

**Listening to a World** (until the simulator plays Worlds): `tests/world_render.c` runs a compiled blob through
the real firmware and writes a WAV and a measurement (`tests/run_tests.sh` builds it into `build/host/`):

```sh
build/host/world_render --scene B --wav /tmp/b.wav /tmp/neon.wblob     # one scene, 8 bars, the measurement
build/host/world_render --scene all --var all /tmp/neon.wblob          # a table: loudness, voices, notes, hits
build/host/world_render --keys --sequence /tmp/neon.wav /tmp/neon.wblob  # A B C D, with a player's phrase
build/host/world_render --bands /tmp/neon.wblob                        # each ENERGY band of each scene
build/host/world_render --macros /tmp/neon.wblob                       # every mapping at 0 and 1, per scene
build/host/world_render --extremes /tmp/neon.wblob                     # each macro at 0 and 1, all at 0, all at 1
build/host/world_render --ctl COLOR=0.2,ENERGY=0.9 --wav /tmp/x.wav /tmp/neon.wblob   # the macros somewhere
build/host/world_render --mute 234 /tmp/neon.wblob                     # track 1 alone (gain staging)
```

The macros start at the World's `defaults` and play through the firmware's own macro engine and ENERGY arrangement
(`firmware/src/macro.c`, `arrange.c`): `--energy X` and `--ctl` move them. `--raw` plays every pattern of the scene,
with no ENERGY arrangement. The simulator plays Worlds too ([simulator.md](simulator.md)), and `sloop-render --world
NAME --ctl COLOR=0.2 --sweep ENERGY` renders one with the macros set or turning.

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

- **The chord root.** It is placed in the track's register: from a fourth below to a tritone above the track's tonic (a chord root up to 6 semitones above the key's root sits above the tonic, one 7–11 above it below). In A minor with a bass register A1, i is A1, VI is F1, III is C2 and VII is G1. The bass and pads therefore move by small steps.
- **The tones.** `c3`, `c5` and `c7` are the chord's own tones, so `c3` is the minor third on a minor chord, and the suspended note (the 2nd or the 4th) on a sus chord. When the chord lacks a tone, the token gives the root an octave up: `c7` on a triad, `c3` on a power chord. On a 6th chord, `c7` is the 6th.
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
| `transition` | 1, 2 or 4 bars, `"bar"` (1) or `"phrase"` (the length of the progression playing when it is asked for); default 1: a change to this scene lands on a bar line that many bars from the section start, with a fill on the bar before it when the drums play |
| `patterns` | per track: a pattern name, or `null` (absent). A track not listed is absent too. |
| `patterns.<drums>` | a pattern name (the GROOVE), `null`, or `{BEAT: name}` for `MINIMAL GROOVE BUSY BREAK`. A BEAT without a pattern plays the GROOVE. |
| `fill` | a drum pattern, played on the last bar of each phrase when the ENERGY band allows fills |
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
  "AIRY":    {"params": {"pad.rev": 80}, "fx": {"rsize": 110}, "energy_bias": -0.1, "macros": {"SPACE": 0.7}},
  "PULSING": {"swap": {"bass_main": "bass_drive"}, "energy_bias": 0.1},
  "HEAVY":   {"sounds": {"pad": "DARK STR", "drums": "909"}, "params": {"bass.DRV": 40}}
}
```

Variations are ordered. The first must be `"ORIGINAL": {}`, and there may be up to 8. Names follow the scene rules.

| Key | What |
| --- | --- |
| `sounds` | per track: another preset of the same engine, or another drum kit |
| `params`, `fx` | as in scenes. A variation cannot set `swing`. |
| `swap` | `{from: to}`: wherever a scene plays synth pattern `from`, play `to` (a pattern of the same track and length class: the same length, or whole bars of which one divides the other). Drum grooves are not swapped (use the BEATs). |
| `energy_bias` | −0.25..0.25, added to the ENERGY knob for the band (not for its parameter mappings) |
| `macros` | `{COLOR, MOTION, SPACE, ENERGY: 0..1}`: where the variation puts the four macros. When it lands, each macro the player has not turned since the World loaded goes there (a macro it does not name: to the World's `defaults.macros`), and the sound glides there; a macro the player turned stays. |

A variation keeps the World's identity (UI spec §6): it cannot change the tempo, key, scale, progressions, swing, the
drum grooves, the scenes' patterns (beyond `swap`) or the Smart Keys. `worldc` refuses `tempo`, `key`, `scale`,
`progression`, `swing`, `beat`, `patterns` and `smart_keys` in a variation with that reason, and warns when one changes
more than two tracks of the groove at once (swapped patterns and the drum kit).

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

The four knobs have fixed meanings: **COLOR** dark to bright, **MOTION** still to alive, **SPACE** close to huge,
**ENERGY** sparse to intense. What each one moves is the World's: its mappings, curves and rules. The player never
sees a parameter; the simulator's inspector (Option+I) shows them.

A **mapping** moves one target by an offset from the authored value.

**The knob position.** A macro sits at 50 % (home):
- turned down, the offset runs to `min`;
- turned up, it runs to `max`;
- at home the sound is exactly what the World says.

So 100 % is the largest value you chose, never "every parameter at its maximum". Controls that rest at 0 (SOFT,
SHORT, BODY, TAIL, DRIFT, WOBBLE, PULSE, ECHO, CRUSH, FREEZE) use only `max`. RATE and FILTER rest at 50 %. The
`defaults` set every position when the World loads (`macros`, `shape`, `movement`).

**Targets**, and what each does when the World plays:

| Form | Meaning | At runtime |
| --- | --- | --- |
| `pad.CUT`, `bass.rev` | a track's parameter | the parameter itself, in its own steps: the engines, effects and the sequencer (gate, glide) hear base + offset |
| `*.level` | the parameter on every synth track (common parameters only) | one offset per track |
| `pad+bass.chor` | on these tracks | as above |
| `keys.@RESO` | the engine role: `@BRIGHT @RESO @DRIVE @SHAPE @DETUNE @AIR @MOVE @BODY` | the parameter that plays the role on the engine the track runs (ANALOG `@BRIGHT` is CUT, DIGITAL's is IDX, ...: `worldc.py names ENGINE`); none on that engine: nothing (worldc warns). Another engine on the track later (Advanced): the role follows it |
| `pad.~bright`, `pad.~shape` | the smooth brightness / timbre offset of the engine | an offset for every voice of the track on what the engine uses as brightness and shape, in 1/256 steps: no zipper, the authored parameter untouched; at most ±64 steps. `~bright` is the filter cutoff on ANALOG, TRIO, SAMPLE and GRAIN, LOFI's TONE (upward only), DIGITAL's FM index, PHASE's DCW (not monotonic: [§18](#18-gotchas)), VOICE's vowel; WHEEL has neither (use `@BRIGHT`, its TOP) |
| `g.dfdbk` | a World global (not `dtime` or `swing`) | the global |
| `drums.level` | the same as `g.drlvl` | the global |

Several mappings (and rules) on one target **add up** into one offset. A World has room for **48 targets** (a
`*` mapping counts once per track); past 48 the rest are dropped, and `worldc check` refuses the World.

**Mapping keys:**
- `min` and `max`: −128..127 in the target's units.
- `curve`, the shape of the way from home to the end (x is how far the knob is from home, 0..1):
  - `lin` (the default) x; `exp` x², a slow start; `log` √x, a fast start; `s` 3x² − 2x³ (smoothstep); `late`
    nothing until halfway, then linear;
  - a name from `curves`;
  - an inline 9-value curve.
- `smooth`: how fast the parameter follows the knob: `fast` (10 ms), `medium` (60 ms, the default), `slow`
  (250 ms), `stepped` (at once). Enum targets (WHEEL's ROTR) are always stepped. A target several mappings move
  takes the slowest of their classes; a target only a rule moves, medium. Every parameter glides to its new value
  (one pole at 1378 steps a second), so a knob turned fast never clicks.
- `saturate`: `true` in a **user** World when the mapping is meant to reach its parameter's maximum at 100 %
  (`worldc check` refuses one that does without it: "100 % is never all-max"). A factory World may not say it: the
  blob carries no such flag, and the factory checks on the real firmware (`world_render --macros`, `macro_test`'s
  grid) hold every factory mapping short of the maximum.

**Limits** that no mapping passes, whatever it says (`firmware/src/guard_limits.h`):
- every value stays inside its parameter's range;
- delay feedback (`g.dfdbk`) at most 120 and reverb size (`g.rsize`) 127: the echo and the reverb always die away;
- a track's `level` at most 120 (+4 dB) and a filter's resonance (each engine's `@RESO`) at most 110;
- `~bright` / `~shape` at most ±64 steps.

A limit never changes what you authored: a base value above one stays, and the macros only cannot push it further.
Inside these, the World's `guard.sound` caps, ranges and combinations apply ([§14](#14-guard),
[guardrails.md](guardrails.md)).

**`worldc check`** evaluates the macros as the firmware does (a Python model of `macro.c` and `guard.c`, checked
against the C engine by `tests/run_tests.sh`) over every scene × variation and the 3⁴ grid of the four macros, and
refuses a World where:
- a mapping alone takes its parameter outside its range, or past a hard limit above (the firmware would clamp it:
  make the mapping smaller);
- a mapping reaches its parameter's maximum at 100 % without `"saturate": true` (a factory World: at all);
- more than 48 targets move at once;
- the Smart Keys range is under an octave, or (a factory World) folds any of the 27 keys at OCT 0 ([§13](#13-smart-keys));
- (a factory World) a LOFI track plays the chip's own arpeggio (`ARP` MAJ or MIN, [§18](#18-gotchas)).

A user World gets the last two as warnings.

Mappings and rules that only together run past the top of a range or a limit are a warning: the firmware holds them there.

**At most 40 mappings** in all, counting `controls`.

**Controls** (`SOFT SHORT BODY TAIL DRIFT WOBBLE PULSE RATE FILTER ECHO CRUSH FREEZE`) replace that control's built-in
mappings with yours: name a control and all of its built-in mappings stand aside; leave it out and the firmware's
apply. They are evaluated as the macros are. The player turns them on the PLAY pages: ENV held (SOUND SHAPE), LFO held
(MOVEMENT), FX held (LIVE FX). The built-in mappings (`world_fmt.h` `WF_CTL_BUILTIN`, Phase 13):
- SOUND SHAPE and MOVEMENT move the Smart Keys track's envelope (`atk dec sus rel`, `rev`) and LFO (`ld_pit ld_flt
  ld_amp lrate`, `@BODY`, `@DETUNE`). Their positions are saved with the session.
- LIVE FX moves the master: FILTER the DJ filter (`g.filt`), ECHO the delay bus (`g.dmix`, `*.dly`, `g.dfdbk`), CRUSH
  DUST (`g.dust`, with level cuts). They are momentary: FX let go, every LIVE FX control goes home, yours too.
- FREEZE has no mapping: from 25 % it plays the punch engine's loop of the beat while FX is held. Mappings you give
  FREEZE move parameters as well; the loop stays.
- Whatever moves them, the Smart Keys track's envelope and LFO stay inside fixed windows, and LIVE ECHO's feedback stays
  at most 96 (72 in a big room): [guardrails.md §2.7](guardrails.md#27-live-fx-sound-shape-and-movement-phase-13).

The `defaults` set SOUND SHAPE (`shape`) and MOVEMENT (`movement`); LIVE FX always starts at home.

**Curves.** Each curve is one of:
- 9 values 0..1 (at x = 0, 1/8, …, 1);
- `{"points": [[x, y], …]}`, from x = 0 to x = 1, interpolated;
- `{"lut": [9 integers 0..255]}`.

A curve must start at 0 and end at 1, and is linear between its 9 points. There are at most 8.

**Gain compensation is yours.** A macro that adds (more reverb, drive, a brighter filter, ENERGY's layers) also makes
the World louder. Give ENERGY and SPACE level mappings that take some of it back (`*.level` down as ENERGY rises,
`drums.level`), and cut levels in the rules. ENERGY should make the World denser, not louder: `tests/run_tests.sh`
checks every factory World at ENERGY 0, 0.5 and 1 (at most 4 LU over 0.5, 6 LU over 0), and that no corner of the
macros is more than 12 LU under the defaults.

---

## 11. Rules

```json
"rules": [
  {"if": {"SPACE": 0.8, "ENERGY": 0.8}, "then": [{"to": "bass.rev", "add": -30}, {"to": "g.dfdbk", "add": -25}]},
  {"if": {"MOTION": 0.9}, "then": [{"to": "g.cdepth", "add": -30}]}
]
```

A rule is a cross-macro constraint: when every named control is above its threshold, each action adds up to `add` to
its target. How much is the rule's **strength**: 0 at the thresholds, 1 when the controls are at 100 %, and with two
controls the lesser of the two (one at 0.9 of the way and one at 0.5: 0.5). So a rule fades in as the knobs pass
their thresholds, and never jumps.

Use them for combinations that would sound wrong, as the factory Worlds do:
- SPACE and ENERGY high: less reverb on the bass and drums, lower delay feedback, a little less level (the mix stays
  clear instead of turning to mud);
- COLOR and ENERGY high: less resonance;
- MOTION and ENERGY high: one modulation layer less (chorus depth, LFO depth).

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

ENERGY is mostly arrangement. There are up to 4 tables; each scene names one. A table has 1–4 bands. The ENERGY knob
plus the variation's `energy_bias` picks the band (`firmware/src/arrange.c`); its parameter mappings (gain
compensation, drive, brightness) play on top.

| Key | What |
| --- | --- |
| `from` | where the band starts: 0 for the first, then ascending |
| `layers` | the tracks heard, or `"all"`. The others are silent (with the same short fade as a mute). The Smart Keys track must be in every band, and is never silenced. |
| `drums` | the drum lanes allowed, or `"all"` (the default) |
| `density` | 16 steps of `x`/`.` applied to `density_lanes`: a hit there sounds only on an `x` |
| `<synth track>` | 16 steps of `x`/`.`: that track's notes play only on an `x` (a note on a `.` rests); never the Smart Keys track's |
| `fills` | the scene's `fill` plays on the last bar of every phrase: `guard.arrangement.fills_every` bars, else the length of the scene's progression |
| `ratchets` | the drums' ratchets play (without: each hit once) |

**When a band changes**, following `guard.arrangement`:
- the band moves only 0.02 past its edge (hysteresis), so a knob resting on an edge does not flip between two bands;
- the layers change on the next bar (`mute_change: "2bars"`: every second bar);
- the lanes, density and play masks on the next beat (`density_change: "bar"`: the bar);
- a change begins at least `min_band_bars` bars after the one before;
- stopped, at once; a scene change brings its own table, on its bar.

BEAT (MINIMAL, GROOVE, BUSY, BREAK) applies on top of these masks (Phase 11): a BEAT the scene has no pattern for
plays its GROOVE through a mask, MINIMAL kick, kick 2, snare, clap and rim (and never a fill), BUSY every density step
with ratchets, BREAK kick, kick 2, snare and snare 2 with the hats on the second eighth of each beat. A scene change
plays a fill on the bar before its line (the new scene's `fill`, else the current one's) when the drums are playing.

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

What the keys play (SMART MELODY, Phase 6, design §4; `firmware/src/smartkeys.c`):
- **White keys** walk `melody_scale` up from the C4 key, which plays `tonic`, and down from it: 16 white keys, about
  three octaves of melody. They never move.
- **Black keys** play the tones of the chord sounding now, ascending: each takes the lowest chord tone above its left
  white key and the black key before it. With `chord+9`, the chord's 9th joins when it is a safe tone of the scale.
  The black keys move with the progression. A key pressed up to a 32nd note before a chord change already gets the
  new chord.
- **Every note** is shifted by OCT−/OCT+ and then folded by octaves into `range`. A range under an octave is widened
  upward to one.
- **The range holds all 27 keys at OCT 0.** The white keys span from a 6th or 7th below the tonic to two octaves and a
  2nd or 3rd above it; the black keys climb one chord tone a key, so over a triad they reach about two octaves and a
  7th above the tonic (tonic D4, MPEN: F3..C7). `worldc` works the span out over every chord the scenes play: a factory
  World whose range folds any key back fails (the keyboard would stop climbing in order, and OCT would not move the
  end keys by an octave), a user World is warned. Put the tonic where the instrument sits (the default is the root
  nearest C4), then set `range` to the span: with a tonic above D4 the black keys climb past C7.
- **A held note never moves.** It sounds until its key is released, whatever the chord, scene or octave is by then.
- `guard.notes.<keys track>.max_poly` caps the keys held at once. A key over the cap stays silent.

MIDI in on the keys track plays the same map, with MIDI note 60 as the C4 key.

---

## 14. Guard

```json
"guard": {
  "notes": {"keys": {"max_poly": 4, "loop_follow": "snap", "avoid": "classic"}, "bass": {"range": ["E1", "A3"]}},
  "record": {"quantize": 0.75, "max_notes": 3},
  "sound": {"ranges": {"keys.RES": [0, 100], "g.dfdbk": [0, 90]}, "max_level": 116, "max_dfdbk": 90,
            "combos": [{"when": {"g.rsize": 110, "g.dfdbk": 70}, "cap": {"g.dfdbk": 70}}]},
  "arrangement": {"mute_change": "bar", "density_change": "beat", "fills_every": 4, "min_band_bars": 2},
  "cpu": {"max_unison": 4, "grain_dens": 90, "max_dist_tracks": 2, "ceiling": 0.85}
}
```

Every field is optional. An unset field takes the firmware default (design §6.1). `guard.notes` is keyed by synth
track. `max_poly`, `loop_follow` and `avoid` apply to the Smart Keys track only. A note `range` folds only what the
player plays (the Smart Keys and MIDI in); the patterns sound as written, and `world_render` holds them to their
role's register: bass E1–G3, pad, chords and keys E2–E6, lead and texture C3–C7.

`sound.ranges` keys are targets as in [§10](#10-macros-controls-curves): the macros keep each one's effective value
inside `[lo, hi]` (a base the World put outside stays). `sound.combos` are sound combinations: while every `when`
target is above its value, the `cap` target stays at or under its value; the cap comes in over the 16 steps past
the thresholds (at once for a condition on the capped target itself), and a per-track target is judged per track.
Targets are parameters, `@roles` or globals. Absent, the firmware's six apply (a big room with a long echo, drive
into resonance, a distorted part through DUST, reverb and echo sends in a huge room or a long echo: see
[guardrails.md](guardrails.md)); `[]` turns them off; up to 8 of your own replace them. The other fields and their
ranges:

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

The arrangement fields (`mute_change`, `density_change`, `fills_every`, `min_band_bars`) time the ENERGY bands
([§12](#12-energy)). What each of the others does, and what holds whatever a World says:
[guardrails.md](guardrails.md).

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
| Factory set | all of `worlds/factory` within the app-slot room the code leaves, less 8 KB: about 1,400 B a World for a library of 30 (`validate-world`'s flash budget, [validation.md §3](validation.md#3-the-budgets)) |
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
because a step that repeats the previous note costs 2 B. A soft, ghost or hard step (`_`, or a ratchet) costs 2 B more
than a plain one; an accent (`!`) and a slide (`~`) are free (flag bits).

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
- **Sounds differ widely in loudness** at the same `level`. VOICE's CHOIR AAH and SOUL OOH are very loud, and more so
  in POLY when the player mashes chords (give them a low level); PHASE's RESO PLUCK and CZ BASS are quiet; GRAIN's FLUTE
  DUST is about 6 LU louder than LOFI CLOUD. The sampled kits DUST and DEEP are 6–7 dB louder than the synthesised
  kits, and JAZZ and VINTAGE very soft: set `drums.level` per kit (a variation that swaps the kit, too).
- **The limiter starts at about −5.2 dBFS** (`fx.c` `LIM_T`). Peaky material drives it: short stabs, several drum hits
  on one step, a stab on every kick. `world_render` fails a render where it takes more than 6 dB over 5 % of the
  time; lower the stab or move it off the kick rather than lowering everything.
- **Loudness is gated** (BS.1770, as `world_render` measures it): silence does not count. Adding a quiet layer to a
  sparse band can lower the measured LUFS, because blocks that were gated out now count; and `--bands` fails a band
  that plays fewer notes or hits than the one below it, so a fill with fewer hits than the groove it replaces fails
  too.
- **Fill toms hit like kicks.** The synthesised kits' toms peak as high as the kick: write fills with soft toms.
- **`c9` follows the scale.** On a chord whose major ninth is outside the World's scale (iii and vii in a major key,
  ii and v in a natural minor), `c9` is the minor ninth: a rootless voicing with `c9` turns harsh there. Keep such
  chords out of progressions that play 9th voicings.
- **Releases hold voices.** A released voice sounds until its envelope is 72 dB down, about 1.8 × its REL time. At
  each chord change the old chord's voices overlap the new one's, and the three synth tracks share 8 voices: keep a
  World's own patterns to about 6 voices so the player's keys never take a held note (`world_render` counts it).
- **Darkening adds up.** Every mapping on `~bright` (COLOR, and ENERGY when it darkens) adds into one offset. COLOR 0
  with a sparse ENERGY band can leave a single pad behind a nearly closed filter, 20 dB down: the engines' filters also
  turn gritty near the bottom of their range. Let COLOR's `min` stop where the sound is dark but present (the factory
  Worlds keep −22..−24 on their pads), and let ENERGY brighten (`min` 0) rather than darken. `world_render --extremes`
  measures every corner.
- **ENERGY moves two things.** Its band (the arrangement) and its parameter mappings. A band edge passes as the knob
  does; levels glide. Measure ENERGY 0, 0.5 and 1 (`--extremes`): denser, not louder.
- **Keep ENERGY band edges away from the defaults.** The default position plus each variation's `energy_bias` should
  sit clearly inside a band (the factory Worlds keep 0.05 or more), or a variation drops a layer by accident.
- **Long echoes need room.** A `1/4` delay at 66–72 BPM repeats every ~0.85 s; with SPACE at 100 % the feedback
  must still let the tail fall below −60 dBFS within 6 s of STOP (design 12.3). The factory Worlds keep the base
  `dfdbk` at 60 or below there and SPACE adds at most +8. Releases count too: a long `rel` that SPACE lengthens
  further (`*.rel` with `rev` and `rsize`) can fail the same tail check (`no invalid feedback`).
- **Swing is per pattern division.** It delays every second step of each pattern's own grid: a `1/4` pattern swings
  beats 2 and 4, a `1/8` pattern its off-beat eighths, a `1/16` pattern (the drums) its second and fourth
  sixteenths, so its eighths stay straight. A `1/8` part therefore swings against the drums' straight eighths: write
  parts that must lock at the same division. Triplet divisions (`8T`, `16T`) swing every second triplet: give a World
  with triplet patterns swing 0 (a track's `sswing` adds to the World's).
- **Phrase transitions can be long.** `"transition": "phrase"` waits for the playing progression's length: 32 beats
  at 60 BPM is 32 s before the scene changes.
- **A swap is one pattern for one.** A pattern without chord tokens that plays over several progressions is one
  pool entry: it cannot swap to a chord-token pattern, which differs per progression (worldc refuses it). Give that
  scene its own pattern.
- **Preset transposition is for the keys.** `TRANS` (−24 on SUB BASS, −12 on GB BASS) moves what the keys play;
  pattern notes sound as written. SAMPLE's `TUNE` is not a transposition: it retunes the sample, so it moves the
  pattern notes too.
- **PHASE's DCW is not a filter.** `@BRIGHT` and `~bright` on PHASE move DCW, the bend of the wave, and what that does
  depends on the wave: SOFT KEYS darkens as DCW rises, CZ STRING is darkest near its preset's 50 and brighter both
  ways, the resonant waves of CZ BASS and RESO PLUCK move a resonance the ear hardly hears as brightness, CZ BRASS
  brightens. The amplitude envelope bends it too (`ENV`). Map COLOR the way that brightens (MEMORY ARCHIVE and TAPE
  MEMORY turn SOFT KEYS' DCW down as COLOR rises, and the "darker" variation turns it up), or leave the PHASE part out
  of COLOR; `tests/macro_test.c` checks that the first part COLOR moves through `~bright` gets brighter at 100 %.
- **WHEEL has no `~bright` or `~shape`**: map `@BRIGHT` (TOP, −8..8) instead. GOSPEL's rotor is FAST: set `"ROTR":
  "SLOW"` (or `OFF`) for a held organ.
- **VOICE: the filter moves the vowel.** `ld_flt`, `ed_flt` and `~bright` move the vowel; `ld_shp` and `~shape` move
  BUZZ (its `@BRIGHT`), which changes the loudness as well as the colour.
- **GRAIN's TONE is a gentle low-pass**: a wide `@BRIGHT` range moves it only a little. LOFI's `~bright` only opens its
  TONE (a negative offset does nothing): darken LOFI with `@BRIGHT`.
- **LOFI's chip arpeggio is out of key.** `ARP` MAJ (the 8BIT ARP preset's own) or MIN plays every note as a fast
  triad, a major one over the minor degrees too, and no in-key check hears it (the sequencer and the keys play one
  note). worldc refuses it in a factory World and warns in a user World: set `"ARP": "OFF"` or `"OCT"`, and write
  arpeggios as patterns (or leave them to PULSE).
- **SAMPLE's VIBES opens `CUT` at 127**, so COLOR's `@BRIGHT` has nowhere to go (100 % is never all-max): lower it in
  the track's params (TAPE MEMORY: 108).
- **A player mashing the keys** (`world_render --mash`, every scene × variation in `tests/run_tests.sh`): one or two
  keys most eighths, held up to half a bar, every note in key and in range and nothing left after STOP. With a
  polyphonic Smart Keys sound, `max_poly` decides how many stack up over the World's own voices: keep it at 2–3 for
  long releases, or play MONO / LEGATO.
- **The Smart Keys reach past the top white key.** The black keys climb to chord tones above the highest white key
  (design 4.1): with the tonic D4 and MPEN the 27 keys reach C7. `range` must hold all of them, and worldc refuses a
  factory World whose range folds one ([§13](#13-smart-keys)). A tonic above about D4 takes them past C7: of the
  Phase 18 Worlds, four moved their tonic down an octave to keep the top of their range, and ARCADE '89 kept G4
  (its chip lead an octave down swung the pulse wave at full scale) with the range up to G7.

---

## 19. Factory Worlds

Thirty Worlds in five categories (Phase 18): the four demo Worlds of the UI spec (§11) and 26 more. CHOOSE WORLD
lists them by category, then name; a first boot opens NEON RAIN (by its id). Each starts on scene B with ORIGINAL,
the macros at home (ENERGY at 0.55 in NEON RAIN and every GROOVE and SYNTHWAVE World), PULSE off and BEAT on GROOVE.
Every one validates clean (`tools/validate-world`, 17 of 17), and `tests/run_tests.sh` plays every scene × variation
of each World it chooses ([§1](#1-quick-start); `WORLDS=all` for all 30). At their defaults they measure −17.3 to
−16.3 LUFS, within 1.1 LU of each other.

| World | Category | Key | BPM | Smart Keys: sound; melody scale, tonic, range | Blob |
| --- | --- | --- | --- | --- | --- |
| DEEP SPACE `deep_space` | AMBIENT | C# phrygian | 52 | VOICE CHOIR AAH; MPEN from C#3, E2–C#6 | 1,034 B |
| FLOATING GLASS `floating_glass` | AMBIENT | A mixolydian | 72 | ANALOG TRAP PLUCK; PEN from A3, B2–A6 | 1,269 B |
| FROZEN LAKE `frozen_lake` | AMBIENT | E major | 66 | DIGITAL MUSIC BOX; PEN from E4, F#3–C7 | 1,315 B |
| LOST SIGNAL `lost_signal` | AMBIENT | B dorian | 84 | DIGITAL WURLI; MPEN from B3, D3–A6 | 1,149 B |
| MORNING HAZE `morning_haze` | AMBIENT | G lydian | 76, swing 14 | DIGITAL MARIMBA; PEN from G3, A2–D6 | 1,375 B |
| SLOW ORBIT `slow_orbit` | AMBIENT | F minor | 60 | SAMPLE STRING STB; MPEN from F3, Ab2–Eb6 | 1,115 B |
| DISTANT TOWERS `distant_towers` | CINEMATIC | C phrygian | 58 | PHASE CZ BRASS; MPEN from C4, Eb3–C7 | 1,224 B |
| MEMORY ARCHIVE `memory_archive` | CINEMATIC | Eb lydian | 80 | SAMPLE LOFI FLUTE; PEN from Eb4, F3–C7 | 1,352 B |
| NEON RAIN `neon_rain` | CINEMATIC | D minor | 72 | ANALOG G-FUNK LD; MPEN from D4, F3–C7 | 1,365 B |
| NIGHT SIGNAL `night_signal` | CINEMATIC | E dorian | 120 | ANALOG SUB BASS; MPEN from E3, G2–E6 | 1,370 B |
| OFF-WORLD `off_world` | CINEMATIC | B harmonic minor | 86 | VOICE TALKBOX; own from B3, C#3–G6 | 1,296 B |
| SYNTHETIC DAWN `synthetic_dawn` | CINEMATIC | A mixolydian | 94 | TRIO SYNC LEAD; PEN from A3, B2–A6 | 1,343 B |
| CIRCUIT FUNK `circuit_funk` | GROOVE | E dorian | 108, swing 6 | VOICE TALKBOX; MPEN from E3, G2–E6 | 1,370 B |
| LATE SHIFT `late_shift` | GROOVE | Bb major | 116, swing 8 | TRIO SYNC LEAD; PEN from Bb3, C3–Bb6 | 1,455 B |
| MAGNETIC `magnetic` | GROOVE | G minor | 126, swing 32 | DIGITAL TRAP BELL; MPEN from G3, Bb2–G6 | 1,401 B |
| METRO BEAT `metro_beat` | GROOVE | A phrygian | 128 | PHASE RESO PLUCK; MPEN from A3, C3–A6 | 1,183 B |
| NIGHT PULSE `night_pulse` | GROOVE | F minor | 122, swing 14 | VOICE SOUL OOH; MPEN from F3, Ab2–F6 | 1,170 B |
| SOFT MACHINE `soft_machine` | GROOVE | D lydian | 96, swing 22 | DIGITAL MARIMBA; PEN from D4, E3–C7 | 1,304 B |
| DUSTY CAFE `dusty_cafe` | LO-FI | F major | 82, swing 34 | SAMPLE LOFI KEYS; PEN from F4, G3–A6 | 1,443 B |
| LATE TRAIN `late_train` | LO-FI | E dorian | 90, swing 12 | DIGITAL WURLI; own from E4, F#3–B6 | 1,429 B |
| RAINY STUDY `rainy_study` | LO-FI | C minor | 74, swing 42 | SAMPLE LOFI FLUTE; MPEN from C4, Eb3–G6 | 1,407 B |
| SOFT STATIC `soft_static` | LO-FI | Eb lydian | 78, swing 48 | DIGITAL DX RHODES; PEN from Eb4, F3–Bb6 | 1,200 B |
| SUNDAY EVENING `sunday_evening` | LO-FI | Bb major | 68, swing 60 | DIGITAL RHODES; PEN from Bb3, C3–G6 | 1,272 B |
| TAPE MEMORY `tape_memory` | LO-FI | D major | 86, swing 24 | SAMPLE VIBES; PEN from D4, E3–E7 | 1,433 B |
| ARCADE '89 `arcade_89` | SYNTHWAVE | G dorian | 128 | LOFI GAME LEAD; MPEN from G4, Bb3–G7 | 1,205 B |
| CASSETTE DREAM `cassette_dream` | SYNTHWAVE | Bb major | 82, swing 28 | ANALOG G-FUNK LD; PEN from Bb3, C3–F6 | 1,072 B |
| MIDNIGHT DRIVE `midnight_drive` | SYNTHWAVE | A minor | 100 | LOFI GAME LEAD; MPEN from A3, C3–A6 | 1,801 B |
| NEON HIGHWAY `neon_highway` | SYNTHWAVE | F# minor | 116 | TRIO SYNC LEAD; MPEN from F#3, A2–F#6 | 1,373 B |
| SPACE STATION `space_station` | SYNTHWAVE | C lydian | 106 | VOICE TALKBOX; PEN from C4, D3–A6 | 1,313 B |
| VHS SUNSET `vhs_sunset` | SYNTHWAVE | D major | 92, swing 10 | PHASE CZ BRASS; PEN from D4, E3–A6 | 1,238 B |

The 30 blobs take 39,276 B (1,034–1,801 B, 1,309 B on average); in the image, with their 4-byte alignment and index
entries, 39,564 B of the 42,296 B the factory set may take ([validation.md §3](validation.md#3-the-budgets)). Each
file's `notes` say what it plays, scene by scene.

### The four demo Worlds

| World | Tracks: 1 · 2 · 3 (Smart Keys) · drums | Scenes A · B · C · D | Variations |
| --- | --- | --- | --- |
| NEON RAIN | pad: ANALOG WARM PAD · bass: ANALOG SUB BASS · lead: ANALOG G-FUNK LD · 808 | INTRO · MAIN · LIFT · BREAKDOWN | ORIGINAL DREAMY DARK PULSING HEAVY |
| MIDNIGHT DRIVE | chords: ANALOG SYN BRASS · bass: TRIO FAT BASS · lead: LOFI GAME LEAD · SYNTHWV | IGNITION · CRUISE · OVERDRIVE · TUNNEL | ORIGINAL DRIVING DREAMY DARK HEAVY |
| FROZEN LAKE | pad: ANALOG ATMOS PAD · texture: GRAIN VIBE HAZE · bell: DIGITAL MUSIC BOX · AMBIENT | FIRST ICE · STILLNESS · AURORA · DEEP ICE | ORIGINAL AIRY FLOATING DARK SPARSE |
| DUSTY CAFE | keys: DIGITAL RHODES · bass: SAMPLE UP BASS · piano: SAMPLE LOFI KEYS · LO-FI | STEAM · WARM CUP · SUNLIGHT · LAST CALL | ORIGINAL DREAMY AIRY DARK SPARSE |

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

**Macros** (what each knob moves; every World also has the three cross-macro rules of [§11](#11-rules), at 0.75):

| World | COLOR | MOTION | SPACE | ENERGY (besides its bands) |
| --- | --- | --- | --- | --- |
| NEON RAIN | pad and lead brightness, delay colour | pad LFO shape, filter and rate, lead vibrato, chorus depth | pad and lead reverb, reverb size, delay feedback, lead echo, drum reverb, pad release | pad level down, drums up, bass drive, pad brighter |
| MIDNIGHT DRIVE | chords and bass brightness, lead tone, delay colour | chords and bass gate, chords LFO filter, lead vibrato, chorus depth | chords and lead reverb, reverb size, delay feedback, lead and chords echo, drum reverb | chords level down, drums up, bass drive, chords brighter |
| FROZEN LAKE | pad, texture and bell brightness, delay colour | pad LFO shape and pitch, grain spread and detune, chorus depth | every reverb, reverb size, delay feedback, bell echo, pad release, drum reverb | drums and texture up, pad down, pad brighter |
| DUSTY CAFE | Rhodes and piano tone, delay colour, less dust | Rhodes tremolo and its rate, chorus rate and depth | Rhodes and piano reverb and echo, reverb size, delay feedback, drum reverb | Rhodes level down, drums and bass up, ducking |

At their extremes (`world_render --extremes`, scene B, 4 bars): every one clean (peaks −3.1 dBFS at most, nothing
at full scale, 6 s after STOP the tails at −65.7 dBFS or lower); ENERGY 0 / 0.5 / 1 measure −15.8 / −16.3 / −17.0 LUFS
(NEON RAIN), −21.9 / −17.2 / −17.1 (MIDNIGHT DRIVE), −15.8 / −16.4 / −16.3 (FROZEN LAKE) and −18.0 / −16.8 / −17.0
(DUSTY CAFE), while the notes and hits a bar grow 2.7 to 8.6 times; the quietest corner (every macro at 0) is 2–7 LU
under the defaults.

**Gain staging.** The levels follow one plan, measured with `world_render` (BS.1770 loudness):
- the default scene at about −16.5 LUFS (the four within 1 LU of each other; the 30 within 1.1 LU), RMS −17.5 to
  −18.5 dBFS;
- A and D (without drums at the default ENERGY) 0.5–3.5 LU under B, C level with B or up to 1.5 LU above;
- each variation within about 2 LU of ORIGINAL in the same scene (a preset swap gets a `level` to match), unless
  its ENERGY bias changes the band: MIDNIGHT DRIVE's DRIVING and HEAVY bring the bass into A (+3 LU);
- the Smart Keys instrument alone, playing a beginner's phrase, 1–2 LU under the backing (the music box, which
  decays at once, 4.5 LU under, with the same peaks);
- the output limiter rarely over 1 dB (the synthwave groove, the densest, 13–18 % of the time and never over
  6 dB), every peak under −3 dBFS.

## 20. User Worlds (MY WORLDS)

Factory Worlds are read-only. What a player changes in Advanced Mode is kept in the **working World**, and SAVE turns it
into a **user World** (Phase 14; design §10.2–10.3, as built §10.5).

**What the device edits, and what the Mac tool edits.** Advanced Mode is SLOOP's UI over the World: the device edits
the *sound, the patterns and the mix* (engines, presets, every track parameter, the sends and the whitelisted globals,
steps, drum hits, pattern lengths, the keys loop). The World's *structure* (the macros and their curves, rules,
ENERGY, GUARD, scenes, variations, progressions, names) is edited in JSON with the authoring tool (Phase 15), not on
the device. A user World keeps the structure of the World it came from.

**How edits combine with scenes and variations.** Leaving Advanced Mode (or pressing SAVE + a scene key in it) turns
the edits into *overrides*. Each one is an absolute value for one parameter, a global, or a synth track's sound
(engine and preset). The stage builds every scene and variation in this order, later steps winning:

1. the engine's defaults and the preset: the player's sound, else the variation's preset swap, else TRACKS;
2. the World's pairs (TRACKS), the variation's, the scene's. If the player chose **another engine**, these pairs
   skip that track's engine parameters, because they belong to the World's engine. The World's macros on those
   parameters also skip the track; macros on roles (`@CUTOFF`, `@BODY`…) follow the new engine;
3. **the overrides**: in every scene and every variation;
4. the pattern, with the length the player left it at; the keys loop; the player's mutes; then every value is
   clamped to its range.

So an edit sticks everywhere. A value the player sets back to what the World gives in the scene playing drops its
override. A value he did not touch keeps its override, even when it was made in another scene. The steps are written
back into the pattern pool when a track leaves a pattern, as before. A `(pattern, length)` change is kept per pool
entry. A drum pattern keeps 16, 32 or 64 steps.

What is not captured: the World's key and scale, the pattern assignment, the mutes, the keys track's arp (PULSE owns
it), the drum track's parameters outside `WF_P_DRUM` except its kit, and the tempo. The tempo is the session's; a save
writes the tempo playing into META. At most 64 parameter overrides plus one sound per synth track: more shows
`TOO MANY EDITS`, and the rest play until the next scene change.

**SAVE** (PLAY MODE, the SAVE button):

| Row | Does |
| --- | --- |
| SAVE AS USER WORLD | A new slot: the World's name with the next free number (`NEON RAIN 2`, `NEON RAIN 3`; a user World's own number is replaced: from `NEON RAIN 2` comes `NEON RAIN 3`), shortened to fit 14 characters (`MIDNIGHT DRI 2`). All 10 slots used: `MY WORLDS FULL`. Renaming is for the Mac (`tools/worldc.py rename`). |
| SAVE | Saves over the user World loaded, keeping its slot and name. A factory World behaves as SAVE AS. |
| RESET WORLD | Asks first (SAVE again). A factory World loads again without the edits, pool edits or loop. A user World loads as last saved. While playing, it lands on the next bar. |
| DELETE USER WORLD | Asks first. Erases the user World loaded (both copies). It plays on, unsaved. A factory World: `NOT A USER WORLD`. |

Saving needs the transport stopped (`STOP TO SAVE`): erasing a sector stalls the audio for about 50 ms. A World over
the slot's 3,584 B shows `WORLD TOO BIG` and nothing is written.

**CHOOSE WORLD** lists the factory Worlds, then a `MY WORLDS` row (the knob steps over it), then the user Worlds by
slot. A user World loads like a factory one: at once while stopped, on the next bar while playing. Its category reads
`MY WORLDS`. It loads at the controls, PULSE and BEAT it was saved with: its saved positions win over the variation's
macro defaults. After any edit or loop, LEAVE WORLD asks first: `LEAVE WORLD? NOT SAVED`, and OK again leaves.

**The blob.** A user World is a whole FWD1 blob with flag USER, so it plays even if a later firmware changes the
factory World it came from. Every section of the source is copied as it is, except:
- META: the name, category `MY WORLDS`, the tempo;
- PATTERNS: the pool re-encoded, plus the keys loop as one more synth pattern;
- KEYS: `loop_pat`;
- DEFAULTS: scene, variation, the 12 controls, PULSE, BEAT;
- OVERRIDES (type 15, the only extra section): `{scope, id, value}` records. Scope 0–3 is a track parameter (the drum
  track also its kit, `P_E0`), scope 4 a whitelisted global, and scope `128 | track` a synth track's sound (`id` the
  engine, `value` the preset).

The world id is FNV-1a over the name. The format: [fwd1-format.md §14](design/fwd1-format.md#14-reserved-types).
Engine and preset *indices* are stored, so the firmware's preset tables stay append-only.

