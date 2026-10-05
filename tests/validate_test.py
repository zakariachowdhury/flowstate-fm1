#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Checks of tools/validate-world (Phase 16; run by tests/run_tests.sh, from the repo root):
  - every factory World validates clean in quick mode, through the command line (the real host sweep of
    tests/guard_sweep.c): all 17 items, no failure, the sweep's numbers in the JSON report;
  - every Worlds in worlds/test/bad is broken in exactly one way: it fails exactly the item named in EXPECT below
    (and no other), the compiler's and the model's findings reaching the right item; the one fixture that only a
    render can catch (feedback_runaway) is swept for real; every fixture is in the table and the other way round;
  - the JSON report's schema is stable (keys, item order, status words) and the exit status follows the failures;
  - the sweep's output is read right: each failure keyword of guard_sweep's judge_pt (clipping, garbage, DC,
    limiter, feedback, silence, parameters, voices, CPU, a crashed render) and its volume jumps reach the item the
    checklist names, from a made-up output, so the detectors that no fixture trips are covered too;
  - ids, directories and --user; unreadable and non-JSON files fail on metadata instead of crashing.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import validate_world as vw  # noqa: E402

FAILS = []
BAD = ROOT / "worlds" / "test" / "bad"
FACTORY = ["neon_rain", "midnight_drive", "frozen_lake", "dusty_cafe"]
ITEMS = ["metadata", "presets", "engines", "patterns", "scenes", "variation", "scale", "harmony", "Smart Keys",
         "macro ranges", "Guardrails", "CPU budget", "RAM budget", "flash budget", "no clipping",
         "no invalid feedback", "no invalid parameter IDs"]
# fixture -> the one item it must fail
EXPECT = {
    "name_charset": "metadata", "unknown_preset": "presets", "unknown_engine": "engines",
    "pattern_bad_step": "patterns", "missing_scene": "scenes", "variation_sets_tempo": "variation",
    "melody_scale_off_key": "scale", "progression_beats": "harmony", "keys_range_octave": "Smart Keys",
    "macro_saturates": "macro ranges", "macro_out_of_range": "macro ranges",
    "feedback_past_hard_limit": "Guardrails", "level_past_hard_limit": "Guardrails",
    "guard_range_inverted": "Guardrails", "unknown_parameter": "no invalid parameter IDs",
    "pattern_pool_overflow": "RAM budget", "too_many_targets": "RAM budget", "blob_too_large": "flash budget",
    "feedback_runaway": "no invalid feedback"}
RENDER_ONLY = {"feedback_runaway"}                  # passes every static item: only the sweep fails it


def check(cond, what):
    if not cond:
        FAILS.append(what)
        print(f"FAIL {what}")


def cli(*args, check_rc=None):
    r = subprocess.run([str(ROOT / "tools" / "validate-world"), *args], cwd=ROOT, capture_output=True, text=True)
    if check_rc is not None:
        check(r.returncode == check_rc, f"validate-world {' '.join(args)}: exit {r.returncode}, want {check_rc}: "
                                        f"{(r.stdout + r.stderr)[-300:]}")
    return r


def ns(**kw):
    d = dict(user=False, static=False, full=False, json=False, jobs=0, seed=None)
    d.update(kw)
    return argparse.Namespace(**d)


def failed(w):
    return [i for i in ITEMS if w.res[i].status == "fail"]


