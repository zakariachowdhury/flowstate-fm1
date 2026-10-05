#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Unit checks of tools/worldc.py and tools/gen_worlds.py (run by tests/run_tests.sh, from the repo root):
  - worlds/schema/sloop-params.json is what tools/dump_params.c prints from the firmware sources now;
  - the schema's enums are world_fmt.h's and sloop-params.json's;
  - compile -> decompile -> compile gives the same bytes (the test Worlds);
  - a scene's transition: 1, 2, 4 bars, "bar" or "phrase" (0 in the blob), its round trip and errors;
  - the notation: degrees, accidentals, octave marks, chord tokens per progression, unrolling, suffixes, drums;
  - bad sources fail with a message that names the place (JSON path) and the problem;
  - the size limits (warning above 2048 B, error above 3072 B), the pool limit;
  - the decoder rejects corruptions with the firmware's error codes;
  - gen_worlds.py: a valid header with no factory World, sorting, alignment;
  - GUARD sound combinations round trip, and their errors; the model of the macros and the guard (design 6.4):
    `check` passes the factory Worlds and refuses a mapping out of its range, past a hard limit, at the maximum at
    100 % without "saturate", a Smart Keys range under an octave (the C engine against the model: tests/run_tests.sh).
"""
import contextlib
import copy
import io
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import gen_worlds  # noqa: E402
import worldc  # noqa: E402

FAILS = []


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print(f"FAIL {what}")


def load(name):
    return json.loads((ROOT / "worlds" / "test" / f"{name}.world.json").read_text())


def compile_(src, user=False):
    return worldc.compile_world(src, user)


def errors_of(src):
    blob, d = compile_(src)
    return blob, "\n".join(d.errors)


def expect_error(src, *needles, what):
    blob, err = errors_of(src)
    ok = blob is None and all(n in err for n in needles)
    check(ok, f"{what}: expected an error with {needles}, got {err or 'none'}")


def test_params_fresh():
    out = ROOT / "build" / "host"
    out.mkdir(parents=True, exist_ok=True)
    exe = out / "dump_params"
    cc = os.environ.get("CC", "cc").split()
    r = subprocess.run([*cc, "-O1", "-w", "-Ibuild/gen", "-Ifirmware/src", "-o", str(exe), "tools/dump_params.c",
                        "-lm"], cwd=ROOT, capture_output=True, text=True)
    check(r.returncode == 0, f"dump_params builds: {r.stderr[-500:]}")
    if r.returncode:
        return
    now = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    committed = (ROOT / "worlds" / "schema" / "sloop-params.json").read_text()
    check(now == committed, "worlds/schema/sloop-params.json is stale: run build/host/dump_params > "
                            "worlds/schema/sloop-params.json (see tools/dump_params.c) and review the diff")


def test_schema_enums():
    s = json.loads(worldc.SCHEMA_JSON.read_text())
    defs, props = s["$defs"], s["properties"]
    check(defs["track"]["properties"]["role"]["enum"] == worldc.ROLE_NAMES, "schema track roles = WF_ROLE_NAMES")
    check(defs["scene"]["properties"]["role"]["enum"] == worldc.SROLE_NAMES, "schema scene roles = WF_SROLE_NAMES")
    check(props["defaults"]["properties"]["beat"]["enum"] == worldc.BEAT_NAMES, "schema BEATs = WF_BEAT_NAMES")
    check(props["defaults"]["properties"]["pulse"]["enum"] == worldc.PULSE_NAMES, "schema PULSE = WF_PULSE_NAMES")
    check(props["macros"]["propertyNames"]["enum"] == worldc.CTL_NAMES[:4], "schema macros = controls 0..3")
    check(props["controls"]["propertyNames"]["enum"] == worldc.CTL_NAMES[4:], "schema controls = controls 4..15")
    check(defs["mapping"]["properties"]["smooth"]["enum"] == worldc.CLASS_NAMES, "schema smooth = WF_CLASS_NAMES")
    check(defs["scale"]["enum"] == [x["name"] for x in worldc.PR.scales], "schema scales = N_SCALE")
    check(defs["pattern"]["properties"]["div"]["enum"] == worldc.PR.divs, "schema divs = N_DIV")
    check(defs["fx"]["propertyNames"]["enum"] == [worldc.PR.G[g]["name"] for g in worldc.G_WHITE],
          "schema fx = WF_G_WHITELIST")
    sk = props["smart_keys"]["properties"]
    check(sk["mode"]["enum"] == worldc.KMODE_NAMES and sk["white"]["enum"] == worldc.WHITE_NAMES and
          sk["black"]["enum"] == worldc.BLACK_NAMES, "schema smart_keys enums")
    check(defs["scene"]["properties"]["transition"]["enum"] == worldc.F["WF_TRANSITIONS"] + ["bar", "phrase"],
          "schema transitions")


def test_roundtrip():
    for name in ("minimal", "full", "guard"):
        blob, d = compile_(load(name))
        check(blob is not None, f"{name} compiles: {d.errors}")
        if blob is None:
            continue
        dec = worldc.decompile(blob)
        blob2, d2 = compile_(json.loads(json.dumps(dec)))
        check(blob2 == blob, f"{name}: compile -> decompile -> compile gives the same bytes")
        check(worldc.decode(blob)["id_hash"] == worldc.fnv1a(f"test_{name}"), f"{name}: world_id = FNV-1a(id)")
    check(worldc.fnv1a("") == 0x811C9DC5 and worldc.fnv1a("a") == 0xE40C292C, "FNV-1a-32 reference values")


def test_transitions():
    """a scene's transition: 1, 2, 4 bars, "bar" (1) or "phrase" (0 in the blob, Phase 11); round trip; errors"""
    src = load("full")
    src["scenes"]["A"]["transition"] = "bar"
    src["scenes"]["D"]["transition"] = "phrase"
    blob, d = compile_(src)
    check(blob is not None, f"\"bar\" and \"phrase\" compile: {d.errors}")
    if blob is None:
        return
    sc = worldc.decode(blob)["scenes"]
    check(sc[0]["transition"] == 1 and sc[2]["transition"] == 2 and sc[3]["transition"] == worldc.F["WF_TRANS_PHRASE"],
          "transitions in the blob: \"bar\" 1, 2, \"phrase\" 0")
    dec = worldc.decompile(blob)
    check(dec["scenes"]["D"].get("transition") == "phrase", "decompiled: \"phrase\"")
    blob2, _ = compile_(json.loads(json.dumps(dec)))
    check(blob2 == blob, "a phrase transition: compile -> decompile -> compile gives the same bytes")
    for bad in (0, 3, "phrases", True):
        src["scenes"]["D"]["transition"] = bad
        expect_error(src, "transition", what=f"transition {json.dumps(bad)}")


