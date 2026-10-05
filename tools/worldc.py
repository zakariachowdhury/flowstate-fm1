#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""worldc: the Musical World compiler (JSON source -> FWD1 blob), decompiler and static checker.

  tools/worldc.py compile WORLD.json -o OUT.wblob [--c-array] [--name SYM] [--user]
  tools/worldc.py decompile BLOB [-o OUT.json]
  tools/worldc.py check WORLD.json... [--user] [--json]
  tools/worldc.py model WORLD.json|BLOB [--random N] [--seed S]
  tools/worldc.py names [ENGINE]

names     what a World may name: engines (EDIT labels, value names, roles, presets), common parameters, the World
          globals, drum kits and lanes, scales, divisions (from worlds/schema/sloop-params.json).

compile   writes the blob (or, with --c-array, a C array of it); fails on any error.
decompile prints a lowered JSON (absolute notes, generated names) that compiles back to the same bytes.
check     the static checks (schema, names, ranges, notation, budgets), then the reference model of the macros and
          the guard (design 6.4) over every scene x variation and the 3^4 grid of the macros: no mapping out of its
          parameter's range or past a hard limit, none at the maximum at 100 % unless "saturate", at most 48 slots,
          a Smart Keys range of an octave at least; no audio.
model     the model's slot tables at sampled macro positions (the format of tests/guard_sweep.c --dump-slots).

Sources of truth (nothing is copied by hand):
  firmware/src/world_fmt.h         every format constant (read by a small parser below)
  worlds/schema/sloop-params.json  SLOOP's tables (tools/dump_params.c prints it from the firmware sources)
  worlds/schema/world.schema.json  the JSON Schema of a World source (checked by a built-in minimal validator)
The format: docs/design/fwd1-format.md. The author's guide: docs/worlds.md. Python stdlib only.
"""
import argparse
import ast
import json
import math
import re
import struct
import sys
import zlib
from fractions import Fraction
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FMT_H = ROOT / "firmware" / "src" / "world_fmt.h"
PARAMS_JSON = ROOT / "worlds" / "schema" / "sloop-params.json"
SCHEMA_JSON = ROOT / "worlds" / "schema" / "world.schema.json"
FORMAT_ID = "flowstate-world/1"


# ============================================================ world_fmt.h ===
def _strip_c(text):
    text = re.sub(r"/\*.*?\*/", lambda m: " " * len(m.group(0)), text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _split_top(s):
    """split s at commas outside braces / parentheses"""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "{(":
            depth += 1
        elif ch in "})":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [x.strip() for x in out]


def parse_fmt(path=FMT_H):
    """#define NAME VALUE of world_fmt.h -> {NAME: int | str | list}"""
    text = _strip_c(Path(path).read_text()).replace("\\\n", " ")
    raw = {}
    for m in re.finditer(r"^[ \t]*#define[ \t]+([A-Za-z_]\w*)(?![\w(])[ \t]*(.*)$", text, re.M):
        if m.group(2).strip():
            raw[m.group(1)] = m.group(2).strip()
    vals = {}

    def ev(expr):
        expr = expr.strip()
        if expr.startswith('"'):
            return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', expr))
        if expr.startswith("{"):
            if not expr.endswith("}"):
                raise SystemExit(f"world_fmt.h: bad list {expr[:40]}")
            return [ev(x) for x in _split_top(expr[1:-1])]
        if re.fullmatch(r"[PG]_\w+(\s*,\s*[PG]_\w+)*", expr):
            return [x.strip() for x in expr.split(",")]
        e = re.sub(r"\b(0[xX][0-9a-fA-F]+|\d+)[uU]?\b", lambda mm: str(int(mm.group(1), 0)), expr)
        e = re.sub(r"\b(W[FE]_\w+)\b", lambda mm: str(get(mm.group(1))), e)
        node = ast.parse(e, mode="eval")
        for n in ast.walk(node):
            if not isinstance(n, (ast.Expression, ast.BinOp, ast.UnaryOp, ast.Constant, ast.BitOr, ast.LShift,
                                  ast.Add, ast.Sub, ast.Mult, ast.USub, ast.BitAnd)):
                raise SystemExit(f"world_fmt.h: unsupported expression {expr!r}")
        return eval(compile(node, "world_fmt.h", "eval"))  # noqa: S307 (checked AST: integers only)

    def get(name):
        if name not in vals:
            if name not in raw:
                raise SystemExit(f"world_fmt.h: {name} is not defined")
            vals[name] = ev(raw[name])
        return vals[name]

    for name in raw:
        if name != "WORLD_FMT_H":
            get(name)
    return vals


F = parse_fmt()


def fnames(key):
    return F[key].split()


ROLE_NAMES = fnames("WF_ROLE_NAMES")
SROLE_NAMES = fnames("WF_SROLE_NAMES")
BEAT_NAMES = fnames("WF_BEAT_NAMES")
CTL_NAMES = fnames("WF_CTL_NAMES")
EROLE_NAMES = fnames("WF_EROLE_NAMES")
CURVE_NAMES = fnames("WF_CURVE_NAMES")
CLASS_NAMES = fnames("WF_CLASS_NAMES")
QUAL_NAMES = fnames("WF_QUAL_NAMES")
PULSE_NAMES = fnames("WF_PULSE_NAMES")
KMODE_NAMES = fnames("WF_KMODE_NAMES")
WHITE_NAMES = fnames("WF_WHITE_NAMES")
BLACK_NAMES = fnames("WF_BLACK_NAMES")
FOLLOW_NAMES = fnames("WF_FOLLOW_NAMES")
AVOID_NAMES = fnames("WF_AVOID_NAMES")
DENSCHG_NAMES = fnames("WF_DENSCHG_NAMES")
SCENE_KEYS = fnames("WF_SCENE_NAMES")
SEC_NAMES = fnames("WF_S_NAMES")
WE_NAMES = fnames("WE_NAMES")
NONE = F["WF_NONE"]
UNIT = F["WF_UNIT"]


# ======================================================== SLOOP's tables ===
class Params:
    def __init__(self, path=PARAMS_JSON):
        d = json.loads(Path(path).read_text())
        self.d = d
        self.P = d["P"]
        self.G = d["G"]
        self.lim = d["limits"]
        self.P_E0 = self.lim["P_E0"]
        self.pid = {p["name"]: p["id"] for p in self.P}
        self.gid = {g["name"]: g["id"] for g in self.G}
        self.engines = d["engines"]
        self.eng_id = {e["name"]: e["id"] for e in self.engines}
        self.kits = d["drum_kits"]
        self.lanes = d["lanes"]
        self.lane_id = {}
        for ln in self.lanes:
            for alias in (ln["id"], ln["name"].lower().replace(" ", ""), ln["short"].replace(" ", "")):
                self.lane_id.setdefault(alias, ln["index"])
        self.scales = d["scales"]
        self.scale_id = {s["name"]: s["id"] for s in self.scales}
        self.divs = d["divs"]
        self.div_den = d["div_den"]

    def sym(self, s):
        """P_ROOT / G_DFDBK -> id"""
        kind, name = s[0], s[2:].lower()
        table = self.pid if kind == "P" else self.gid
        if name not in table:
            raise SystemExit(f"world_fmt.h: {s} is not a SLOOP parameter (sloop-params.json)")
        return table[name]

    def pdesc(self, pid, engine=None):
        """descriptor of track parameter pid on an engine (None: common only)"""
        if pid < self.P_E0:
            return self.P[pid]
        if engine is None:
            return None
        return self.engines[engine]["edit"][pid - self.P_E0]

    def scale_mask(self, sid):
        return self.scales[sid]["mask"]


PR = Params()
P_FIXED = {PR.sym(s) for s in F["WF_P_FIXED"]}
P_DRUM = {PR.sym(s) for s in F["WF_P_DRUM"]}
P_STRUCT = {PR.sym(s) for s in F["WF_P_STRUCT"]}
G_WHITE = [PR.sym(s) for s in F["WF_G_WHITELIST"]]
G_STRUCT = {PR.sym(s) for s in F["WF_G_STRUCT"]}
G_NOVAR = {PR.sym(s) for s in (F["WF_G_NOVAR"] if isinstance(F["WF_G_NOVAR"], list) else [F["WF_G_NOVAR"]])}
G_DRLVL, G_DRREV, G_SWING, G_BPM = PR.gid["drlvl"], PR.gid["drrev"], PR.gid["swing"], PR.gid["bpm"]
QUAL_C3, QUAL_C5, QUAL_C7 = F["WF_QUAL_C3"], F["WF_QUAL_C5"], F["WF_QUAL_C7"]
NTRK, TRK_DRUM = F["WF_NTRK"], F["WF_TRK_DRUM"]
PC = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
MAJOR_MASK = PR.scale_mask(PR.scale_id["MAJ"])
MINOR_MASK = PR.scale_mask(PR.scale_id["MIN"])


def bits(mask):
    return [i for i in range(12) if mask >> i & 1]


def degree_mask(scale_mask):
    """the 7-note scale roman numerals (and c9) refer to: the World scale, or its parent"""
    if len(bits(scale_mask)) == 7:
        return scale_mask
    if scale_mask == PR.scale_mask(PR.scale_id["MPEN"]) or scale_mask == PR.scale_mask(PR.scale_id["BLUES"]):
        return MINOR_MASK
    return MAJOR_MASK


def note_name(n):
    return f"{NOTE_NAMES[n % 12]}{n // 12 - 1}"


def parse_note(s):
    m = re.fullmatch(r"([A-G])([#b]?)(-1|[0-9])", s)
    if not m:
        return None
    n = 12 * (int(m.group(3)) + 1) + PC[m.group(1)] + {"": 0, "#": 1, "b": -1}[m.group(2)]
    return n if 0 <= n <= 127 else None


def fnv1a(s):
    h = F["WF_FNV_BASIS"]
    for b in s.encode():
        h = ((h ^ b) * F["WF_FNV_PRIME"]) & 0xFFFFFFFF
    return h


def crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def blob_crc(b, L):
    """the FWD1 CRC: bytes [0, 12) then [16, L) (all but the CRC field)"""
    return zlib.crc32(b[F["WF_CRC_FROM"]:L], zlib.crc32(b[:F["WF_CRC_AT"]])) & 0xFFFFFFFF


# ========================================================= diagnostics ===
class WorldError(Exception):
    pass


class Diag:
    def __init__(self):
        self.errors, self.warnings = [], []

    def err(self, path, msg):
        self.errors.append(f"{path}: {msg}")

    def warn(self, path, msg):
        self.warnings.append(f"{path}: {msg}")


def jp(path, key):
    if isinstance(key, int):
        return f"{path}[{key}]"
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", str(key)):
        return f"{path}.{key}"
    return f"{path}[{json.dumps(key)}]"


# =============================================== minimal JSON Schema check ===
class Schema:
    """the subset of JSON Schema (2020-12) world.schema.json uses"""

    def __init__(self, path=SCHEMA_JSON):
        self.root = json.loads(Path(path).read_text())

    def ref(self, r):
        node = self.root
        for part in r.lstrip("#/").split("/"):
            node = node[part]
        return node

    @staticmethod
    def type_ok(v, t):
        if isinstance(t, list):
            return any(Schema.type_ok(v, x) for x in t)
        return {"object": lambda: isinstance(v, dict), "array": lambda: isinstance(v, list),
                "string": lambda: isinstance(v, str), "boolean": lambda: isinstance(v, bool),
                "null": lambda: v is None,
                "integer": lambda: isinstance(v, int) and not isinstance(v, bool),
                "number": lambda: isinstance(v, (int, float)) and not isinstance(v, bool)}[t]()

    def check(self, v, s, path, out):
        if "$ref" in s:
            self.check(v, self.ref(s["$ref"]), path, out)
        if "type" in s and not self.type_ok(v, s["type"]):
            out.append(f"{path}: expected {s['type'] if isinstance(s['type'], str) else ' or '.join(s['type'])}, "
                       f"got {json.dumps(v)[:40]}")
            return
        if "const" in s and v != s["const"]:
            out.append(f"{path}: must be {json.dumps(s['const'])}")
        if "enum" in s and v not in s["enum"]:
            out.append(f"{path}: {json.dumps(v)} is not one of {', '.join(json.dumps(x) for x in s['enum'])}")
        for k in ("anyOf", "oneOf"):
            if k in s:
                tries = []
                for sub in s[k]:
                    o = []
                    self.check(v, sub, path, o)
                    if not o:
                        break
                    tries.append(o)
                else:
                    best = min(tries, key=len)
                    out.extend(best if len(best) == 1 else [f"{path}: does not match any allowed form ({best[0]})"])
        if isinstance(v, str):
            if "minLength" in s and len(v) < s["minLength"]:
                out.append(f"{path}: shorter than {s['minLength']} characters")
            if "maxLength" in s and len(v) > s["maxLength"]:
                out.append(f"{path}: longer than {s['maxLength']} characters ({json.dumps(v)})")
            if "pattern" in s and not re.search(s["pattern"], v):
                out.append(f"{path}: {json.dumps(v)} does not match {s['pattern']}")
        if isinstance(v, (int, float)) and not isinstance(v, bool):
            if "minimum" in s and v < s["minimum"]:
                out.append(f"{path}: {v} is below {s['minimum']}")
            if "maximum" in s and v > s["maximum"]:
                out.append(f"{path}: {v} is above {s['maximum']}")
        if isinstance(v, list):
            if "minItems" in s and len(v) < s["minItems"]:
                out.append(f"{path}: fewer than {s['minItems']} items")
            if "maxItems" in s and len(v) > s["maxItems"]:
                out.append(f"{path}: more than {s['maxItems']} items")
            if "items" in s:
                for i, x in enumerate(v):
                    self.check(x, s["items"], jp(path, i), out)
        if isinstance(v, dict):
            for k in s.get("required", []):
                if k not in v:
                    out.append(f"{path}: missing \"{k}\"")
            if "minProperties" in s and len(v) < s["minProperties"]:
                out.append(f"{path}: fewer than {s['minProperties']} entries")
            if "maxProperties" in s and len(v) > s["maxProperties"]:
                out.append(f"{path}: more than {s['maxProperties']} entries")
            props, pats = s.get("properties", {}), s.get("patternProperties", {})
            for k, x in v.items():
                if "propertyNames" in s:
                    pn = s["propertyNames"]
                    if "pattern" in pn and not re.search(pn["pattern"], k):
                        out.append(f"{jp(path, k)}: the name {json.dumps(k)} does not match {pn['pattern']}")
                        continue
                    if "enum" in pn and k not in pn["enum"]:
                        out.append(f"{jp(path, k)}: unknown key (allowed: {', '.join(pn['enum'])})")
                        continue
                if k in props:
                    self.check(x, props[k], jp(path, k), out)
                    continue
                hit = [p for p in pats if re.search(p, k)]
                for p in hit:
                    self.check(x, pats[p], jp(path, k), out)
                if hit:
                    continue
                ap = s.get("additionalProperties", True)
                if ap is False:
                    allowed = ", ".join(props) if props else "none"
                    why = s.get("x-refused", {}).get(k)  # (a field refused on purpose: the reason)
                    out.append(f"{jp(path, k)}: {why}" if why else f"{jp(path, k)}: unknown key (allowed: {allowed})")
                elif isinstance(ap, dict):
                    self.check(x, ap, jp(path, k), out)


# ============================================================= notation ===
TOKEN_ITEM = re.compile(r"(?P<deg>(?P<acc>[b#]*)(?P<num>[1-9][0-9]*)(?P<oct>['’,]*))$|"
                        r"(?P<ct>c(?P<ctn>[13579])(?P<coct>['’,]*))$|"
                        r"(?P<abs>[A-G][#b]?(?:-1|[0-9]))$")
SUFFIX = re.compile(r"(!|_|~|\*[2-4])")


def scan_steps(s):
    """a melodic step string -> tokens and '|' separators (brackets keep their spaces)"""
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c.isspace():
            i += 1
        elif c == "|":
            out.append("|")
            i += 1
        else:
            j = i
            depth = 0
            while j < n and (depth or not (s[j].isspace() or s[j] == "|")):
                if s[j] == "[":
                    depth += 1
                elif s[j] == "]":
                    depth -= 1
                    if depth < 0:
                        break
                j += 1
            out.append(s[i:j] if depth == 0 else s[i:])
            i = j if depth == 0 else n
    return out


class Step:
    """a synth step as step_t holds it"""
    __slots__ = ("time", "notes", "flags", "lvl", "rat", "vel", "micro")

    def __init__(self, time=2, notes=(), flags=0, lvl=0, rat=0, vel=0, micro=0):
        self.time, self.notes, self.flags, self.lvl, self.rat, self.vel, self.micro = \
            time, tuple(notes), flags, lvl, rat, vel, micro

    def key(self):
        return (self.time, self.notes, self.flags, self.lvl, self.rat, self.vel, self.micro)

    def __eq__(self, o):
        return self.key() == o.key()

    def __hash__(self):
        return hash(self.key())


ST_NOTE, ST_TIE, ST_REST = 0, 1, 2
LV_NORM, LV_GHOST, LV_SOFT, LV_HARD = 0, 1, 2, 3
SF_ACCENT, SF_SLIDE = 1, 2
DRUM_CH = {"x": (LV_NORM, 0), "X": (LV_HARD, 0), "s": (LV_SOFT, 0), "g": (LV_GHOST, 0),
           "2": (LV_NORM, 1), "3": (LV_NORM, 2), "4": (LV_NORM, 3)}
DRUM_OUT = {v: k for k, v in DRUM_CH.items()}