# ------------------------------------------------------------------------------------ factory ---
def test_factory():
    r = cli("--json", *FACTORY, check_rc=0)
    rep = json.loads(r.stdout)
    check(rep["schema"] == "validate-world/1" and rep["mode"] == "quick" and rep["ok"], "factory: report header")
    check([w["id"] for w in rep["worlds"]] == FACTORY, "factory: four Worlds in the order given")
    for w in rep["worlds"]:
        st = {i["item"]: i["status"] for i in w["items"]}
        check(list(st) == ITEMS, f"{w['id']}: the 17 items in the checklist's order")
        check(all(s in ("pass", "warn") for s in st.values()), f"{w['id']}: clean: {st}")
        sw = w["sweep"]
        check(sw and sw["mode"] == "quick" and sw["points"] >= 300 and sw["failed_points"] == 0,
              f"{w['id']}: swept {sw and sw['points']} points, none failed")
        check(sw and sw["worst_peak_dbfs"] <= -0.3 and sw["worst_tail_dbfs"] <= -60 and sw["values_checked"] > 1e6,
              f"{w['id']}: the sweep's numbers inside the limits")
        check(0 < sw["cpu_mean_max"] <= 2300 and sw["cpu_half_max"] <= 2700, f"{w['id']}: CPU {sw['cpu_mean_max']}")
        it = {i["item"]: i for i in w["items"]}
        check(it["flash budget"]["data"]["bytes"] == w["bytes"] <= 3072, f"{w['id']}: flash budget data")
        check(it["RAM budget"]["data"]["pool_patterns"] <= 16 and it["RAM budget"]["data"]["slots"] <= 48,
              f"{w['id']}: RAM budget data")
        check("factory_set" in it["flash budget"]["data"], f"{w['id']}: the factory set is in the flash budget")
    print("factory: " + ", ".join(f"{w['id']} {w['sweep']['points']} renders, CPU {w['sweep']['cpu_mean_max']:.0f}/"
                                  f"{w['sweep']['cpu_half_max']:.0f}, tail {w['sweep']['worst_tail_dbfs']} dBFS"
                                  for w in rep["worlds"]))


# -------------------------------------------------------------------------------------- fixtures ---
def test_fixtures():
    files = sorted(p.name[:-len(".world.json")] for p in BAD.glob("*.world.json"))
    check(files == sorted(EXPECT), f"fixtures and EXPECT differ: {set(files) ^ set(EXPECT)}")
    for name in files:
        if name not in EXPECT:
            continue
        w = vw.validate(BAD / f"{name}.world.json", ns(static=name not in RENDER_ONLY), None)
        got = failed(w)
        check(got == [EXPECT[name]], f"{name}: fails {got}, want only {EXPECT[name]}: "
                                     f"{[(i, w.res[i].summary()) for i in got]}")
        if name in RENDER_ONLY:
            check(w.sweep is not None, f"{name}: swept")
        if got:
            # the render items are not run on a World that already failed (and say so)
            if name not in RENDER_ONLY:
                swept = [i for i in vw.SWEEP_ITEMS if w.res[i].status in ("pass", "warn")]
                check(not swept, f"{name}: render items ran on a failed World: {swept}")
        txt = vw.render(w, False, False)
        check("FAILED: " + EXPECT[name] in txt, f"{name}: render names the failure")
    # a World cut off at the schema or the tracks does not claim items it never reached
    w = vw.validate(BAD / "unknown_engine.world.json", ns(static=True), None)
    check(w.res["scenes"].status == "skip" and w.res["metadata"].status == "pass",
          f"unknown_engine: later items skip, metadata passes: {[(i, w.res[i].status) for i in ITEMS]}")
    # one mapping past a hard limit is one finding (Guardrails), not also a range and a saturation error
    w = vw.validate(BAD / "feedback_past_hard_limit.world.json", ns(static=True), None)
    check(len(w.res["Guardrails"].errors) == 1 and not w.res["macro ranges"].errors,
          f"the hard limit is reported once: {w.res['Guardrails'].errors} {w.res['macro ranges'].errors}")
    print(f"fixtures: {len(EXPECT)} broken Worlds, each fails exactly its item")