def pattern_steps(src, scene="B", track=1):
    """compile src and return the steps the scene's pattern for track holds"""
    blob, d = compile_(src)
    check(blob is not None, f"compiles: {d.errors}")
    ir = worldc.decode(blob)
    si = worldc.SCENE_KEYS.index(scene)
    return ir["patterns"][ir["scenes"][si]["pat"][track]]


def notes(st):
    return [s.notes if s.time == worldc.ST_NOTE else ("-" if s.time == worldc.ST_TIE else ".") for s in st["steps"]]


def test_notation():
    w = load("minimal")
    w["key"] = {"root": "D", "scale": "DOR"}
    w["tracks"][1]["register"] = "C2"
    w["patterns"] = {"b": {"track": "bass", "div": "1/4", "steps": "1 b3 #4' 7,, [1 5] - . F#3"}}
    w["scenes"]["B"]["patterns"] = {"bass": "b"}
    st = pattern_steps(w)
    # D dorian from the register C2 (36): tonic D2 = 38; degrees 0 2 3 5 7 9 10
    check(notes(st) == [(38,), (40,), (56,), (24,), (38, 45), "-", ".", (54,)],
          f"degrees, accidentals, octave marks, chords, absolute notes: {notes(st)}")
    w["patterns"]["b"]["steps"] = "c1! c3_ c5~ c7*3 c9 c1, c1' ."
    w["progressions"] = {"p": ["ii7:2", "V7:2", "Imaj7:2", "bVII:2"]}
    for k in "ABCD":
        w["scenes"][k]["progression"] = "p"
    st = pattern_steps(w)
    n = notes(st)
    # 8 steps at 1/4 (one per beat) over 8 beats: no unrolling. ii7 = E min7, V7 = A7, Imaj7 = Dmaj7, and bVII
    # = B: numerals count the World's scale, whose VII is already C in dorian (roots placed a fifth below to a
    # tritone above the tonic D2 = 38: E2 40, A1 33, D2 38, B1 35)
    check(len(st["steps"]) == 8 and n[0] == (40,) and n[1] == (43,) and n[2] == (40,) and n[3] == (43,),
          f"chord tokens c1 c3 (ii7) c5 c7 (V7): {n}")
    check(n[4] == (52,) and n[5] == (26,) and n[6] == (47,), f"c9 over Imaj7 (E3), c1, over Imaj7, c1' over bVII: {n}")
    s = st["steps"]
    check(s[0].flags == worldc.SF_ACCENT and s[1].lvl == worldc.LV_SOFT and s[2].flags == worldc.SF_SLIDE and
          s[3].rat == 2, "suffixes ! _ ~ *3")
    w["patterns"]["b"] = {"track": "bass", "steps": "c1 . . . c1 . . . c1 . . . c1 . . ."}
    w["progressions"] = {"p": ["i:4", "iv:4"]}
    st = pattern_steps(w)
    check(len(st["steps"]) == 32 and notes(st)[0] == (38,) and notes(st)[16] == (43,),
          f"unrolled to lcm(16, 32) = 32 steps: {len(st['steps'])}")
    w["progressions"] = {"p": ["i:4", "i:4"]}
    st = pattern_steps(w)
    check(len(st["steps"]) == 4, f"the shortest period is kept (4 steps, not 32): {len(st['steps'])}")
    w["patterns"]["dr"] = {"track": "drums", "lanes": {"kick": "xXsg|2.3.4.......", "open": "x" * 16}}
    w["patterns"]["dr"]["lanes"]["kick"] = "xXsg2.3.4......."
    w["scenes"]["B"]["patterns"]["drums"] = {"BUSY": "dr"}
    blob, d = compile_(w)
    ir = worldc.decode(blob)
    dr = ir["patterns"][ir["scenes"][1]["beat"][2]]["steps"]
    check(dr[0] == {0: (0, 0), 5: (0, 0)} and dr[1][0] == (3, 0) and dr[2][0] == (2, 0) and dr[3][0] == (1, 0) and
          dr[4][0] == (0, 1) and dr[6][0] == (0, 2) and dr[8][0] == (0, 3), f"drum characters: {dr[:9]}")


