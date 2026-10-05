#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Injected bugs in the Musical Guardrail Engine (Phase 8): each mutant below breaks one rule of firmware/src/guard.c,
the H6 read-site clamps (fx.c, voice.c), the CPU guard's hooks (macro.c, voice.c) or loop follow (seq.c), in a copy
of the sources; tests/guard_test.c built on the copy must fail, or else the C engine's slot tables must differ from
tools/worldc.py's model (tests/guard_sweep.c --dump-slots on worlds/test/guard.world.json, the factory Worlds).
Every mutant has to be caught. Run from the repo root after ./build.sh or tools/build.py --gen-only (tests/run_tests.sh
runs it with SWEEP=full):

  python3 tests/guard_mutants.py [--jobs N] [--only NAME]
"""
import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# name, file, the text, its mutation (each text must occur exactly once in its file)
MUTANTS = [
    ("level cap ignored", "guard.c", "cap = (int32_t)guard_byte(gu.fix, WF_G_MAXLEVEL);", "cap = 0x7FFF;"),
    ("reso cap ignored", "guard.c", "cap = (int32_t)guard_byte(gu.fix, WF_G_MAXRESO);", "cap = 0x7FFF;"),
    ("grain DENS cap ignored", "guard.c", "cap = (int32_t)guard_byte(gu.fix, WF_G_GRAINDENS);", "cap = 0x7FFF;"),
    ("target ranges skipped", "guard.c", "for (i = 0; i < gu.nranges; i++, r += WF_GRANGE_LEN)",
     "for (i = 0; i < 0u; i++, r += WF_GRANGE_LEN)"),
    ("range top not applied", "guard.c", "if ((int8_t)r[3] < h)", "if ((int8_t)r[3] > h)"),
    ("combination ramp twice as steep", "guard.c", "return (v - over) * 4096 / WF_COMBO_RAMP;",
     "return (v - over) * 8192 / WF_COMBO_RAMP;"),
    ("combination b's threshold for a", "guard.c", "st = gu_strength(gu_value(nt, c[0], c[1], t), (int8_t)c[2],",
     "st = gu_strength(gu_value(nt, c[0], c[1], t), (int8_t)c[5],"),
    ("combination moves the base", "guard.c", "if (v > cap && v > b)", "if (v > cap)"),
    ("combination ramp skipped", "guard.c", "if (self || v - over >= WF_COMBO_RAMP)", "if (1)"),
    ("combinations per track: first only", "guard.c", "if ((c[6] & WF_TMASK) >> t & 1u)",
     "if ((c[6] & WF_TMASK) >> t & 1u && !t)"),
    ("one distorted track too many", "guard.c", "if (n < maxd)", "if (n <= maxd)"),
    ("vmod range unclamped", "guard.c", "s->tgt = clamp(s->tgt, s->lo * 256, s->hi * 256);", "(void)0;"),
    ("CPU hold at once", "guard.c", "if (gcpu.over >= GL_CPU_HOLD)", "if (gcpu.over >= 1u)"),
    ("CPU hold never let go", "guard.c", "                wg.hold = 0;\n                gcpu.under = 0;",
     "                gcpu.under = 0;"),
    ("CPU estimate ignores voices", "guard.c", "n += v * c;", "n += 0u * v * c;"),
    ("costly slots not held", "macro.c", "if (wg.hold && tg > 0 && guard_costly(s))", "if (0)"),
    ("UNISON uncapped", "voice.c", "if (u && c > u)", "if (0)"),
    ("loop follow off", "guard.c", "return (harm.safe >> (n % 12u) & 1u) ? n : guard_snap(n, harm.ct);", "return n;"),
    ("snap ties upward", "guard.c", "        if (n >= d && (mask >> ((n - d) % 12u) & 1u))\n            return n - d;\n"
     "        if (n + d <= 127u && (mask >> ((n + d) % 12u) & 1u))\n            return n + d;",
     "        if (n + d <= 127u && (mask >> ((n + d) % 12u) & 1u))\n            return n + d;\n"
     "        if (n >= d && (mask >> ((n - d) % 12u) & 1u))\n            return n - d;"),
    ("range not widened", "guard.c", "if (n->hi < n->lo + 11u)", "if (0)"),
    ("polyphony one over", "guard.c", "return n < poly;", "return n <= poly;"),
    ("record: a note too many", "guard.c", "return s->time != ST_NOTE || s->n < (mx < 4u ? mx : 4u);",
     "return s->time != ST_NOTE || s->n <= (mx < 4u ? mx : 4u);"),
    ("H6: delay feedback unclamped", "fx.c", "clamp(song.g[G_DFDBK], 0, GL_DFDBK_MAX) * 230", "song.g[G_DFDBK] * 230"),
    ("H6: reverb size unclamped", "fx.c", "clamp(song.g[G_RSIZE], 0, GL_RSIZE_MAX) * 50", "song.g[G_RSIZE] * 50"),
    ("H6: bus values unclamped", "fx.c", "static int32_t gbus(uint32_t i) { return clamp(song.g[i], GP[i].min, GP[i].max); }",
     "static int32_t gbus(uint32_t i) { return song.g[i]; }"),
    ("H6: DIST unclamped", "fx.c", "int32_t d = clamp(t->p[P_DIST], 0, TP[P_DIST].max)", "int32_t d = t->p[P_DIST]"),
    ("H6: EDIT values unclamped", "voice.c", "        if (x < lo || x > hi) {\n            pe_keep[i] = (int16_t)x;",
     "        if (0) {\n            pe_keep[i] = (int16_t)x;"),
    ("H6: EDIT values not restored", "voice.c", "    for (i = 0; pfix; i++, pfix >>= 1)", "    for (i = 0; pfix && 0; i++, pfix >>= 1)"),
]
FILES = ["firmware/src", "firmware/hal", "tests"]


def sh(cmd, cwd, timeout=600):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)


def blobs(tmp):
    out = []
    for w in ("minimal", "full", "extreme", "guard"):
        b = tmp / f"gt-{w}.wblob"
        r = sh([sys.executable, "tools/worldc.py", "compile", f"worlds/test/{w}.world.json", "-o", str(b)], ROOT)
        if r.returncode:
            raise SystemExit(r.stderr)
        out.append(str(b))
    for f in sorted((ROOT / "worlds" / "factory").glob("*.world.json")):
        b = tmp / f"gt-{f.stem}.wblob"
        sh([sys.executable, "tools/worldc.py", "compile", str(f), "-o", str(b)], ROOT)
    return out


def model_dumps(tmp):
    """the worldc model's slot tables of the guard World and the factory Worlds (once: Python is not mutated)"""
    out = {}
    for b in sorted(tmp.glob("gt-*.wblob")):
        if b.stem in ("gt-minimal", "gt-full", "gt-extreme"):
            continue
        out[b] = sh([sys.executable, "tools/worldc.py", "model", str(b)], ROOT).stdout
    return out


