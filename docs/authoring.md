<!-- SPDX-License-Identifier: GPL-3.0-only -->
# The World authoring tool

`tools/world-author` is the Mac tool for writing Musical Worlds (Phase 15). It is a local web page with a small Python
server. The page edits a World as JSON, the server reads and writes the `.world.json` files and runs the compiler, the
model of the macros and `validate-world`, and the native simulator plays the World. The browser plays nothing.

![The authoring tool: NEON RAIN's COLOR mappings, the curve and the model's values at 85 %, the diagnostics drawer](images/author.png)

Where to look:
- The format, every key and the notation: [worlds.md](worlds.md). The tool covers all of it, and its JSON tab edits
  anything a form does not show.
- The checks it runs: [validation.md](validation.md).
- The simulator it plays the World in: [simulator.md](simulator.md).
- The design: [design/play-mode-architecture.md](design/play-mode-architecture.md) D16 and §12.2.

---

## 1. Running it

```sh
./tools/world-author                                  # the page opens; it starts on NEON RAIN (or your first World)
./tools/world-author worlds/user/my_world.world.json  # open this World
./tools/world-author ~/my-worlds                      # list that directory too (it is writable)
./tools/world-author --no-browser --port 8800         # print the URL only
```

| Option | |
| --- | --- |
| `--no-browser` | Do not open the page (`open` on macOS). The URL is printed. |
| `--port N` | The port. By default the system picks a free one. The server listens on 127.0.0.1 only. |
| `--allow-factory` | Let Save write `worlds/factory`. Without it the factory Worlds are read-only here. |
| `--worlds DIR` | The worlds directory: `DIR/factory` and `DIR/user`. Default `worlds/`. |
| `--sim PATH` | The simulator. Default `build/host-bin/flowstate-sim`. |

It needs Python 3 (stdlib only); Validate's renders also need the C compiler `validate-world` uses, and Preview the
simulator. The page needs no network: it loads no script, font or style from anywhere.
Ctrl-C stops the server, and the simulator it started.

**Files.** The Open dialog lists three places:
- `worlds/factory`: read-only. Save becomes Save As, which writes a copy under `worlds/user/`.
- `worlds/user`: your Worlds. It is created on the first save.
- the directory of the file or directory given on the command line.

Nothing outside these is read or written, and the server serves `web/author.html` and its API only.

**Saving.** Save writes the file atomically, then checks it. The layout is stable, so a diff shows only what changed:
- the keys in the order of [worlds.md](worlds.md);
- the entries you name (patterns, progressions, variations, tables) in your order. That order matters for variations;
- a value on one line when it fits in 110 columns, and a drum pattern's lanes one a line, aligned.

Save refuses to overwrite a file that changed on disk since it was loaded, unless you confirm. Cmd+S saves.

---

## 2. What the tool covers

The owner's list for Phase 15, and where each item is:

| # | Item | Where |
| --- | --- | --- |
| 1 | create a World | New: the template (four tracks, two progressions, scenes A–D, a pattern a part, macros, ENERGY), which validates clean |
| 2–6 | title, category, BPM, key, scale | World: name, category, blurb, notes, BPM with its nudge range, root, scale, swing |
| 7 | harmony | Harmony: progressions as roman-numeral chips with the chord each makes in the key, beats, a palette of chords |
| 8, 9, 11 | tracks, presets, drum kit | Tracks: name, role, register, engine, the presets of that engine, the kit, parameters with their ranges; global FX |
| 10 | patterns | Patterns: a lane grid for drums (16, 32, 64 steps; `x X s g 2 3 4`), a step editor for synth parts with the notes as compiled (chord tokens unrolled over a progression) |
| 12 | scenes A–D | Scenes: name, role, progression, ENERGY table, transition, fill, a pattern per track (per BEAT for the drums), params, FX |
| 13 | variations | Variations: ORIGINAL and up to 7 more, in order: sounds, params, FX, swaps, energy bias, macro defaults |
| 14–17 | COLOR, MOTION, SPACE, ENERGY | Macros: mapping rows (target, min, max, curve, smoothing, saturate), the curve and the model's values, rules, curves, ENERGY tables; the 12 other controls too |
| 18 | safe ranges | Guardrails: `sound.ranges` per target, the caps; a mapping's min and max |
| 19 | Smart Keys | Smart Keys: track, mode, melody scale, tonic, range, white and black keys; the 27 keys over any chord |
| 20 | Guardrails | Guardrails: every GUARD field with its firmware default, the sound combinations |
| 21 | preview live | Preview: the simulator plays the saved file and reloads it on every save |
| 22 | test all macros | Macros: "test all macros" (every macro at 0–100 %); Validate: quick or full renders the macro space |
| 23 | estimate CPU | Validate: the CPU budget, measured over the renders |
| 24 | estimate flash | Validate: the blob against its limit, the factory set; the problems panel shows the size as you type |
| 25 | save / export | Save, Save As, Export (blob, C array, JSON) |
| — | hidden mappings | the diagnostics drawer (Alt+D), for developers |

