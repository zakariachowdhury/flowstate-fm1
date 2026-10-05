#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""validate-world: the automated check of a Musical World (design 12.3, docs/validation.md).

  tools/validate-world WORLD|ID... [--full] [--static] [--user] [--json] [--jobs N] [--seed N] [-v]

Each argument is a World source (*.world.json), a directory of them, or a factory id (neon_rain, neon-rain:
worlds/factory/<id>.world.json). For every World it prints a checklist, one line per item:

  metadata presets engines patterns scenes variation scale harmony "Smart Keys" "macro ranges" Guardrails
      tools/worldc.py: the compiler's checks, the reference model of the macros and the guard, the hard limits
  "CPU budget" "no clipping" "no invalid feedback" "no invalid parameter IDs"  (and audibility and loudness steps,
      under Guardrails)
      tests/guard_sweep.c: the real firmware playing the World over the macro space (quick: a few seconds a World;
      --full: every scene x variation, 4-D grid, edges and random points, plus the corners of controls 4..15)
  "RAM budget" "flash budget"
      the pattern pool, the overlay's slots, the blob against its limit, the factory set against the app slot

A mark per item: v passed, x failed, ! passed with warnings, - not run (and why). The exit status is 1 when any
item failed, 2 for a usage error. With --json the report goes to stdout as JSON (schema "validate-world/1"), with
every message and the sweep's numbers; nothing else is printed there. A World that fails a static item is not
swept: the render items show "-". Python stdlib only; the host sweep is built on first use into build/host/validate.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import worldc  # noqa: E402  (the compiler, its reference model and the format's constants: nothing is copied)

SCHEMA = "validate-world/1"
ITEMS = ("metadata", "presets", "engines", "patterns", "scenes", "variation", "scale", "harmony", "Smart Keys",
         "macro ranges", "Guardrails", "CPU budget", "RAM budget", "flash budget", "no clipping",
         "no invalid feedback", "no invalid parameter IDs")
SWEEP_ITEMS = ("CPU budget", "no clipping", "no invalid feedback", "no invalid parameter IDs")
CODE_RESERVE = 8192          # of the app-slot room the code leaves, kept for the code (design 11.1: 8 KB free); the
                             # factory Worlds may take the rest (docs/validation.md 3; Phase 18, after FONT_L went)
WORLD_INDEX_ENTRY = 8        # tools/gen_worlds.py: a world_index_t {id, off, len} per factory World
HOST_DIR = ROOT / "build" / "host" / "validate"
FACTORY_DIR = ROOT / "worlds" / "factory"
F, GL = worldc.F, worldc.GL

# ------------------------------------------------------------------ where a message belongs ---
PATH_TOKEN = re.compile(r'\.([A-Za-z_]\w*)|\[(\d+)\]|\["((?:[^"\\]|\\.)*)"\]')
PATH_HEAD = re.compile(r'^(\$(?:\.[A-Za-z_]\w*|\[\d+\]|\["(?:[^"\\]|\\.)*"\])*)(?:: )?(.*)$', re.S)
# a message that names a parameter that does not exist or may not be named (worldc: resolve_tparam, gvalue, targets)
PARAM_NAME = re.compile(
    r"is neither a common parameter|is not a World global|is not a global a \w+ may move|is structural|is set by the World itself|a target "
    r"track\.param|engine roles are|is for synth tracks|takes a common parameter|the drum track has level, rev|"
    r"the drum track's level and rev are globals|names parameters, @roles or globals|the same parameter is set twice|"
    r"has no \w+: skipped")
TOP_ITEM = {"format": "metadata", "id": "metadata", "name": "metadata", "category": "metadata",
            "blurb": "metadata", "notes": "metadata", "tempo": "metadata", "swing": "metadata", "key": "scale",
            "fx": "presets", "patterns": "patterns", "progressions": "harmony", "energy": "scenes",
            "scenes": "scenes", "variations": "variation", "macros": "macro ranges", "controls": "macro ranges",
            "curves": "macro ranges", "rules": "macro ranges", "guard": "Guardrails", "smart_keys": "Smart Keys"}


def split_msg(text):
    """'$.a.b[0]: message' -> (['a', 'b', 0], 'message'); a missing key at the root maps to that key"""
    m = PATH_HEAD.match(text)
    if not m:
        return [], text
    keys = [ident if ident else int(idx) if idx else json.loads('"' + quoted + '"')
            for ident, idx, quoted in PATH_TOKEN.findall(m.group(1))]
    msg = m.group(2)
    mk = re.match(r'missing "([^"]+)"', msg)
    if not keys and mk:
        keys = [mk.group(1)]
    return keys, msg