# --------------------------------------------------------------------------------------- the CLI ---
def test_cli():
    r = cli("--static", str(BAD / "unknown_preset.world.json"), check_rc=1)
    check("✗ presets" in r.stdout and "FAILED: presets" in r.stdout and "\033[" not in r.stdout,
          f"human report: a ✗ line, no colour off a TTY: {r.stdout[:300]}")
    r = cli("--static", "minimal", check_rc=0)
    check(r.stdout.startswith("test_minimal  MINIMAL  263 B") and "✓ metadata" in r.stdout and
          "–" in r.stdout and "not run: --static" in r.stdout, f"a clean static run: {r.stdout[:200]}")
    r = cli("--static", "neon-rain", check_rc=0)                    # an id, with a dash
    check(r.stdout.startswith("neon_rain  NEON RAIN"), "factory ids: neon-rain")
    cli("--static", "no-such-world", check_rc=2)
    r = cli("--static", "--json", str(BAD), check_rc=1)             # a directory
    rep = json.loads(r.stdout)
    check(len(rep["worlds"]) == len(EXPECT) and not rep["ok"], "a directory validates every World in it")
    check([w["id"] for w in rep["worlds"] if w["ok"]] == ["bad_feedback_runaway"],
          "statically, only the one that needs a render passes")
    # the JSON schema
    check(sorted(rep) == ["mode", "ok", "schema", "seconds", "worlds"] and rep["mode"] == "static", "report keys")
    w = rep["worlds"][0]
    check(sorted(w) == ["bytes", "file", "id", "items", "kind", "name", "ok", "sweep"], f"world keys {sorted(w)}")
    check(all(sorted(i) == ["data", "errors", "info", "item", "status", "summary", "warnings"] for i in w["items"]),
          "item keys")
    check([i["item"] for i in w["items"]] == ITEMS, "item order")
    check({i["status"] for x in rep["worlds"] for i in x["items"]} <= {"pass", "fail", "warn", "skip"},
          "status words")
    check(all(i["status"] == "skip" or i["item"] not in vw.SWEEP_ITEMS or i["errors"]
              for x in rep["worlds"] for i in x["items"]), "--static runs no sweep item")
    # --user: the limit of a user World
    r = cli("--static", "--user", "--json", "minimal", check_rc=0)
    fl = [i for i in json.loads(r.stdout)["worlds"][0]["items"] if i["item"] == "flash budget"][0]
    check(fl["data"]["kind"] == "user" and fl["data"]["limit"] == vw.F["WF_MAX_LEN"], f"--user: {fl['data']}")
    # files that are not Worlds fail on metadata
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        for name, text in (("empty.world.json", ""), ("array.world.json", "[1, 2]"), ("junk.world.json", "{nope"),
                           ("extra_key.world.json", None)):
            if text is None:
                src = json.loads((ROOT / "worlds/test/minimal.world.json").read_text())
                src["colour"] = "red"
                text = json.dumps(src)
            (td / name).write_text(text)
            w = vw.validate(td / name, ns(static=True), None)
            check(failed(w) == ["metadata"], f"{name}: fails on metadata only: {failed(w)}")
        r = cli("--static", str(td / "junk.world.json"), check_rc=1)
        check("✗ metadata" in r.stdout, "a non-JSON file is a ✗ metadata line")


# ----------------------------------------------------------------------------- the sweep's output ---
def pt(scene="B", var=0, macros=(500, 500, 500, 500), peak=-6.0, tail="-80.0", lufs=-18.0, cpu="1500/1900", why=""):
    return (f"  x.wblob {scene}/{var} {macros[0]:4} {macros[1]:4} {macros[2]:4} {macros[3]:4}: peak {peak:6.2f} "
            f"tail {tail:>6} LUFS {lufs:6.1f} rms  -20.0 slew 0.01 lim 1.00 cpu {cpu.split('/')[0]:>5}/"
            f"{cpu.split('/')[1]:<5} est 1600 vmax 5 lay 5{why}")


def sweep_text(points, jumps=(), fails=0):
    return "\n".join(["guard_sweep: quick, %d points over 1 World(s), 2 bars + 6 s tail each, 10 jobs" % len(points),
                      "  World                    points     peak     tail  loudness (LUFS) jump/alone CPU mean/half "
                      "estimate    values fails", *points, *jumps,
                      "  x.wblob                    %d    -6.00    -80.0   -18.0..-18.0     1.0/0.0    1500/1900   "
                      "    1600  12345678     %d" % (len(points), fails),
                      "guard_sweep: %d points, %s" % (len(points), "FAILED" if fails else "all clean")])


def judged(points, jumps=()):
    w = vw.World(ROOT / "worlds/test/minimal.world.json")
    w.compile()
    nfail = sum(1 for p in points if p.split(" lay 5")[1].strip()) + len(jumps)
    ps = vw.parse_sweep(sweep_text(points, jumps, fails=nfail))
    ps["mode"], ps["exit"] = "quick", 1
    vw.settle_sweep(w, [ps], ["ORIGINAL", "DREAMY"])
    return w