def test_errors():
    base = load("full")

    def mod(f):
        s = copy.deepcopy(base)
        f(s)
        return s

    expect_error(mod(lambda s: s["tracks"][0]["sound"].update(preset="WARM PADD")),
                 "$.tracks[0].sound.preset", "is not a preset of ANALOG", what="unknown preset")
    expect_error(mod(lambda s: s["tracks"][0]["sound"]["params"].update(CUTOFF=3)),
                 "$.tracks[0].sound.params.CUTOFF", "EDIT label of ANALOG", what="unknown parameter")
    expect_error(mod(lambda s: s["tracks"][0]["sound"]["params"].update(root=3)),
                 "set by the World itself", what="P_ROOT in a sound")
    expect_error(mod(lambda s: s["tracks"][0]["sound"]["params"].update(CUT=200)),
                 "200 is outside CUT 0..127", what="value out of range")
    expect_error(mod(lambda s: s["scenes"]["C"]["params"].update({"pad.voice": 1})),
                 "$.scenes.C.params", "structural", what="structural parameter in a scene")
    expect_error(mod(lambda s: s["scenes"]["C"]["params"].update({"pad.WAVE": 1})),
                 "structural", what="engine enum in a scene")
    expect_error(mod(lambda s: s["fx"].update(swing=3)), "top-level", what="swing in fx")
    expect_error(mod(lambda s: s["variations"]["AIRY"].update(fx={"swing": 30})), "keeps the groove",
                 what="swing in a variation")
    expect_error(mod(lambda s: s["progressions"].update(main=["i:4", "VI:4", "III:4"])),
                 "$.progressions.main", "4, 8, 16 or 32 beats", what="progression of 12 beats")
    expect_error(mod(lambda s: s["progressions"].update(main=["i:4", "Q:4", "III:4", "VII:4"])),
                 "$.progressions.main[1]", "roman numeral", what="bad roman numeral")
    expect_error(mod(lambda s: s["progressions"].update(main=["imaj7:16"])), "unknown chord suffix",
                 what="minor-major 7th")
    expect_error(mod(lambda s: s["progressions"].update(main=["i:16", "VI:16"])),
                 "repeats only after 128 steps", what="chord pattern over 32 beats at 1/16")
    expect_error(mod(lambda s: s["patterns"]["dr_main"]["lanes"].update(kick="x...y...x...x...")),
                 "$.patterns.dr_main.lanes.kick", "does not match", what="bad drum character (schema)")
    expect_error(mod(lambda s: s["patterns"]["dr_main"]["lanes"].update(snare="....x...")),
                 "8 steps; the other lanes have 16", what="drum lanes of different lengths")
    expect_error(mod(lambda s: s["patterns"]["bass_main"].update(steps="c1 . c1 . | c1 . c5 .")),
                 "a bar is 16 steps", what="bar separators")
    expect_error(mod(lambda s: s["patterns"]["bass_main"].update(steps="c1 . c2 .")),
                 "is not a degree", what="c2 is not a chord token")
    expect_error(mod(lambda s: s["patterns"]["bass_main"].update(steps="8 . . .")),
                 "the scale MIN has 7 notes", what="degree 8 in a 7-note scale")
    expect_error(mod(lambda s: s["patterns"]["bass_main"].update(steps="[1 2 3 4 5]")),
                 "1..4 notes", what="5 notes in a step")
    expect_error(mod(lambda s: s["scenes"].pop("D")), "$.scenes: missing \"D\"", what="missing scene")
    expect_error(mod(lambda s: s.update(colour=1)), "$.colour: unknown key", what="unknown top-level key")
    expect_error(mod(lambda s: s.update(name="Midnight")), "$.name", what="lowercase name")
    expect_error(mod(lambda s: s["smart_keys"].update(track="drums")), "$.smart_keys.track", "a synth track",
                 what="smart keys on the drum track")
    expect_error(mod(lambda s: s["scenes"]["B"]["patterns"].update(keys="pad_main")),
                 "plays the user loop", what="a scene pattern on the keys track")
    expect_error(mod(lambda s: s["scenes"]["B"]["patterns"].update(bass="pad_main")),
                 "belongs to track pad", what="a pattern of another track")
    expect_error(mod(lambda s: s.update(variations={"AIRY": {}, "ORIGINAL": {}})), "the first variation",
                 what="ORIGINAL first")
    expect_error(mod(lambda s: s["macros"]["COLOR"].append({"to": "pad.lwave", "max": 2})), "structural",
                 what="structural macro target")
    expect_error(mod(lambda s: s["macros"]["COLOR"].append({"to": "*.CUT", "max": 2})), "common parameter",
                 what="engine label on *")
    expect_error(mod(lambda s: s["macros"]["COLOR"].append({"to": "g.dtime", "max": 2})), "is not a global",
                 what="structural global target")
    expect_error(mod(lambda s: s["controls"]["SOFT"].append({"to": "keys.atk", "min": -3, "max": 2})),
                 "rests at 0", what="min on a home-0 control")
    expect_error(mod(lambda s: s["energy"]["e"]["bands"][0].update(layers=["pad"])),
                 "the Smart Keys track is in every band", what="keys muted by ENERGY")
    expect_error(mod(lambda s: s["energy"]["e"]["bands"][1].update({"from": 0})), "ascend", what="band order")
    expect_error(mod(lambda s: s["smart_keys"].update(melody_scale="PEN")), "inside the World scale",
                 what="melody scale outside the World scale")
    expect_error(mod(lambda s: s["tempo"].update(min=60)), "wider than bpm", what="tempo range")
    expect_error(mod(lambda s: s["guard"]["notes"]["bass"].update(max_poly=2)), "Smart Keys track only",
                 what="max_poly on another track")

    def shared_swap(s):                  # a drone without chord tokens in A (dark) and B (main), swapped to a
        s["patterns"]["drone"] = {"track": "bass", "div": "1/4", "steps": "1 - - -"}   # chord-token line
        s["scenes"]["A"]["patterns"]["bass"] = s["scenes"]["B"]["patterns"]["bass"] = "drone"
        s["variations"]["PULSING"]["swap"] = {"drone": "bass_main"}
    expect_error(mod(shared_swap), "$.variations.PULSING.swap.drone", "one pattern in scenes over different "
                 "progressions", what="a shared pattern swapped to one that differs per progression (the firmware "
                 "takes the first match)")
    s = mod(shared_swap)
    s["scenes"]["A"]["progression"] = "main"
    blob, d = compile_(s)
    check(blob is not None and len(worldc.decode(blob)["vars"][3]["swaps"]) == 1,
          f"the same swap in two scenes over one progression: one pair: {d.errors}")
    blob, d = compile_(mod(lambda s: s["macros"]["COLOR"].append({"to": "pad.@RESO", "max": 2})))
    blob, d = compile_(mod(lambda s: s["macros"]["COLOR"].append({"to": "keys.@MOVE", "max": 2})))
    check(blob is not None and any("TRIO (track keys) has no MOVE" in w for w in d.warnings),
          f"a role the engine lacks warns: {d.warnings}")