def classify(text):
    """the checklist item of a worldc message"""
    keys, msg = split_msg(text)
    if "past the hard limit" in msg:
        return "Guardrails"
    if "targets moving at once" in msg or "patterns after unrolling" in msg:
        return "RAM budget"
    if "Smart Keys track is in every band" in msg:
        return "Smart Keys"
    if PARAM_NAME.search(msg) or (keys and keys[0] in ("macros", "controls", "rules", "guard") and
                                  re.search(r"is not a track\b|not a track name", msg)):
        return "no invalid parameter IDs"
    top = keys[0] if keys else None
    if top == "tracks":
        sub = [k for k in keys[2:] if isinstance(k, str)]
        if sub[:2] in (["sound", "preset"], ["sound", "kit"], ["sound", "params"]):
            return "presets"
        return "engines"
    if top == "smart_keys" and len(keys) > 1 and keys[1] == "melody_scale":
        return "scale"
    if top == "defaults":
        sub = keys[1] if len(keys) > 1 else ""
        return "variation" if sub == "variation" else "macro ranges" if sub in ("macros", "shape", "movement") else "scenes"
    return TOP_ITEM.get(top, "metadata")


LAB_RE = re.compile(r"^(.*?): (?:base -?\d+ [+-]\d+ = -?\d+: (outside|past the hard limit)|\d+ at 100 %: the parameter)")


def dedupe(errors):
    """one mapping past a hard limit is also outside its descriptor and at its maximum: report the hard limit only"""
    hard = set()
    for e in errors:
        m = LAB_RE.match(e)
        if m and m.group(2) == "past the hard limit":
            hard.add(m.group(1))
    return [e for e in errors if not ((m := LAB_RE.match(e)) and m.group(1) in hard and m.group(2) != "past the hard limit")]


CASCADE = re.compile(r'^"([^"]+)" is not an? (progression|pattern|energy table)\b')
CASCADE_KEY = {"progression": "progressions", "pattern": "patterns", "energy table": "energy"}


def suppress_cascades(errors, src):
    """a scene that names a progression, pattern or energy table which exists but has errors of its own is one
    problem, not two: report the definition"""
    out = []
    for e in errors:
        m = CASCADE.match(split_msg(e)[1])
        if m and isinstance(src, dict):
            key = CASCADE_KEY[m.group(2)]
            if m.group(1) in (src.get(key) or {}) and any(
                    o is not e and split_msg(o)[0][:2] == [key, m.group(1)] for o in errors):
                continue
        out.append(e)
    return out


# ------------------------------------------------------------------------------ the World ---
class Result:
    def __init__(self, item):
        self.item, self.status, self.errors, self.warnings, self.info, self.data = item, None, [], [], "", {}
        self.unit = ""

    def as_json(self):
        return {"item": self.item, "status": self.status, "summary": self.summary(), "info": self.info,
                "errors": self.errors, "warnings": self.warnings, "data": self.data}

    def summary(self, width=100):
        msgs = self.errors or self.warnings
        if not msgs:
            return self.info
        s = short(msgs[0], width)
        more = len(msgs) - 1
        return s + (f" (+{more} more{' ' + self.unit if self.unit else ''})" if more else "")


def short(s, n=118):
    s = " ".join(str(s).split())
    return s if len(s) <= n else s[:n - 1] + "…"