# ============================================================= compiler ===
class Compiler:
    def __init__(self, src, diag, user=False):
        self.s, self.d, self.user = src, diag, user

    # ---- small helpers
    def e(self, path, msg):
        self.d.err(path, msg)

    def w(self, path, msg):
        self.d.warn(path, msg)

    def name_str(self, path, v, maxlen):
        ok = set(F["WF_NAME_CHARS"])
        if not v or len(v) > maxlen or any(c not in ok for c in v):
            self.e(path, f"{json.dumps(v)}: 1..{maxlen} characters of A-Z 0-9 space & ' - .")
        return v[:maxlen]

    def unit(self, path, v, lo=0.0, hi=1.0):
        """a position 0..1 -> 0..250 (rounded to the nearest 1/250)"""
        if not isinstance(v, (int, float)) or isinstance(v, bool) or not lo <= v <= hi:
            self.e(path, f"{json.dumps(v)}: a number {lo}..{hi}")
            return 0
        return int(round(v * UNIT))

    def enum_ix(self, path, v, names, what):
        if v in names:
            return names.index(v)
        lower = [n.lower() for n in names]
        if isinstance(v, str) and v.lower() in lower:
            return lower.index(v.lower())
        self.e(path, f"{json.dumps(v)} is not a {what} ({', '.join(names)})")
        return 0

    # ---- top level
    def run(self):
        s = self.s
        if s.get("format") != FORMAT_ID:
            self.e("$.format", f"must be \"{FORMAT_ID}\"")
        self.meta()
        self.tracks()
        if self.d.errors:
            return None
        self.globals_()
        self.patterns_src()
        self.progressions()
        self.energy_src()
        self.keys_src()
        self.scenes()
        self.variations()
        self.curves_src()
        self.macros()
        self.rules()
        self.guard()
        self.defaults()
        self.unused()
        if self.d.errors:
            return None
        return self.ir()

    # ---- META
    def meta(self):
        s = self.s
        wid = s.get("id", "")
        m = re.fullmatch(r"fnv_([0-9a-f]{8})", wid)
        if m:
            self.id_hash = int(m.group(1), 16)
            self.w("$.id", "a raw device id (decompiler output): give the World a real id")
        else:
            self.id_hash = fnv1a(wid)
        self.name = self.name_str("$.name", s.get("name", ""), F["WF_NAME_LEN"] - 1)
        self.cat = self.name_str("$.category", s.get("category", ""), F["WF_CAT_LEN"] - 1)
        blurb = s.get("blurb", "")
        try:
            bb = blurb.encode("latin-1")
            if any(not (32 <= c <= 126 or c >= 160) for c in bb) or len(bb) > F["WF_BLURB_LEN"] - 1:
                raise ValueError
        except (UnicodeEncodeError, ValueError):
            self.e("$.blurb", f"{json.dumps(blurb)}: at most {F['WF_BLURB_LEN'] - 1} printable Latin-1 characters")
            bb = b""
        self.blurb = bb
        t = s.get("tempo", {})
        bpm = t.get("bpm", 120)
        lo = t.get("min", max(F["WF_BPM_MIN"], round(bpm * 0.9)))
        hi = t.get("max", min(F["WF_BPM_MAX"], round(bpm * 1.1)))
        span = bpm * F["WF_TEMPO_RANGE_PCT"] // 100
        if not lo <= bpm <= hi:
            self.e("$.tempo", f"min {lo} <= bpm {bpm} <= max {hi} does not hold")
        if lo < bpm - span or hi > bpm + span:
            self.e("$.tempo", f"min..max ({lo}..{hi}) is wider than bpm +-{F['WF_TEMPO_RANGE_PCT']} % "
                              f"({bpm - span}..{bpm + span})")
        self.bpm, self.bpm_lo, self.bpm_hi = bpm, lo, hi
        k = s.get("key", {})
        m = re.fullmatch(r"([A-G])([#b]?)", k.get("root", ""))
        if not m:
            self.e("$.key.root", f"{json.dumps(k.get('root'))}: a note name C..B with # or b")
            self.root = 0
        else:
            self.root = (PC[m.group(1)] + {"": 0, "#": 1, "b": -1}[m.group(2)]) % 12
        self.scale = self.enum_ix("$.key.scale", k.get("scale", "MAJ"), [x["name"] for x in PR.scales], "scale")
        self.smask = PR.scale_mask(self.scale)
        self.dmask = degree_mask(self.smask)
        self.swing = s.get("swing", 0)

    # ---- TRACKS
    def tracks(self):
        src = self.s.get("tracks", [])
        self.trk = []
        self.tname = {}
        for i, t in enumerate(src):
            p = jp("$.tracks", i)
            role = self.enum_ix(jp(p, "role"), t.get("role"), ROLE_NAMES, "track role")
            nm = t.get("name", "")
            if nm in self.tname or nm in ("g", "*") or "." in nm or "+" in nm:
                self.e(jp(p, "name"), f"{json.dumps(nm)}: track names are unique and not g or *")
            self.tname[nm] = i
            drum = i == TRK_DRUM
            if drum != (role == F["WF_ROLE_DRUMS"]):
                self.e(jp(p, "role"), "the fourth track is the drum track (role \"drums\"), the first three are not")
                continue
            snd = t.get("sound", {})
            rec = {"name": nm, "role": role, "pairs": [], "gpairs": []}
            if drum:
                if "register" in t:
                    self.e(jp(p, "register"), "the drum track has no register")
                kit = snd.get("kit", PR.d["drum_default_kit"])
                rec["engine"], rec["register"] = NONE, 0
                rec["preset"] = self.kit_ix(jp(jp(p, "sound"), "kit"), kit)
            else:
                en = snd.get("engine", "")
                if en.upper() not in PR.eng_id:
                    self.e(jp(jp(p, "sound"), "engine"),
                           f"{json.dumps(en)} is not an engine ({', '.join(PR.eng_id)})")
                    continue
                rec["engine"] = PR.eng_id[en.upper()]
                rec["preset"] = self.preset_ix(jp(jp(p, "sound"), "preset"), rec["engine"], snd.get("preset"))
                reg = t.get("register")
                if reg is None:
                    rec["register"] = F["WF_REG_DEFAULT"][role]
                else:
                    n = parse_note(reg) if isinstance(reg, str) else None
                    if n is None:
                        self.e(jp(p, "register"), f"{json.dumps(reg)}: a note name such as A1 or C#3")
                        n = 48
                    rec["register"] = n
            self.trk.append(rec)
        if len(self.trk) != NTRK and not self.d.errors:
            self.e("$.tracks", "exactly 4 tracks")
        if self.d.errors:
            return
        for i, t in enumerate(src):
            pp = jp(jp(jp("$.tracks", i), "sound"), "params")
            for k, v in t.get("sound", {}).get("params", {}).items():
                r = self.param_target(jp(pp, k), i, k, v, where="track")
                if r:
                    (self.trk[i]["gpairs"] if r[0] == "g" else self.trk[i]["pairs"]).append(r[1:])
            self.dedupe(pp, self.trk[i]["pairs"])

    def dedupe(self, path, pairs):
        seen = {}
        for pr in pairs:
            key = pr[:-1]
            if key in seen:
                self.e(path, f"the same parameter is set twice ({key})")
            seen[key] = 1

    def kit_ix(self, path, kit):
        if isinstance(kit, int) and not isinstance(kit, bool) and 0 <= kit < len(PR.kits):
            return kit
        up = [k.upper() for k in PR.kits]
        if isinstance(kit, str) and kit.upper() in up:
            return up.index(kit.upper())
        self.e(path, f"{json.dumps(kit)} is not a drum kit ({', '.join(PR.kits)})")
        return 0

    def preset_ix(self, path, eng, name):
        pres = PR.engines[eng]["presets"]
        if isinstance(name, int) and not isinstance(name, bool) and 0 <= name < len(pres):
            return name
        up = [x.upper() for x in pres]
        if isinstance(name, str) and name.upper() in up:
            return up.index(name.upper())
        self.e(path, f"{json.dumps(name)} is not a preset of {PR.engines[eng]['name']} ({', '.join(pres)})")
        return 0

    def pvalue(self, path, desc, v):
        """a parameter value: an integer in the descriptor's range, or an enum value name"""
        if isinstance(v, str) and desc.get("names"):
            up = [x.upper() for x in desc["names"]]
            if v.upper() in up:
                return desc["min"] + up.index(v.upper())
            self.e(path, f"{json.dumps(v)} is not a value of {desc['label']} ({', '.join(desc['names'])})")
            return None
        if not isinstance(v, int) or isinstance(v, bool):
            self.e(path, f"{json.dumps(v)}: an integer" + (" or a value name" if desc.get("names") else ""))
            return None
        if not desc["min"] <= v <= desc["max"]:
            self.e(path, f"{v} is outside {desc['label']} {desc['min']}..{desc['max']}")
            return None
        return v

    def resolve_tparam(self, path, t, name, quiet=False):
        """parameter name on track t -> P id (None: unknown); the drum track's level / rev -> ('g', G id)"""
        trk = self.trk[t]
        if t == TRK_DRUM:
            if name in ("level", "rev"):
                return ("g", G_DRLVL if name == "level" else G_DRREV)
            pid = PR.pid.get(name)
            if pid is None or pid not in P_DRUM:
                if not quiet:
                    self.e(path, f"{json.dumps(name)}: the drum track has level, rev and "
                                 f"{', '.join(PR.P[x]['name'] for x in sorted(P_DRUM))}")
                return None
            return pid
        if name in PR.pid:
            return PR.pid[name]
        labels = [x["label"] for x in PR.engines[trk["engine"]]["edit"]]
        if name in labels and name != "-":
            return PR.P_E0 + labels.index(name)
        if not quiet:
            common = "lowercase common names (level, atk, ..., rev)"
            self.e(path, f"{json.dumps(name)} is neither a common parameter ({common}) nor an EDIT label of "
                         f"{PR.engines[trk['engine']]['name']} ({', '.join(x for x in labels if x != '-')})")
        return None

    def tdesc(self, t, pid):
        if t == TRK_DRUM or pid < PR.P_E0:
            return PR.P[pid]
        return PR.engines[self.trk[t]["engine"]]["edit"][pid - PR.P_E0]

    def structural(self, t, pid):
        if pid in P_STRUCT or pid in P_FIXED:
            return True
        if t != TRK_DRUM and pid >= PR.P_E0:
            k = pid - PR.P_E0
            eng = self.trk[t]["engine"]
            ok = eng == F["WF_ENUM_OK_ENGINE"] and k == F["WF_ENUM_OK_PARAM"]
            if PR.engines[eng]["edit"][k]["fmt"] == "ENUM" and not ok:
                return True
        return False

    def param_target(self, path, t, name, v, where):
        """one track parameter value -> ('p', pid, v) or ('g', gid, v); where: track | scene | var"""
        r = self.resolve_tparam(path, t, name)
        if r is None:
            return None
        if isinstance(r, tuple):
            gid = r[1]
            val = self.pvalue(path, PR.G[gid], v)
            return None if val is None else ("g", gid, val)
        pid = r
        if pid in P_FIXED:
            self.e(path, f"{name} is set by the World itself (key, pattern) or by the player")
            return None
        if where != "track" and self.structural(t, pid):
            self.e(path, f"{name} is structural: only the track's sound may set it, not a {where}")
            return None
        val = self.pvalue(path, self.tdesc(t, pid), v)
        return None if val is None else ("p", pid, val)

    # ---- GLOBALS
    def gvalue(self, path, name, v, where):
        if name not in PR.gid or PR.gid[name] not in G_WHITE:
            self.e(path, f"{json.dumps(name)} is not a World global "
                         f"({', '.join(PR.G[g]['name'] for g in G_WHITE)})")
            return None
        gid = PR.gid[name]
        if where == "world" and gid == G_SWING:
            self.e(path, "set the swing with the top-level \"swing\"")
            return None
        if where == "var" and gid in G_NOVAR:
            self.e(path, f"a variation keeps the groove: it cannot set {name}")
            return None
        val = self.pvalue(path, PR.G[gid], v)
        return None if val is None else (gid, val)

    def globals_(self):
        self.glob = []
        for k, v in self.s.get("fx", {}).items():
            r = self.gvalue(jp("$.fx", k), k, v, "world")
            if r:
                self.glob.append(r)
        for t in self.trk:
            self.glob += t["gpairs"]
        self.dedupe("$.fx", [(g, v) for g, v in self.glob])

    # ---- patterns: sources (resolved per progression later)
    def patterns_src(self):
        self.psrc = {}
        for name, p in self.s.get("patterns", {}).items():
            path = jp("$.patterns", name)
            tn = p.get("track")
            t = self.tname.get(tn, TRK_DRUM if tn == "drums" else None)
            if t is None:
                self.e(jp(path, "track"), f"{json.dumps(tn)} is not a track name")
                continue
            drum = t == TRK_DRUM
            if drum != ("lanes" in p) or drum == ("steps" in p):
                self.e(path, "a drum pattern has \"lanes\", a synth pattern \"steps\"")
                continue
            div = p.get("div", "1/16")
            if div not in PR.divs:
                self.e(jp(path, "div"), f"{json.dumps(div)} is not a division ({', '.join(PR.divs)})")
                continue
            div = PR.divs.index(div)
            if drum:
                if div != F["WF_DIV_16"]:
                    self.e(jp(path, "div"), "the drum track is always 1/16")
                    continue
                steps = self.drum_lanes(path, p["lanes"])
                if steps is not None:
                    self.psrc[name] = {"t": t, "drum": True, "div": div, "fixed": steps}
            else:
                toks = self.melodic(jp(path, "steps"), p["steps"], div, t)
                if toks is not None:
                    self.psrc[name] = {"t": t, "drum": False, "div": div, "toks": toks,
                                       "chordy": any(any(it[0] == "c" for it in tk[1]) for tk in toks
                                                     if tk[0] == "n")}

    def drum_lanes(self, path, lanes):
        n = None
        steps = None
        for ln, s in lanes.items():
            lp = jp(jp(path, "lanes"), ln)
            if ln not in PR.lane_id:
                self.e(lp, f"{json.dumps(ln)} is not a drum lane ({', '.join(x['id'] for x in PR.lanes)})")
                return None
            li = PR.lane_id[ln]
            bars = s.split("|")
            if len(bars) > 1 and any(len(b) != 16 for b in bars):
                self.e(lp, "with | separators every bar is 16 steps")
                return None
            s = s.replace("|", "")
            if n is None:
                n = len(s)
                if n not in (16, 32, 64):
                    self.e(lp, f"{n} steps: a drum pattern is 16, 32 or 64 steps")
                    return None
                steps = [dict() for _ in range(n)]
            elif len(s) != n:
                self.e(lp, f"{len(s)} steps; the other lanes have {n}")
                return None
            for i, c in enumerate(s):
                if c == ".":
                    continue
                if c not in DRUM_CH:
                    self.e(lp, f"step {i + 1}: {json.dumps(c)} (use . x X s g 2 3 4)")
                    return None
                if li in steps[i]:
                    self.e(lp, f"lane {ln} is given twice")
                    return None
                steps[i][li] = DRUM_CH[c]
        if steps is None:
            self.e(jp(path, "lanes"), "no lanes")
        return steps

    def melodic(self, path, text, div, t):
        """steps text -> [('r',) | ('t',) | ('n', [items], suffix dict)] with items ('d', note) | ('c', n, oct)"""
        toks = scan_steps(text)
        spb = 4 * PR.div_den[div]
        out, bar, bars = [], 0, []
        for tk in toks:
            if tk == "|":
                bars.append(bar)
                bar = 0
                continue
            r = self.step_token(path, tk, len(out) + 1, t)
            if r is None:
                return None
            out.append(r)
            bar += 1
        if bars:
            bars.append(bar)
            if any(b != spb for b in bars):
                self.e(path, f"bars of {bars} steps; at {PR.divs[div]} a bar is {spb} steps")
                return None
        if not 1 <= len(out) <= F["WF_MAX_LEN_STEPS"]:
            self.e(path, f"{len(out)} steps: a pattern is 1..64 steps")
            return None
        return out

    def step_token(self, path, tk, no, t):
        if tk == ".":
            return ("r",)
        if tk == "-":
            return ("t",)
        m = re.fullmatch(r"(\[[^\]]*\]|[^\[\]!_~*]+)((?:!|_|~|\*[2-4])*)", tk)
        if not m:
            self.e(path, f"step {no}: {json.dumps(tk)} is not a step (see docs/worlds.md, notation)")
            return None
        body, suf = m.group(1), m.group(2)
        sfx = SUFFIX.findall(suf)
        if len(sfx) != len(set(x[0] for x in sfx)):
            self.e(path, f"step {no}: {json.dumps(tk)}: a suffix twice")
            return None
        items = body[1:-1].split() if body.startswith("[") else [body]
        if not 1 <= len(items) <= 4:
            self.e(path, f"step {no}: {json.dumps(tk)}: 1..4 notes in a step")
            return None
        res = []
        for it in items:
            mm = TOKEN_ITEM.fullmatch(it)
            if not mm:
                self.e(path, f"step {no}: {json.dumps(it)} is not a degree (1..7, b3, 5'), a chord tone "
                             f"(c1 c3 c5 c7 c9) or a note (A#3)")
                return None
            if mm.group("deg"):
                deg = int(mm.group("num"))
                offs = bits(self.smask)
                if not 1 <= deg <= len(offs):
                    self.e(path, f"step {no}: degree {deg}: the scale {PR.scales[self.scale]['name']} has "
                                 f"{len(offs)} notes")
                    return None
                acc = mm.group("acc").count("#") - mm.group("acc").count("b")
                octv = mm.group("oct").count("'") + mm.group("oct").count("’") - mm.group("oct").count(",")
                n = self.tonic_reg(t) + offs[deg - 1] + acc + 12 * octv
                res.append(("d", n))
            elif mm.group("ct"):
                octv = mm.group("coct").count("'") + mm.group("coct").count("’") - mm.group("coct").count(",")
                res.append(("c", int(mm.group("ctn")), octv))
            else:
                res.append(("d", parse_note(it) if parse_note(it) is not None else -1))
        for r in res:
            if r[0] == "d" and not 0 <= r[1] <= 127:
                self.e(path, f"step {no}: {json.dumps(tk)} is outside MIDI 0..127")
                return None
        sd = {x[0]: x for x in sfx}
        return ("n", res, sd)

    def tonic_reg(self, t):
        reg = self.trk[t]["register"]
        return reg + (self.root - reg) % 12

    # ---- progressions
    ROMAN = re.compile(r"([b#]?)(VII|VI|IV|V|III|II|I|vii|vi|iv|v|iii|ii|i)(.*?)(?::(\d+))?")
    SUF_ANY = {"°": "DIM", "o": "DIM", "dim": "DIM", "+": "AUG", "aug": "AUG", "sus2": "SUS2", "sus4": "SUS4",
               "sus": "SUS4", "5": "POW5", "ø": "M7B5", "ø7": "M7B5", "m7b5": "M7B5", "°7": "DIM7", "o7": "DIM7",
               "dim7": "DIM7"}
    SUF_CASE = {"": ("MAJ", "MIN"), "7": ("DOM7", "MIN7"), "6": ("MAJ6", "MIN6"), "add9": ("ADD9", "MADD9"),
                "9": ("ADD9", "MADD9"), "maj7": ("MAJ7", None), "M7": ("MAJ7", None), "Δ": ("MAJ7", None),
                "Δ7": ("MAJ7", None)}
    NUMERALS = ["I", "II", "III", "IV", "V", "VI", "VII"]

    def chord(self, path, c):
        m = self.ROMAN.fullmatch(c) if isinstance(c, str) else None
        if not m:
            self.e(path, f"{json.dumps(c)}: a roman numeral such as i, VI, bVII7, ii°:2 (see docs/worlds.md)")
            return None
        acc, num, suf, beats = m.groups()
        upper = num.isupper()
        if suf in self.SUF_ANY:
            q = self.SUF_ANY[suf]
        elif suf in self.SUF_CASE and self.SUF_CASE[suf][0 if upper else 1]:
            q = self.SUF_CASE[suf][0 if upper else 1]
        else:
            self.e(path, f"{json.dumps(c)}: unknown chord suffix {json.dumps(suf)}")
            return None
        deg = self.NUMERALS.index(num.upper())
        root = (bits(self.dmask)[deg] + {"": 0, "b": -1, "#": 1}[acc]) % 12
        b = int(beats) if beats else 4
        if not 1 <= b <= F["WF_CHORD_BEATS_MAX"]:
            self.e(path, f"{json.dumps(c)}: 1..32 beats")
            return None
        return (root, QUAL_NAMES.index(q), b)

    def progressions(self):
        self.prog = []
        self.prog_ix = {}
        for name, chords in self.s.get("progressions", {}).items():
            path = jp("$.progressions", name)
            out = [self.chord(jp(path, i), c) for i, c in enumerate(chords)]
            if None in out:
                continue
            tot = sum(c[2] for c in out)
            if tot not in F["WF_PROG_BEATS"]:
                self.e(path, f"{tot} beats in all; a progression is 4, 8, 16 or 32 beats")
                continue
            self.prog_ix[name] = None          # index assigned on first use
            self.prog.append((name, out))
        self.prog_src = dict(self.prog)
        self.prog = []

    def prog_use(self, name):
        if self.prog_ix.get(name) is None:
            self.prog_ix[name] = len(self.prog)
            self.prog.append((name, self.prog_src[name]))
        return self.prog_ix[name]

    # ---- pattern instances (pool)
    def chord_note(self, t, chord, n, octv):
        root, q, _ = chord
        r = root if root <= 6 else root - 12
        base = self.tonic_reg(t) + r
        if n == 1:
            iv = 0
        elif n == 3:
            iv = QUAL_C3[q]
        elif n == 5:
            iv = QUAL_C5[q]
        elif n == 7:
            iv = QUAL_C7[q]
        else:
            if q in (QUAL_NAMES.index("ADD9"), QUAL_NAMES.index("MADD9")):
                iv = 14
            elif not (self.dmask >> ((root + 2) % 12) & 1) and (self.dmask >> ((root + 1) % 12) & 1):
                iv = 13
            else:
                iv = 14
        return base + iv + 12 * octv

    def resolve(self, path, name, prog):
        """pattern name (+ progression index for chord tokens) -> pool index"""
        p = self.psrc[name]
        key = (name, prog if not p["drum"] and p["chordy"] else None)
        if key in self.inst:
            return self.inst[key]
        if p["drum"]:
            content = ("d", p["div"], tuple(tuple(sorted(s.items())) for s in p["fixed"]))
        else:
            toks = p["toks"]
            L = len(toks)
            if p["chordy"]:
                chords = self.prog[prog][1]
                spb = PR.div_den[p["div"]]
                beat_ci = [ci for ci, c in enumerate(chords) for _ in range(c[2])]
                N = L * len(beat_ci) * spb // math.gcd(L, len(beat_ci) * spb)
            else:
                N = L
            steps = []
            for i in range(N):
                tk = toks[i % L]
                if tk[0] == "r":
                    steps.append(Step())
                elif tk[0] == "t":
                    steps.append(Step(ST_TIE))
                else:
                    notes = []
                    for it in tk[1]:
                        if it[0] == "d":
                            nn = it[1]
                        else:
                            ch = self.prog[prog][1][beat_ci[(i // spb) % len(beat_ci)]]
                            nn = self.chord_note(p["t"], ch, it[1], it[2])
                        if not 0 <= nn <= 127:
                            self.e(path, f"pattern {name}: a chord tone outside MIDI 0..127 at step {i + 1}")
                            nn = 60
                        if nn not in notes:
                            notes.append(nn)
                    sd = tk[2]
                    flags = (SF_ACCENT if "!" in sd else 0) | (SF_SLIDE if "~" in sd else 0)
                    lvl = sum(LV_SOFT << 2 * k for k in range(len(notes))) if "_" in sd else 0
                    rat = sum((int(sd["*"][1]) - 1) << 2 * k for k in range(len(notes))) if "*" in sd else 0
                    steps.append(Step(ST_NOTE, notes, flags, lvl, rat))
            per = N
            for q in range(1, N + 1):
                if N % q == 0 and all(steps[i] == steps[i % q] for i in range(N)):
                    per = q
                    break
            if per > F["WF_MAX_LEN_STEPS"]:
                pn = [k for k, v in self.prog_ix.items() if v == prog][0]
                self.e(path, f"pattern {name} ({L} steps at {PR.divs[p['div']]}) over progression {pn} "
                             f"repeats only after {per} steps; a pattern holds 64. Use a slower div, a shorter "
                             f"progression, or fewer chord tokens")
                return NONE
            content = ("s", p["div"], tuple(steps[:per]))
        if content in self.pool_ix:
            ix = self.pool_ix[content]
        else:
            ix = len(self.pool)
            self.pool_ix[content] = ix
            self.pool.append({"drum": p["drum"], "div": p["div"], "t": p["t"],
                              "steps": list(content[2]) if not p["drum"] else [dict(s) for s in content[2]],
                              "src": name})
        self.inst[key] = ix
        self.used_pat.add(name)
        return ix

    # ---- ENERGY (sources; tables are emitted in order of first use)
    def mask16(self, path, s):
        s2 = s.replace("|", "")
        if len(s2) != 16 or any(c not in ".xX" for c in s2):
            self.e(path, f"{json.dumps(s)}: 16 steps of x and .")
            return 0xFFFF
        return sum(1 << i for i, c in enumerate(s2) if c != ".")

    def lanes_mask(self, path, v):
        if v == "all":
            return 0xFFFF
        m = 0
        for i, ln in enumerate(v):
            if ln not in PR.lane_id:
                self.e(jp(path, i), f"{json.dumps(ln)} is not a drum lane")
            else:
                m |= 1 << PR.lane_id[ln]
        return m

    def energy_src(self):
        self.esrc = {}
        for name, tab in self.s.get("energy", {}).items():
            path = jp("$.energy", name)
            dl = self.lanes_mask(jp(path, "density_lanes"), tab.get("density_lanes", []))
            bands, prev = [], -1
            for bi, b in enumerate(tab.get("bands", [])):
                bp = jp(jp(path, "bands"), bi)
                fr = self.unit(jp(bp, "from"), b.get("from", 0))
                if (bi == 0 and fr != 0) or fr <= prev:
                    self.e(jp(bp, "from"), "bands start at 0 and their \"from\" values ascend")
                prev = fr
                lay = b.get("layers", "all")
                if lay == "all":
                    lm = 15
                else:
                    lm = 0
                    for li, tn in enumerate(lay):
                        if tn not in self.tname:
                            self.e(jp(jp(bp, "layers"), li), f"{json.dumps(tn)} is not a track")
                        else:
                            lm |= 1 << self.tname[tn]
                play = [0xFFFF] * 3
                for k, v in b.items():
                    if k in self.tname and self.tname[k] < TRK_DRUM:
                        play[self.tname[k]] = self.mask16(jp(bp, k), v)
                    elif k not in ("from", "layers", "drums", "density", "fills", "ratchets", "notes"):
                        self.e(jp(bp, k), "unknown key (from, layers, drums, density, fills, ratchets, or a synth "
                                          "track name with its 16-step play mask)")
                bands.append({"from": fr, "layers": lm, "fills": bool(b.get("fills", False)),
                              "ratchets": bool(b.get("ratchets", False)),
                              "lanes": self.lanes_mask(jp(bp, "drums"), b.get("drums", "all")),
                              "dens": self.mask16(jp(bp, "density"), b["density"]) if "density" in b else 0xFFFF,
                              "play": play, "path": bp})
            self.esrc[name] = {"dlanes": dl, "bands": bands}
        self.energy, self.energy_ix = [], {}

    def energy_use(self, name):
        if name not in self.energy_ix:
            self.energy_ix[name] = len(self.energy)
            self.energy.append(self.esrc[name])
        return self.energy_ix[name]

    # ---- smart keys
    def keys_src(self):
        sk = self.s.get("smart_keys")
        path = "$.smart_keys"
        if sk is None:
            self.e(path, "missing (one synth track plays the Smart Keys and the user loop)")
            self.keys = {"trk": 0, "mode": 0, "mask": 1, "white": 0, "black": 0, "tonic": 60, "loop": NONE}
            self.keys_range = None
            return
        tn = sk.get("track")
        t = self.tname.get(tn)
        if t is None or t == TRK_DRUM:
            self.e(jp(path, "track"), f"{json.dumps(tn)}: a synth track's name")
            t = 0
        mode = self.enum_ix(jp(path, "mode"), sk.get("mode", "melody"), KMODE_NAMES, "keys mode")
        ms = sk.get("melody_scale")
        if ms is None:
            for cand in ("PEN", "MPEN"):
                cm = PR.scale_mask(PR.scale_id[cand])
                if cm & self.smask == cm:
                    mask = cm
                    break
            else:
                mask = self.smask
        elif isinstance(ms, list):
            mask = 0
            for x in ms:
                if not isinstance(x, int) or not 0 <= x <= 11:
                    self.e(jp(path, "melody_scale"), "a scale name or a list of semitones 0..11")
                else:
                    mask |= 1 << x
        else:
            mask = PR.scale_mask(self.enum_ix(jp(path, "melody_scale"), ms, [x["name"] for x in PR.scales],
                                              "scale"))
        if not mask & 1 or mask & ~self.smask & 0xFFF:
            self.e(jp(path, "melody_scale"), "the melody scale holds the tonic and stays inside the World scale "
                                             f"({PR.scales[self.scale]['name']})")
        tonic = sk.get("tonic")
        if tonic is None:
            tn_ = 60 + (self.root + 6) % 12 - 6
        else:
            tn_ = parse_note(tonic) if isinstance(tonic, str) else None
            if tn_ is None:
                self.e(jp(path, "tonic"), f"{json.dumps(tonic)}: a note name such as A3")
                tn_ = 60
            elif tn_ % 12 != self.root:
                self.e(jp(path, "tonic"), f"{tonic} is not the key's root ({NOTE_NAMES[self.root]})")
        rng = sk.get("range")
        self.keys_range = None
        if rng is not None:
            lo, hi = (parse_note(x) if isinstance(x, str) else None for x in rng)
            if lo is None or hi is None or lo > hi or not lo <= tn_ <= hi:
                self.e(jp(path, "range"), "two note names, low <= high, around the tonic")
            else:
                self.keys_range = (lo, hi)
        arp = range(PR.pid["amode"], PR.pid["aorder"] + 1)
        for pid, _ in self.trk[t]["pairs"]:
            if pid in arp:
                self.e(jp(jp(jp("$.tracks", t), "sound"), "params"),
                       f"{PR.P[pid]['name']}: the Smart Keys track's arpeggiator belongs to PULSE (defaults.pulse)")
        self.keys = {"trk": t, "mode": mode, "mask": mask,
                     "white": self.enum_ix(jp(path, "white"), sk.get("white", "scale"), WHITE_NAMES, "white mode"),
                     "black": self.enum_ix(jp(path, "black"), sk.get("black", "chord"), BLACK_NAMES, "black mode"),
                     "tonic": tn_, "loop": NONE}

    # ---- SCENES
    def scoped_pairs(self, path, params, fx, where):
        out = []
        for k, v in (params or {}).items():
            kp = jp(path + ".params", k)
            if "." not in k:
                self.e(kp, "a target track.param, *.param or g.name")
                continue
            tn, pn = k.split(".", 1)
            if tn == "g":
                r = self.gvalue(kp, pn, v, where)
                if r:
                    out.append((F["WF_SCOPE_G"],) + r)
                continue
            if tn == "*":
                ts = [i for i in range(TRK_DRUM)]
                if pn not in PR.pid or PR.pid[pn] >= PR.P_E0:
                    self.e(kp, "* takes a common parameter (engine EDIT labels differ per engine)")
                    continue
            elif tn in self.tname:
                ts = [self.tname[tn]]
            else:
                self.e(kp, f"{json.dumps(tn)} is not a track")
                continue
            for t in ts:
                r = self.param_target(kp, t, pn, v, where)
                if r:
                    out.append((F["WF_SCOPE_G"], r[1], r[2]) if r[0] == "g" else (t, r[1], r[2]))
        for k, v in (fx or {}).items():
            r = self.gvalue(jp(path + ".fx", k), k, v, where)
            if r:
                out.append((F["WF_SCOPE_G"],) + r)
        self.dedupe(path, out)
        return out

    def scenes(self):
        src = self.s.get("scenes", {})
        self.inst, self.pool, self.pool_ix, self.used_pat = {}, [], {}, set()
        self.sc = []
        kt = self.keys["trk"]
        for key in SCENE_KEYS:
            path = jp("$.scenes", key)
            s = src.get(key)
            if s is None:
                self.e(path, "every World has scenes A, B, C and D")
                continue
            rec = {"name": self.name_str(jp(path, "name"), s.get("name", ""), F["WF_LABEL_LEN"] - 1),
                   "role": self.enum_ix(jp(path, "role"), s.get("role"), SROLE_NAMES, "scene role")}
            pn = s.get("progression")
            if pn not in self.prog_src:
                self.e(jp(path, "progression"), f"{json.dumps(pn)} is not a progression")
                continue
            rec["prog"] = self.prog_use(pn)
            en = s.get("energy")
            if en is None:
                rec["energy"] = NONE
            elif en not in self.esrc:
                self.e(jp(path, "energy"), f"{json.dumps(en)} is not an energy table")
                rec["energy"] = NONE
            else:
                rec["energy"] = self.energy_use(en)
            tr = s.get("transition", 1)
            if isinstance(tr, str):              # "bar" = 1; "phrase": the playing progression's length (0)
                tr = {"bar": 1, "phrase": F["WF_TRANS_PHRASE"]}.get(tr)
            elif isinstance(tr, bool) or tr not in F["WF_TRANSITIONS"]:
                tr = None
            if tr is None:
                self.e(jp(path, "transition"), '1, 2 or 4 bars, "bar" or "phrase"')
                tr = 1
            rec["transition"] = tr
            pats = s.get("patterns", {})
            rec["pat"] = [NONE] * 3
            rec["beat"] = [NONE] * 4
            for tn in pats:
                if tn not in self.tname:
                    self.e(jp(jp(path, "patterns"), tn), f"{json.dumps(tn)} is not a track")
            # the pool is filled in a canonical order (tracks, then BEATs in their order, then the fill), so
            # that the order of the keys in the source does not change the blob
            for t in range(NTRK):
                tn = self.trk[t]["name"]
                pv = pats.get(tn)
                pp = jp(jp(path, "patterns"), tn)
                if pv is None:
                    continue
                if t == kt:
                    self.e(pp, "the Smart Keys track plays the user loop: it has no scene pattern")
                    continue
                if t == TRK_DRUM:
                    if isinstance(pv, str):
                        pv = {"GROOVE": pv}
                    for bn in pv:
                        if bn not in BEAT_NAMES:
                            self.e(jp(pp, bn), f"{json.dumps(bn)} is not a BEAT ({', '.join(BEAT_NAMES)})")
                    for bi, bn in enumerate(BEAT_NAMES):
                        if pv.get(bn) is not None:
                            rec["beat"][bi] = self.pat_ref(jp(pp, bn), pv[bn], t, rec["prog"])
                else:
                    rec["pat"][t] = self.pat_ref(pp, pv, t, rec["prog"])
            fill = s.get("fill")
            rec["fill"] = NONE if fill is None else self.pat_ref(jp(path, "fill"), fill, TRK_DRUM, rec["prog"])
            rec["pairs"] = self.scoped_pairs(path, s.get("params"), s.get("fx"), "scene")
            self.sc.append(rec)

    def pat_ref(self, path, name, t, prog):
        if name not in self.psrc:
            self.e(path, f"{json.dumps(name)} is not a pattern")
            return NONE
        if self.psrc[name]["t"] != t:
            self.e(path, f"pattern {name} belongs to track {self.trk[self.psrc[name]['t']]['name']}")
            return NONE
        return self.resolve(path, name, prog)

    # ---- VARS
    # a variation keeps the World's identity (spec 6): the format leaves it no tempo, key, scale, progression, swing,
    # drum pattern or Smart Keys setting to change (root / scale are fixed parameters, swing NOVAR, the rest
    # structural; the schema's "x-refused" names the fields someone might try, with the reason)
    VAR_GROOVE_TRACKS = 2                # more tracks of the groove changed at once (patterns, the drum kit): a warning

    def same_class(self, a, b):
        """pool entries a, b: the same length, or whole bars of which one divides the other"""
        la, lb = (Fraction(len(self.pool[x]["steps"]), PR.div_den[self.pool[x]["div"]]) for x in (a, b))
        bars = la % 4 == 0 and lb % 4 == 0
        return la == lb or (bars and (la % lb == 0 or lb % la == 0))

    def variations(self):
        src = self.s.get("variations", {"ORIGINAL": {}})
        self.vr = []
        for vi, (name, v) in enumerate(src.items()):
            path = jp("$.variations", name)
            if vi == 0 and (name != "ORIGINAL" or v):
                self.e(path, "the first variation is \"ORIGINAL\" and empty")
            rec = {"name": self.name_str(path, name, F["WF_LABEL_LEN"] - 1), "sounds": [], "swaps": []}
            b = v.get("energy_bias", 0)
            if not isinstance(b, (int, float)) or isinstance(b, bool) or abs(b) > 0.25:
                self.e(jp(path, "energy_bias"), "-0.25..0.25")
                b = 0
            rec["bias"] = int(round(b * UNIT))
            for tn, ps in v.get("sounds", {}).items():
                sp = jp(jp(path, "sounds"), tn)
                if tn not in self.tname:
                    self.e(sp, f"{json.dumps(tn)} is not a track")
                    continue
                t = self.tname[tn]
                if t == TRK_DRUM:
                    rec["sounds"].append((t, self.kit_ix(sp, ps)))
                else:
                    rec["sounds"].append((t, self.preset_ix(sp, self.trk[t]["engine"], ps)))
            swap_to = {}
            for fr, to in v.get("swap", {}).items():
                sp = jp(jp(path, "swap"), fr)
                if fr not in self.psrc or to not in self.psrc:
                    self.e(sp, f"{json.dumps(fr)} -> {json.dumps(to)}: both must be patterns")
                    continue
                if self.psrc[fr]["drum"] or self.psrc[fr]["t"] != self.psrc[to]["t"]:
                    self.e(sp, "a swap replaces a synth pattern by another of the same track (drums: BEAT)")
                    continue
                hit = False
                for s in self.sc:
                    pg = s["prog"]
                    a = self.inst.get((fr, pg if self.psrc[fr]["chordy"] else None))
                    if a is None or a not in s["pat"]:
                        continue
                    bix = self.resolve(sp, to, pg)
                    hit = True
                    # one {from, to} per pool entry, and the firmware takes the first match: a source shared by
                    # scenes over different progressions (no chord tokens, or the same unrolled steps) cannot
                    # swap to a target that resolves differently over them
                    if swap_to.setdefault(a, bix) != bix:
                        self.e(sp, f"{fr} -> {to}: {fr} is one pattern in scenes over different progressions, "
                                   f"where {to} (chord tokens) differs; a swap maps one pattern to one. Give {fr} "
                                   f"chord tokens, or swap a pattern that plays over one progression only")
                        break
                    if bix != NONE and not self.same_class(a, bix):
                        la, lb = (len(self.pool[x]["steps"]) / PR.div_den[self.pool[x]["div"]] for x in (a, bix))
                        self.e(sp, f"{fr} -> {to}: a swap keeps the pattern's length class ({fr}: {la:g} beats, {to}: "
                                   f"{lb:g}): the same length, or whole bars that divide each other (1, 2, 4 bars), "
                                   f"so the groove stays in phase")
                        break
                    if a != bix and (a, bix) not in rec["swaps"]:
                        rec["swaps"].append((a, bix))
                if not hit:
                    self.w(sp, f"no scene plays {fr}: the swap does nothing")
            rec["pairs"] = self.scoped_pairs(path, v.get("params"), v.get("fx"), "var")
            for cn, x in (v.get("macros") or {}).items():   # macro defaults (Phase 12): {WF_SCOPE_CTL, ctl, u8 position}
                mp = jp(jp(path, "macros"), cn)
                if cn not in CTL_NAMES[:4]:
                    self.e(mp, f"a macro default is one of {', '.join(CTL_NAMES[:4])}")
                elif not isinstance(x, (int, float)) or isinstance(x, bool) or not 0 <= x <= 1:
                    self.e(mp, "a position 0..1")
                else:
                    u = int(round(x * UNIT))
                    rec["pairs"].append((F["WF_SCOPE_CTL"], CTL_NAMES.index(cn), u - 256 if u > 127 else u))
            groove = {self.pool[a]["t"] for a, _ in rec["swaps"]} | {t for t, _ in rec["sounds"] if t == TRK_DRUM}
            if len(groove) > self.VAR_GROOVE_TRACKS:
                self.w(path, f"changes {len(groove)} tracks of the groove at once ("
                             f"{', '.join(self.trk[t]['name'] if t < TRK_DRUM else 'the drum kit' for t in sorted(groove))}"
                             f"): it may not sound like the same World")
            self.vr.append(rec)
        if not 1 <= len(self.vr) <= F["WF_MAX_VARS"]:
            self.e("$.variations", f"1..{F['WF_MAX_VARS']} variations")

    # ---- curves, MAPS, RULES
    def curves_src(self):
        self.curve_names = {}
        self.curves = []
        for name, c in self.s.get("curves", {}).items():
            lut = self.lut(jp("$.curves", name), c)
            if lut is not None:
                self.curve_names[name] = lut

    def lut(self, path, c):
        if isinstance(c, dict) and "lut" in c:
            lut = c["lut"]
            if len(lut) != 9 or any(not isinstance(x, int) or not 0 <= x <= 255 for x in lut):
                self.e(path, "lut: 9 integers 0..255")
                return None
        elif isinstance(c, dict) and "points" in c:
            pts = c["points"]
            if (len(pts) < 2 or pts[0][0] != 0 or pts[-1][0] != 1 or
                    any(pts[i][0] >= pts[i + 1][0] for i in range(len(pts) - 1))):
                self.e(path, "points: [x, y] pairs, x ascending from 0 to 1")
                return None
            lut = []
            for k in range(9):
                x = k / 8
                for i in range(len(pts) - 1):
                    (x0, y0), (x1, y1) = pts[i], pts[i + 1]
                    if x0 <= x <= x1:
                        lut.append(int(round((y0 + (y1 - y0) * (x - x0) / (x1 - x0)) * 255)))
                        break
        elif isinstance(c, list) and len(c) == 9:
            lut = [int(round(x * 255)) for x in c]
        else:
            self.e(path, "a curve is 9 values 0..1 (x = 0, 1/8 .. 1), {\"points\": [[x, y], ...]} or {\"lut\": [...]}")
            return None
        if lut[0] != 0 or lut[8] != 255 or any(not 0 <= x <= 255 for x in lut):
            self.e(path, "a curve runs from 0 (at x = 0) to 1 (at x = 1)")
            return None
        return lut

    def curve_ix(self, path, c):
        if c is None:
            return 0
        if isinstance(c, str) and c in CURVE_NAMES:
            return CURVE_NAMES.index(c)
        if isinstance(c, str):
            if c not in self.curve_names:
                self.e(path, f"{json.dumps(c)} is neither a built-in curve ({', '.join(CURVE_NAMES)}) nor in curves")
                return 0
            lut = self.curve_names[c]
        else:
            lut = self.lut(path, c)
            if lut is None:
                return 0
        if lut not in self.curves:
            self.curves.append(lut)
        if len(self.curves) > F["WF_MAX_CURVES"]:
            self.e(path, f"more than {F['WF_MAX_CURVES']} custom curves")
        return F["WF_CURVE_CUSTOM"] + self.curves.index(lut)

    def target(self, path, to, what):
        """'pad.CUT' / '*.level' / 'pad+bass.rev' / 'keys.@RESO' / 'pad.~bright' / 'g.dfdbk' ->
        (kind, mask, id, stepped)"""
        if not isinstance(to, str) or "." not in to:
            self.e(path, f"{json.dumps(to)}: a target track.param, *.param, track.@ROLE, track.~bright, g.name")
            return None
        tp, pn = to.split(".", 1)
        if tp == "g":
            gid = PR.gid.get(pn)
            if gid is None or gid not in G_WHITE or gid in G_STRUCT:
                self.e(path, f"{json.dumps(pn)} is not a global a {what} may move "
                             f"({', '.join(PR.G[g]['name'] for g in G_WHITE if g not in G_STRUCT)})")
                return None
            return (F["WF_K_GLOBAL"], 0, gid, False)
        if tp == "*":
            ts = [0, 1, 2]
        else:
            ts = []
            for tn in tp.split("+"):
                if tn not in self.tname:
                    self.e(path, f"{json.dumps(tn)} is not a track")
                    return None
                ts.append(self.tname[tn])
        mask = sum(1 << t for t in set(ts))
        if pn in ("~bright", "~shape") or pn.startswith("@"):
            if TRK_DRUM in ts:
                self.e(path, f"{pn} is for synth tracks")
                return None
            if pn.startswith("@"):
                if pn[1:] not in EROLE_NAMES:
                    self.e(path, f"{json.dumps(pn)}: engine roles are @{', @'.join(EROLE_NAMES)}")
                    return None
                ri = EROLE_NAMES.index(pn[1:])
                for t in ts:
                    eng = PR.engines[self.trk[t]["engine"]]
                    if eng["roles"][pn[1:]] is None:
                        self.w(path, f"{eng['name']} (track {self.trk[t]['name']}) has no {pn[1:]}: skipped there")
                return (F["WF_K_ROLE"], mask, ri, False)
            return (F["WF_K_BRIGHT"] if pn == "~bright" else F["WF_K_SHAPE"], mask, 0, False)
        if TRK_DRUM in ts and pn in ("level", "rev"):
            if len(ts) > 1:
                self.e(path, "the drum track's level and rev are globals (g.drlvl, g.drrev): target it alone")
                return None
            return (F["WF_K_GLOBAL"], 0, G_DRLVL if pn == "level" else G_DRREV, False)
        pids = set()
        for t in ts:
            r = self.resolve_tparam(path, t, pn)
            if r is None or isinstance(r, tuple):
                return None
            if self.structural(t, r):
                self.e(path, f"{pn} is structural: never a {what} target")
                return None
            pids.add(r)
        if len(pids) != 1 or (len(ts) > 1 and min(pids) >= PR.P_E0):
            self.e(path, "several tracks take a common parameter (engine EDIT labels differ per engine)")
            return None
        pid = pids.pop()
        stepped = any(self.tdesc(t, pid)["fmt"] == "ENUM" for t in ts)
        return (F["WF_K_PARAM"], mask, pid, stepped)

    def mapping(self, path, ctl, m):
        tg = self.target(jp(path, "to"), m.get("to"), "macro")
        if tg is None:
            return None
        kind, mask, tid, stepped = tg
        mn, mx = m.get("min", 0), m.get("max", 0)
        for k, v in (("min", mn), ("max", mx)):
            if not isinstance(v, int) or isinstance(v, bool) or not -128 <= v <= 127:
                self.e(jp(path, k), f"{json.dumps(v)}: an integer offset -128..127")
                return None
        if F["WF_CTL_HOME"][ctl] == 0 and mn != 0:
            self.e(jp(path, "min"), f"{CTL_NAMES[ctl]} rests at 0: only \"max\" applies")
        if mn == 0 and mx == 0:
            self.w(path, "min and max are 0: the mapping does nothing")
        smooth = m.get("smooth", CLASS_NAMES[F["WF_CLASS_DEFAULT"]])
        cls = F["WF_CLASS_STEPPED"] if stepped else self.enum_ix(jp(path, "smooth"), smooth, CLASS_NAMES, "smoothing")
        return (ctl, kind, mask, tid, cls, self.curve_ix(jp(path, "curve"), m.get("curve")), mn, mx)

    def macros(self):
        self.maps = []
        self.map_src = []                # per mapping: its JSON path, "saturate" (the model's checks)
        for group, base in (("macros", 0), ("controls", 4)):
            for cn, lst in self.s.get(group, {}).items():
                path = jp(f"$.{group}", cn)
                names = CTL_NAMES[:4] if base == 0 else CTL_NAMES[4:]
                if cn not in names:
                    self.e(path, f"{json.dumps(cn)} is not one of {', '.join(names)}")
                    continue
                ctl = CTL_NAMES.index(cn)
                for i, m in enumerate(lst):
                    r = self.mapping(jp(path, i), ctl, m)
                    if r:
                        self.maps.append(r)
                        self.map_src.append((jp(path, i), bool(isinstance(m, dict) and m.get("saturate"))))
        if len(self.maps) > F["WF_MAX_MAPS"]:
            self.e("$.macros", f"{len(self.maps)} mappings (with controls); at most {F['WF_MAX_MAPS']}")

    def rules(self):
        self.rl = []
        src = self.s.get("rules", [])
        if len(src) > F["WF_MAX_RULES"]:
            self.e("$.rules", f"at most {F['WF_MAX_RULES']} rules")
        for i, r in enumerate(src):
            path = jp("$.rules", i)
            cond = list(r.get("if", {}).items())
            if not 1 <= len(cond) <= 2:
                self.e(jp(path, "if"), "one or two conditions {CONTROL: threshold}")
                continue
            cs = []
            for cn, th in cond:
                if cn not in CTL_NAMES:
                    self.e(jp(jp(path, "if"), cn), f"{json.dumps(cn)} is not a control")
                    break
                t = self.unit(jp(jp(path, "if"), cn), th, 0, F["WF_THRESH_MAX"] / UNIT)
                cs.append((CTL_NAMES.index(cn), t))
            else:
                if len(cs) == 1:
                    cs.append(cs[0])
                acts = []
                for j, a in enumerate(r.get("then", [])):
                    ap = jp(jp(path, "then"), j)
                    tg = self.target(jp(ap, "to"), a.get("to"), "rule")
                    add = a.get("add")
                    if not isinstance(add, int) or isinstance(add, bool) or not -128 <= add <= 127:
                        self.e(jp(ap, "add"), f"{json.dumps(add)}: an integer -128..127")
                        continue
                    if tg:
                        acts.append((tg[0], tg[1], tg[2], add))
                if not 1 <= len(acts) <= F["WF_MAX_ACTS"]:
                    self.e(jp(path, "then"), f"1..{F['WF_MAX_ACTS']} actions")
                    continue
                self.rl.append({"a": cs[0][0], "ta": cs[0][1], "b": cs[1][0], "tb": cs[1][1], "acts": acts})

    # ---- GUARD
    def guard(self):
        g = self.s.get("guard", {})
        fix = [NONE] * F["WF_GUARD_FIX"]
        used = False

        def setf(off, path, v, lo=None, hi=None):
            nonlocal used
            lo = F["WF_GUARD_MIN"][off - F["WF_G_POLY"]] if lo is None else lo
            hi = F["WF_GUARD_MAX"][off - F["WF_G_POLY"]] if hi is None else hi
            if not isinstance(v, int) or isinstance(v, bool) or not lo <= v <= hi:
                self.e(path, f"{json.dumps(v)}: an integer {lo}..{hi}")
                return
            fix[off] = v
            used = True

        kt = self.keys["trk"]
        for tn, nt in g.get("notes", {}).items():
            path = jp("$.guard.notes", tn)
            t = self.tname.get(tn)
            if t is None or t == TRK_DRUM:
                self.e(path, f"{json.dumps(tn)}: a synth track's name")
                continue
            if "range" in nt:
                lo, hi = (parse_note(x) if isinstance(x, str) else None for x in nt["range"])
                if lo is None or hi is None or lo > hi:
                    self.e(jp(path, "range"), "two note names, low <= high")
                else:
                    if t == kt and self.keys_range and self.keys_range != (lo, hi):
                        self.e(jp(path, "range"), "differs from smart_keys.range")
                    fix[2 * t], fix[2 * t + 1] = lo, hi
                    used = True
            for k, off, names in (("loop_follow", F["WF_G_FOLLOW"], FOLLOW_NAMES),
                                  ("avoid", F["WF_G_AVOID"], AVOID_NAMES)):
                if k in nt:
                    if t != kt:
                        self.e(jp(path, k), "applies to the Smart Keys track only")
                    else:
                        fix[off] = self.enum_ix(jp(path, k), nt[k], names, k)
                        used = True
            if "max_poly" in nt:
                if t != kt:
                    self.e(jp(path, "max_poly"), "applies to the Smart Keys track only")
                else:
                    setf(F["WF_G_POLY"], jp(path, "max_poly"), nt["max_poly"])
            for k in nt:
                if k not in ("range", "loop_follow", "avoid", "max_poly", "notes"):
                    self.e(jp(path, k), "unknown key (range, max_poly, loop_follow, avoid)")
        if self.keys_range and fix[2 * kt] == NONE:
            fix[2 * kt], fix[2 * kt + 1] = self.keys_range
            used = True
        rec = g.get("record", {})
        if "quantize" in rec:
            fix[F["WF_G_QUANT"]] = self.unit("$.guard.record.quantize", rec["quantize"])
            used = True
        if "max_notes" in rec:
            setf(F["WF_G_RECNOTES"], "$.guard.record.max_notes", rec["max_notes"])
        snd = g.get("sound", {})
        for k, off in (("max_level", "WF_G_MAXLEVEL"), ("max_reso", "WF_G_MAXRESO"), ("max_dfdbk", "WF_G_MAXDFDBK"),
                       ("max_rsize", "WF_G_MAXRSIZE"), ("max_dist", "WF_G_MAXDIST"), ("max_dust", "WF_G_MAXDUST")):
            if k in snd:
                setf(F[off], jp("$.guard.sound", k), snd[k])
        ranges = []
        for k, lh in snd.get("ranges", {}).items():
            path = jp("$.guard.sound.ranges", k)
            tg = self.target(path, k, "guard range")
            if not tg:
                continue
            if (not isinstance(lh, list) or len(lh) != 2 or any(not isinstance(x, int) or isinstance(x, bool) or
                                                                not -128 <= x <= 127 for x in lh) or lh[0] > lh[1]):
                self.e(path, "[lo, hi]: integers -128..127, lo <= hi")
                continue
            ranges.append((tg[0], tg[1], tg[2], lh[0], lh[1]))
        if len(ranges) > F["WF_MAX_GRANGES"]:
            self.e("$.guard.sound.ranges", f"at most {F['WF_MAX_GRANGES']}")
        combos = None
        if "combos" in snd:
            combos = []
            src = snd["combos"]
            if not isinstance(src, list) or len(src) > F["WF_MAX_COMBOS"]:
                self.e("$.guard.sound.combos", f"a list of at most {F['WF_MAX_COMBOS']} combinations")
                src = []
            for i, cb in enumerate(src):
                path = jp("$.guard.sound.combos", i)
                when, cap = (cb.get("when"), cb.get("cap")) if isinstance(cb, dict) else (None, None)
                if not isinstance(when, dict) or not 1 <= len(when) <= 2 or not isinstance(cap, dict) or len(cap) != 1:
                    self.e(path, '{"when": {TARGET: over, ...} (one or two), "cap": {TARGET: value}}')
                    continue
                rec = []
                for key, (to, v) in [("when", x) for x in when.items()] + [("cap", x) for x in cap.items()]:
                    tp = jp(jp(path, key), to)
                    tg = self.target(tp, to, "guard combination")
                    if not isinstance(v, int) or isinstance(v, bool) or not -128 <= v <= 127:
                        self.e(tp, f"{json.dumps(v)}: an integer -128..127")
                        tg = None
                    elif tg and tg[0] not in (F["WF_K_PARAM"], F["WF_K_ROLE"], F["WF_K_GLOBAL"]):
                        self.e(tp, "a combination names parameters, @roles or globals")
                        tg = None
                    if not tg:
                        break
                    rec.append((tg[0], tg[1], tg[2], v))
                else:
                    if len(rec) == 2:
                        rec.insert(1, rec[0])
                    combos.append(tuple(rec))
            fix[F["WF_G_COMBOS"]] = len(combos)
            used = True
        arr = g.get("arrangement", {})
        if "mute_change" in arr:
            mc = {"bar": 1, "2bars": 2}.get(arr["mute_change"], arr["mute_change"])
            setf(F["WF_G_MUTECHG"], "$.guard.arrangement.mute_change", mc)
        if "density_change" in arr:
            fix[F["WF_G_DENSCHG"]] = self.enum_ix("$.guard.arrangement.density_change", arr["density_change"],
                                                  DENSCHG_NAMES, "density change")
            used = True
        if "fills_every" in arr:
            setf(F["WF_G_FILLS"], "$.guard.arrangement.fills_every", arr["fills_every"])
        if "min_band_bars" in arr:
            setf(F["WF_G_BANDBARS"], "$.guard.arrangement.min_band_bars", arr["min_band_bars"])
        cpu = g.get("cpu", {})
        if "max_unison" in cpu:
            setf(F["WF_G_UNISON"], "$.guard.cpu.max_unison", cpu["max_unison"])
        if "grain_dens" in cpu:
            setf(F["WF_G_GRAINDENS"], "$.guard.cpu.grain_dens", cpu["grain_dens"])
        if "max_dist_tracks" in cpu:
            setf(F["WF_G_DISTTRK"], "$.guard.cpu.max_dist_tracks", cpu["max_dist_tracks"])
        if "ceiling" in cpu:
            c = cpu["ceiling"]
            if not isinstance(c, (int, float)) or isinstance(c, bool) or not 0 < c <= 1:
                self.e("$.guard.cpu.ceiling", "a fraction 0..1 of the audio interrupt's time")
            else:
                o = F["WF_G_CPU"] - F["WF_G_POLY"]
                fix[F["WF_G_CPU"]] = max(F["WF_GUARD_MIN"][o], min(F["WF_GUARD_MAX"][o], int(round(c * 256))))
                used = True
        self.gd = {"fix": fix, "ranges": ranges, "combos": combos or []} if used or ranges else None

    # ---- DEFAULTS
    def defaults(self):
        d = self.s.get("defaults", {})
        sc = d.get("scene")
        if sc is None:
            mains = [i for i, s in enumerate(self.sc) if s["role"] == SROLE_NAMES.index("main")]
            sc = mains[0] if mains else 0
        elif sc in SCENE_KEYS:
            sc = SCENE_KEYS.index(sc)
        else:
            self.e("$.defaults.scene", "A, B, C or D")
            sc = 0
        vn = d.get("variation")
        names = [v["name"] for v in self.vr]
        if vn is None:
            vi = 0
        elif vn in names:
            vi = names.index(vn)
        else:
            self.e("$.defaults.variation", f"{json.dumps(vn)} is not a variation")
            vi = 0

        def four(key, dflt):
            v = d.get(key, dflt)
            if not isinstance(v, list) or len(v) != 4:
                self.e(jp("$.defaults", key), "four positions 0..1")
                return [0] * 4
            return [self.unit(jp(jp("$.defaults", key), i), x) for i, x in enumerate(v)]

        self.df = {"scene": sc, "var": vi, "ctl": four("macros", [0.5] * 4),
                   "pulse": self.enum_ix("$.defaults.pulse", d.get("pulse", "OFF"), PULSE_NAMES, "PULSE"),
                   "beat": self.enum_ix("$.defaults.beat", d.get("beat", "GROOVE"), BEAT_NAMES, "BEAT"),
                   "shape": four("shape", [0, 0, 0, 0]), "move": four("movement", [0, 0, 0, 0.5])}
        kt = self.keys["trk"]
        for i, e in enumerate(self.energy):
            for b in e["bands"]:
                if not b["layers"] >> kt & 1:
                    self.e(jp(b["path"], "layers"), "the Smart Keys track is in every band")

    def unused(self):
        for name in self.psrc:
            if name not in self.used_pat:
                self.w(jp("$.patterns", name), "not used by any scene, fill or swap: not compiled")
        for name in self.prog_src:
            if self.prog_ix.get(name) is None:
                self.w(jp("$.progressions", name), "not used by any scene")
        for name in self.esrc:
            if name not in self.energy_ix:
                self.w(jp("$.energy", name), "not used by any scene")
        if len(self.pool) > F["WF_MAX_PAT"]:
            self.e("$.patterns", f"{len(self.pool)} patterns after unrolling chord tokens per progression; the "
                                 f"pool holds {F['WF_MAX_PAT']}: " +
                   ", ".join(f"{i}={p['src']}" for i, p in enumerate(self.pool)))
        if len(self.prog) > F["WF_MAX_PROG"]:
            self.e("$.progressions", f"at most {F['WF_MAX_PROG']} progressions are used")
        if len(self.energy) > F["WF_MAX_ENERGY"]:
            self.e("$.energy", f"at most {F['WF_MAX_ENERGY']} energy tables are used")

    def ir(self):
        return {
            "id_hash": self.id_hash, "flags": F["WF_F_USER"] if self.user else 0,
            "meta": {"name": self.name, "cat": self.cat, "blurb": self.blurb, "bpm": self.bpm,
                     "bpm_min": self.bpm_lo, "bpm_max": self.bpm_hi, "root": self.root, "scale": self.scale,
                     "swing": self.swing},
            "tracks": [{"role": t["role"], "engine": t["engine"], "preset": t["preset"], "register": t["register"],
                        "pairs": t["pairs"]} for t in self.trk],
            "globals": self.glob, "patterns": self.pool, "progs": [c for _, c in self.prog],
            "scenes": self.sc, "vars": self.vr, "maps": self.maps, "curves": self.curves, "rules": self.rl,
            "energy": self.energy, "guard": self.gd, "keys": self.keys, "defaults": self.df,
        }


# ============================================================== encoder ===
def enc_str(s, n):
    b = s.encode("latin-1") if isinstance(s, str) else bytes(s)
    return b[:n - 1].ljust(n, b"\0")


def enc_synth(steps):
    out = bytearray()
    rests = 0
    prev = None
    i = 0
    n = len(steps)
    while i < n:
        s = steps[i]
        if s.time == ST_REST or (s.time == ST_NOTE and not s.notes):
            rests += 1
            i += 1
            continue
        if s.time == ST_TIE:
            while rests:
                k = min(rests, 63)
                out.append(WF_R(F["WF_R_REST"], k))
                rests -= k
            k = 0
            while i < n and steps[i].time == ST_TIE and k < 63:
                k += 1
                i += 1
            out.append(WF_R(F["WF_R_TIE"], k))
            continue
        while rests > 63:
            out.append(WF_R(F["WF_R_REST"], 63))
            rests -= 63
        out.append(WF_R(F["WF_R_NOTE"], rests))
        rests = 0
        same = prev is not None and s.notes == prev
        nf = (0 if same else len(s.notes)) | (F["WF_NF_ACCENT"] if s.flags & SF_ACCENT else 0) | \
             (F["WF_NF_SLIDE"] if s.flags & SF_SLIDE else 0) | (F["WF_NF_LVLRAT"] if s.lvl or s.rat else 0) | \
             (F["WF_NF_VEL"] if s.vel else 0) | (F["WF_NF_MICRO"] if s.micro else 0)
        out.append(nf)
        if not same:
            out += bytes(s.notes)
        if s.lvl or s.rat:
            out += bytes([s.lvl, s.rat])
        if s.vel:
            out.append(s.vel)
        if s.micro:
            out.append(s.micro)
        prev = s.notes
        i += 1
    return bytes(out)


def WF_R(kind, arg):
    return kind << 6 | arg


def pack2(vals):
    """2-bit values, LSB first"""
    out = bytearray((len(vals) * 2 + 7) // 8)
    for i, v in enumerate(vals):
        out[i // 4] |= (v & 3) << (2 * (i % 4))
    return bytes(out)


def enc_drum(steps):
    n = len(steps)
    lanes = sorted({ln for s in steps for ln in s})
    out = bytearray([len(lanes)])
    for ln in lanes:
        hits = [(i, s[ln]) for i, s in enumerate(steps) if ln in s]
        lv = [h[1][0] for h in hits]
        rt = [h[1][1] for h in hits]
        hl, hr = any(lv), any(rt)
        out.append(ln | (F["WF_DL_LVL"] if hl else 0) | (F["WF_DL_RAT"] if hr else 0))
        bm = bytearray(n // 8)
        for i, _ in hits:
            bm[i >> 3] |= 1 << (i & 7)
        out += bm
        if hl:
            out += pack2(lv)
        if hr:
            out += pack2(rt)
    return bytes(out)


def i8(v):
    return struct.pack("<b", v)


def encode(ir):
    secs = []
    m = ir["meta"]
    secs.append((F["WF_S_META"], 1, enc_str(m["name"], F["WF_NAME_LEN"]) + enc_str(m["cat"], F["WF_CAT_LEN"]) +
                 enc_str(m["blurb"], F["WF_BLURB_LEN"]) +
                 bytes([m["bpm"], m["bpm_min"], m["bpm_max"], m["root"], m["scale"], m["swing"]])))
    tb = b""
    for t in ir["tracks"]:
        tb += bytes([t["role"], t["engine"], t["preset"], t["register"], len(t["pairs"])])
        for pid, v in t["pairs"]:
            tb += bytes([pid]) + i8(v)
    secs.append((F["WF_S_TRACKS"], len(ir["tracks"]), tb))
    if ir["globals"]:
        secs.append((F["WF_S_GLOBALS"], len(ir["globals"]), b"".join(bytes([g]) + i8(v) for g, v in ir["globals"])))
    if ir["patterns"]:
        pb = b""
        for p in ir["patterns"]:
            data = enc_drum(p["steps"]) if p["drum"] else enc_synth(p["steps"])
            pb += bytes([(F["WF_PAT_DRUM"] if p["drum"] else 0) | p["div"], len(p["steps"])]) + \
                struct.pack("<H", len(data)) + data
        secs.append((F["WF_S_PATTERNS"], len(ir["patterns"]), pb))
    secs.append((F["WF_S_PROGS"], len(ir["progs"]),
                 b"".join(bytes([len(c)]) + b"".join(bytes([r << 4 | q, b]) for r, q, b in c) for c in ir["progs"])))
    sb = b""
    for s in ir["scenes"]:
        sb += enc_str(s["name"], F["WF_LABEL_LEN"]) + bytes([s["role"], s["prog"], s["energy"], s["transition"],
                                                             s["fill"], *s["pat"], *s["beat"], len(s["pairs"])])
        sb += b"".join(bytes([sc, i]) + i8(v) for sc, i, v in s["pairs"])
    secs.append((F["WF_S_SCENES"], len(ir["scenes"]), sb))
    vb = b""
    for v in ir["vars"]:
        vb += enc_str(v["name"], F["WF_LABEL_LEN"]) + i8(v["bias"]) + bytes([len(v["sounds"])])
        vb += b"".join(bytes(x) for x in v["sounds"]) + bytes([len(v["swaps"])])
        vb += b"".join(bytes(x) for x in v["swaps"]) + bytes([len(v["pairs"])])
        vb += b"".join(bytes([sc, i]) + i8(val) for sc, i, val in v["pairs"])
    secs.append((F["WF_S_VARS"], len(ir["vars"]), vb))
    if ir["maps"]:
        secs.append((F["WF_S_MAPS"], len(ir["maps"]),
                     b"".join(bytes([c, k << 5 | msk, i, cl << 6 | cv]) + i8(mn) + i8(mx)
                              for c, k, msk, i, cl, cv, mn, mx in ir["maps"])))
    if ir["curves"]:
        secs.append((F["WF_S_CURVES"], len(ir["curves"]), b"".join(bytes(c) for c in ir["curves"])))
    if ir["rules"]:
        rb = b""
        for r in ir["rules"]:
            rb += bytes([r["a"] << 4 | r["b"], r["ta"], r["tb"], len(r["acts"])])
            rb += b"".join(bytes([k << 5 | msk, i]) + i8(a) + b"\0" for k, msk, i, a in r["acts"])
        secs.append((F["WF_S_RULES"], len(ir["rules"]), rb))
    if ir["energy"]:
        eb = b""
        for e in ir["energy"]:
            eb += struct.pack("<HB", e["dlanes"], len(e["bands"]))
            for b in e["bands"]:
                eb += bytes([b["from"], b["layers"] | (F["WF_B_FILLS"] if b["fills"] else 0) |
                             (F["WF_B_RATCHETS"] if b["ratchets"] else 0)])
                eb += struct.pack("<HH3H", b["lanes"], b["dens"], *b["play"])
        secs.append((F["WF_S_ENERGY"], len(ir["energy"]), eb))
    if ir["guard"]:
        g = ir["guard"]
        secs.append((F["WF_S_GUARD"], len(g["ranges"]),
                     bytes(g["fix"]) + b"".join(bytes([k << 5 | msk, i]) + i8(lo) + i8(hi)
                                                for k, msk, i, lo, hi in g["ranges"]) +
                     b"".join(bytes([k << 5 | msk, i]) + i8(v) for cb in g.get("combos", []) for k, msk, i, v in cb)))
    k = ir["keys"]
    secs.append((F["WF_S_KEYS"], 1, bytes([k["trk"], k["mode"]]) + struct.pack("<H", k["mask"]) +
                 bytes([k["white"], k["black"], k["tonic"], k["loop"]])))
    d = ir["defaults"]
    secs.append((F["WF_S_DEFAULTS"], 1, bytes([d["scene"], d["var"], *d["ctl"], d["pulse"], d["beat"],
                                               *d["shape"], *d["move"]])))
    table = b"".join(struct.pack("<BBH", t, c, len(p)) for t, c, p in secs)
    body = bytes([len(secs), 0, 0, 0]) + table + b"".join(p for _, _, p in secs)
    L = F["WF_HDR"] - 4 + len(body)
    head = F["WF_MAGIC"].encode() + bytes([F["WF_VERSION"], ir["flags"]]) + struct.pack("<HI", L, ir["id_hash"])
    blob = head + b"\0\0\0\0" + body
    return head + struct.pack("<I", blob_crc(blob, L)) + body


# ============================================================== decoder ===
class BlobError(Exception):
    def __init__(self, code, msg):
        super().__init__(f"WORLD ERROR {code} ({WE_NAMES[code]}): {msg}")
        self.code = code


def decode(b):
    """FWD1 bytes -> IR (the structural checks of world.c wb_check; it raises BlobError)"""
    def need(c, code, msg):
        if not c:
            raise BlobError(code, msg)

    need(len(b) >= F["WF_HDR"], F["WE_SIZE"], "shorter than the header")
    need(b[:4] == F["WF_MAGIC"].encode(), F["WE_MAGIC"], "not FWD1")
    need(b[4] <= F["WF_VERSION"] and b[4] >= 1, F["WE_VERSION"], f"version {b[4]}")
    need(not b[5] & ~F["WF_F_KNOWN"], F["WE_FLAGS"], f"flags {b[5]:#x}")
    L, wid, crc = struct.unpack_from("<HII", b, 6)
    need(F["WF_MIN_LEN"] <= L <= F["WF_MAX_LEN"] and L <= len(b), F["WE_SIZE"], f"L = {L}")
    need(blob_crc(b, L) == crc, F["WE_CRC"], "CRC")
    nsec = b[16]
    need(1 <= nsec <= F["WF_MAX_SEC"] and b[17:20] == b"\0\0\0", F["WE_HEADER"], "nsec / reserved")
    need(20 + 4 * nsec <= L, F["WE_TABLE"], "table")
    secs, off, prev = {}, 20 + 4 * nsec, 0
    for i in range(nsec):
        t, c, ln = struct.unpack_from("<BBH", b, 20 + 4 * i)
        need(prev < t <= F["WF_S_LAST"], F["WE_SECTION"], f"section type {t}")
        prev = t
        secs[t] = (c, off, ln)
        off += ln
    need(off == L, F["WE_TABLE"], "sum of lengths")
    need(all(t in secs for t in range(32) if F["WF_S_REQUIRED"] >> t & 1), F["WE_MISSING"], "a required section")

    def sec(t):
        c, o, ln = secs[t]
        return c, b[o:o + ln]

    ir = {"id_hash": wid, "flags": b[5]}
    c, s = sec(F["WF_S_META"])
    need(c == 1 and len(s) == F["WF_META_LEN"], F["WE_LENGTH"], "META")
    nl, cl, bl = F["WF_NAME_LEN"], F["WF_CAT_LEN"], F["WF_BLURB_LEN"]
    meta = {"name": s[:nl].split(b"\0")[0].decode("latin-1"), "cat": s[nl:nl + cl].split(b"\0")[0].decode("latin-1"),
            "blurb": s[nl + cl:nl + cl + bl].split(b"\0")[0]}
    meta.update(zip(("bpm", "bpm_min", "bpm_max", "root", "scale", "swing"), s[nl + cl + bl:]))
    ir["meta"] = meta
    c, s = sec(F["WF_S_TRACKS"])
    need(c == NTRK, F["WE_COUNT"], "TRACKS")
    tr, o = [], 0
    for i in range(c):
        need(o + 5 <= len(s), F["WE_LENGTH"], "TRACKS")
        role, eng, pre, reg, n = s[o:o + 5]
        need(o + 5 + 2 * n <= len(s), F["WE_LENGTH"], "TRACKS")
        pairs = [(s[o + 5 + 2 * k], struct.unpack_from("<b", s, o + 6 + 2 * k)[0]) for k in range(n)]
        tr.append({"role": role, "engine": eng, "preset": pre, "register": reg, "pairs": pairs})
        o += 5 + 2 * n
    need(o == len(s), F["WE_LENGTH"], "TRACKS")
    ir["tracks"] = tr
    ir["globals"] = []
    if F["WF_S_GLOBALS"] in secs:
        c, s = sec(F["WF_S_GLOBALS"])
        need(len(s) == 2 * c, F["WE_LENGTH"], "GLOBALS")
        ir["globals"] = [(s[2 * k], struct.unpack_from("<b", s, 2 * k + 1)[0]) for k in range(c)]
    ir["patterns"] = []
    if F["WF_S_PATTERNS"] in secs:
        c, s = sec(F["WF_S_PATTERNS"])
        need(1 <= c <= F["WF_MAX_PAT"], F["WE_COUNT"], "PATTERNS")
        o = 0
        for i in range(c):
            need(o + 4 <= len(s), F["WE_LENGTH"], "PATTERNS")
            kd, ln = s[o], s[o + 1]
            nb = struct.unpack_from("<H", s, o + 2)[0]
            need(o + 4 + nb <= len(s), F["WE_LENGTH"], "PATTERNS")
            data = s[o + 4:o + 4 + nb]
            drum = bool(kd & F["WF_PAT_DRUM"])
            div = kd & F["WF_PAT_DIVMASK"]
            need(not kd & 0x78 and div < F["WF_NDIV"] and 1 <= ln <= 64, F["WE_PATTERN"], f"pattern {i} header")
            steps = dec_drum(data, ln) if drum else dec_synth(data, ln)
            ir["patterns"].append({"drum": drum, "div": div, "steps": steps})
            o += 4 + nb
        need(o == len(s), F["WE_LENGTH"], "PATTERNS")
    c, s = sec(F["WF_S_PROGS"])
    progs, o = [], 0
    for i in range(c):
        need(o < len(s), F["WE_LENGTH"], "PROGS")
        n = s[o]
        need(o + 1 + 2 * n <= len(s), F["WE_LENGTH"], "PROGS")
        progs.append([(s[o + 1 + 2 * k] >> 4, s[o + 1 + 2 * k] & 15, s[o + 2 + 2 * k]) for k in range(n)])
        o += 1 + 2 * n
    need(o == len(s), F["WE_LENGTH"], "PROGS")
    ir["progs"] = progs
    c, s = sec(F["WF_S_SCENES"])
    need(c == F["WF_NSCENE"], F["WE_COUNT"], "SCENES")
    scs, o, LL = [], 0, F["WF_LABEL_LEN"]
    for i in range(c):
        need(o + F["WF_SCENE_HDR"] <= len(s), F["WE_LENGTH"], "SCENES")
        h = s[o:o + F["WF_SCENE_HDR"]]
        n = h[23]
        need(o + 24 + 3 * n <= len(s), F["WE_LENGTH"], "SCENES")
        scs.append({"name": h[:LL].split(b"\0")[0].decode("latin-1"), "role": h[11], "prog": h[12],
                    "energy": h[13], "transition": h[14], "fill": h[15], "pat": list(h[16:19]),
                    "beat": list(h[19:23]), "pairs": dec_spairs(s, o + 24, n)})
        o += 24 + 3 * n
    need(o == len(s), F["WE_LENGTH"], "SCENES")
    ir["scenes"] = scs
    c, s = sec(F["WF_S_VARS"])
    vrs, o = [], 0
    for i in range(c):
        need(o + 13 <= len(s), F["WE_LENGTH"], "VARS")
        name = s[o:o + LL].split(b"\0")[0].decode("latin-1")
        bias = struct.unpack_from("<b", s, o + 11)[0]
        na = s[o + 12]
        o += 13
        need(o + 2 * na + 1 <= len(s), F["WE_LENGTH"], "VARS")
        snds = [(s[o + 2 * k], s[o + 2 * k + 1]) for k in range(na)]
        o += 2 * na
        nb = s[o]
        o += 1
        need(o + 2 * nb + 1 <= len(s), F["WE_LENGTH"], "VARS")
        sw = [(s[o + 2 * k], s[o + 2 * k + 1]) for k in range(nb)]
        o += 2 * nb
        n = s[o]
        o += 1
        need(o + 3 * n <= len(s), F["WE_LENGTH"], "VARS")
        vrs.append({"name": name, "bias": bias, "sounds": snds, "swaps": sw, "pairs": dec_spairs(s, o, n)})
        o += 3 * n
    need(o == len(s), F["WE_LENGTH"], "VARS")
    ir["vars"] = vrs
    ir["maps"] = []
    if F["WF_S_MAPS"] in secs:
        c, s = sec(F["WF_S_MAPS"])
        need(len(s) == 6 * c, F["WE_LENGTH"], "MAPS")
        for k in range(c):
            ctl, tg, i, cc, mn, mx = struct.unpack_from("<BBBBbb", s, 6 * k)
            ir["maps"].append((ctl, tg >> 5, tg & 31, i, cc >> 6, cc & 63, mn, mx))
    ir["curves"] = []
    if F["WF_S_CURVES"] in secs:
        c, s = sec(F["WF_S_CURVES"])
        need(len(s) == 9 * c, F["WE_LENGTH"], "CURVES")
        ir["curves"] = [list(s[9 * k:9 * k + 9]) for k in range(c)]
    ir["rules"] = []
    if F["WF_S_RULES"] in secs:
        c, s = sec(F["WF_S_RULES"])
        o = 0
        for k in range(c):
            need(o + 4 <= len(s), F["WE_LENGTH"], "RULES")
            ab, ta, tb, n = s[o:o + 4]
            need(o + 4 + 4 * n <= len(s), F["WE_LENGTH"], "RULES")
            acts = []
            for j in range(n):
                tg, i, add, z = struct.unpack_from("<BBbB", s, o + 4 + 4 * j)
                acts.append((tg >> 5, tg & 31, i, add))
            ir["rules"].append({"a": ab >> 4, "b": ab & 15, "ta": ta, "tb": tb, "acts": acts})
            o += 4 + 4 * n
        need(o == len(s), F["WE_LENGTH"], "RULES")
    ir["energy"] = []
    if F["WF_S_ENERGY"] in secs:
        c, s = sec(F["WF_S_ENERGY"])
        o = 0
        for k in range(c):
            need(o + 3 <= len(s), F["WE_LENGTH"], "ENERGY")
            dl, nb = struct.unpack_from("<HB", s, o)
            need(o + 3 + 12 * nb <= len(s), F["WE_LENGTH"], "ENERGY")
            bands = []
            for j in range(nb):
                fr, lay, lanes, dens, p0, p1, p2 = struct.unpack_from("<BBHH3H", s, o + 3 + 12 * j)
                bands.append({"from": fr, "layers": lay & 15, "fills": bool(lay & 16), "ratchets": bool(lay & 32),
                              "lanes": lanes, "dens": dens, "play": [p0, p1, p2]})
            ir["energy"].append({"dlanes": dl, "bands": bands})
            o += 3 + 12 * nb
        need(o == len(s), F["WE_LENGTH"], "ENERGY")
    ir["guard"] = None
    if F["WF_S_GUARD"] in secs:
        c, s = sec(F["WF_S_GUARD"])
        need(len(s) >= 32, F["WE_LENGTH"], "GUARD")
        nc = 0 if s[F["WF_G_COMBOS"]] == NONE else s[F["WF_G_COMBOS"]]
        need(len(s) == 32 + 4 * c + F["WF_COMBO_LEN"] * nc, F["WE_LENGTH"], "GUARD")
        rg = []
        for k in range(c):
            tg, i, lo, hi = struct.unpack_from("<BBbb", s, 32 + 4 * k)
            rg.append((tg >> 5, tg & 31, i, lo, hi))
        cbs, o = [], 32 + 4 * c
        for k in range(nc):
            cbs.append(tuple((s[o + 3 * j] >> 5, s[o + 3 * j] & 31, s[o + 3 * j + 1],
                              struct.unpack_from("<b", s, o + 3 * j + 2)[0]) for j in range(3)))
            o += F["WF_COMBO_LEN"]
        ir["guard"] = {"fix": list(s[:32]), "ranges": rg, "combos": cbs}
    c, s = sec(F["WF_S_KEYS"])
    need(c == 1 and len(s) == 8, F["WE_LENGTH"], "KEYS")
    ir["keys"] = {"trk": s[0], "mode": s[1], "mask": struct.unpack_from("<H", s, 2)[0], "white": s[4],
                  "black": s[5], "tonic": s[6], "loop": s[7]}
    c, s = sec(F["WF_S_DEFAULTS"])
    need(c == 1 and len(s) == 16, F["WE_LENGTH"], "DEFAULTS")
    ir["defaults"] = {"scene": s[0], "var": s[1], "ctl": list(s[2:6]), "pulse": s[6], "beat": s[7],
                      "shape": list(s[8:12]), "move": list(s[12:16])}
    return ir


def dec_spairs(s, o, n):
    return [(s[o + 3 * k], s[o + 3 * k + 1], struct.unpack_from("<b", s, o + 3 * k + 2)[0]) for k in range(n)]


def dec_synth(d, ln):
    steps = [Step() for _ in range(ln)]
    c, i, prev = 0, 0, None
    while i < len(d):
        kind, arg = d[i] >> 6, d[i] & 63
        i += 1
        if kind == F["WF_R_END"]:
            if arg or i != len(d):
                raise BlobError(F["WE_PATTERN"], "END")
            break
        if kind in (F["WF_R_TIE"], F["WF_R_REST"]):
            if not arg or c + arg > ln:
                raise BlobError(F["WE_PATTERN"], "cursor")
            for k in range(arg):
                steps[c + k] = Step(ST_TIE if kind == F["WF_R_TIE"] else ST_REST)
            c += arg
            continue
        c += arg
        if c >= ln or i >= len(d):
            raise BlobError(F["WE_PATTERN"], "cursor")
        nf = d[i]
        i += 1
        n = nf & 7
        if n > 4 or (n == 0 and prev is None):
            raise BlobError(F["WE_PATTERN"], "note count")
        if n:
            if i + n > len(d):
                raise BlobError(F["WE_PATTERN"], "notes")
            notes = tuple(d[i:i + n])
            i += n
            if any(x > 127 for x in notes):
                raise BlobError(F["WE_NOTE"], "note")
        else:
            notes = prev
        extra = 2 * bool(nf & 32) + bool(nf & 64) + bool(nf & 128)
        if i + extra > len(d):
            raise BlobError(F["WE_PATTERN"], "step bytes")
        lvl = rat = vel = mic = 0
        if nf & 32:
            lvl, rat = d[i], d[i + 1]
            i += 2
        if nf & 64:
            vel = d[i]
            i += 1
        if nf & 128:
            mic = d[i]
            i += 1
        steps[c] = Step(ST_NOTE, notes, (SF_ACCENT if nf & 8 else 0) | (SF_SLIDE if nf & 16 else 0), lvl, rat, vel, mic)
        prev = notes
        c += 1
    return steps


def dec_drum(d, ln):
    if ln not in (16, 32, 64) or not d:
        raise BlobError(F["WE_PATTERN"], "drum length")
    steps = [dict() for _ in range(ln)]
    nl, i, prev = d[0], 1, -1
    for _ in range(nl):
        if i >= len(d):
            raise BlobError(F["WE_PATTERN"], "lanes")
        lb = d[i]
        i += 1
        ln_ = lb & 15
        if lb & 0xC0 or ln_ <= prev:
            raise BlobError(F["WE_PATTERN"], "lane order")
        prev = ln_
        bm = d[i:i + ln // 8]
        i += ln // 8
        if len(bm) != ln // 8:
            raise BlobError(F["WE_PATTERN"], "bitmap")
        hits = [k for k in range(ln) if bm[k >> 3] >> (k & 7) & 1]
        nb = (2 * len(hits) + 7) // 8
        lv = [0] * len(hits)
        rt = [0] * len(hits)
        for flag, arr in ((16, lv), (32, rt)):
            if lb & flag:
                pk = d[i:i + nb]
                if len(pk) != nb:
                    raise BlobError(F["WE_PATTERN"], "levels")
                for k in range(len(hits)):
                    arr[k] = pk[k // 4] >> (2 * (k % 4)) & 3
                i += nb
        for k, h in enumerate(hits):
            steps[h][ln_] = (lv[k], rt[k])
    if i != len(d):
        raise BlobError(F["WE_PATTERN"], "trailing bytes")
    return steps


# ============================================================ decompiler ===
def decompile(b):
    """FWD1 bytes -> a lowered World JSON (dict) that compiles back to the same bytes"""
    ir = decode(b)
    m, k = ir["meta"], ir["keys"]
    roles = [ROLE_NAMES[t["role"]] if t["role"] < len(ROLE_NAMES) else f"t{i}" for i, t in enumerate(ir["tracks"])]
    tn = []
    for r in roles:
        n, j = r, 2
        while n in tn:
            n, j = f"{r}{j}", j + 1
        tn.append(n)
    scale_names = [s["name"] for s in PR.scales]
    smask = PR.scale_mask(m["scale"])
    w = {"format": FORMAT_ID, "id": f"fnv_{ir['id_hash']:08x}", "name": m["name"], "category": m["cat"],
         "blurb": m["blurb"].decode("latin-1"),
         "tempo": {"bpm": m["bpm"], "min": m["bpm_min"], "max": m["bpm_max"]},
         "key": {"root": NOTE_NAMES[m["root"]], "scale": scale_names[m["scale"]]}, "swing": m["swing"]}
    eng = [t["engine"] for t in ir["tracks"]]

    def pname(t, pid):
        if t == TRK_DRUM or pid < PR.P_E0:
            return PR.P[pid]["name"]
        lab = PR.engines[eng[t]]["edit"][pid - PR.P_E0]["label"]
        return lab if lab != "-" else PR.P[pid]["name"]

    def pval(desc, v):
        if desc.get("names") and desc["fmt"] == "ENUM" and desc["min"] <= v <= desc["max"]:
            nm = desc["names"][v - desc["min"]]
            if [x.upper() for x in desc["names"]].count(nm.upper()) == 1:
                return nm
        return v

    def tdesc(t, pid):
        return PR.P[pid] if t == TRK_DRUM or pid < PR.P_E0 else PR.engines[eng[t]]["edit"][pid - PR.P_E0]

    tracks = []
    for i, t in enumerate(ir["tracks"]):
        rec = {"name": tn[i], "role": roles[i]}
        if i == TRK_DRUM:
            rec["sound"] = {"kit": PR.kits[t["preset"]]}
        else:
            rec["register"] = note_name(t["register"])
            rec["sound"] = {"engine": PR.engines[t["engine"]]["name"],
                            "preset": PR.engines[t["engine"]]["presets"][t["preset"]]}
        if t["pairs"]:
            rec["sound"]["params"] = {pname(i, pid): pval(tdesc(i, pid), v) for pid, v in t["pairs"]}
        tracks.append(rec)
    w["tracks"] = tracks
    fx = {PR.G[g]["name"]: pval(PR.G[g], v) for g, v in ir["globals"]}
    if fx:
        w["fx"] = fx
    pat_trk = {}
    for s in ir["scenes"]:
        for t, p in enumerate(s["pat"]):
            if p != NONE:
                pat_trk[p] = t
    for v in ir["vars"]:
        for a, bb in v["swaps"]:
            if a in pat_trk:
                pat_trk.setdefault(bb, pat_trk[a])
    if k["loop"] != NONE:
        pat_trk[k["loop"]] = k["trk"]
    pats = {}
    for i, p in enumerate(ir["patterns"]):
        if p["drum"]:
            lanes = {}
            for ln in sorted({x for s in p["steps"] for x in s}):
                lanes[PR.lanes[ln]["id"]] = "".join(DRUM_OUT.get(s[ln], "x") if ln in s else "." for s in p["steps"])
            pats[f"p{i}"] = {"track": tn[TRK_DRUM], "lanes": lanes}
        else:
            spb = 4 * PR.div_den[p["div"]]
            toks = [step_text(s) for s in p["steps"]]
            if len(toks) % spb == 0 and len(toks) > spb:
                txt = " | ".join(" ".join(toks[j:j + spb]) for j in range(0, len(toks), spb))
            else:
                txt = " ".join(toks)
            pats[f"p{i}"] = {"track": tn[pat_trk.get(i, 0)], "div": PR.divs[p["div"]], "steps": txt}
    w["patterns"] = pats
    dm = degree_mask(smask)
    w["progressions"] = {f"prog{i}": [roman(r, q, bt, dm) for r, q, bt in c] for i, c in enumerate(ir["progs"])}

    def spairs(pairs):
        params, fxs = {}, {}
        for sc, i, v in pairs:
            if sc == F["WF_SCOPE_G"]:
                fxs[PR.G[i]["name"]] = pval(PR.G[i], v)
            else:
                params[f"{tn[sc]}.{pname(sc, i)}"] = pval(tdesc(sc, i), v)
        return params, fxs

    scenes = {}
    for si, s in enumerate(ir["scenes"]):
        rec = {"name": s["name"], "role": SROLE_NAMES[s["role"]], "progression": f"prog{s['prog']}"}
        if s["energy"] != NONE:
            rec["energy"] = f"e{s['energy']}"
        if s["transition"] != 1:
            rec["transition"] = "phrase" if s["transition"] == F["WF_TRANS_PHRASE"] else s["transition"]
        pt = {}
        for t in range(3):
            if t != k["trk"]:
                pt[tn[t]] = f"p{s['pat'][t]}" if s["pat"][t] != NONE else None
        beats = {BEAT_NAMES[j]: f"p{x}" for j, x in enumerate(s["beat"]) if x != NONE}
        pt[tn[TRK_DRUM]] = beats or None
        rec["patterns"] = pt
        if s["fill"] != NONE:
            rec["fill"] = f"p{s['fill']}"
        params, fxs = spairs(s["pairs"])
        if params:
            rec["params"] = params
        if fxs:
            rec["fx"] = fxs
        scenes[SCENE_KEYS[si]] = rec
    w["scenes"] = scenes
    vs = {}
    for v in ir["vars"]:
        rec = {}
        if v["bias"]:
            rec["energy_bias"] = v["bias"] / UNIT
        if v["sounds"]:
            rec["sounds"] = {tn[t]: (PR.kits[p] if t == TRK_DRUM else PR.engines[eng[t]]["presets"][p])
                             for t, p in v["sounds"]}
        if v["swaps"]:
            sw = {}
            for a, bb in v["swaps"]:
                sw.setdefault(f"p{a}", f"p{bb}")
            rec["swap"] = sw
        params, fxs = spairs([p for p in v["pairs"] if p[0] != F["WF_SCOPE_CTL"]])
        if params:
            rec["params"] = params
        if fxs:
            rec["fx"] = fxs
        mc = {CTL_NAMES[i]: (x & 255) / UNIT for sc, i, x in v["pairs"] if sc == F["WF_SCOPE_CTL"]}
        if mc:
            rec["macros"] = mc
        vs[v["name"]] = rec
    w["variations"] = vs
    if ir["curves"]:
        w["curves"] = {f"c{i}": {"lut": c} for i, c in enumerate(ir["curves"])}

    def tgt(kind, mask, i):
        if kind == F["WF_K_GLOBAL"]:
            return f"g.{PR.G[i]['name']}"
        ts = [t for t in range(4) if mask >> t & 1]
        who = "*" if mask == 7 else "+".join(tn[t] for t in ts)
        if kind == F["WF_K_ROLE"]:
            return f"{who}.@{EROLE_NAMES[i]}"
        if kind in (F["WF_K_BRIGHT"], F["WF_K_SHAPE"]):
            return f"{who}.{'~bright' if kind == F['WF_K_BRIGHT'] else '~shape'}"
        return f"{who}.{pname(ts[0], i) if len(ts) == 1 else PR.P[i]['name']}"

    macros, controls = {}, {}
    for ctl, kind, mask, i, cl, cv, mn, mx in ir["maps"]:
        mp = {"to": tgt(kind, mask, i), "min": mn, "max": mx}
        if cv:
            mp["curve"] = CURVE_NAMES[cv] if cv < F["WF_CURVE_CUSTOM"] else f"c{cv - F['WF_CURVE_CUSTOM']}"
        if cl != F["WF_CLASS_DEFAULT"]:
            mp["smooth"] = CLASS_NAMES[cl]
        (macros if ctl < 4 else controls).setdefault(CTL_NAMES[ctl], []).append(mp)
    if macros:
        w["macros"] = macros
    if controls:
        w["controls"] = controls
    if ir["rules"]:
        rl = []
        for r in ir["rules"]:
            cond = {CTL_NAMES[r["a"]]: r["ta"] / UNIT}
            if (r["b"], r["tb"]) != (r["a"], r["ta"]):
                cond[CTL_NAMES[r["b"]]] = r["tb"] / UNIT
            rl.append({"if": cond, "then": [{"to": tgt(kd, msk, i), "add": a} for kd, msk, i, a in r["acts"]]})
        w["rules"] = rl

    def lanes_txt(m_):
        return "all" if m_ == 0xFFFF else [PR.lanes[ln]["id"] for ln in range(16) if m_ >> ln & 1]

    def m16(m_):
        return "".join("x" if m_ >> j & 1 else "." for j in range(16))

    if ir["energy"]:
        en = {}
        for ei, e in enumerate(ir["energy"]):
            bands = []
            for bd in e["bands"]:
                rec = {"from": bd["from"] / UNIT,
                       "layers": "all" if bd["layers"] == 15 else [tn[t] for t in range(4) if bd["layers"] >> t & 1],
                       "drums": lanes_txt(bd["lanes"])}
                if bd["dens"] != 0xFFFF:
                    rec["density"] = m16(bd["dens"])
                for t in range(3):
                    if bd["play"][t] != 0xFFFF:
                        rec[tn[t]] = m16(bd["play"][t])
                if bd["fills"]:
                    rec["fills"] = True
                if bd["ratchets"]:
                    rec["ratchets"] = True
                bands.append(rec)
            en[f"e{ei}"] = {"density_lanes": [] if not e["dlanes"] else lanes_txt(e["dlanes"]), "bands": bands}
            if en[f"e{ei}"]["density_lanes"] == "all":
                en[f"e{ei}"]["density_lanes"] = [x["id"] for x in PR.lanes]
        w["energy"] = en
    mel = [s["name"] for s in PR.scales if s["mask"] == k["mask"]]
    w["smart_keys"] = {"track": tn[k["trk"]], "mode": KMODE_NAMES[k["mode"]],
                       "melody_scale": mel[0] if mel else bits(k["mask"]), "white": WHITE_NAMES[k["white"]],
                       "black": BLACK_NAMES[k["black"]], "tonic": note_name(k["tonic"])}
    if ir["guard"]:
        g, fix = ir["guard"], ir["guard"]["fix"]
        gd = {}
        notes = {}
        for t in range(3):
            if fix[2 * t] != NONE:
                notes.setdefault(tn[t], {})["range"] = [note_name(fix[2 * t]), note_name(fix[2 * t + 1])]
        kk = tn[k["trk"]]
        if fix[F["WF_G_POLY"]] != NONE:
            notes.setdefault(kk, {})["max_poly"] = fix[F["WF_G_POLY"]]
        if fix[F["WF_G_FOLLOW"]] != NONE:
            notes.setdefault(kk, {})["loop_follow"] = FOLLOW_NAMES[fix[F["WF_G_FOLLOW"]]]
        if fix[F["WF_G_AVOID"]] != NONE:
            notes.setdefault(kk, {})["avoid"] = AVOID_NAMES[fix[F["WF_G_AVOID"]]]
        if notes:
            gd["notes"] = notes
        rec = {}
        if fix[F["WF_G_QUANT"]] != NONE:
            rec["quantize"] = fix[F["WF_G_QUANT"]] / UNIT
        if fix[F["WF_G_RECNOTES"]] != NONE:
            rec["max_notes"] = fix[F["WF_G_RECNOTES"]]
        if rec:
            gd["record"] = rec
        snd = {}
        for key, off in (("max_level", "WF_G_MAXLEVEL"), ("max_reso", "WF_G_MAXRESO"), ("max_dfdbk", "WF_G_MAXDFDBK"),
                         ("max_rsize", "WF_G_MAXRSIZE"), ("max_dist", "WF_G_MAXDIST"), ("max_dust", "WF_G_MAXDUST")):
            if fix[F[off]] != NONE:
                snd[key] = fix[F[off]]
        if g["ranges"]:
            snd["ranges"] = {tgt(kd, msk, i): [lo, hi] for kd, msk, i, lo, hi in g["ranges"]}
        if fix[F["WF_G_COMBOS"]] != NONE:
            cl = []
            for a, b_, c_ in g["combos"]:
                when = {tgt(*a[:3]): a[3]}
                if b_ != a:
                    when[tgt(*b_[:3])] = b_[3]
                cl.append({"when": when, "cap": {tgt(*c_[:3]): c_[3]}})
            snd["combos"] = cl
        if snd:
            gd["sound"] = snd
        arr = {}
        if fix[F["WF_G_MUTECHG"]] != NONE:
            arr["mute_change"] = fix[F["WF_G_MUTECHG"]]
        if fix[F["WF_G_DENSCHG"]] != NONE:
            arr["density_change"] = DENSCHG_NAMES[fix[F["WF_G_DENSCHG"]]]
        if fix[F["WF_G_FILLS"]] != NONE:
            arr["fills_every"] = fix[F["WF_G_FILLS"]]
        if fix[F["WF_G_BANDBARS"]] != NONE:
            arr["min_band_bars"] = fix[F["WF_G_BANDBARS"]]
        if arr:
            gd["arrangement"] = arr
        cpu = {}
        for key, off in (("max_unison", "WF_G_UNISON"), ("grain_dens", "WF_G_GRAINDENS"),
                         ("max_dist_tracks", "WF_G_DISTTRK")):
            if fix[F[off]] != NONE:
                cpu[key] = fix[F[off]]
        if fix[F["WF_G_CPU"]] != NONE:
            cpu["ceiling"] = fix[F["WF_G_CPU"]] / 256
        if cpu:
            gd["cpu"] = cpu
        w["guard"] = gd
    d = ir["defaults"]
    w["defaults"] = {"scene": SCENE_KEYS[d["scene"]], "variation": ir["vars"][d["var"]]["name"],
                     "macros": [x / UNIT for x in d["ctl"]], "pulse": PULSE_NAMES[d["pulse"]],
                     "beat": BEAT_NAMES[d["beat"]], "shape": [x / UNIT for x in d["shape"]],
                     "movement": [x / UNIT for x in d["move"]]}
    return w


def step_text(s):
    if s.time == ST_TIE:
        return "-"
    if s.time == ST_REST or not s.notes:
        return "."
    names = [note_name(n) for n in s.notes]
    body = names[0] if len(names) == 1 else "[" + " ".join(names) + "]"
    if s.flags & SF_ACCENT:
        body += "!"
    if s.flags & SF_SLIDE:
        body += "~"
    if s.lvl:
        body += "_"
    if s.rat:
        body += f"*{(s.rat & 3) + 1}"
    return body


def roman(r, q, beats, dmask):
    offs = bits(dmask)
    if r in offs:
        acc, d = "", offs.index(r)
    elif (r + 1) % 12 in offs:
        acc, d = "b", offs.index((r + 1) % 12)
    else:
        acc, d = "#", offs.index((r - 1) % 12)
    num = Compiler.NUMERALS[d]
    qn = QUAL_NAMES[q]
    canon = {"MAJ": (True, ""), "MIN": (False, ""), "DIM": (False, "o"), "AUG": (True, "+"), "SUS2": (True, "sus2"),
             "SUS4": (True, "sus4"), "MAJ7": (True, "maj7"), "MIN7": (False, "7"), "DOM7": (True, "7"),
             "M7B5": (False, "m7b5"), "MAJ6": (True, "6"), "MIN6": (False, "6"), "ADD9": (True, "add9"),
             "MADD9": (False, "add9"), "POW5": (True, "5"), "DIM7": (False, "o7")}[qn]
    return f"{acc}{num if canon[0] else num.lower()}{canon[1]}:{beats}"


# ================================================================== API ===
# ============================================= the model of the macros and the guard ===
# design 6.4: firmware/src/macro.c (macro_eval) and guard.c (guard_range, guard_sound) once more, integer for integer,
# with guard_limits.h's limits and world.c's stage (world_stage: the bases a scene x variation gives each parameter).
# `check` judges a World on a grid of macro positions with it, no firmware needed; `model` prints its slot tables in
# the format of tests/guard_sweep.c --dump-slots, and tests/run_tests.sh compares the two: the same slots, ranges,
# targets and effective values (docs/guardrails.md).
GL_H = ROOT / "firmware" / "src" / "guard_limits.h"
RT_H = ROOT / "firmware" / "src" / "world_rt.h"


def parse_limits(path=GL_H):
    """#define GL_NAME integer of guard_limits.h -> {GL_NAME: int}"""
    return {m.group(1): int(m.group(2)) for m in
            re.finditer(r"^[ \t]*#define[ \t]+(GL_\w+)[ \t]+(-?\d+)\b", _strip_c(Path(path).read_text()), re.M)}


GL = parse_limits()
OV_MAX = int(re.search(r"#define OV_MAX (\d+)", RT_H.read_text()).group(1))
OV_P, OV_G, OV_VCUT, OV_VSHP = range(4)
CTL_HOME = F["WF_CTL_HOME"]
ENG_ROLE = F["WF_ENG_ROLE"]
P_LEVEL, P_DIST, P_ROOT, P_SCALE = (PR.pid[x] for x in ("level", "dist", "root", "scale"))
G_DFDBK, G_RSIZE, G_DUST = (PR.gid[x] for x in ("dfdbk", "rsize", "dust"))
_cd = [PR.sym(x[0]) if isinstance(x, list) else x for x in F["WF_COMBO_DEFAULT"]]
COMBO_DEF = [tuple((_cd[o + 3 * j] >> 5, _cd[o + 3 * j] & 31, _cd[o + 3 * j + 1],
                    _cd[o + 3 * j + 2] - 256 if _cd[o + 3 * j + 2] > 127 else _cd[o + 3 * j + 2]) for j in range(3))
             for o in range(0, len(_cd), F["WF_COMBO_LEN"])]


def stage_bases(ir, scene, var):
    """world.c world_stage: each track's parameters and the globals a scene x variation starts from, and the
    engines (only what the macros read: the arpeggiator, step length and division of the keys track are left out)"""
    meta, sc, vr = ir["meta"], ir["scenes"][scene], ir["vars"][var]
    p0, pn = PR.P_E0, PR.lim["P_COUNT"]
    ps, eng = [], []
    for t in range(NTRK):
        tr = ir["tracks"][t]
        e = 0 if t == TRK_DRUM else tr["engine"]
        pre = tr["preset"]
        for vt, vp in vr["sounds"]:
            if vt == t:
                pre = vp
        if t == TRK_DRUM:
            q = [PR.P[i]["def"] for i in range(p0)] + [0] * (pn - p0)
            q[p0] = pre
        else:
            q = list(PR.engines[e]["preset_p"][pre])
            q[P_ROOT], q[P_SCALE] = meta["root"], meta["scale"]
        for pid, v in tr["pairs"]:
            q[pid] = v
        for pairs in (vr["pairs"], sc["pairs"]):
            for scope, pid, v in pairs:
                if scope == t:
                    q[pid] = v
        for i in range(pn):
            if t == TRK_DRUM:
                d = (0, PR.lim["DRUM_KITS"] - 1) if i == p0 else None if i > p0 else (PR.P[i]["min"], PR.P[i]["max"])
            else:
                dd = PR.engines[e]["edit"][i - p0] if i >= p0 else PR.P[i]
                d = (dd["min"], dd["max"])
            if d:
                q[i] = max(d[0], min(d[1], q[i]))
        ps.append(q)
        eng.append(e)
    g = [x["def"] for x in PR.G]
    g[G_SWING] = meta["swing"]
    for gid, v in ir["globals"]:
        g[gid] = v
    for pairs in (vr["pairs"], sc["pairs"]):
        for scope, gid, v in pairs:
            if scope == F["WF_SCOPE_G"]:
                g[gid] = v
    for gid in G_WHITE:
        g[gid] = max(PR.G[gid]["min"], min(PR.G[gid]["max"], g[gid]))
    return ps, g, eng


class Slot:
    __slots__ = ("kind", "part", "pid", "tgt", "lo", "hi", "cls", "dmin", "dmax", "raw", "maps")

    def __init__(self, kind, part, pid):
        self.kind, self.part, self.pid = kind, part, pid
        self.tgt, self.raw, self.cls, self.maps = 0, 0, NONE, []


def ov_effective(s, b, c):
    """guard.c ov_effective: base b with offset c (Q8) through slot s's range"""
    v = b + ((c + 128) >> 8)
    if v > s.hi and v > b:
        v = b if b > s.hi else s.hi
    elif v < s.lo and v < b:
        v = b if b < s.lo else s.lo
    return v


class Model:
    """a World's macros and guard as the firmware evaluates them (decode()'s IR)"""
    NOVAL = -0x8000

    def __init__(self, ir):
        self.ir = ir
        gd = ir["guard"]
        self.fix = gd["fix"] if gd else None
        self.ranges = gd["ranges"] if gd else []
        nc = self.fix[F["WF_G_COMBOS"]] if self.fix else NONE
        self.combos = COMBO_DEF if nc == NONE else gd["combos"]
        d = ir["defaults"]
        self.pos = list(CTL_HOME)
        for i in range(4):
            self.pos[i], self.pos[4 + i], self.pos[8 + i] = d["ctl"][i] * 4, d["shape"][i] * 4, d["move"][i] * 4
        self.p = self.g = self.eng = None

    def stage(self, scene, var):
        self.p, self.g, self.eng = stage_bases(self.ir, scene, var)

    def gbyte(self, off):
        return self.fix[off] if self.fix and self.fix[off] != NONE else F["WF_GUARD_DEFAULT"][off - F["WF_G_POLY"]]

    # ---- macro.c
    def curve(self, c, u):
        if c == 0:
            return u
        if c == 1:
            return u * u >> 12
        if c == 2:
            return math.isqrt(u << 12)
        if c == 3:
            u2 = u * u >> 12
            return 3 * u2 - 2 * (u2 * u >> 12)
        if c == 4:
            return 2 * (u - 2048) if u > 2048 else 0
        k = c - F["WF_CURVE_CUSTOM"]
        if 0 <= k < len(self.ir["curves"]):
            lut, x = self.ir["curves"][k], 8 * u
            i, f = x >> 12, x & 4095
            return 4096 if i >= 8 else (lut[i] * 4096 + (lut[i + 1] - lut[i]) * f + 127) // 255
        return u

    def offset(self, m, pos):
        ctl, cv, mn, mx = m[0] % F["WF_NCTL"], m[5], m[6], m[7]
        x, h = pos[ctl], CTL_HOME[ctl]
        if x < h:
            return mn * self.curve(cv, (h - x) * 4096 // h) >> 4
        if x > h:
            return mx * self.curve(cv, (x - h) * 4096 // (1000 - h)) >> 4
        return 0

    @staticmethod
    def strength(r, pos):
        ta, tb = 4 * r["ta"], 4 * r["tb"]
        if pos[r["a"]] <= ta or pos[r["b"]] <= tb:
            return 0
        return min((pos[r["a"]] - ta) * 4096 // (1000 - ta), (pos[r["b"]] - tb) * 4096 // (1000 - tb))

    def desc(self, kind, t, pid):
        if kind == OV_G:
            return PR.G[pid]
        e = self.eng[t] if t < NTRK - 1 else 0
        return PR.P[pid] if t == TRK_DRUM or pid < PR.P_E0 else PR.engines[e]["edit"][pid - PR.P_E0]

    def slot(self, kind, t, pid, make):
        for s in self.tab:
            if s.kind == kind and (s.pid == pid if kind == OV_G else s.part == t and s.pid == pid):
                return s
        if not make:
            return None
        if len(self.tab) >= OV_MAX:
            self.over += 1
            return None
        s = Slot(kind, t, pid)
        e = self.eng[t] if t < NTRK - 1 else 0
        if kind >= OV_VCUT:
            s.lo, s.hi = self.guard_range(kind, t, 0, e, -GL["GL_VMOD_MAX"], GL["GL_VMOD_MAX"])
            s.dmin, s.dmax = -GL["GL_VMOD_MAX"], GL["GL_VMOD_MAX"]
        else:
            d = self.desc(kind, t, pid)
            lo, hi = s.dmin, s.dmax = d["min"], d["max"]
            if d["fmt"] == "ENUM":
                s.cls = F["WF_CLASS_STEPPED"]
            lim = None
            if kind == OV_G and pid == G_DFDBK:
                lim = GL["GL_DFDBK_MAX"]
            elif kind == OV_G and pid == G_RSIZE:
                lim = GL["GL_RSIZE_MAX"]
            elif kind == OV_P and pid == P_LEVEL:
                lim = GL["GL_LEVEL_MAX"]
            elif kind == OV_P and t < NTRK - 1 and pid >= PR.P_E0 and ENG_ROLE[e][F["WF_EROLE_RESO"]] == pid - PR.P_E0:
                lim = GL["GL_RESO_MAX"]
            if lim is not None and hi > lim:
                hi = lim
            s.lo, s.hi = self.guard_range(kind, t, pid, e, lo, hi)
        self.tab.append(s)
        return s

    def add(self, kind_, mask, tid, cls, off, mi=None):
        make = off != 0 if cls is None else False
        hits = []
        if kind_ == F["WF_K_GLOBAL"]:
            hits.append(self.slot(OV_G, NONE, tid, make))
        else:
            for t in range(NTRK):
                if not mask >> t & 1:
                    continue
                if kind_ == F["WF_K_PARAM"]:
                    hits.append(self.slot(OV_P, t, tid, make))
                elif t >= NTRK - 1:
                    continue
                elif kind_ == F["WF_K_ROLE"]:
                    e = self.eng[t]
                    if e < len(ENG_ROLE) and tid < F["WF_NEROLES"] and ENG_ROLE[e][tid] != NONE:
                        hits.append(self.slot(OV_P, t, PR.P_E0 + ENG_ROLE[e][tid], make))
                else:
                    hits.append(self.slot(OV_VCUT if kind_ == F["WF_K_BRIGHT"] else OV_VSHP, t, 0, make))
        for s in hits:
            if s is None:
                continue
            if cls is None:
                s.tgt += off
                if mi is not None and off:
                    s.maps.append(mi)
            elif s.cls != F["WF_CLASS_STEPPED"]:
                s.cls = cls if s.cls == NONE or cls > s.cls else s.cls

    def evaluate(self, pos):
        """macro_eval at positions pos (the 16 controls), a fresh overlay: the slot table"""
        self.tab, self.over = [], 0
        for mi, m in enumerate(self.ir["maps"]):
            self.add(m[1], m[2], m[3], None, self.offset(m, pos), mi)
        for r in self.ir["rules"]:
            st = self.strength(r, pos)
            for kd, msk, i, a in r["acts"]:
                self.add(kd, msk, i, None, a * st >> 4)
        for m in self.ir["maps"]:
            self.add(m[1], m[2], m[3], m[4], 0)
        for s in self.tab:
            span = GL["GL_VMOD_MAX"] if s.kind >= OV_VCUT else s.hi - s.lo
            if s.cls == NONE:
                s.cls = F["WF_CLASS_DEFAULT"]
            s.raw = s.tgt
            s.tgt = max(-(span << 8), min(span << 8, s.tgt))
        self.guard_sound()
        return self.tab

    def base(self, s):
        return self.g[s.pid] if s.kind == OV_G else self.p[s.part][s.pid]

    def effective(self, s):
        return s.tgt if s.kind >= OV_VCUT else ov_effective(s, self.base(s), s.tgt)

    # ---- guard.c
    def names(self, tg, kind, t, pid, e):
        k, m, i = tg
        if kind == OV_G:
            return k == F["WF_K_GLOBAL"] and i == pid
        if t >= NTRK or not m >> t & 1:
            return False
        if kind in (OV_VCUT, OV_VSHP):
            return k == (F["WF_K_BRIGHT"] if kind == OV_VCUT else F["WF_K_SHAPE"])
        if k == F["WF_K_PARAM"]:
            return i == pid
        return (k == F["WF_K_ROLE"] and t < NTRK - 1 and e < len(ENG_ROLE) and i < F["WF_NEROLES"] and
                ENG_ROLE[e][i] != NONE and PR.P_E0 + ENG_ROLE[e][i] == pid)

    def guard_range(self, kind, t, pid, e, lo, hi):
        cap = 0x7FFF
        if kind == OV_G:
            cap = {G_DFDBK: "WF_G_MAXDFDBK", G_RSIZE: "WF_G_MAXRSIZE", G_DUST: "WF_G_MAXDUST"}.get(pid)
            cap = self.gbyte(F[cap]) if cap else 0x7FFF
        elif kind == OV_P and pid == P_LEVEL:
            cap = self.gbyte(F["WF_G_MAXLEVEL"])
        elif kind == OV_P and pid == P_DIST:
            cap = self.gbyte(F["WF_G_MAXDIST"])
        elif kind == OV_P and t < NTRK - 1 and pid >= PR.P_E0 and e < len(ENG_ROLE):
            if ENG_ROLE[e][F["WF_EROLE_RESO"]] == pid - PR.P_E0:
                cap = self.gbyte(F["WF_G_MAXRESO"])
            elif e == F["WF_ENG_GRAIN"] and pid - PR.P_E0 == F["WF_GRAIN_DENS"]:
                cap = self.gbyte(F["WF_G_GRAINDENS"])
        hi = min(hi, cap)
        for k, m, i, rlo, rhi in self.ranges:
            if self.names((k, m, i), kind, t, pid, e):
                lo, hi = max(lo, rlo), min(hi, rhi)
        return lo, (lo if hi < lo else hi)

    def resolve(self, tg, t):
        k, m, i = tg
        if k == F["WF_K_GLOBAL"]:
            return (OV_G, i) if i < len(PR.G) else None
        if t >= NTRK or not m >> t & 1:
            return None
        if k == F["WF_K_PARAM"]:
            return (OV_P, i)
        if k != F["WF_K_ROLE"] or t >= NTRK - 1 or i >= F["WF_NEROLES"]:
            return None
        r = ENG_ROLE[self.eng[t]][i]
        return None if r == NONE else (OV_P, PR.P_E0 + r)

    def find(self, kind, t, pid):
        for s in self.tab:
            if s.kind == kind and s.pid == pid and (kind == OV_G or s.part == t):
                return s
        return None

    def value(self, tg, t):
        k, m, i = tg
        if k != F["WF_K_GLOBAL"] and (t >= NTRK or not m >> t & 1):
            return max([self.value(tg, u) for u in range(NTRK) if m >> u & 1] + [self.NOVAL])
        r = self.resolve(tg, t)
        if r is None:
            return self.NOVAL
        b = self.g[r[1]] if r[0] == OV_G else self.p[t][r[1]]
        s = self.find(r[0], t, r[1])
        return ov_effective(s, b, s.tgt) if s else b

    def strength_c(self, v, over, self_):
        if v == self.NOVAL or v <= over:
            return 0
        if self_ or v - over >= F["WF_COMBO_RAMP"]:
            return 4096
        return (v - over) * 4096 // F["WF_COMBO_RAMP"]

    def combo(self, c, t):
        a, b, cc = c
        r = self.resolve(cc[:3], t)
        s = self.find(r[0], t, r[1]) if r else None
        if s is None:
            return
        st = min(self.strength_c(self.value(a[:3], t), a[3], a[:3] == cc[:3]),
                 self.strength_c(self.value(b[:3], t), b[3], b[:3] == cc[:3]))
        hi, cap = s.hi, cc[3]
        if not st or cap >= hi:
            return
        cap = hi - ((hi - cap) * st >> 12)
        bs = self.base(s)
        v = ov_effective(s, bs, s.tgt)
        if v > cap and v > bs:
            s.tgt = (max(bs, cap) - bs) * 256

    def guard_sound(self):
        n, mx = sum(self.p[t][P_DIST] > 0 for t in range(NTRK - 1)), self.gbyte(F["WF_G_DISTTRK"])
        for s in self.tab:
            if (s.kind == OV_P and s.part < NTRK - 1 and s.pid == P_DIST and self.p[s.part][P_DIST] <= 0 and
                    ov_effective(s, self.p[s.part][P_DIST], s.tgt) > 0):
                if n < mx:
                    n += 1
                else:
                    s.hi = self.p[s.part][P_DIST]
            if s.kind >= OV_VCUT:
                s.tgt = max(s.lo * 256, min(s.hi * 256, s.tgt))
        for c in self.combos:
            if c[2][0] == F["WF_K_GLOBAL"]:
                self.combo(c, NONE)
            else:
                for t in range(NTRK):
                    if c[2][1] >> t & 1:
                        self.combo(c, t)


def model_positions(nrand=24, seed=1):
    """the dump's sample of COLOR MOTION SPACE ENERGY: the 3^4 grid (0, 0.5, 1), then nrand seeded points (the LCG
    of tests/guard_sweep.c)"""
    out = [(a, b, c, d) for a in (0, 500, 1000) for b in (0, 500, 1000) for c in (0, 500, 1000) for d in (0, 500, 1000)]
    x = seed
    for _ in range(nrand):
        q = []
        for _ in range(4):
            x = (x * 1664525 + 1013904223) & 0xFFFFFFFF
            q.append((x >> 8) % 1001)
        out.append(tuple(q))
    return out


def model_dump(blob, nrand=24, seed=1):
    """`model`: every scene x variation at model_positions, the slot table as tests/guard_sweep.c --dump-slots prints"""
    ir = decode(blob)
    md = Model(ir)
    out = []
    for sc in range(F["WF_NSCENE"]):
        for v in range(len(ir["vars"])):
            md.stage(sc, v)
            for q in model_positions(nrand, seed):
                pos = list(md.pos)
                pos[:4] = q
                tab = md.evaluate(pos)
                out.append(f"@ {sc} {v} {q[0]} {q[1]} {q[2]} {q[3]} {len(tab)} {md.over}")
                for s in tab:
                    out.append(f"{s.kind} {s.part} {s.pid} {s.lo} {s.hi} {s.cls} {s.tgt} {md.effective(s)}")
    return "\n".join(out) + "\n"


def model_check(blob, d, map_src=()):
    """the static checks of design 6.4 and 12.3 on the model: every scene x variation, the 3^4 grid of the macros
    (the other controls at the World's defaults). Errors: more than OV_MAX slots; a mapping that alone takes its
    parameter out of the descriptor (the firmware would clamp it: its range is wrong) or past a hard limit of
    guard_limits.h (feedback, reverb gain, level, resonance); a mapping whose parameter reaches the descriptor's
    maximum at 100 % without "saturate": true; any effective value out of its descriptor (a model fault); the Smart
    Keys range under an octave. Warnings: mappings and rules that only summed run past the top of a range or a hard
    limit (the firmware holds them there; summed under a parameter's floor they only fall silent)"""
    ir = decode(blob)
    md = Model(ir)
    kt = ir["keys"]["trk"]
    if md.fix and md.fix[2 * kt] != NONE and md.fix[2 * kt + 1] - md.fix[2 * kt] < 11:
        d.err("$.smart_keys.range", f"{note_name(md.fix[2 * kt])}..{note_name(md.fix[2 * kt + 1])}: under an octave "
                                    "(the keys would fold some pitch classes out of the key)")
    hard = {(OV_G, G_DFDBK): ("GL_DFDBK_MAX", "delay feedback"), (OV_G, G_RSIZE): ("GL_RSIZE_MAX", "reverb size")}
    seen, worst = set(), {}
    grid = [(a, b, c, e) for a in (0, 500, 1000) for b in (0, 500, 1000) for c in (0, 500, 1000) for e in (0, 500, 1000)]

    def where(mi):
        return map_src[mi][0] if mi < len(map_src) else f"$.maps[{mi}]"

    def once(key, fn, path, msg):
        if key not in seen:
            seen.add(key)
            fn(path, msg)

    for sc in range(F["WF_NSCENE"]):
        for v in range(len(ir["vars"])):
            md.stage(sc, v)
            at = f"scene {SCENE_KEYS[sc]} / {ir['vars'][v]['name']}"
            # one mapping at a time, at its control's ends (design 6.4: the range and "100 % is never all-max")
            for mi, m in enumerate(ir["maps"]):
                ctl = m[0]
                for end in (0, 1000):
                    pos = list(md.pos)
                    for k in range(F["WF_NCTL"]):
                        pos[k] = CTL_HOME[k]
                    pos[ctl] = end
                    md.tab, md.over = [], 0
                    md.add(m[1], m[2], m[3], None, md.offset(m, pos), mi)
                    for s in md.tab:
                        if s.kind >= OV_VCUT:
                            continue
                        b, want = md.base(s), md.base(s) + ((s.tgt + 128) >> 8)
                        lab = f"{where(mi)}: {at}: {param_label(md, s)}"
                        if want < s.dmin or want > s.dmax:
                            once(("range", mi), d.err, lab, f"base {b} {want - b:+d} = {want}: outside "
                                 f"{s.dmin}..{s.dmax} (the firmware clamps it; make the mapping smaller)")
                        lim = hard_limit(md, s)
                        if lim is not None and want > lim[0] >= b:
                            once(("hard", mi), d.err, lab, f"base {b} {want - b:+d} = {want}: past the hard limit "
                                 f"{lim[0]} ({lim[1]}, guard_limits.h)")
                        sat = mi < len(map_src) and map_src[mi][1]
                        if end == 1000 and m[7] > 0 and want >= s.dmax > b and not sat:
                            once(("sat", mi), d.err, lab, f"{want} at 100 %: the parameter's maximum ({s.dmax}); "
                                 "100 % is never all-max (\"saturate\": true if it is meant)")
            # the four macros summed with the rules and the guard, on the grid
            for q in grid:
                pos = list(md.pos)
                pos[:4] = q
                tab = md.evaluate(pos)
                if md.over:
                    once(("over",), d.err, "$.macros", f"{len(tab) + md.over} targets moving at once ({at}, "
                         f"macros {q}): at most {OV_MAX} (the rest would do nothing)")
                for s in tab:
                    eff = md.effective(s)
                    if s.kind >= OV_VCUT:
                        if abs(s.raw) > GL["GL_VMOD_MAX"] << 8:
                            once(("vmod", s.kind, s.part), d.warn, "$.macros", f"{param_label(md, s)}: the offsets "
                                 f"sum to {s.raw / 256:+.0f}, past +-{GL['GL_VMOD_MAX']} ({at})")
                        continue
                    b, want = md.base(s), md.base(s) + ((s.raw + 128) >> 8)
                    if eff < s.dmin or eff > s.dmax:
                        once(("eff", s.kind, s.part, s.pid), d.err, "$.macros", f"{param_label(md, s)}: effective "
                             f"{eff} outside {s.dmin}..{s.dmax} ({at}, macros {q}): a fault of the model")
                    lim = hard_limit(md, s)
                    if (want > s.dmax or (lim and want > lim[0] >= b)) and want != eff:   # (a floor: harmless)
                        once(("sum", s.kind, s.part, s.pid), d.warn, "$.macros", f"{param_label(md, s)}: the macros "
                             f"and rules sum to {want} ({at}, macros {q}); the firmware holds it at {eff}")
                    key = (s.kind, s.part, s.pid)
                    if eff < want and eff >= b and want <= s.dmax:
                        worst[key] = max(worst.get(key, 0), want - eff)
    return worst


def param_label(md, s):
    if s.kind == OV_G:
        return f"g.{PR.G[s.pid]['name']}"
    if s.kind >= OV_VCUT:
        return f"track {s.part + 1} {'~bright' if s.kind == OV_VCUT else '~shape'}"
    return f"track {s.part + 1} {md.desc(s.kind, s.part, s.pid)['label']}"


def hard_limit(md, s):
    if s.kind == OV_G and s.pid == G_DFDBK:
        return GL["GL_DFDBK_MAX"], "delay feedback under 1"
    if s.kind == OV_G and s.pid == G_RSIZE:
        return GL["GL_RSIZE_MAX"], "reverb comb gain under 1"
    if s.kind == OV_P and s.pid == P_LEVEL:
        return GL["GL_LEVEL_MAX"], "a track's level"
    if (s.kind == OV_P and s.part < NTRK - 1 and s.pid >= PR.P_E0 and
            ENG_ROLE[md.eng[s.part]][F["WF_EROLE_RESO"]] == s.pid - PR.P_E0):
        return GL["GL_RESO_MAX"], "resonance"
    return None


def compile_world(src, user=False):
    """World JSON (dict) -> (blob or None, Diag)"""
    d = Diag()
    out = []
    sch = Schema()
    sch.check(src, sch.root, "$", out)
    for o in out:
        d.errors.append(o)
    if d.errors:
        return None, d
    comp = Compiler(src, d, user)
    ir = comp.run()
    d.map_src = getattr(comp, "map_src", [])
    if ir is None:
        return None, d
    blob = encode(ir)
    cap = F["WF_MAX_LEN"] if user else F["WF_FACTORY_LEN"]
    if len(blob) > cap:
        d.err("$", f"the blob is {len(blob)} B; a {'user' if user else 'factory'} World holds at most {cap} B")
        return None, d
    if len(blob) > F["WF_SOFT_LEN"]:
        d.warn("$", f"the blob is {len(blob)} B, above the {F['WF_SOFT_LEN']} B guideline")
    decode(blob)                        # the compiler's own output passes the structural checks
    return blob, d


def load_json(path):
    try:
        return json.loads(Path(path).read_text())
    except json.JSONDecodeError as e:
        raise SystemExit(f"{path}: not JSON: {e}")


def c_array(blob, name):
    lines = [f"/* generated by tools/worldc.py: an FWD1 blob, {len(blob)} B */",
             f"static const uint8_t {name}[{len(blob)}] __attribute__((aligned(4))) = {{"]
    for i in range(0, len(blob), 16):
        lines.append("    " + ", ".join(f"0x{x:02x}" for x in blob[i:i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines) + "\n"


def report(path, d, as_json=False):
    if as_json:
        return
    for w_ in d.warnings:
        print(f"{path}: warning: {w_}", file=sys.stderr)
    for e in d.errors:
        print(f"{path}: error: {e}", file=sys.stderr)


def names(engine=None):
    def desc(d):
        rng = ", ".join(d["names"]) if d.get("names") else f"{d['min']}..{d['max']}"
        return f"{rng} (default {d['names'][d['def'] - d['min']] if d.get('names') else d['def']})"
    for e in PR.engines:
        if engine and e["name"] != engine.upper():
            continue
        print(f"{e['name']}")
        for x in e["edit"]:
            if x["label"] != "-":
                print(f"  {x['label']:<6} {desc(x)}")
        roles = [f"@{r}={e['edit'][k]['label']}" for r, k in e["roles"].items() if k is not None]
        print(f"  roles: {' '.join(roles)}")
        print(f"  presets: {', '.join(e['presets'])}")
    if engine:
        return 0
    print("common track parameters (any synth track):")
    for p in PR.P[:PR.P_E0]:
        tag = " [set by the World]" if p["id"] in P_FIXED else " [structural]" if p["id"] in P_STRUCT else ""
        print(f"  {p['name']:<8} {p['label']:<6} {desc(p)}{tag}")
    print("drum track: level (g.drlvl), rev (g.drrev), " + ", ".join(PR.P[x]["name"] for x in sorted(P_DRUM)))
    print("World globals (fx, g.<name>):")
    for g in G_WHITE:
        tag = " [not a macro / rule target]" if g in G_STRUCT else ""
        print(f"  {PR.G[g]['name']:<8} {PR.G[g]['label']:<6} {desc(PR.G[g])}{tag}")
    print(f"drum kits: {', '.join(PR.kits)}")
    print(f"drum lanes: {', '.join(x['id'] + ' (' + x['name'] + ')' for x in PR.lanes)}")
    print(f"scales: {', '.join(x['name'] for x in PR.scales)}")
    print(f"divisions: {', '.join(PR.divs)}")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("compile", help="World JSON -> FWD1 blob")
    c.add_argument("world")
    c.add_argument("-o", "--out", required=True)
    c.add_argument("--c-array", action="store_true", help="write a C array instead of the raw blob")
    c.add_argument("--name", default="WORLD_BLOB", help="the C array's name (--c-array)")
    c.add_argument("--user", action="store_true", help="a user World: up to 3840 B, flag USER")
    dc = sub.add_parser("decompile", help="FWD1 blob -> lowered World JSON")
    dc.add_argument("blob")
    dc.add_argument("-o", "--out")
    ck = sub.add_parser("check", help="static checks of World sources, with the model of the macros and guard")
    ck.add_argument("worlds", nargs="+")
    ck.add_argument("--user", action="store_true")
    ck.add_argument("--json", action="store_true", help="a machine-readable report on stdout")
    md = sub.add_parser("model", help="the model's slot tables (as tests/guard_sweep.c --dump-slots)")
    md.add_argument("world", help="a World JSON or a compiled blob")
    md.add_argument("--random", type=int, default=24, help="seeded positions after the 3^4 grid (default 24)")
    md.add_argument("--seed", type=int, default=1)
    nm = sub.add_parser("names", help="engines, presets, parameters, kits, lanes a World may name")
    nm.add_argument("engine", nargs="?")
    a = ap.parse_args(argv)
    if a.cmd == "names":
        return names(a.engine)
    if a.cmd == "compile":
        blob, d = compile_world(load_json(a.world), a.user)
        report(a.world, d)
        if blob is None:
            return 1
        Path(a.out).write_text(c_array(blob, a.name)) if a.c_array else Path(a.out).write_bytes(blob)
        print(f"{a.world}: {len(blob)} B -> {a.out}")
        return 0
    if a.cmd == "model":
        if a.world.endswith(".json"):
            blob, d = compile_world(load_json(a.world))
            report(a.world, d)
            if blob is None:
                return 1
        else:
            blob = Path(a.world).read_bytes()
        try:
            sys.stdout.write(model_dump(blob, a.random, a.seed))
        except BlobError as e:
            print(f"{a.world}: {e}", file=sys.stderr)
            return 1
        return 0
    if a.cmd == "decompile":
        try:
            w = decompile(Path(a.blob).read_bytes())
        except BlobError as e:
            print(f"{a.blob}: {e}", file=sys.stderr)
            return 1
        txt = json.dumps(w, indent=2, ensure_ascii=False) + "\n"
        if a.out:
            Path(a.out).write_text(txt)
        else:
            sys.stdout.write(txt)
        return 0
    rc, rep = 0, []
    for p in a.worlds:
        blob, d = compile_world(load_json(p), a.user)
        if blob is not None:
            model_check(blob, d, d.map_src)
            if d.errors:
                blob = None
        report(p, d, a.json)
        rep.append({"file": p, "ok": blob is not None, "bytes": len(blob) if blob else None,
                    "errors": d.errors, "warnings": d.warnings})
        if blob is None:
            rc = 1
        elif not a.json:
            print(f"{p}: ok, {len(blob)} B" + (f", {len(d.warnings)} warning(s)" if d.warnings else ""))
    if a.json:
        print(json.dumps(rep, indent=2))
    return rc


if __name__ == "__main__":
    sys.exit(main())