Every edit is checked as you type (`worldc check`, below a second). The problems panel on the right lists the errors
and warnings with their JSON paths; a click goes to the field, which is outlined, and the tabs count their errors.

---

## 3. A walkthrough: a new World

1. **New.** Give it a name (`RAIN CITY`). The id follows (`rain_city`). The World opens unsaved, and Save writes
   `worlds/user/rain_city.world.json`.
2. **World.** Category, blurb, BPM, key and scale. The hint lists the scale's tones and the degrees the numerals count.
3. **Harmony.** Each progression is a row of chips:
   - type numerals in `+ chord` (`i:4 VI:4 III:4 VII:4`, Enter), or click the palette;
   - each chip shows the chord it makes in the key (`Am`, `Fmaj7`);
   - ◀ ▶ move a chord, − + halve or double its beats, and a double-click edits it;
   - the total must be 4, 8, 16 or 32 beats. The card shows which scenes play the progression.
4. **Tracks.** Choose each engine and preset, the drum kit, and set parameters by their EDIT labels (`CUT`, `RES`)
   or common names (`rev`, `level`). The range of each value is shown. "Play the Smart Keys" chooses the keys track.
   Renaming a track renames it everywhere: patterns, scenes, targets, ENERGY, GUARD.
5. **Patterns.** `+ bass` makes a pattern for that track.
   - **Drums:** click a cell to cycle `. x X s g 2 3 4`; shift-click or right-click goes back. Lanes are added from
     the list; the length is 16, 32 or 64.
   - **Synth parts:** type steps (`c1 - . c1 . . c5 .`), or click a step and use the buttons (degrees, chord tones,
     rest, tie, accent, soft, slide, ratchet).
   - "As compiled" draws the notes the device will hold. Chord tokens are unrolled over a progression: the one of the
     first scene that plays the pattern, or the one you choose.
6. **Scenes.** For A–D: the progression, the ENERGY table, a pattern per track (the drums per BEAT), the transition
   and the fill. The Smart Keys track has none: it plays the player's loop.
7. **Variations.** Add up to seven after ORIGINAL. Each has its sounds, parameters, swaps, energy bias and where it
   puts the macros.
8. **Macros.** Choose COLOR … ENERGY (or a control: SOFT … FREEZE) and add mappings.
   - **The plot.** The line is the curve you wrote. The dots are the model's effective value of the target (the
     firmware's macro engine and guard, as `tools/worldc.py` models them). Where they leave the line, a limit or
     the guard holds the value.
   - **The readout.** The slider's position, the scene and the variation give each target's base, offset and
     effective value.
   - **Test all macros** turns each macro to 0, 25, 50, 75 and 100 %, and lists every target it moves.
   - **Rules, curves and the ENERGY tables** follow on the same tab.
   - **A control without your own mappings** shows its built-in ones (Phase 13). "Replace them with my own" copies
     them as editable mappings.