def test_sweep_output():
    clean = judged([pt(), pt(macros=(0, 0, 0, 0))])
    check(all(clean.res[i].status == "pass" for i in vw.SWEEP_ITEMS + ("Guardrails",)), "a clean sweep passes")
    check(clean.sweep["points"] == 2 and clean.sweep["cpu_mean_max"] == 1500 and clean.sweep["values_checked"] == 12345678,
          f"the clean sweep's numbers: {clean.sweep}")
    cases = [  # (guard_sweep's failure text, the item it must fail, a word the message must carry)
        (" clip(peak 0.00 dBFS, 3 at full scale)", "no clipping", "clip"),
        (" garbage(a jump of 0.95 of full scale)", "no clipping", "garbage"),
        (" dc 0.0030/0.0000", "no clipping", "dc"),
        (" limiter input 5.2 x LIM_T", "no clipping", "limiter"),
        (" feedback(tail -41.0 dBFS, 0 voices)", "no invalid feedback", "tail -41.0"),
        (" 12 invalid parameter values", "no invalid parameter IDs", "12 invalid"),
        (" voices 9", "CPU budget", "voices 9"),
        (" cpu(mean 2500, half 2900)", "CPU budget", "mean 2500"),
        (" cpu estimate 1000 under 1400", "CPU budget", "estimate"),
        (" silent(rms -52.0, 3.1 s)", "Guardrails", "silent"),
        (" crashed", "Guardrails", "crashed")]
    for why, item, word in cases:
        w = judged([pt(), pt(macros=(1000, 1000, 1000, 0), scene="D", var=1, why=why)])
        bad = [i for i in vw.SWEEP_ITEMS + ("Guardrails",) if w.res[i].status == "fail"]
        check(bad == [item], f"'{why.strip()}' fails {bad}, want only {item}")
        msg = w.res[item].errors[0] if w.res[item].errors else ""
        check(word in msg and "scene D, DREAMY, macros 100/100/100/0 %" in msg, f"the message names the render: {msg}")
    # two keywords in one render reach two items; the worst case is listed first
    w = judged([pt(why=" feedback(tail -50.0 dBFS, 0 voices)"), pt(macros=(0, 0, 0, 0), why=" feedback(tail -30.0 dBFS, "
               "0 voices) clip(peak -0.10 dBFS, 0 at full scale)")])
    check([i for i in vw.SWEEP_ITEMS if w.res[i].status == "fail"] == ["no clipping", "no invalid feedback"],
          "one render, two items")
    check("tail -30.0" in w.res["no invalid feedback"].errors[0] and len(w.res["no invalid feedback"].errors) == 2,
          "the worst tail first")
    # loudness jumps
    jump = "  JUMP x.wblob C/1 (no player): 500 500 500 0 -> 500 500 500 500: 13.5 LU"
    w = judged([pt()], [jump])
    check(w.res["Guardrails"].status == "fail" and "13.5 LU" in w.res["Guardrails"].errors[0] and
          "no player" in w.res["Guardrails"].errors[0] and w.res["CPU budget"].status == "pass", "a volume jump")
    # a sweep that did not finish fails every render item
    w = vw.World(ROOT / "worlds/test/minimal.world.json")
    w.compile()
    ps = vw.parse_sweep("guard_sweep: world_load failed")
    ps["mode"], ps["exit"] = "quick", 1
    vw.settle_sweep(w, [ps], ["ORIGINAL"])
    check(all(w.res[i].status == "fail" and "did not complete" in w.res[i].errors[0] for i in vw.SWEEP_ITEMS),
          "an unfinished sweep fails the render items")
    # no instruction counter (not macOS): CPU is a warning, not a pass
    w = judged([pt(cpu="0/0")])
    check(w.res["CPU budget"].status == "warn" and w.sweep["cpu_mean_max"] == 0, "no CPU counter: a warning")
    print("sweep output: every keyword of guard_sweep reaches its checklist item")