class World:
    def __init__(self, path, user=False):
        self.path, self.user = Path(path), user
        self.src = self.d = self.comp = self.ir = self.blob = self.md = None
        self.stage = "load"              # load -> schema -> tracks -> full -> clean (compiled, model run)
        self.res = {i: Result(i) for i in ITEMS}
        self.internal = None
        self.id = self.name = ""
        self.pool = 0
        self.model_ran = False
        self.sweep = None
        self.cap = F["WF_MAX_LEN"] if user else F["WF_FACTORY_LEN"]

    def rel(self):
        try:
            return str(self.path.resolve().relative_to(ROOT))
        except ValueError:
            return str(self.path)

    def add(self, item, kind, text):
        getattr(self.res[item], "errors" if kind == "err" else "warnings").append(text)

    # ---- the compiler, as worldc.compile_world minus its blob-size check (flash budget does that here)
    def compile(self):
        try:
            self.src = json.loads(self.path.read_text())
        except (OSError, UnicodeDecodeError) as e:
            self.add("metadata", "err", f"$: cannot read the file: {e}")
            return
        except json.JSONDecodeError as e:
            self.add("metadata", "err", f"$: not JSON: {e}")
            return
        if not isinstance(self.src, dict):
            self.add("metadata", "err", "$: a World is a JSON object")
            return
        self.id, self.name = str(self.src.get("id", "")), str(self.src.get("name", ""))
        self.stage = "schema"
        d = self.d = worldc.Diag()
        out = []
        try:
            sch = worldc.Schema()
            sch.check(self.src, sch.root, "$", out)
            d.errors.extend(out)
            if d.errors:
                return
            self.comp = worldc.Compiler(self.src, d, self.user)
            self.ir = self.comp.run()
            d.map_src = getattr(self.comp, "map_src", [])
            self.stage = "full" if hasattr(self.comp, "df") else "tracks"
            self.pool = len(getattr(self.comp, "pool", []))
            if self.ir is None:
                return
            self.blob = worldc.encode(self.ir)
            if len(self.blob) <= F["WF_MAX_LEN"]:        # (the decoder refuses a longer blob: flash budget says so)
                worldc.decode(self.blob)
                self.stage = "clean"
        except Exception as e:                       # a crash of the compiler on odd input is a finding too
            self.internal = f"worldc raised {type(e).__name__}: {e}"

    def model(self):
        """worldc's model check (ranges, "100 % is never all-max", hard limits, 48 slots), then this tool's own:
        the slot count with every control up, and the authored bases against the hard limits"""
        d = self.d
        try:
            worldc.model_check(self.blob, d, d.map_src)
            self.md = worldc.Model(self.ir)
            self.slots, self.slots_all = self.slot_counts()
            self.base_limits()
            self.model_ran = True
        except Exception as e:
            self.internal = f"the model raised {type(e).__name__}: {e}"

    def slot_counts(self):
        md, ir = self.md, self.ir
        most = most_all = 0
        for sc in range(F["WF_NSCENE"]):
            for v in range(len(ir["vars"])):
                md.stage(sc, v)
                for q in ((a, b, c, e) for a in (0, 500, 1000) for b in (0, 500, 1000) for c in (0, 500, 1000)
                          for e in (0, 500, 1000)):
                    pos = list(md.pos)
                    pos[:4] = q
                    most = max(most, len(md.evaluate(pos)) + md.over)
                    for k in range(4, F["WF_NCTL"]):
                        pos[k] = 1000
                    most_all = max(most_all, len(md.evaluate(pos)) + md.over)
        return most, most_all

    def base_limits(self):
        """an authored base past a hard limit of guard_limits.h stays where it is (docs/guardrails.md 2.4): a World
        that sets one is wrong. Level and resonance can be authored past theirs (descriptors reach 127);
        delay feedback and reverb size stop at theirs."""
        md, ir, d = self.md, self.ir, self.d
        names = [t.get("name", f"track {i + 1}") for i, t in enumerate(self.src["tracks"])]
        seen = set()
        for sc in range(F["WF_NSCENE"]):
            for v in range(len(ir["vars"])):
                md.stage(sc, v)
                where = f"scene {worldc.SCENE_KEYS[sc]} / {ir['vars'][v]['name']}"
                for t in range(worldc.NTRK - 1):
                    reso = worldc.ENG_ROLE[md.eng[t]][F["WF_EROLE_RESO"]]
                    for what, val, lim in (("level", md.p[t][worldc.P_LEVEL], GL["GL_LEVEL_MAX"]),
                                           ("resonance", md.p[t][worldc.PR.P_E0 + reso] if reso < 8 else 0,
                                            GL["GL_RESO_MAX"])):
                        if val > lim and (t, what) not in seen:
                            seen.add((t, what))
                            d.err("$.guard", f"track {names[t]}: authored {what} {val} is past the hard limit {lim} "
                                             f"({where}, guard_limits.h)")
                for gid, what, lim in ((worldc.G_DFDBK, "delay feedback", GL["GL_DFDBK_MAX"]),
                                       (worldc.G_RSIZE, "reverb size", GL["GL_RSIZE_MAX"])):
                    if md.g[gid] > lim and what not in seen:
                        seen.add(what)
                        d.err("$.guard", f"g.{worldc.PR.G[gid]['name']}: authored {what} {md.g[gid]} is past the hard "
                                         f"limit {lim} ({where}, guard_limits.h)")

    # ---- the static items
    def attribute(self):
        d = self.d
        if d is not None:
            for e in suppress_cascades(dedupe(d.errors), self.src):
                self.add(classify(e), "err", e)
            for w in d.warnings:
                self.add(classify(w), "warn", w)
        if self.internal:
            self.add("metadata", "err", f"internal error: {self.internal}")

    def settle_static(self):
        """pass only when everything the item covers ran: metadata, presets and engines once the compiler got past
        the tracks; the other compile items once it ran to its end; macro ranges, Guardrails and RAM budget once the
        model ran too"""
        stage = ("load", "schema", "tracks", "full", "clean").index(self.stage)
        for item in ITEMS[:11] + ("RAM budget",):
            r = self.res[item]
            ran = (stage >= 2 if item in ("metadata", "presets", "engines") else
                   self.model_ran if item in ("macro ranges", "Guardrails", "RAM budget") else stage >= 3)
            if r.errors:
                r.status = "fail"
            elif not ran:
                r.status, r.info = "skip", "not checked: the World does not compile" if stage < 3 else \
                    "not checked: the model needs a World that compiles"
            else:
                r.status = "warn" if r.warnings else "pass"

    # ---- budgets
    def settle_budgets(self, factory_set):
        ram, flash = self.res["RAM budget"], self.res["flash budget"]
        if self.blob is not None:
            n, cap = len(self.blob), self.cap
            kind = "user" if self.user else "factory"
            flash.data = {"bytes": n, "limit": cap, "guideline": F["WF_SOFT_LEN"], "kind": kind}
            flash.info = f"{n:,} B of {cap:,} B ({kind} World)"
            if n > cap:
                flash.errors.append(f"$: the blob is {n:,} B; a {kind} World holds at most {cap:,} B")
            elif n > F["WF_SOFT_LEN"]:
                flash.warnings.append(f"$: the blob is {n:,} B, above the {F['WF_SOFT_LEN']:,} B guideline")
            if factory_set is not None and self.in_factory():
                flash.data["factory_set"] = factory_set
                if "skipped" in factory_set:
                    flash.warnings.append("factory set not checked: " + factory_set["skipped"])
                else:
                    fs = factory_set
                    flash.info += f"; factory set {fs['bytes']:,} B of {fs['budget']:,} B"
                    if not fs["ok"]:
                        flash.errors.append(
                            f"factory set: {fs['worlds']} Worlds take {fs['bytes']:,} B, over {fs['budget']:,} B "
                            f"(the {fs['room']:,} B of the app slot the code leaves, less {CODE_RESERVE:,} B kept "
                            f"for the code)")
            flash.status = "fail" if flash.errors else "warn" if flash.warnings else "pass"
        else:
            flash.status, flash.info = "skip", "not checked: the World does not compile"
        if self.model_ran and ram.status in ("pass", "warn"):
            lim = worldc.OV_MAX
            ram.data = {"pool_patterns": self.pool, "pool": F["WF_MAX_PAT"], "slots": self.slots,
                        "slots_all_controls": self.slots_all, "slot_limit": lim}
            ram.info = (f"pool {self.pool} of {F['WF_MAX_PAT']} patterns; targets {self.slots}"
                        f" ({self.slots_all} with every control up) of {lim}")
            if self.slots_all > lim:
                ram.errors.append(f"$.controls: {self.slots_all} targets move at once with every control at 100 %: "
                                  f"at most {lim} (the rest would do nothing)")
                ram.status = "fail"

    def in_factory(self):
        try:
            return self.path.resolve().parent == FACTORY_DIR.resolve()
        except OSError:
            return False