9. **Smart Keys.** Choose the melody scale and the range, then pick a chord. The keyboard shows what each of the 27
   keys plays over it, and the chord tones are marked.
10. **Guardrails, Defaults.** An empty field takes the firmware default, which is shown.
11. **Save, Preview, Validate, Export** (below).

---

## 4. Previewing in the simulator

Preview saves the World if needed, then starts `build/host-bin/flowstate-sim --world <the file>`.
- The simulator is started once and kept. Every 500 ms it looks at the file, so each Save is heard at once: patterns,
  sounds and macros reload while playing. A World with errors keeps the last good one playing, and worldc's message
  shows on its status line.
- Opening another World and previewing it restarts the simulator with that file.
- The chip in the header shows the state. A click on it stops the simulator.
- Without the simulator, the chip reads `SIM missing` and the status line gives the command: `make -C host` (it needs
  SDL2: `brew install sdl2`).

In the simulator (see [simulator.md](simulator.md)):
- Option+Space plays.
- Shift+Z/X, C/V, B/N, M/, (or the four knobs) turn COLOR, MOTION, SPACE and ENERGY.
- While FX, ENV or LFO is held (U, O, P, a click, or a right-click latch), the same keys and knobs turn the
  device's KNOB 1–4, so the LIVE FX, SOUND SHAPE or MOVEMENT page gets them. The knob row shows that page's four
  controls (FILTER ECHO CRUSH FREEZE, SOFT SHORT BODY TAIL, DRIFT WOBBLE PULSE RATE) and their values. Let go, and
  they are the macros again; LIVE FX goes home.
- Option+I opens the inspector, the simulator's own view of the hidden mappings.

---

## 5. Validating

The Validate tab runs `tools/validate-world --json` in the background ([validation.md](validation.md)):

| Button | Runs | Time |
| --- | --- | --- |
| Static | the compiler, the model, the budgets | about a second |
| Quick | the above, then 372 renders through the real firmware: each scene over the 3⁴ grid of the four macros with a player, random points, the corners of the other controls | about 6 s |
| Full | every scene × variation × the 4-D grid | about 40 s |

- **The checklist.** The 17 items show first from the static pass, then from the renders. A click on an item lists
  its messages.
- **The budget bars.** CPU is measured in host instructions a sample against 2,300 (and 2,700 for one DMA half). The
  flash bar is the blob against 3,072 B (3,840 B for a user World), with the 2,048 B guideline. For a World in
  `worlds/factory`, the factory set is measured against its share of the app slot. RAM is the pattern pool and the
  targets.
- **"As a user World"** validates with the user limits.

The World is validated as it is in the page. A saved factory file is validated in place, so the factory-set budget
applies; otherwise a temporary copy is used.

---

## 6. Exporting and importing

**Export** writes to `build/author/<id>/`, and each file can also be downloaded:
- `<id>.wblob`: the FWD1 blob (`worldc compile`);
- `<id>.h`: the same blob as a C array;
- `<id>.world.json`: a copy of the JSON in the stable layout.

"As a user World" compiles with flag USER (up to 3,840 B), as the device's MY WORLDS hold them. A factory World ships
by putting its JSON in `worlds/factory` (Save with `--allow-factory`, or copy the exported JSON there), and the build
compiles it.

