<!-- SPDX-License-Identifier: GPL-3.0-only -->
# FWD1: the compiled Musical World

| | |
| --- | --- |
| Status | **Normative** for the bytes of a World blob, version 1 (Phase 5). |
| Constants | [`firmware/src/world_fmt.h`](../../firmware/src/world_fmt.h) holds every number and code below. `tools/worldc.py` reads that header with a small parser and the firmware includes it, so the two sides share one definition. |
| Producer | `tools/worldc.py compile` (and later the firmware's user-World encoder, Phase 14) |
| Consumer | `firmware/src/world.c`: `wb_check` validates; `world_load`, `world_stage` and `world_commit` decode |
| Design | [play-mode-architecture.md](play-mode-architecture.md) §2.5 and §2.6. This document fills in what that table leaves open. Where they differ, this document wins (see [Deviations](#deviations-from-design-25)). |
| Sources | The author-facing JSON is described in [../worlds.md](../worlds.md) and checked against [`worlds/schema/world.schema.json`](../../worlds/schema/world.schema.json). |

Conventions:
- Little-endian.
- Byte-aligned: no padding anywhere.
- `u8`/`i8`/`u16`/`u32` are unsigned or signed integers of that width.
- "255" in an index field means *none*.
- The parser reads byte by byte with explicit bounds and never casts the blob to a struct.
- A blob is rejected **entirely** on the first failed check, with the error code from [Error codes](#error-codes).

---

## 1. Header

| Offset | Size | Field | Rule |
| --- | --- | --- | --- |
| 0 | 4 | magic | `'F' 'W' 'D' '1'` |
| 4 | 1 | version | 1. A parser rejects 0 and anything above the version it knows. |
| 5 | 1 | flags | b0 USER (Phase 14), b1 SESSION (Phase 9). Every other bit must be 0. |
| 6 | 2 | `L` | the total length, `24 ≤ L ≤ 3840`, and `L` ≤ the bytes available |
| 8 | 4 | world_id | FNV-1a-32 over the bytes of the source `id` (basis 2166136261, prime 16777619) |
| 12 | 4 | CRC | CRC-32 (zlib, the same as `storage.c st_crc32`) over bytes `[0, 12)` followed by `[16, L)`: everything except the CRC field |
| 16 | 1 | nsec | number of sections, 1..16 |
| 17 | 3 | reserved | 0 |
| 20 | 4·nsec | section table | `{u8 type, u8 count, u16 len}` per section |

The payloads follow the table back to back, in table order.

**Section table rules.**
- Types are **strictly ascending**, which also means each type appears at most once.
- Types run from 1 to 14. Types 15–17 are reserved (see [§14](#14-reserved-types)). Version 1 rejects any other type.
- `20 + 4·nsec + Σlen == L`.
- The required sections must be present: META, TRACKS, PROGS, SCENES, VARS, KEYS, DEFAULTS.
- All other sections are optional. An optional list section is present only with `count ≥ 1`. GUARD is the exception: it may be present with zero range records.
- Each section's `len` must equal exactly the length its counts imply.

Sizes:
- A **factory** World may be at most **3,072 B** (worldc refuses more).
- Above **2,048 B** worldc warns.
- A **user** World (`--user`) may be at most 3,840 B, the storage payload.

---

## 2. Strings

| Field | Bytes | Characters (the parser) | Characters (worldc) |
| --- | --- | --- | --- |
| META name | 15 | 1–14, glyphs 32–95 (FONT_L) | `A–Z 0–9 space & ' - .` |
| META category | 11 | 0–10, glyphs 32–95 | 1–10 of the same set |
| META blurb | 25 | 0–24, printable Latin-1: 32–126 and 160–255 (FONT_S) | the same |
| scene and variation names | 11 | 1–10, glyphs 32–95 | `A–Z 0–9 space & ' - .` |

Every string field is NUL-padded:
- Its last byte is always 0.
- After the first NUL every byte is 0.

The parser checks that the text can be drawn. The compiler additionally enforces the house style.

---

## 3. META (type 1, count 1, 57 B)

| Offset | Size | Field | Range |
| --- | --- | --- | --- |
| 0 | 15 | name | [§2](#2-strings) |
| 15 | 11 | category | |
| 26 | 25 | blurb | |
| 51 | 1 | bpm | `bpm_min ≤ bpm ≤ bpm_max` |
| 52 | 1 | bpm_min | ≥ 40 |
| 53 | 1 | bpm_max | ≤ 240. worldc: within bpm ± 20 % (integer). The default range is ±10 %. |
| 54 | 1 | root | 0–11 (C = 0) |
| 55 | 1 | scale | 0–15, an index into SLOOP's `N_SCALE` / `SCALE_MASK` |
| 56 | 1 | swing | 0–100 (`G_SWING`) |

---

## 4. TRACKS (type 2, count 4)

There are four records, one per track: three synth tracks, then the drum track. Each record is `5 + 2·npairs` bytes.

| Offset | Field | Synth tracks 0–2 | Drum track 3 |
| --- | --- | --- | --- |
| 0 | role | 0 pad, 1 chords, 2 bass, 3 lead, 4 keys, 5 texture (not 6) | 6 drums |
| 1 | engine | `< NENGINES`, an index into `ENGINES[]` | 255 |
| 2 | preset | `< ENGINES[engine]->npresets` | the kit, `< DRUM_KITS` (`DRUM_KIT_NAMES`) |
| 3 | register | a MIDI note 0–127 (C4 = 60) | 0 |
| 4 | npairs | 0–64 | 0–64 |
| 5 | pairs | `{u8 P_id, i8 value}` × npairs | |

**Register.** The register is the floor of the part's home octave. The track's *tonic* is the lowest note at or above the register whose pitch class is the World root. Degree and chord tokens are placed from that tonic (see [../worlds.md](../worlds.md#melodic-patterns)). The firmware stores the register for later phases and does not otherwise use it.

The register a source omits depends on the role:

| Role | Default register |
| --- | --- |
| pad, chords, keys, texture | C3 (48) |
| bass | C2 (36) |
| lead | C4 (60) |

**Pairs.** These are the World's sound on top of the preset.
- `P_id < P_COUNT`, and never in `WF_P_FIXED`: `P_ROOT P_SCALE P_SLEN P_SDIV P_MUTE P_ED_FX`. The World's key, its patterns and the player own these.
- On the drum track only `WF_P_DRUM` is allowed: `P_PAN P_SSWING P_SLCR P_SLPAT P_SLRATE P_SLDEPTH`.
- The drum level and reverb are the globals `G_DRLVL` and `G_DRREV` (GLOBALS). worldc maps the source names `drums.level` and `drums.rev` onto them.

**Values.** The parser accepts any value. The stage clamps every value to its descriptor, as `proj_apply` does. worldc refuses out-of-range values.

---

## 5. GLOBALS (type 3, count 1–14, 2 B each)

`{u8 G_id, i8 value}`. `G_id` must be in `WF_G_WHITELIST`:

`G_SWING G_DTIME G_DFDBK G_DCOLOR G_DMIX G_RSIZE G_RDAMP G_CRATE G_CDEPTH G_DRLVL G_DRREV G_DUST G_DUCK G_FILT`

worldc writes:
- the top-level `fx`, without swing (swing is set by META);
- the drum track's `level`/`rev` aliases.

---

## 6. PATTERNS (type 4, count 1–16)

Each record is `u8 kind_div, u8 len, u16 nbytes`, then `nbytes` of data.

| Field | Rule |
| --- | --- |
| `kind_div` | b7 = drum pattern; b0–2 = div (`N_DIV`: 0 1/4, 1 1/8, 2 1/16, 3 1/32, 4 8T, 5 16T); b3–6 = 0. div < 6. |
| `len` | 1–64 steps. A drum pattern is always div 2 (1/16) with `len` 16, 32 or 64. |
| `nbytes` | the exact length of the data |

The record index is the pool index: the firmware decodes record *i* into `wpool[i]`. The track's `P_SLEN = len` and `P_SDIV = div` come from the pattern it plays.

### 6.1 Synth data: a record stream

A cursor starts at step 0. Each record begins with `ctl = kind << 6 | arg`:

| Kind | Name | Meaning |
| --- | --- | --- |
| 0 | NOTE | Skip `arg` (0–63) REST steps, then one NOTE step, encoded below. |
| 1 | TIE | `arg` (1–63) TIE steps |
| 2 | REST | `arg` (1–63) REST steps |
| 3 | END | `arg` = 0. The rest of the pattern is REST. It must be the last byte. |

A NOTE step is encoded as `u8 nf`, then `note[n]`, then the optional `[lvl rat]`, `[vel]`, `[micro]`, in that order:

| `nf` bits | Meaning |
| --- | --- |
| b0–2 | `n`, 0–4. 0 means "the same notes as the previous NOTE record", which must exist. 5–7 are invalid. |
| b3 | accent (`SF_ACCENT`) |
| b4 | slide (`SF_SLIDE`) |
| b5 | two bytes follow: `lvl`, `rat` (2 bits per note: `LV_*`, ratchet − 1) |
| b6 | one byte follows: `vel`, 1–127 (0 is "absent") |
| b7 | one byte follows: `micro`, 1–7, step flags b2–4 (Phase 10 micro-timing) |

Notes are 0–127.

Rules:
- The cursor never passes `len`.
- When the bytes run out, the remaining steps are REST.

Decoded steps are exactly what SLOOP keeps:
- REST is a zeroed `step_t` with `time = ST_REST`, as `steps_clear` writes it.
- TIE is a zeroed `step_t` with `time = ST_TIE`, as `rec_hold` writes it.
- NOTE is `time = ST_NOTE`, `n`, the notes, the flags and the optional bytes.

**Canonical form (what worldc emits):**
- No END record, and no trailing REST records.
- Runs longer than 63 are split.
- `n = 0` whenever a NOTE step repeats the previous NOTE's notes.
- `lvl`/`rat` only when non-zero; bits of unused notes are 0.
- No `vel` or `micro`.

Version 1 cannot express a TIE or REST step that carries notes or flags. The Phase 14 encoder normalises such steps (SLOOP ignores their contents, except `SF_SLIDE` on a TIE).

### 6.2 Drum data: lane-major

The data starts with `u8 nlanes` (0–16). Each lane record follows:
- `u8 lane | haslvl << 4 | hasrat << 5`. Bits 6–7 are 0. Lanes are strictly ascending: 0 kick … 15 bell, in `drums.c` lane order.
- A bitmap of `len / 8` bytes. Step *i* is byte `i >> 3`, bit `i & 7`.
- If `haslvl`: 2 bits per hit, in step order, LSB first, in `ceil(2·hits / 8)` bytes. These are `LV_NORM 0`, `LV_GHOST 1`, `LV_SOFT 2`, `LV_HARD 3`.
- If `hasrat`: the same for ratchets (`hits − 1`).

The data must be consumed exactly.

**Canonical form:**
- Only lanes with hits are written.
- Each flag is set only when some hit needs it.
- Padding bits are 0. The parser does not check padding bits.

---

## 7. PROGS (type 5, count 1–8)

Each record is `u8 nchords` (1–16), then `{u8 root << 4 | quality, u8 beats}` × nchords.

Rules:
- `root` is in semitones above the World root, 0–11.
- `beats` is 1–32.
- The total is 4, 8, 16 or 32 beats.

| Code | Quality | Tone mask (bit *i* = *i* semitones) | `c3` | `c5` | `c7` |
| --- | --- | --- | --- | --- | --- |
| 0 | MAJ | 0 4 7 | 4 | 7 | 12 |
| 1 | MIN | 0 3 7 | 3 | 7 | 12 |
| 2 | DIM | 0 3 6 | 3 | 6 | 12 |
| 3 | AUG | 0 4 8 | 4 | 8 | 12 |
| 4 | SUS2 | 0 2 7 | 2 | 7 | 12 |
| 5 | SUS4 | 0 5 7 | 5 | 7 | 12 |
| 6 | MAJ7 | 0 4 7 11 | 4 | 7 | 11 |
| 7 | MIN7 | 0 3 7 10 | 3 | 7 | 10 |
| 8 | DOM7 | 0 4 7 10 | 4 | 7 | 10 |
| 9 | M7B5 | 0 3 6 10 | 3 | 6 | 10 |
| 10 | MAJ6 | 0 4 7 9 | 4 | 7 | 9 |
| 11 | MIN6 | 0 3 7 9 | 3 | 7 | 9 |
| 12 | ADD9 | 0 2 4 7 (the 9th as bit 2) | 4 | 7 | 12 |
| 13 | MADD9 | 0 2 3 7 | 3 | 7 | 12 |
| 14 | POW5 | 0 7 | 12 | 7 | 12 |
| 15 | DIM7 | 0 3 6 9 | 3 | 6 | 9 |

How the chord tokens resolve:
- **12** means "the root an octave up", used for a tone the chord lacks: the 7th of a triad, the 3rd of POW5.
- `c1` is 0.
- `c9` is 14 on ADD9/MADD9. Otherwise it is the scale's 9th: 13 when the major 9th is outside the World's degree scale and the minor 9th is inside it, else 14.

The *degree scale* is the World scale when that has 7 notes. Otherwise it is its parent: MAJ for PEN, CHR, WHOLE, DIMHW and DIMWH; MIN for MPEN and BLUES.

Chord tokens and roman numerals are compiled away. The firmware only needs the masks, which Phase 6's harmony uses.

---

## 8. SCENES (type 6, count 4: A, B, C, D)

Each record is `24 + 3·npairs` bytes.

| Offset | Size | Field | Rule |
| --- | --- | --- | --- |
| 0 | 11 | name | [§2](#2-strings) |
| 11 | 1 | role | 0 intro, 1 main, 2 lift, 3 breakdown |
| 12 | 1 | prog | `< PROGS count` |
| 13 | 1 | energy | `< ENERGY count`, or 255 |
| 14 | 1 | transition | 1, 2 or 4 bars; 0: the phrase (the playing progression's length, Phase 11) |
| 15 | 1 | fill | a **drum** pattern, or 255 |
| 16 | 3 | pat[3] | per synth track: a **synth** pattern, or 255. The Smart Keys track's entry must be 255 (it plays the user loop). |
| 19 | 4 | beat[4] | the drum pattern per BEAT (0 MINIMAL, 1 GROOVE, 2 BUSY, 3 BREAK): a **drum** pattern, or 255 |
| 23 | 1 | npairs | 0–64 |
| 24 | 3·n | pairs | scoped pairs `{u8 scope, u8 id, i8 value}` |

**Scoped pairs.**
- Scope 0–3 is a track, and the id is a `P_*` that is neither fixed nor structural ([§15](#15-parameter-lists)). On a synth track an `F_ENUM` engine parameter is structural, except WHEEL E7 ROTR. The drum track allows `P_PAN` and `P_SLDEPTH`.
- Scope 4 is the globals, and the id is a whitelisted `G_*`.

**Pool order (canonical).** worldc assigns pool indices in first-use order:
1. scenes A..D;
2. within a scene: synth tracks 0..2, then beat 0..3, then the fill;
3. then the patterns that variation swaps need.

The order of keys in the source therefore never changes the blob.

---

## 9. VARS (type 7, count 1–8)

Each record is `15 + 2a + 2b + 3n` bytes:

| Field | Size | Rule |
| --- | --- | --- |
| name | 11 | [§2](#2-strings) |
| bias | 1 | `i8` energy bias, units of 1/250, −63..63 (worldc: ±0.25) |
| nsound (a) | 1 | 0–4 |
| sounds | 2a | `{u8 trk, u8 preset}`: a synth track's preset (same engine) `< npresets`, or the drum track's kit `< DRUM_KITS` |
| nswap (b) | 1 | 0–16 |
| swaps | 2b | `{u8 from, u8 to}`: two **synth** patterns |
| npairs (n) | 1 | 0–64 |
| pairs | 3n | scoped pairs as in SCENES. Scope 4 may not set `WF_G_NOVAR` (`G_SWING`): a variation keeps the groove. |

Variation 0 must be empty: no sounds, no swaps, no pairs, bias 0. worldc requires its name to be ORIGINAL.

---

## 10. Targets (MAPS, RULES, GUARD)

A target is two bytes:
- the **target byte** `kind << 5 | mask`, where `mask` has one bit per track 0..3 and bit 4 is 0;
- an id.

| Kind | Name | Mask | Id |
| --- | --- | --- | --- |
| 0 | track parameter | ≥ 1 track | a `P_*` neither fixed nor structural on every track in the mask. An engine parameter (≥ `P_E0`) takes a single track. |
| 1 | engine role | synth tracks only | role 0–7: BRIGHT RESO DRIVE SHAPE DETUNE AIR MOVE BODY |
| 2 | vmod brightness (`~bright`) | synth tracks only | 0 |
| 3 | vmod shape (`~shape`) | synth tracks only | 0 |
| 4 | global | 0 | a whitelisted `G_*` not in `WF_G_STRUCT` (`G_SWING`, `G_DTIME`) |

**Engine roles** (`WF_ENG_ROLE`): for each engine, the EDIT slot of each role. A role the engine lacks makes the mapping a no-op on that track, and worldc warns.

| Engine | BRIGHT | RESO | DRIVE | SHAPE | DETUNE | AIR | MOVE | BODY |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| ANALOG | E4 CUT | E5 RES | E6 DRV | E2 MIX | E1 DTN | E3 NOIS | — | — |
| DIGITAL | E4 IDX | — | E6 FB | E5 MDEC | — | — | — | — |
| PHASE | E2 DCW | — | — | E3 ENV | E4 DTN | — | — | E6 SUB |
| LOFI | E7 TONE | — | E3 CRSH | E2 DUTY | — | — | E5 VIB | — |
| SAMPLE | E4 CUT | — | E6 DRV | — | — | — | — | — |
| VOICE | E4 BUZZ | E6 Q | — | E0 VOWL | — | E5 BRTH | E7 RAND | — |
| TRIO | E5 CUT | E6 RES | — | E7 PW | E3 DTN | — | — | — |
| WHEEL | E3 TOP | — | E6 DRV | E5 CLICK | — | — | E7 ROTR | E1 SUB |
| GRAIN | E7 TONE | — | — | E1 POS | E6 RAND | — | E5 SPRD | E2 SIZE |

---

## 11. MAPS (type 8, count 1–40, 6 B each) and CURVES (type 9, count 1–8, 9 B each)

A MAPS record is `u8 ctl, u8 target, u8 id, u8 class << 6 | curve, i8 min, i8 max`.

| Field | Values |
| --- | --- |
| ctl | 0–15. Each control has a home position where its offset is zero. |
| class | 0 fast (10 ms), 1 medium (60 ms, the default), 2 slow (250 ms), 3 stepped. worldc forces 3 on an `F_ENUM` target (WHEEL ROTR). |
| curve | 0 lin, 1 exp, 2 log, 3 s, 4 late, 8 + *k* = CURVES record *k* (*k* < CURVES count). 5–7 and others are invalid. |
| min, max | offsets in the target's own units (parameter steps, or vmod Q8/256). For a control whose home is 0, worldc requires `min = 0`. |

The controls and their home positions:

| Ids | Controls | Home |
| --- | --- | --- |
| 0–3 | COLOR, MOTION, SPACE, ENERGY | 500 |
| 4–7 | SOFT, SHORT, BODY, TAIL | 0 |
| 8–10 | DRIFT, WOBBLE, PULSE | 0 |
| 11 | RATE | 500 |
| 12 | FILTER | 500 |
| 13–15 | ECHO, CRUSH, FREEZE | 0 |

Records for controls 4–15 replace that control's built-in mappings (`controls` in the source). `firmware/src/macro.c` evaluates the mappings (design §5.2–5.5).

A CURVES record is a LUT9: `u8 y[9]`, the values at x = 0, 1/8, …, 1, in units of 1/255. `y[0] = 0` and `y[8] = 255`, so the centre is the authored sound and the end is the full offset.

---

## 12. RULES (type 10, count 1–8)

Each record is `u8 a << 4 | b, u8 ta, u8 tb, u8 nact`, then `{u8 target, u8 id, i8 add, u8 0}` × nact.
- `a` and `b` are controls 0–15.
- The thresholds `ta` and `tb` are 0–249, in units of 1/250. 250 would make the strength's divisor `1000 − t` zero.
- A rule with one condition repeats it: `b = a`, `tb = ta`.
- `nact` is 1–4.
- The fourth byte of each action is 0.

---

## 13. ENERGY (type 11, count 1–4), GUARD (type 12), KEYS (type 13), DEFAULTS (type 14)

### ENERGY

Each table is `u16 density_lanes, u8 nbands` (1–4), then 12-byte bands:

| Offset | Size | Field | Rule |
| --- | --- | --- | --- |
| 0 | 1 | from | 0–250, in units of 1/250. The first band is 0, then strictly ascending. |
| 1 | 1 | flags | b0–3 layers (the tracks heard), b4 fills, b5 ratchets. b6–7 are 0. |
| 2 | 2 | lanes | allowed drum lanes, a bit per lane |
| 4 | 2 | dens_steps | a 16-step mask applied to `density_lanes` |
| 6 | 6 | play[3] | per synth track, a 16-step mask: a NOTE step whose bit is clear plays as REST |

All masks are neutral when all ones. worldc requires the Smart Keys track in every band's layers, and the firmware never mutes it. `firmware/src/arrange.c` applies the bands (design §5.7) and the BEAT masks with them (Phase 11).

### GUARD

The fixed part is 32 bytes. Every byte is 255 when unset, and the firmware default then applies. The fixed part is followed by `{u8 target, u8 id, i8 lo, i8 hi}` × count, with `lo ≤ hi` and count ≤ 32, then by the sound combinations: `{u8 target, u8 id, i8 value}` × 3 (9 B) × the fixed byte 25 (0–8; 255 = none follow). The three are condition a (over a value), condition b (a one-condition combination repeats a) and the capped target with its cap. Their targets are of kind 0, 1 or 4 (§10). The section is `32 + 4 × count + 9 × combinations` bytes.

| Offset | Field | Range | Default when 255 |
| --- | --- | --- | --- |
| 0–5 | range lo, hi of synth tracks 0, 1, 2 (MIDI) | both 255, or `lo ≤ hi ≤ 127` | per role (design §6.1) |
| 6 | max_poly (Smart Keys track) | 1–4 | 4 |
| 7 | loop_follow | 0 off, 1 snap | snap |
| 8 | avoid | 0 classic, 1 none, 2 strict | classic |
| 9 | record quantise | 0–250 | 188 (0.75) |
| 10 | notes per recorded step | 1–4 | 4 |
| 11 | max_level | 0–127 | 116 |
| 12 | max_reso | 0–127 | 110 |
| 13 | max_dfdbk | 0–120 | 96 |
| 14 | max_rsize | 0–127 | 120 |
| 15 | max_dist | 0–127 | 100 |
| 16 | max_dust | 0–127 | 100 |
| 17 | mute_change (bars) | 1–2 | 1 |
| 18 | density_change | 0 beat, 1 bar | beat |
| 19 | fills_every (bars) | 1–32 | the progression's length |
| 20 | bars between ENERGY band changes | 1–16 | 1 |
| 21 | max unison voices | 1–8 | 4 |
| 22 | GRAIN DENS cap | 0–127 | 90 |
| 23 | tracks with DIST > 0 | 0–3 | 2 |
| 24 | cpu_q8 ceiling | 1–254 | 217 (85 %) |
| 25 | sound combinations that follow the ranges | 0–8 | none follow: the firmware's six (`WF_COMBO_DEFAULT`) apply; 0: none apply |
| 26–31 | reserved | 255 | |

The ranges and defaults are `WF_GUARD_MIN`, `WF_GUARD_MAX` and `WF_GUARD_DEFAULT`; the firmware's combinations are `WF_COMBO_DEFAULT`, in the record format above. Byte 25 was reserved (255) before Phase 8, so every older blob reads as "the firmware's combinations".

### KEYS (count 1, 8 B)

| Offset | Field | Rule |
| --- | --- | --- |
| 0 | trk | the Smart Keys track, 0–2 (not the drum track) |
| 1 | mode | 0 melody, 1 chords, 2 bass, 3 drums |
| 2 | melody_mask (`u16`) | 12 bits relative to the World root. Bit 0 is set, and the mask lies inside `SCALE_MASK[scale]`. Bits 12–15 are 0. |
| 4 | white | 0 scale, 1 safe |
| 5 | black | 0 chord, 1 chord+9 |
| 6 | tonic | MIDI 0–127 |
| 7 | loop_pat | a synth pattern holding the user loop, or 255 (always 255 in a factory World) |

### DEFAULTS (count 1, 16 B)

| Offset | Field | Rule |
| --- | --- | --- |
| 0 | scene | 0–3 |
| 1 | variation | `< VARS count` |
| 2–5 | ctl[4] | COLOR MOTION SPACE ENERGY, 0–250 |
| 6 | pulse | 0 OFF, 1 SLOW, 2 PULSE, 3 DRIVE |
| 7 | beat | 0 MINIMAL, 1 GROOVE, 2 BUSY, 3 BREAK |
| 8–11 | shape[4] | SOFT SHORT BODY TAIL, 0–250 |
| 12–15 | move[4] | DRIFT WOBBLE PULSE RATE, 0–250 |

---

## 14. Reserved types

Types 15 OVERRIDES, 16 PATCHES and 17 PLAYSTATE are for user Worlds and the session (design §10.3–10.4). Phase 9 and Phase 14 define them. The version-1 parser of Phase 5 rejects them with `WE_SECTION`. Version stays 1 until the first release, because no parser has shipped yet.

---

## 15. Parameter lists

These are defined in `world_fmt.h`. worldc resolves the symbols through `sloop-params.json`.

| List | Members | Used by |
| --- | --- | --- |
| `WF_P_FIXED` | `P_ROOT P_SCALE P_SLEN P_SDIV P_MUTE P_ED_FX` | never in any pair |
| `WF_P_DRUM` | `P_PAN P_SSWING P_SLCR P_SLPAT P_SLRATE P_SLDEPTH` | the drum track's only parameters |
| `WF_P_STRUCT` | `P_VOICE P_ROOT P_SCALE P_QUANT P_TRANS P_SLEN P_SDIV P_CHORD P_MUTE P_SLCR P_SLPAT`, the arp group `P_AMODE … P_AORDER`, `P_LWAVE P_GLMODE P_PRIO P_ALLOC P_SLRATE P_SSWING P_ED_FX`, and every `F_ENUM` engine parameter except WHEEL E7 | never in SCENES/VARS pairs or targets. TRACKS pairs may set them. |
| `WF_G_WHITELIST` | see [§5](#5-globals-type-3-count-114-2-b-each) | GLOBALS, scope 4 |
| `WF_G_STRUCT` | `G_SWING G_DTIME` | never a target |
| `WF_G_NOVAR` | `G_SWING` | never in a variation |

---

## 16. Staging: what a blob means

`world_stage(scene, var)` composes each track as follows. Later steps win.

1. **Start.** Each `p[i]` for `i < P_E0` is set to `TP[i].def`.
   - A synth track takes its engine's EDIT defaults, then `preset_fill(p, engine, preset)`. The preset is the variation's swap for this track if it has one, otherwise the TRACKS preset. `P_ROOT` and `P_SCALE` come from META.
   - The drum track sets `P_E0` to the kit (or the variation's kit) and `P_E1..E7` to 0.
2. **The World's sound:** the TRACKS pairs.
3. **The variation's pairs** for this track.
4. **The scene's pairs** for this track.
5. **(Phase 14) The overrides.**
6. **The pattern.**
   - A synth track plays `pat[t]`.
   - The drum track plays `beat[wrt.beat]`, else `beat[GROOVE]`. `wrt.beat` starts at `DEFAULTS.beat`.
   - The variation's swaps apply, the first match winning.
   - The track's `P_SLEN`/`P_SDIV` come from the pattern record. With no pattern they keep their defaults and the track plays an empty pattern.
7. **The Smart Keys track** never takes a pattern; it keeps its loop.
   - At a World switch, its loop is `P_SLEN = 16 × min(bars of the scene's progression, 4)` with `P_SDIV` = 1/16. Its arp group (`P_AMODE … P_AORDER`) is reset to the defaults, then `AMODE ARATE AOCT AGATE` are taken from the PULSE preset of `DEFAULTS.pulse`:

     | PULSE | AMODE | ARATE | AOCT | AGATE |
     | --- | --- | --- | --- | --- |
     | OFF | OFF | 1/16 | 1 | 64 |
     | SLOW | UP | 1/8 | 1 | 60 |
     | PULSE | UPDN | 1/16 | 1 | 50 |
     | DRIVE | UP | 1/16 | 2 | 35 |

   - Otherwise its current `P_SLEN`, `P_SDIV` and arp group are kept.
   - worldc refuses arp parameters in that track's TRACKS pairs.
8. **`P_MUTE`** is the player's, kept, except at a World switch.
9. **Clamping.** Every value is clamped to its descriptor: `TP`, the engine's `edit[]`, or `DRUM_KIT_DESC`.

Globals:
1. Every whitelisted global starts at `GP` default.
2. `G_SWING` is set from META.swing.
3. Then GLOBALS, the variation's scope-4 pairs and the scene's scope-4 pairs apply, in that order.
4. The result is clamped.

Globals outside the whitelist (`G_BPM`, `G_CLOCK`, `G_TUNE`, `G_ROLL`, `G_DRCH`, the system ones) are never touched, except `G_BPM = META.bpm` at a World switch.

`world_commit` (the audio ISR on a boundary, Phase 11; IRQs off while stopped, `world_apply`):
1. **Patterns.** For each track whose pool entry changes, it writes the outgoing steps back into their pool entry (not the Smart Keys track), then copies the new entry in, or clears the track.
2. **Parameters.** It copies `p[]`. A synth track's `eng_req` is set, and `engine_block` fades an engine change.
3. **Globals.** It copies the whitelisted globals.
4. **Indices.** It stores the scene, variation, progression, energy and fill indices.
5. **World switch.** At a World switch it also sets `G_BPM`, releases every track (`panic_req`), clears the keys loop and selects the Smart Keys track.

---

## Error codes

`wb_check` and `world_load` return one of these codes, `world_apply` returns BUSY or STATE, and the PLAY UI shows the code as `WORLD ERROR n`.

| n | Name | Meaning |
| --- | --- | --- |
| 0 | OK | |
| 1 | SIZE | shorter than the header; `L` outside 24..3840 or beyond the buffer |
| 2 | MAGIC | not `FWD1` |
| 3 | VERSION | version 0, or newer than the parser |
| 4 | FLAGS | an unknown flag bit |
| 5 | CRC | |
| 6 | HEADER | nsec outside 1..16, or a reserved byte not 0 |
| 7 | TABLE | the lengths do not add up to `L` |
| 8 | SECTION | an unknown or reserved type, or types not ascending |
| 9 | MISSING | a required section is absent |
| 10 | LENGTH | a section's length differs from what its counts imply |
| 11 | COUNT | a count outside its range |
| 12 | STRING | a name or label outside its charset, or not NUL-padded |
| 13 | META | tempo, root, scale or swing out of range |
| 14 | TRACK | role, engine, preset, kit or register |
| 15 | PARAM | a parameter or global id not allowed there |
| 16 | PATTERN | a pattern header or its data |
| 17 | NOTE | a note above 127 |
| 18 | PROG | chord count, root, beats or total |
| 19 | SCENE | role, transition, or a pattern on the Smart Keys track |
| 20 | VAR | bias, sounds, or a non-empty first variation |
| 21 | MAP | control or target |
| 22 | CURVE | a LUT that does not run from 0 to 255 |
| 23 | RULE | threshold, action count or target |
| 24 | ENERGY | band count, order or flags |
| 25 | GUARD | a fixed field, a range or combination record |
| 26 | KEYS | track, mode, melody mask, white/black, tonic |
| 27 | DEFAULTS | scene, variation, position, PULSE or BEAT |
| 28 | INDEX | a pattern, progression, energy or curve index out of range, or of the wrong kind |
| 29 | BUSY | `world_load`/`world_apply` while a World plays, or with a stage pending |
| 30 | STATE | no World loaded, or a scene or variation out of range |

worldc's decoder (used by `decompile`) raises the same codes. The fuzz tests in `tests/world_test.c` check them.

---

## Deviations from design §2.5

| Design §2.5 | Here | Why |
| --- | --- | --- |
| CRC over `[16, L)` | over `[0, 12)` + `[16, L)` | A flipped `world_id` or flag bit would otherwise pass unnoticed. The test flips every byte. |
| GUARD "32 B fixed (…)" | the layout in [§13](#guard), 255 = unset | It was unspecified. |
| DEFAULTS "`shape[4]? …` (pad to 16)" | scene, var, ctl[4], pulse, beat, shape[4], move[4] = 16 B | It was unspecified. |
| PATTERNS END "the rest is REST" | also implicit when the bytes run out; END must be the last byte | Shorter, and canonical without END. |
| unknown sections | rejected; the table is strictly ascending | One encoding per World. |
| registers, roles, transitions, the target byte, string charsets, control homes | defined here | They were unspecified. |