def run_mutant(m, tmp, wbs, dumps):
    name, fn, a, b = m
    d = Path(tempfile.mkdtemp(dir=tmp))
    for f in FILES:
        shutil.copytree(ROOT / f, d / f)
    p = d / "firmware" / "src" / fn
    s = p.read_text()
    if s.count(a) != 1:
        return name, "BROKEN (the text is not there once)"
    p.write_text(s.replace(a, b))
    gen = str(ROOT / "build" / "gen")
    exe = d / "guard_test"
    r = sh(["cc", "-O1", "-w", f"-I{gen}", "-Ifirmware/src", "-o", str(exe), "tests/guard_test.c", "-lm"], d)
    if r.returncode:
        return name, "BROKEN: does not build: " + r.stderr[-200:]
    r = sh([str(exe), *wbs], d)
    if r.returncode:
        first = next((ln for ln in r.stdout.splitlines() if ln.startswith("FAIL")), "")
        return name, f"caught by guard_test: {first[:110]}"
    sw = d / "guard_sweep"
    r = sh(["cc", "-O2", "-w", f"-I{gen}", "-Ifirmware/src", "-o", str(sw), "tests/guard_sweep.c", "-lm"], d)
    if r.returncode:
        return name, "BROKEN: does not build: " + r.stderr[-200:]
    for blob, want in dumps.items():
        got = sh([str(sw), "--dump-slots", str(blob)], d).stdout
        if got != want:
            return name, f"caught by the model cross-check ({blob.stem[3:]})"
    return name, "MISSED"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--only")
    a = ap.parse_args()
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        wbs = blobs(tmp)
        dumps = model_dumps(tmp)
        ms = [m for m in MUTANTS if not a.only or a.only in m[0]]
        missed = 0
        with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
            for name, res in ex.map(lambda m: run_mutant(m, tmp, wbs, dumps), ms):
                print(f"mutant {name:<36} {res}")
                missed += not res.startswith("caught")
    print(f"guard mutants: {len(ms) - missed} of {len(ms)} caught")
    return 1 if missed else 0


if __name__ == "__main__":
    sys.exit(main())