**Import** reads a user World blob (`.wblob` with flag USER, as the device's SAVE writes it) with
`tools/worldc.py import`:
- the source is decompiled, with the player's edits (the OVERRIDES section) folded in: another engine or preset, track
  parameters, globals, the kit;
- the notes are absolute and the names generated;
- the keys loop is left out, because a source has no key for it.

The World opens unsaved under `worlds/user/<id>.world.json`.

From the command line:

```sh
python3 tools/worldc.py import my.wblob -o worlds/user/my_world.world.json
python3 tools/worldc.py rename my.wblob "RAIN CITY 2"     # a blob's name (a user World's id follows it)
```

---

## 7. The diagnostics drawer (developers)

Alt+D, or `?dev=1` in the URL, opens a drawer under the page. It is hidden by default. It shows what the beginner UI
never shows: for a scene, a variation and the positions of all 16 controls, every hidden target the controls move.

| Column | |
| --- | --- |
| target | `pad.CUT`, `lead.~bright`, `g.dfdbk` (the role it plays, `@BRIGHT`, beside it) |
| base | the value the World authors in that scene and variation |
| offset | what the mappings and rules add, after the guard (steps) |
| effective | what the firmware plays; orange when a range, a cap or a hard limit holds it |
| normalised | the effective value on the parameter's range, 0..1 (smooth offsets: 0.5 is the authored sound) |
| range | the range the guard allows there |
| smooth | the slowest smoothing class of its mappings |
| controls, mappings | the controls and mappings behind it (JSON paths; `built-in FILTER` for Phase 13's own) |

The numbers come from the model in `tools/worldc.py`. `tests/run_tests.sh` checks that model against the firmware's C
engine, and `tests/author_test.py` checks this view against the model. The simulator's inspector (Option+I) shows the
same targets live.

---

## 8. The API

The page uses these, and so can scripts. Every body is JSON; a file is named by a ref, `<dir>/<name>.world.json`, with
`<dir>` one of `factory`, `user` and `dir`.

| Call | Does |
| --- | --- |
| `GET /api/state`, `GET /api/list` | the directories, the file opened at the start, the simulator; the Worlds of each directory |
| `GET /api/load?ref=R` | the World (`writable`, `mtime`) |
| `POST /api/save {ref, world, new?, base_mtime?, force?}` | writes it (403 for a factory World, 409 for a name taken or a file changed on disk), then checks it |
| `GET /api/new?id=&name=` | the template |
| `GET /api/names` | engines (EDIT labels, roles, presets), parameters and globals with their ranges and flags, kits, lanes, scales, the format's enums, the GUARD fields with their defaults, the default combinations, the built-in mappings |
| `POST /api/check {world, user?}` | `worldc check`: `errors` and `warnings`, each with `path`, `msg` and its checklist `item` |
| `POST /api/compile {world, user?}` | the blob's size, the pool, the limits |
| `POST /api/model {world, scene?, var?, pos? \| batch?}` | the targets at those positions (`pos`: `{"COLOR": 0.2, …}`, 0..1); `batch`: many positions on one compile |
| `POST /api/harmony {world}` | each progression's chords (names, tones, safe tones) and the 27 keys over each |
| `POST /api/pattern {world, name, progression?}` | the pattern's steps as compiled |
| `POST /api/validate {world, ref?, mode, user?}`, `GET /api/job?id=` | `validate-world` as a job: phase, progress, the static report, then the full one |
| `POST /api/export {world, user?, kinds}` | `build/author/<id>/`: `wblob`, `c`, `json` |
| `POST /api/import {blob}` | a user World blob (base64) as a source |
| `GET /api/preview`, `POST /api/preview {ref}` or `{stop: true}` | the simulator |

The server answers requests to `127.0.0.1:<port>` or `localhost:<port>` only, takes JSON bodies only, and sends the
page with a content security policy that allows no other origin.

`tests/author_test.py` (a group of `tests/run_tests.sh`, about 6 s) starts the server over a temporary copy of
`worlds/` and calls every endpoint. It also checks the page (it parses, calls only APIs that exist, names no URL) and
runs the page's notation helpers in node against `tools/worldc.py`.

---

## 9. Limits

- One World at a time, one browser tab. Undo is the browser's, inside a field; the JSON tab and Save are the safety
  net.
- The macros are not sent to the running simulator: turn them there. The page shows what they do through the model.
- The CPU estimate is host instructions, until Phase 17 calibrates on the device.
- Import is best effort: notes come back absolute, names are generated, the keys loop is dropped. It reads a blob
  file; reading MY WORLDS straight from a simulator flash image (`--flash FILE`) is not there yet.