# ----------------------------------------------------------------------- factory set budget ---
def app_slot():
    m = re.search(r"^APP_SLOT\s*=\s*(0x[0-9A-Fa-f]+|\d+)", (ROOT / "tools" / "fm1pkg_make.py").read_text(), re.M)
    return int(m.group(1), 0) if m else None


def world_flash(n):
    """what a factory blob of n B takes in the image: WORLD_DATA keeps each blob 4-byte aligned, plus its index entry"""
    return (n + 3) // 4 * 4 + WORLD_INDEX_ENTRY


def factory_set_budget(user):
    """Worlds in worlds/factory against the app slot: the slot minus the code (the image minus the factory Worlds the
    image carries: blobs, alignment, index) is the room; the Worlds may take all of it but CODE_RESERVE"""
    files = sorted(FACTORY_DIR.glob("*.world.json"))
    if not files:
        return {"skipped": "no worlds/factory/*.world.json"}
    total = 0
    for f in files:
        w = World(f, user)
        w.compile()
        if w.blob is None:
            return {"skipped": f"{f.name} does not compile"}
        total += world_flash(len(w.blob))
    img = ROOT / "build" / "felucca.bin"
    slot = app_slot()
    if not img.exists():
        return {"skipped": "build/felucca.bin is absent (run ./build.sh)", "bytes": total, "worlds": len(files)}
    if slot is None:
        return {"skipped": "APP_SLOT not found in tools/fm1pkg_make.py", "bytes": total, "worlds": len(files)}
    image = img.stat().st_size
    room = slot - (image - total)
    budget = room - CODE_RESERVE
    return {"bytes": total, "worlds": len(files), "image": image, "app_slot": slot, "room": room,
            "budget": budget, "reserve": CODE_RESERVE, "free_now": slot - image, "ok": total <= budget}


# ------------------------------------------------------------------------- the host sweep ---
class HostError(Exception):
    pass


def newest(paths):
    t = 0.0
    for p in paths:
        try:
            t = max(t, os.stat(p).st_mtime)
        except OSError:
            pass
    return t


def source_stamp():
    files = []
    for d, pats in (("tests", (".c", ".h")), ("firmware/src", (".c", ".h")), ("firmware/hal", (".c", ".h")),
                    ("host", (".c", ".h")), ("build/gen", (".h",))):
        base = ROOT / d
        if base.is_dir():
            files += [str(base / n) for n in os.listdir(base) if n.endswith(pats)]
    return newest(files)