def big_world(npat, steps=64):
    w = load("minimal")
    w["patterns"] = {}
    names = []
    for i in range(npat):
        toks = []
        for k in range(steps):
            a = (k * 7 + i * 13) % 48 + 30
            toks.append(f"[{worldc.note_name(a)} {worldc.note_name(a + 3 + i % 5)} {worldc.note_name(a + 7)} "
                        f"{worldc.note_name(a + 12 + k % 3)}]")
        w["patterns"][f"p{i}"] = {"track": "pad", "steps": " ".join(toks)}
        names.append(f"p{i}")
    w["variations"] = {"ORIGINAL": {}}
    for j in range(1, 8):
        if names[1:]:
            w["variations"][f"V{j}"] = {"swap": {"p0": names[(j % (len(names) - 1)) + 1]}} if len(names) > 1 else {}
    w["scenes"]["B"]["patterns"] = {"pad": "p0"}
    return w


def test_limits():
    blob, d = compile_(big_world(5))
    check(blob is not None and len(blob) > worldc.F["WF_SOFT_LEN"] and
          any("above the 2048 B guideline" in w for w in d.warnings), f"a {len(blob or b'')} B World warns")
    blob, d = compile_(big_world(8))
    check(blob is None and any("holds at most 3072 B" in e for e in d.errors), "a factory World above 3072 B fails")
    blob, d = compile_(big_world(8), user=True)
    check(blob is None or len(blob) <= worldc.F["WF_MAX_LEN"], "a user World up to 3840 B")
    w = load("minimal")
    w["patterns"] = {f"p{i}": {"track": "pad", "steps": f"{1 + i % 7} . . ."} for i in range(17)}
    w["variations"] = {"ORIGINAL": {}}
    for j in range(1, 8):
        w["variations"][f"V{j}"] = {"swap": {"p0": f"p{j}"}}
    w["scenes"]["B"]["patterns"] = {"pad": "p0"}
    w["scenes"]["C"]["patterns"] = {"pad": "p8"}
    w["scenes"]["D"]["patterns"] = {"pad": "p9"}
    w["scenes"]["A"]["patterns"] = {"pad": "p10"}
    w["scenes"]["A"]["params"] = {}
    blob, d = compile_(w)
    check(blob is not None, f"12 patterns fit: {d.errors}")