def test_classify():
    cases = [("$.name: bad", "metadata"), ("$.tempo: min..max too wide", "metadata"),
             ('$: missing "scenes"', "scenes"), ("$.tracks[0].sound.preset: nope", "presets"),
             ("$.tracks[0].sound.params.level: 130 is outside LVL 0..127", "presets"),
             ('$.tracks[1].sound.engine: "X" is not an engine', "engines"),
             ('$.tracks[2].name: "g": track names are unique', "engines"),
             ("$.patterns.a.steps: step 3", "patterns"), ("$.progressions.p: 5 beats", "harmony"),
             ('$.scenes.A.patterns.pad: "x" is not a pattern', "scenes"), ("$.energy.e.bands[0].from: x", "scenes"),
             ('$.variations["DEEP SEA"].swap: x', "variation"), ("$.key.scale: not a scale", "scale"),
             ("$.smart_keys.melody_scale: inside the World scale", "scale"), ("$.smart_keys.range: x", "Smart Keys"),
             ("$.energy.e.bands[1].layers: the Smart Keys track is in every band", "Smart Keys"),
             ("$.macros.COLOR[0]: scene A / ORIGINAL: g.dfdbk: base 60 +70 = 130: outside 0..120", "macro ranges"),
             ("$.macros.COLOR[0]: scene A / ORIGINAL: g.dfdbk: base 60 +70 = 130: past the hard limit 120",
              "Guardrails"),
             ("$.controls.SOFT[0].to: x", "macro ranges"), ("$.rules[0].if: x", "macro ranges"),
             ("$.guard.sound.ranges: at most 32", "Guardrails"),
             ('$.macros.COLOR[0].to: "NOPE" is neither a common parameter (x) nor an EDIT label of ANALOG',
              "no invalid parameter IDs"),
             ('$.macros.SPACE[1].to: "x" is not a global a macro may move', "no invalid parameter IDs"),
             ('$.tracks[0].sound.params.mode: "mode" is structural: only the track', "no invalid parameter IDs"),
             ("$.macros: 57 targets moving at once (scene A)", "RAM budget"),
             ("$.patterns: 17 patterns after unrolling chord tokens", "RAM budget"),
             ("$.defaults.variation: x is not a variation", "variation"), ("$.defaults.scene: A, B, C or D", "scenes"),
             ("$.defaults.macros: four positions", "macro ranges"), ("$.surprise: unknown key", "metadata")]
    for text, item in cases:
        check(vw.classify(text) == item, f"classify({text!r}) = {vw.classify(text)}, want {item}")
    check(vw.resolve("neon_rain") == vw.resolve("NEON-RAIN") == [ROOT / "worlds/factory/neon_rain.world.json"],
          "factory ids resolve")
    check(vw.resolve("minimal") == [ROOT / "worlds/test/minimal.world.json"] and vw.resolve("nothing_here") is None,
          "test ids resolve; an unknown id does not")
    print("classify: worldc's messages reach the checklist's items")


def test_budgets():
    fs = vw.factory_set_budget(False)
    if "skipped" in fs:
        print(f"budgets: factory set not checked ({fs['skipped']})")
        return
    check(fs["ok"] and fs["bytes"] <= fs["budget"] == fs["room"] - vw.CODE_RESERVE,
          f"the factory set is inside the app-slot room the code leaves, less its reserve: {fs}")
    check(vw.world_flash(1365) == 1376 and vw.world_flash(1316) == 1324, "a blob in the image: 4-byte aligned + index")
    print(f"budgets: factory set {fs['bytes']:,} B of {fs['budget']:,} B (the {fs['room']:,} B the code leaves, "
          f"less {vw.CODE_RESERVE:,} B for the code; {fs['free_now']:,} B free in the slot now)")


def main():
    for t in (test_classify, test_sweep_output, test_cli, test_fixtures, test_budgets, test_factory):
        n = len(FAILS)
        t()
        print(f"{t.__name__[5:]:<14} {'ok' if len(FAILS) == n else 'FAIL'}")
    print("validate-world: all checks passed" if not FAILS else f"validate-world: {len(FAILS)} check(s) failed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