def sweep_binary():
    """tests/guard_sweep.c, built as tests/run_tests.sh builds it, into build/host/validate (rebuilt when any source
    it includes is newer)"""
    exe = HOST_DIR / "guard_sweep"
    if not (ROOT / "build" / "gen").is_dir():
        r = subprocess.run([sys.executable, "tools/build.py", "--gen-only"], cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            raise HostError("cannot make build/gen (tools/build.py --gen-only): " + (r.stdout + r.stderr).strip()[-300:])
    if exe.exists() and exe.stat().st_mtime >= source_stamp():
        return exe
    HOST_DIR.mkdir(parents=True, exist_ok=True)
    tmp = HOST_DIR / f"guard_sweep.{os.getpid()}.tmp"
    cc = os.environ.get("CC", "cc").split()
    cmd = cc + ["-O2", "-w", "-Ibuild/gen", "-Ifirmware/src", "-o", str(tmp), "tests/guard_sweep.c", "-lm"]
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    except OSError as e:
        raise HostError(f"cannot run the C compiler ({cc[0]}): {e}")
    if r.returncode:
        raise HostError("tests/guard_sweep.c does not build: " + (r.stdout + r.stderr).strip()[-400:])
    os.replace(tmp, exe)
    return exe


POINT = re.compile(r"^\s+(\S+) ([A-D])/(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+): peak\s+(\S+) tail\s+(\S+) LUFS\s+(\S+) "
                   r"rms\s+(\S+) slew (\S+) lim (\S+) cpu\s+(\d+)/(\d+)\s+est\s+(\d+) vmax (\d+) lay ([0-9a-f]+)(.*)$")
ROW = re.compile(r"^\s+(\S+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+?)\.\.(\S+)\s+(\S+)/(\S+)\s+(\d+)/(\d+)\s+(\d+)\s+(\d+)\s+(\d+)")
JUMP = re.compile(r"^\s+JUMP \S+ ([A-D])/(\d+)( \(no player\))?: (\d+) (\d+) (\d+) (\d+) -> (\d+) (\d+) (\d+) (\d+): "
                  r"([\d.]+) LU")
WHY_TOK = ((r"clip\(.*?\)", "no clipping"), (r"garbage\(.*?\)", "no clipping"), (r"dc \S+", "no clipping"),
           (r"limiter input \S+ x LIM_T", "no clipping"), (r"feedback\(.*?\)", "no invalid feedback"),
           (r"\d+ invalid parameter values", "no invalid parameter IDs"), (r"voices \d+", "CPU budget"),
           (r"cpu\(.*?\)", "CPU budget"), (r"cpu estimate \d+ under \d+", "CPU budget"),
           (r"silent\(.*?\)", "Guardrails"), (r"crashed", "Guardrails"))
WHY_TOK = tuple((re.compile(r), item) for r, item in WHY_TOK)


def why_issues(why):
    """guard_sweep's failure text of a render -> [(item, text, severity)] (judge_pt: clip(..) garbage(..) dc ..)"""
    out, i = [], 0
    while i < len(why):
        if why[i] == " ":
            i += 1
            continue
        for rx, item in WHY_TOK:
            m = rx.match(why, i)
            if m:
                text = m.group(0)
                n = re.search(r"-?\d+(?:\.\d+)?", text)
                sev = float(n.group(0)) if n else 0.0          # how bad: larger is worse (silence: quieter is)
                out.append((item, text, -sev if text.startswith("silent") else sev))
                i = m.end()
                break
        else:
            i += 1
    return out


def num(s):
    try:
        return float(s)
    except ValueError:
        return float("nan")


def parse_sweep(text):
    """guard_sweep --list output -> {points, jumps, rows, clean, other}"""
    out = {"points": [], "jumps": [], "rows": [], "clean": False, "other": []}
    for ln in text.splitlines():
        m = POINT.match(ln)
        if m:
            g = m.groups()
            why = g[18].strip()
            out["points"].append({
                "scene": g[1], "var": int(g[2]), "macros": [int(x) for x in g[3:7]], "peak": num(g[7]),
                "tail": num(g[8]), "lufs": num(g[9]), "rms": num(g[10]), "slew": num(g[11]), "lim": num(g[12]),
                "cpu_mean": float(g[13]), "cpu_half": float(g[14]), "est": int(g[15]), "vmax": int(g[16]),
                "issues": why_issues(why)[:6]})
            continue
        m = JUMP.match(ln)
        if m:
            g = m.groups()
            out["jumps"].append({"scene": g[0], "var": int(g[1]), "player": not g[2],
                                 "from": [int(x) for x in g[3:7]], "to": [int(x) for x in g[7:11]], "lu": float(g[11])})
            continue
        m = ROW.match(ln)
        if m and not ln.strip().startswith(("World", "FAIL")):
            g = m.groups()
            out["rows"].append({"points": int(g[1]), "jump_player": num(g[6]), "jump_alone": num(g[7]),
                                "values": int(g[11]), "fails": int(g[12])})
            continue
        if re.match(r"^guard_sweep: \d+ points, ", ln):
            out["clean"] = "all clean" in ln
            out["seen_end"] = True
        elif ln.strip() and not ln.startswith(("guard_sweep: quick", "guard_sweep: full", "  FAIL", "  World")):
            out["other"].append(ln.strip())
    return out


def sweep_limits():
    """the limits guard_sweep.c judges by (its #defines are the source: nothing is copied here)"""
    try:
        t = (ROOT / "tests" / "guard_sweep.c").read_text()
    except OSError:
        t = ""

    def get(name, dflt):
        m = re.search(rf"^#define {name}\s+\(?(-?[\d.]+)\)?", t, re.M)
        return float(m.group(1)) if m else dflt
    return {"peak_dbfs": get("PEAK_DB", -0.3), "tail_dbfs": get("TAIL_DB", -60.0), "jump_lu": get("JUMP_LU", 9.0),
            "jump_lu_alone": get("JUMP_LU_ALONE", 12.0)}


def run_sweep(blob, full, jobs, seed):
    """the sweep's passes over one blob: quick, or (--full) the full grid and then the quick pass again for the
    corners of controls 4..15 (guard_sweep's full mode does not visit them). Returns [parse_sweep(...)] or raises"""
    exe = sweep_binary()
    passes = [["--full"], []] if full else [[]]
    outs = []
    for extra in passes:
        cmd = [str(exe), "--list"] + extra + (["--jobs", str(jobs)] if jobs else []) + \
              (["--seed", str(seed)] if seed is not None else []) + [str(blob)]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
        p = parse_sweep(r.stdout + r.stderr)
        p["mode"], p["exit"] = "full" if extra else "quick", r.returncode
        outs.append(p)
    return outs


def macro_text(p):
    return "macros " + "/".join(f"{x / 10:g}" for x in p["macros"]) + " %"      # COLOR/MOTION/SPACE/ENERGY


def settle_sweep(w, passes, var_names):
    """the sweep's numbers onto CPU budget, no clipping, no invalid feedback, no invalid parameter IDs and, for
    silence and loudness steps, Guardrails"""
    pts = [p for ps in passes for p in ps["points"]]
    jumps = [j for ps in passes for j in ps["jumps"]]
    for ps in passes:
        if not ps.get("seen_end") or not ps["points"]:
            tail = ps["other"][-1] if ps["other"] else f"exit {ps['exit']}"
            for item in SWEEP_ITEMS + ("Guardrails",):
                w.res[item].errors.append(f"the sweep did not complete: {tail}")
                w.res[item].status = "fail"
            return
    rows = [r for ps in passes for r in ps["rows"]]
    fin = lambda xs: [x for x in xs if x == x and abs(x) != float("inf")]   # noqa: E731
    sw = {"mode": "full" if len(passes) > 1 else "quick", "points": len(pts),
          "worst_peak_dbfs": max(p["peak"] for p in pts),
          "worst_tail_dbfs": max(p["tail"] for p in pts), "loudness_lufs": None,
          "cpu_mean_max": max(p["cpu_mean"] for p in pts), "cpu_half_max": max(p["cpu_half"] for p in pts),
          "cpu_estimate_max": max(p["est"] for p in pts), "voices_max": max(p["vmax"] for p in pts),
          "values_checked": sum(r["values"] for r in rows),
          "max_jump_lu": {"player": max([r["jump_player"] for r in rows] or [0]),
                          "alone": max([r["jump_alone"] for r in rows] or [0])},
          "failed_points": 0}
    lu = fin([p["lufs"] for p in pts])
    sw["loudness_lufs"] = [min(lu), max(lu)] if lu else None
    w.sweep = sw
    by_item = {i: [] for i in SWEEP_ITEMS + ("Guardrails",)}
    nfail = 0
    for p in pts:
        if p["issues"]:
            nfail += 1
        for item, text, sev in p["issues"]:
            by_item[item].append((sev, f"scene {p['scene']}, {var_names[p['var']] if p['var'] < len(var_names) else p['var']}"
                                       f", {macro_text(p)}: {text}"))
    sw["failed_points"] = nfail
    for j in jumps:
        by_item["Guardrails"].append((j["lu"] - 100, (
            f"scene {j['scene']}, {var_names[j['var']] if j['var'] < len(var_names) else j['var']}"
            f"{'' if j['player'] else ' (no player)'}: loudness moves {j['lu']:.1f} LU between "
            f"{'/'.join(str(x // 10) for x in j['from'])} and {'/'.join(str(x // 10) for x in j['to'])} % of the macros")))
    for item, found in by_item.items():
        w.res[item].errors.extend(m for _, m in sorted(found, key=lambda t: -t[0]))
        w.res[item].unit = "renders"
    if sum(r["fails"] for r in rows) and not any(by_item.values()):
        w.res["Guardrails"].errors.append(f"{sum(r['fails'] for r in rows)} sweep point(s) failed (run guard_sweep)")
    n = sw["points"]
    dbs = lambda x: "-inf" if x == float("-inf") else f"{x:.1f}"    # noqa: E731
    cpu_ok = sw["cpu_mean_max"] > 0
    lim = sweep_limits()
    sw["limits"] = lim
    info = {
        "CPU budget": (f"mean {sw['cpu_mean_max']:,.0f} of {GL['GL_CPU_BUDGET']:,}, costliest half "
                       f"{sw['cpu_half_max']:,.0f} of {GL['GL_CPU_FULL']:,} instr/sample") if cpu_ok else "",
        "no clipping": f"peak at most {dbs(sw['worst_peak_dbfs'])} dBFS (limit {lim['peak_dbfs']:g}) over {n} renders",
        "no invalid feedback": f"worst tail {dbs(sw['worst_tail_dbfs'])} dBFS after STOP (limit {lim['tail_dbfs']:g})",
        "no invalid parameter IDs": f"{sw['values_checked']:,} effective values inside their descriptors"}
    for item in SWEEP_ITEMS:
        r = w.res[item]
        r.data = {k: v for k, v in sw.items() if k in {
            "CPU budget": ("cpu_mean_max", "cpu_half_max", "cpu_estimate_max", "voices_max"),
            "no clipping": ("worst_peak_dbfs", "limits"), "no invalid feedback": ("worst_tail_dbfs", "limits"),
            "no invalid parameter IDs": ("values_checked",)}[item]}
        if item == "CPU budget":
            r.data.update({"budget": GL["GL_CPU_BUDGET"], "limit": GL["GL_CPU_FULL"]})
        r.info = info[item]
        r.status = "fail" if r.errors else "warn" if r.warnings else "pass"
    r = w.res["CPU budget"]
    if not cpu_ok and not r.errors:
        r.status, r.info = "warn", ""
        r.warnings.append("the instruction counter is not available here (macOS only): CPU not measured")
    g = w.res["Guardrails"]
    if g.errors:
        g.status = "fail"
    else:
        g.status = "warn" if g.warnings else "pass"
        j = sw["max_jump_lu"]
        g.info = (f"audible; loudness steps {j['player']:.1f} LU with a player, {j['alone']:.1f} without "
                  f"(limits {lim['jump_lu']:g} / {lim['jump_lu_alone']:g})")
        g.data = {"max_jump_lu": j}


# ------------------------------------------------------------------------------- the driver ---
def resolve(arg):
    """a path, a directory, a factory id -> list of files; None when nothing matches"""
    p = Path(arg)
    if p.is_dir():
        return sorted(p.glob("*.world.json"))
    if p.exists():
        return [p]
    base = re.sub(r"(\.world)?(\.json)?$", "", p.name).replace("-", "_").lower()
    for d in (FACTORY_DIR, ROOT / "worlds" / "test", ROOT / "worlds" / "test" / "bad"):
        c = d / f"{base}.world.json"
        if c.exists():
            return [c]
    return None


def validate(path, args, factory_set):
    w = World(path, args.user)
    w.compile()
    if w.stage == "clean":
        w.model()
    w.attribute()
    w.settle_static()
    w.settle_budgets(factory_set)
    for item in SWEEP_ITEMS:                      # (a bad parameter name is found statically, under the last item)
        w.res[item].status = "fail" if w.res[item].errors else "skip"
    static_failed = any(r.status == "fail" for r in w.res.values())
    reason = None
    if args.static:
        reason = "not run: --static"
    elif static_failed:
        reason = "not run: fix the failed checks above first"
    elif w.blob is None:
        reason = "not run: the World does not compile"
    if reason is None:
        blob = HOST_DIR / f"{re.sub(r'[^A-Za-z0-9_]', '_', w.path.stem)}.{os.getpid()}.wblob"
        try:
            HOST_DIR.mkdir(parents=True, exist_ok=True)
            blob.write_bytes(w.blob)
            tty = sys.stderr.isatty() and not args.json
            if tty:
                sys.stderr.write(f"  sweeping the macro space ({'full' if args.full else 'quick'}) ...\r")
                sys.stderr.flush()
            passes = run_sweep(blob, args.full, args.jobs, args.seed)
            if tty:
                sys.stderr.write(" " * 60 + "\r")
            settle_sweep(w, passes, [v["name"] for v in w.ir["vars"]])
        except HostError as e:
            for item in SWEEP_ITEMS:
                w.res[item].errors.append(str(e))
                w.res[item].status = "fail"
        finally:
            try:
                blob.unlink()
            except OSError:
                pass
    else:
        for item in SWEEP_ITEMS:
            w.res[item].info = reason
    return w


MARK = {"pass": "✓", "fail": "✗", "warn": "⚠", "skip": "–"}
COLOUR = {"pass": "32", "fail": "31", "warn": "33", "skip": "2"}


def render(w, color, verbose):
    def c(code, t):
        return f"\033[{code}m{t}\033[0m" if color else t
    head = c("1", w.id or w.path.stem)
    if w.blob is not None:
        head += f"  {w.name}  {len(w.blob):,} B"
    lines = [f"{head}  {c('2', '(' + w.rel() + ')')}"]
    for item in ITEMS:
        r = w.res[item]
        text = r.summary(250 if verbose else 100)
        lines.append(f"  {c(COLOUR[r.status], MARK[r.status])} " +
                     (f"{item:<24}  " + c('0' if r.status in ('fail', 'warn') else '2', short(text, 300)) if text else item))
        more = r.errors[1:] + (r.warnings if verbose else []) if r.errors else r.warnings[1:]
        for m in more[:None if verbose else 3]:
            lines.append(f"      {c('2', short(m, 220 if verbose else 150))}")
        if not verbose and len(more) > 3:
            lines.append(f"      {c('2', f'... {len(more) - 3} more (-v, or --json)')}")
    bad = [i for i in ITEMS if w.res[i].status == "fail"]
    warn = [i for i in ITEMS if w.res[i].status == "warn"]
    skip = [i for i in ITEMS if w.res[i].status == "skip"]
    if bad:
        lines.append(c("31", f"  FAILED: {', '.join(bad)}"))
    else:
        lines.append(c("32", f"  passed: {len(ITEMS) - len(skip) - len(warn)} clean" +
                       (f", {len(warn)} with warnings" if warn else "") + (f", {len(skip)} not run" if skip else "")))
    return "\n".join(lines)


def report(w):
    return {"file": w.rel(), "id": w.id, "name": w.name, "kind": "user" if w.user else "factory",
            "ok": all(r.status != "fail" for r in w.res.values()), "bytes": len(w.blob) if w.blob else None,
            "items": [w.res[i].as_json() for i in ITEMS], "sweep": w.sweep}


def main(argv=None):
    ap = argparse.ArgumentParser(prog="validate-world", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("worlds", nargs="+", help="World JSON files, directories of them, or factory ids")
    ap.add_argument("--full", action="store_true", help="the full macro sweep (about 20 s a World on 10 cores)")
    ap.add_argument("--static", action="store_true", help="no host sweep: the compiler's and the model's checks only")
    ap.add_argument("--user", action="store_true", help="validate as a user World (blob limit %d B)" % F["WF_MAX_LEN"])
    ap.add_argument("--json", action="store_true", help="a machine-readable report on stdout")
    ap.add_argument("--jobs", type=int, default=0, help="parallel renders of the sweep (default: every core)")
    ap.add_argument("--seed", type=int, default=None, help="the sweep's random points (default: its fixed seed)")
    ap.add_argument("-v", "--verbose", action="store_true", help="every message of every item")
    a = ap.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    files = []
    for arg in a.worlds:
        r = resolve(arg)
        if not r:
            ids = ", ".join(sorted(p.name[:-len(".world.json")] for p in FACTORY_DIR.glob("*.world.json")))
            print(f"validate-world: no World '{arg}' (a *.world.json file, a directory, or a factory id: {ids})",
                  file=sys.stderr)
            return 2
        files += r
    color = sys.stdout.isatty() and not a.json and "NO_COLOR" not in os.environ
    fset = None
    if any(Path(f).resolve().parent == FACTORY_DIR.resolve() for f in files if Path(f).exists()) and not a.user:
        fset = factory_set_budget(a.user)
    t0 = time.time()
    reps, worlds = [], []
    for f in files:
        w = validate(f, a, fset)
        worlds.append(w)
        reps.append(report(w))
        if not a.json:
            print(render(w, color, a.verbose))
            sys.stdout.flush()
    ok = all(r["ok"] for r in reps)
    if a.json:
        print(json.dumps({"schema": SCHEMA, "mode": "static" if a.static else "full" if a.full else "quick",
                          "ok": ok, "seconds": round(time.time() - t0, 1), "worlds": reps}, indent=2))
    elif len(files) > 1:
        nbad = sum(not r["ok"] for r in reps)
        print(f"\n{len(files)} Worlds: {len(files) - nbad} passed" + (f", {nbad} FAILED" if nbad else "") +
              f" ({time.time() - t0:.0f} s)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