def test_decoder():
    blob, _ = compile_(load("full"))
    F = worldc.F
    cases = [(lambda b: b.__setitem__(0, ord("X")), F["WE_MAGIC"]), (lambda b: b.__setitem__(4, 2), F["WE_VERSION"]),
             (lambda b: b.__setitem__(20, 0x0F), F["WE_SECTION"]),
             (lambda b: b.__setitem__(100, b[100] ^ 1), F["WE_CRC"])]
    for f, code in cases:
        b = bytearray(blob)
        f(b)
        if code != F["WE_CRC"]:
            L = b[6] | b[7] << 8
            c = worldc.blob_crc(bytes(b), L)
            b[12:16] = c.to_bytes(4, "little")
        try:
            worldc.decode(bytes(b))
            check(False, f"decode accepts a corruption (expected {code})")
        except worldc.BlobError as e:
            check(e.code == code, f"decode error {e.code}, expected {code}")


def test_gen_worlds():
    with tempfile.TemporaryDirectory() as td:
        empty = Path(td) / "empty"
        empty.mkdir()
        text = gen_worlds.header(gen_worlds.build(empty))
        check("#define WORLD_NFACTORY 0" in text and "WORLD_DATA[1]" in text, "a valid header with no World")
        two = Path(td) / "two"
        two.mkdir()
        for n in ("full", "minimal"):
            (two / f"{n}.world.json").write_text(json.dumps(load(n)))
        with contextlib.redirect_stderr(io.StringIO()):          # (full.world.json warns on purpose)
            ws = gen_worlds.build(two)
        check([w["cat"] for w in ws] == ["SYNTHWAVE", "TEST"], "sorted by category")
        text = gen_worlds.header(ws)
        offs = [int(x.split(",")[1]) for x in text.split("WORLD_INDEX")[1].splitlines()[1:3]]
        check(all(o % 4 == 0 for o in offs), f"blobs 4-byte aligned: {offs}")
        dup = Path(td) / "dup"
        dup.mkdir()
        (dup / "a.world.json").write_text(json.dumps(load("minimal")))
        (dup / "b.world.json").write_text(json.dumps(load("minimal")))
        try:
            gen_worlds.build(dup)
            check(False, "a duplicate id fails the build")
        except SystemExit as e:
            check("the same device id" in str(e), "duplicate id message")


def model_errors(src):
    blob, d = compile_(src)
    if blob is None:
        return None, "\n".join(d.errors)
    worldc.model_check(blob, d, d.map_src)
    return blob, "\n".join(d.errors)


def test_guard():
    w = load("guard")
    blob, d = compile_(w)
    ir = worldc.decode(blob)
    check(len(ir["guard"]["combos"]) == 4 and ir["guard"]["fix"][worldc.F["WF_G_COMBOS"]] == 4, "four combinations")
    w2 = copy.deepcopy(w)
    w2["guard"]["sound"]["combos"] = []
    blob2, _ = compile_(w2)
    check(worldc.decode(blob2)["guard"]["fix"][worldc.F["WF_G_COMBOS"]] == 0, "combos [] = none (0)")
    del w2["guard"]["sound"]["combos"]
    blob2, _ = compile_(w2)
    check(worldc.decode(blob2)["guard"]["fix"][worldc.F["WF_G_COMBOS"]] == worldc.NONE, "no combos: the firmware's")
    w2 = copy.deepcopy(w)
    w2["guard"]["sound"]["combos"] = [{"when": {"pad.~bright": 3}, "cap": {"g.dfdbk": 1}}]
    expect_error(w2, '$.guard.sound.combos[0].when["pad.~bright"]', "parameters, @roles or globals",
                 what="vmod in a combo")
    w2["guard"]["sound"]["combos"] = [{"when": {"g.dfdbk": 300}, "cap": {"g.dfdbk": 1}}]
    expect_error(w2, '$.guard.sound.combos[0].when["g.dfdbk"]', "above 127", what="a combo value out of i8")
    w2["guard"]["sound"]["combos"] = [{"when": {"g.dfdbk": 3}}] * 2
    expect_error(w2, "$.guard.sound.combos[0]", what="a combo without cap")
    w2["guard"]["sound"]["combos"] = [{"when": {"g.dfdbk": 3}, "cap": {"g.dfdbk": 1}}] * 9
    expect_error(w2, "$.guard.sound.combos", "more than 8", what="nine combos")
    check(len(worldc.COMBO_DEF) == 6 and all(len(c) == 3 for c in worldc.COMBO_DEF), "WF_COMBO_DEFAULT: six records")
    check(worldc.GL["GL_DFDBK_MAX"] == 120 and worldc.GL["GL_RSIZE_MAX"] == 127, "guard_limits.h parsed")


def test_model():
    for f in sorted((ROOT / "worlds" / "factory").glob("*.world.json")):
        blob, err = model_errors(json.loads(f.read_text()))
        check(blob is not None and not err, f"{f.name}: worldc check passes: {err[:300]}")
    blob, err = model_errors(load("extreme"))
    check(blob is None or err, "extreme.world.json is refused")
    for needle in ("outside", "past the hard limit", "100 % is never all-max"):
        check(needle in err, f"extreme: an error with {needle!r}")
    w = load("minimal")
    w["macros"] = {"SPACE": [{"to": "g.dfdbk", "min": -10, "max": 100}]}
    _, err = model_errors(w)
    check("past the hard limit 120" in err and "$.macros.SPACE[0]" in err, f"dfdbk +100 refused: {err}")
    w["macros"] = {"COLOR": [{"to": "pad.CUT", "min": -10, "max": 127}]}
    _, err = model_errors(w)
    check("all-max" in err, f"CUT to its maximum refused: {err}")
    w["macros"]["COLOR"][0]["saturate"] = True
    _, err = model_errors(w)
    check("all-max" not in err, f"... unless saturate: {err}")
    w = load("minimal")
    w["smart_keys"]["range"] = ["C4", "F4"]
    w["smart_keys"]["tonic"] = "C4"
    _, err = model_errors(w)
    check("under an octave" in err, f"a keys range under an octave refused: {err}")
    # more than 48 targets moving at once is refused (17 parameters on the three synth tracks: 51 slots)
    w = load("minimal")
    w["macros"] = {"COLOR": [{"to": f"*.{pn}", "min": -1, "max": 1} for pn in
                             ("atk", "dec", "sus", "rel", "ed_flt", "lrate", "ld_flt", "ld_amp", "sgate", "chor", "dly",
                              "rev", "pan", "glide", "ld_pit", "ld_shp", "level")]}
    blob, err = model_errors(w)
    check("at most 48" in err, f"51 slots refused: {err[:300]}")


def main():
    for t in (test_params_fresh, test_schema_enums, test_roundtrip, test_transitions, test_notation, test_errors,
              test_limits, test_decoder, test_gen_worlds, test_guard, test_model):
        n = len(FAILS)
        t()
        print(f"{t.__name__[5:]:<14} {'ok' if len(FAILS) == n else 'FAIL'}")
    print("worldc: all checks passed" if not FAILS else f"worldc: {len(FAILS)} check(s) failed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
