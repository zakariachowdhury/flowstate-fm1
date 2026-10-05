#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""world-author: the Musical World authoring tool (Phase 15; design D16 and 12.2; docs/authoring.md).

  tools/world-author [WORLD.json|DIR] [--port N] [--no-browser] [--allow-factory] [--worlds DIR] [--sim PATH]

A small local web server (127.0.0.1 only, a free port by default) that serves web/author.html and a JSON API over
the World sources. JSON is the source of truth (D16): the page edits a World as JSON, and this server reads and
writes the files, runs tools/worldc.py (check, compile, the model of the macros), tools/validate-world (in a
background job) and the native simulator for audio, which hot-reloads the file on every save. The browser plays
nothing.

Files. worlds/factory (read-only here unless --allow-factory: "Save as" writes a copy under worlds/user/), worlds/user
(created on the first save), and the directory of the file or DIR given on the command line. A file is named in the
API by a ref, "<dir key>/<name>.world.json"; nothing outside those directories is read or written, and only
web/author.html and /api/... are served. Saves are atomic and pretty-printed in a stable key order (the order of
docs/worlds.md; named entries such as variations keep the author's order), so diffs stay small.

API (JSON; POST bodies are application/json):
  GET  /api/state                 dirs, the file opened at the start, the simulator
  GET  /api/list                  the Worlds of every directory
  GET  /api/load?ref=R            {world, writable, mtime} (a file that is not JSON: {text, error})
  POST /api/save                  {ref, world, new?, base_mtime?} -> written, then checked
  GET  /api/new?id=&name=         a minimal valid World (not written)
  GET  /api/names                 what a World may name (sloop-params.json, world_fmt.h): engines, presets,
                                  parameters, globals, kits, lanes, scales, the format's enums, GUARD defaults
  POST /api/check                 {world, user?} -> worldc check: errors and warnings with their JSON paths
  POST /api/compile               {world, user?} -> the blob's size, the pool, the limits
  POST /api/model                 {world, scene, var, pos} -> every target the controls move at those positions:
                                  base, offset, effective, normalised, range, smoothing, the mappings behind it
  POST /api/harmony               {world} -> each progression's chords (names, tones) and the 27 Smart Keys
  POST /api/pattern               {world, name, progression?} -> the pattern's steps as compiled (notes)
  POST /api/validate              {world, ref?, mode: static|quick|full, user?} -> {job}
  GET  /api/job?id=J              the job: phase, progress, the static report first, then the full one
  POST /api/export                {world, user?, kinds} -> build/author/<id>/: .wblob, C array, JSON copy
  POST /api/import                {blob (base64)} -> worldc import: a user World blob as a source
  GET  /api/preview, POST /api/preview {ref | stop}   build/host-bin/flowstate-sim --world FILE, spawned once
Python stdlib only.
"""
import argparse
import base64
import json
import os
import re
import signal
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import validate_world as vw  # noqa: E402  (where a message belongs: the checklist items)
import worldc  # noqa: E402  (the compiler, the model and the format's constants: nothing is copied)

F, PR = worldc.F, worldc.PR
PAGE = ROOT / "web" / "author.html"
SIM_BIN = ROOT / "build" / "host-bin" / "flowstate-sim"
EXPORT_DIR = ROOT / "build" / "author"
SIM_BUILD = "make -C host (it needs SDL2: brew install sdl2); see docs/simulator.md"
REF_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,80}\.world\.json$")
MAX_BODY = 8 << 20


class ApiError(Exception):
    def __init__(self, status, msg, **extra):
        super().__init__(msg)
        self.status, self.extra = status, extra


# ======================================================== stable JSON ===
TOP = ["notes", "format", "id", "name", "category", "blurb", "tempo", "key", "swing", "tracks", "fx", "patterns",
       "progressions", "scenes", "variations", "macros", "controls", "curves", "rules", "energy", "smart_keys",
       "guard", "defaults"]
ORDER = {
    "tempo": ["notes", "bpm", "min", "max"],
    "key": ["notes", "root", "scale"],
    "track": ["notes", "name", "role", "register", "sound"],
    "sound": ["notes", "engine", "preset", "kit", "params"],
    "pattern": ["notes", "track", "div", "steps", "lanes"],
    "scene": ["notes", "name", "role", "progression", "energy", "transition", "fill", "patterns", "params", "fx"],
    "variation": ["notes", "sounds", "params", "fx", "swap", "energy_bias", "macros"],
    "mapping": ["notes", "to", "min", "max", "curve", "smooth", "saturate"],
    "rule": ["notes", "if", "then"],
    "action": ["notes", "to", "add"],
    "energy": ["notes", "density_lanes", "bands"],
    "band": ["notes", "from", "layers", "drums", "density"],
    "band_end": ["fills", "ratchets"],
    "smart_keys": ["notes", "track", "mode", "melody_scale", "white", "black", "tonic", "range"],
    "guard": ["notes", "record", "sound", "arrangement", "cpu"],
    "gnote": ["notes", "range", "max_poly", "loop_follow", "avoid"],
    "record": ["notes", "quantize", "max_notes"],
    "gsound": ["notes", "max_level", "max_reso", "max_dfdbk", "max_rsize", "max_dist", "max_dust", "ranges", "combos"],
    "combo": ["notes", "when", "cap"],
    "arrangement": ["notes", "mute_change", "density_change", "fills_every", "min_band_bars"],
    "cpu": ["notes", "max_unison", "grain_dens", "max_dist_tracks", "ceiling"],
    "defaults": ["notes", "scene", "variation", "macros", "pulse", "beat", "shape", "movement"],
    "curve": ["notes", "points", "lut"],
}


def _ord(d, keys, tail=()):
    """d with keys first (in that order), then its other keys as they were, then tail"""
    if not isinstance(d, dict):
        return d
    out = {k: d[k] for k in keys if k in d}
    out.update((k, v) for k, v in d.items() if k not in out and k not in tail)
    out.update((k, d[k]) for k in tail if k in d)
    return out


def _each(d, fn):
    return {k: fn(v) for k, v in d.items()} if isinstance(d, dict) else d


def _list(v, fn):
    return [fn(x) for x in v] if isinstance(v, list) else v


def canonical(w):
    """a World source in the stable order of docs/worlds.md. Only fixed-schema objects are reordered: the entries
    an author names (patterns, progressions, variations, energy tables, curves, params, lanes ...) keep their order,
    which is meaningful for variations"""
    if not isinstance(w, dict):
        return w
    w = _ord(w, TOP)
    if isinstance(w.get("tempo"), dict):
        w["tempo"] = _ord(w["tempo"], ORDER["tempo"])
    if isinstance(w.get("key"), dict):
        w["key"] = _ord(w["key"], ORDER["key"])

    def track(t):
        t = _ord(t, ORDER["track"])
        if isinstance(t, dict) and isinstance(t.get("sound"), dict):
            t["sound"] = _ord(t["sound"], ORDER["sound"])
        return t
    if "tracks" in w:
        w["tracks"] = _list(w["tracks"], track)
    if "patterns" in w:
        w["patterns"] = _each(w["patterns"], lambda p: _ord(p, ORDER["pattern"]))
    if isinstance(w.get("scenes"), dict):
        sc = w["scenes"]
        w["scenes"] = _each(_ord(sc, sorted(k for k in sc if k in worldc.SCENE_KEYS)),
                            lambda s: _ord(s, ORDER["scene"]))
    if "variations" in w:
        w["variations"] = _each(w["variations"], lambda v: _ord(v, ORDER["variation"]))

    def maps(group, names):
        if isinstance(w.get(group), dict):
            w[group] = _each(_ord(w[group], names),
                             lambda lst: _list(lst, lambda m: _ord(m, ORDER["mapping"])))
    maps("macros", worldc.CTL_NAMES[:4])
    maps("controls", worldc.CTL_NAMES[4:])
    if "curves" in w:
        w["curves"] = _each(w["curves"], lambda c: _ord(c, ORDER["curve"]))
    def rule(r):
        r = _ord(r, ORDER["rule"])
        if isinstance(r, dict) and "then" in r:
            r["then"] = _list(r["then"], lambda a: _ord(a, ORDER["action"]))
        return r
    if "rules" in w:
        w["rules"] = _list(w["rules"], rule)

    def energy(e):
        e = _ord(e, ORDER["energy"])
        if isinstance(e, dict) and "bands" in e:
            e["bands"] = _list(e["bands"], lambda b: _ord(b, ORDER["band"], ORDER["band_end"]))
        return e
    if "energy" in w:
        w["energy"] = _each(w["energy"], energy)
    if isinstance(w.get("smart_keys"), dict):
        w["smart_keys"] = _ord(w["smart_keys"], ORDER["smart_keys"])
    g = w.get("guard")
    if isinstance(g, dict):
        g = _ord(g, ["notes"] + ORDER["guard"][1:])
        if isinstance(g.get("notes"), dict):
            g["notes"] = _each(g["notes"], lambda n: _ord(n, ORDER["gnote"]))
        for k in ("record", "arrangement", "cpu"):
            if k in g:
                g[k] = _ord(g[k], ORDER[k])
        if isinstance(g.get("sound"), dict):
            g["sound"] = _ord(g["sound"], ORDER["gsound"])
            if "combos" in g["sound"]:
                g["sound"]["combos"] = _list(g["sound"]["combos"], lambda c: _ord(c, ORDER["combo"]))
        w["guard"] = g
    if isinstance(w.get("defaults"), dict):
        w["defaults"] = _ord(w["defaults"], ORDER["defaults"])
    return w


def _fmt(v, ind, col, width, expand=False):
    one = json.dumps(v, ensure_ascii=False, separators=(", ", ": "))
    if not isinstance(v, (dict, list)) or not v or (col + len(one) <= width and not expand):
        return one
    if isinstance(v, list) and not any(isinstance(x, (dict, list)) for x in v):
        return one                               # chords, positions, curve values: one line
    pad = " " * (ind + 2)
    if isinstance(v, list):
        return "[\n" + ",\n".join(pad + _fmt(x, ind + 2, ind + 2, width) for x in v) + "\n" + " " * ind + "]"
    body = []
    keyw = max(len(json.dumps(k, ensure_ascii=False)) for k in v) if expand else 0   # (lanes: the strings aligned)
    for k, x in v.items():
        key = (json.dumps(k, ensure_ascii=False) + ":").ljust(keyw + 1) + " "
        body.append(pad + key + _fmt(x, ind + 2, ind + 2 + len(key), width, k == "lanes" and isinstance(x, dict)))
    return "{\n" + ",\n".join(body) + "\n" + " " * ind + "}"


def dumps(w, width=110):
    """the file text of a World: canonical order; a value on one line when it fits, else one entry a line; a drum
    pattern's lanes one a line (design 2.1: one pattern string per line)"""
    return _fmt(canonical(w), 0, 0, width) + "\n"


def write_atomic(path, text):
    path = Path(path)
    fd, tmp = tempfile.mkstemp(prefix=f".{path.name}.", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


# ============================================================ the files ===
class Store:
    """the directories the tool reads and writes, and refs ("user/x.world.json") into them"""

    def __init__(self, worlds, extra=None, allow_factory=False):
        self.factory = (Path(worlds) / "factory").resolve()
        self.dirs = {"factory": [self.factory, allow_factory, "factory Worlds"],
                     "user": [(Path(worlds) / "user").resolve(), True, "your Worlds"]}
        self.allow_factory = allow_factory
        if extra is not None:
            e = Path(extra).resolve()
            if all(e != d[0] for d in self.dirs.values()):
                self.dirs["dir"] = [e, e != self.factory or allow_factory, str(e)]

    def key_of(self, directory):
        directory = Path(directory).resolve()
        for k, d in self.dirs.items():
            if d[0] == directory:
                return k
        return None

    def path(self, ref):
        """ref -> (path, writable); refuses anything outside the directories"""
        if not isinstance(ref, str) or ref.count("/") != 1:
            raise ApiError(400, f"{json.dumps(ref)}: a ref is <dir>/<name>.world.json")
        key, name = ref.split("/")
        if key not in self.dirs or not REF_NAME.match(name) or ".." in name:
            raise ApiError(403, f"{json.dumps(ref)}: not a World file of this tool (dirs {', '.join(self.dirs)}; "
                                "names [A-Za-z0-9_.-].world.json)")
        d, writable, _ = self.dirs[key]
        p = (d / name).resolve()
        if p.parent != d:
            raise ApiError(403, f"{json.dumps(ref)}: outside {d}")
        return p, writable

    def listing(self):
        out = []
        for key, (d, writable, label) in self.dirs.items():
            files = []
            for p in sorted(d.glob("*.world.json")) if d.is_dir() else []:
                e = {"ref": f"{key}/{p.name}", "file": p.name, "mtime": p.stat().st_mtime}
                try:
                    w = json.loads(p.read_text(encoding="utf-8"))
                    e.update(id=w.get("id"), name=w.get("name"), category=w.get("category"),
                             bpm=(w.get("tempo") or {}).get("bpm"), key=" ".join(
                                 str(x) for x in ((w.get("key") or {}).get("root"), (w.get("key") or {}).get("scale"))
                                 if x))
                except (ValueError, OSError, AttributeError) as ex:
                    e["error"] = str(ex)[:200]
                files.append(e)
            out.append({"key": key, "label": label, "path": rel(d), "writable": writable, "files": files})
        return out


def rel(p):
    p = Path(p).resolve()
    try:
        return str(p.relative_to(ROOT))
    except ValueError:
        return str(p)


# ======================================================= the template ===
def template(wid="my_world", name="MY WORLD"):
    """a minimal valid World to start from (docs/worlds.md): four tracks, two progressions, four scenes, a pattern
    per part, one mapping or two per macro with gain compensation, an ENERGY table, Smart Keys on the lead"""
    wid = worldc.world_id_of(wid or "my_world")
    ok = set(F["WF_NAME_CHARS"])
    name = "".join(c for c in (name or "MY WORLD").upper() if c in ok)[:F["WF_NAME_LEN"] - 1].strip() or "MY WORLD"
    return {
        "notes": "A new World from the authoring tool's template (tools/world_author.py). Replace everything.",
        "format": worldc.FORMAT_ID, "id": wid, "name": name, "category": "NEW", "blurb": "A new Musical World",
        "tempo": {"bpm": 90, "min": 81, "max": 99},
        "key": {"root": "A", "scale": "MIN"},
        "swing": 0,
        "tracks": [
            {"name": "pad", "role": "pad", "register": "A2",
             "sound": {"engine": "ANALOG", "preset": "WARM PAD", "params": {"level": 78, "rev": 60}}},
            {"name": "bass", "role": "bass", "register": "A1",
             "sound": {"engine": "ANALOG", "preset": "SUB BASS", "params": {"level": 70}}},
            {"name": "lead", "role": "lead", "register": "A3",
             "sound": {"engine": "DIGITAL", "preset": "RHODES", "params": {"level": 80, "rev": 40}}},
            {"name": "drums", "role": "drums", "sound": {"kit": "808", "params": {"level": 100, "rev": 20}}},
        ],
        "fx": {"dtime": "1/8", "dfdbk": 40, "dmix": 60, "rsize": 90, "rdamp": 60},
        "patterns": {
            "pad_main": {"track": "pad", "div": "1/4", "steps": "[c1 c5 c3'] - - -"},
            "bass_main": {"track": "bass", "div": "1/8", "steps": "c1 - . c1 . . c5 ."},
            "dr_main": {"track": "drums", "lanes": {"kick": "x.......x.x.....", "snare": "....x.......x...",
                                                    "hat": "x.x.x.x.x.x.x.x."}},
            "dr_fill": {"track": "drums", "lanes": {"kick": "x.......x.......", "snare": "....x...x.x.xxxx"}},
        },
        "progressions": {"main": ["i:4", "VI:4", "III:4", "VII:4"], "calm": ["i:8", "VI:8"]},
        "scenes": {
            "A": {"name": "INTRO", "role": "intro", "progression": "calm", "energy": "e",
                  "patterns": {"pad": "pad_main", "bass": None, "drums": None}},
            "B": {"name": "MAIN", "role": "main", "progression": "main", "energy": "e", "fill": "dr_fill",
                  "patterns": {"pad": "pad_main", "bass": "bass_main", "drums": "dr_main"}},
            "C": {"name": "LIFT", "role": "lift", "progression": "main", "energy": "e", "fill": "dr_fill",
                  "patterns": {"pad": "pad_main", "bass": "bass_main", "drums": "dr_main"},
                  "params": {"pad.CUT": 80}},
            "D": {"name": "BREAKDOWN", "role": "breakdown", "progression": "calm", "energy": "e",
                  "patterns": {"pad": "pad_main", "bass": "bass_main", "drums": None}, "fx": {"rsize": 104}},
        },
        "variations": {"ORIGINAL": {}},
        "macros": {
            "COLOR": [{"to": "*.~bright", "min": -22, "max": 22}],
            "MOTION": [{"to": "g.cdepth", "min": -30, "max": 30}],
            "SPACE": [{"to": "pad+lead.rev", "min": -30, "max": 30, "curve": "s"}, {"to": "g.rsize", "min": -20, "max": 16},
                      {"to": "g.dfdbk", "min": -20, "max": 16}],
            "ENERGY": [{"to": "*.level", "min": 4, "max": -4}],
        },
        "rules": [{"if": {"SPACE": 0.75, "ENERGY": 0.75}, "then": [{"to": "g.dfdbk", "add": -12}]}],
        "energy": {"e": {"bands": [
            {"from": 0.0, "layers": ["pad", "lead"]},
            {"from": 0.35, "layers": ["pad", "bass", "lead"]},
            {"from": 0.6, "layers": "all", "fills": True},
        ]}},
        "smart_keys": {"track": "lead", "mode": "melody", "melody_scale": "MPEN", "tonic": "A3"},
        "defaults": {"scene": "B", "variation": "ORIGINAL", "macros": [0.5, 0.5, 0.5, 0.5]},
    }


# ========================================================= names (the page's lists) ===
def _parse_c(path, pattern, flags=re.S):
    m = re.search(pattern, Path(path).read_text(), flags)
    if not m:
        raise SystemExit(f"world_author: {path}: {pattern} not found")
    return m


SRC = ROOT / "firmware" / "src"
HARM_PARENT = [int(x) for x in _parse_c(SRC / "harmony.c", r"HARM_PARENT\[16\] = \{([^}]*)\}").group(1).split(",")]
CHORD_SUFFIX = re.findall(r'"([^"]*)"', _parse_c(SRC / "harmony.c", r"SUFFIX\[WF_NQUAL\] = \{(.*?)\};").group(1))
SK_LO = int(_parse_c(SRC / "smartkeys.c", r"#define SK_LO (\d+)u").group(1))
SK_NKEY = int(_parse_c(SRC / "smartkeys.c", r"#define SK_NKEY (\d+)u").group(1))
SK_BLACK = int(_parse_c(SRC / "smartkeys.c", r"#define SK_BLACK (0x[0-9A-Fa-f]+)u").group(1), 16)
GU_RANGE = (int(_parse_c(SRC / "guard.c", r"#define GU_RANGE_LO (\d+)u").group(1)),
            int(_parse_c(SRC / "guard.c", r"#define GU_RANGE_HI (\d+)u").group(1)))
SHARP = worldc.NOTE_NAMES
FLAT = ["C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"]
GUARD_FIELDS = [   # (where in "guard", WF_G_* offset, kind): the page's GUARD tab, defaults from world_fmt.h
    ("notes.KEYS.max_poly", "WF_G_POLY", "int"), ("notes.KEYS.loop_follow", "WF_G_FOLLOW", "follow"),
    ("notes.KEYS.avoid", "WF_G_AVOID", "avoid"), ("record.quantize", "WF_G_QUANT", "unit"),
    ("record.max_notes", "WF_G_RECNOTES", "int"), ("sound.max_level", "WF_G_MAXLEVEL", "int"),
    ("sound.max_reso", "WF_G_MAXRESO", "int"), ("sound.max_dfdbk", "WF_G_MAXDFDBK", "int"),
    ("sound.max_rsize", "WF_G_MAXRSIZE", "int"), ("sound.max_dist", "WF_G_MAXDIST", "int"),
    ("sound.max_dust", "WF_G_MAXDUST", "int"), ("arrangement.mute_change", "WF_G_MUTECHG", "mute"),
    ("arrangement.density_change", "WF_G_DENSCHG", "dens"), ("arrangement.fills_every", "WF_G_FILLS", "int"),
    ("arrangement.min_band_bars", "WF_G_BANDBARS", "int"), ("cpu.max_unison", "WF_G_UNISON", "int"),
    ("cpu.grain_dens", "WF_G_GRAINDENS", "int"), ("cpu.max_dist_tracks", "WF_G_DISTTRK", "int"),
    ("cpu.ceiling", "WF_G_CPU", "q8")]


def _target_text(kind, mask, i):
    """a target record -> its source notation ("*" for every synth track, "keys" for the Smart Keys track)"""
    if kind == F["WF_K_GLOBAL"]:
        return f"g.{PR.G[i]['name']}"
    tr = ("keys" if mask & F["WF_TMASK_KEYS"] else "*" if mask & 7 == 7 else
          "+".join(f"track{t + 1}" for t in range(4) if mask >> t & 1))
    if kind == F["WF_K_ROLE"]:
        return f"{tr}.@{worldc.EROLE_NAMES[i]}"
    if kind == F["WF_K_BRIGHT"]:
        return f"{tr}.~bright"
    if kind == F["WF_K_SHAPE"]:
        return f"{tr}.~shape"
    return f"{tr}.{PR.P[i]['name'] if i < PR.P_E0 else 'e%d' % (i - PR.P_E0)}"


def names_json():
    gd, gmin, gmax = F["WF_GUARD_DEFAULT"], F["WF_GUARD_MIN"], F["WF_GUARD_MAX"]
    enum_ok = (F["WF_ENUM_OK_ENGINE"], F["WF_ENUM_OK_PARAM"])
    engines = []
    for e in PR.engines:
        role_of = {k: r for r, k in e["roles"].items() if k is not None}
        engines.append({"name": e["name"], "presets": e["presets"], "roles": e["roles"], "edit": [
            dict(x, role=role_of.get(k), structural=x["fmt"] == "ENUM" and (e["id"], k) != enum_ok)
            for k, x in enumerate(e["edit"])]})
    guard = []
    for path, off, kind in GUARD_FIELDS:
        o = F[off] - F["WF_G_POLY"]
        guard.append({"path": path, "default": gd[o], "min": gmin[o], "max": gmax[o], "kind": kind})
    return {
        "format": worldc.FORMAT_ID,
        "limits": {k: F[k] for k in ("WF_MAX_PAT", "WF_MAX_MAPS", "WF_MAX_CURVES", "WF_MAX_RULES", "WF_MAX_ENERGY",
                                     "WF_MAX_GRANGES", "WF_MAX_COMBOS", "WF_MAX_LEN", "WF_FACTORY_LEN",
                                     "WF_SOFT_LEN", "WF_NAME_LEN", "WF_CAT_LEN", "WF_BLURB_LEN", "WF_LABEL_LEN",
                                     "WF_MAX_LEN_STEPS", "WF_MAX_PROG", "WF_TEMPO_RANGE_PCT", "WF_MAX_CHORDS")}
        | {"OV_MAX": worldc.OV_MAX, "MAX_VARS": F["WF_MAX_VARS"]},
        "P": [dict(p, fixed=p["id"] in worldc.P_FIXED, structural=p["id"] in worldc.P_STRUCT,
                   drum=p["id"] in worldc.P_DRUM) for p in PR.P[:PR.P_E0]],
        "G": [dict(PR.G[g], structural=g in worldc.G_STRUCT, novar=g in worldc.G_NOVAR) for g in worldc.G_WHITE],
        "engines": engines, "kits": PR.kits, "default_kit": PR.d["drum_default_kit"],
        "lanes": PR.lanes, "scales": PR.scales, "divs": PR.divs, "div_den": PR.div_den,
        "roles": worldc.ROLE_NAMES, "register_default": F["WF_REG_DEFAULT"], "scene_roles": worldc.SROLE_NAMES,
        "scenes": worldc.SCENE_KEYS, "beats": worldc.BEAT_NAMES, "controls": worldc.CTL_NAMES,
        "control_home": [x / 1000 for x in worldc.CTL_HOME], "eroles": worldc.EROLE_NAMES,
        "curves": worldc.CURVE_NAMES, "smooth": worldc.CLASS_NAMES, "pulse": worldc.PULSE_NAMES,
        "keys_modes": worldc.KMODE_NAMES, "white": worldc.WHITE_NAMES, "black": worldc.BLACK_NAMES,
        "follow": worldc.FOLLOW_NAMES, "avoid": worldc.AVOID_NAMES, "density_change": worldc.DENSCHG_NAMES,
        "transitions": F["WF_TRANSITIONS"] + ["bar", "phrase"], "qualities": worldc.QUAL_NAMES,
        "guard": guard, "default_combos": [{"when": {_target_text(*a[:3]): a[3]} | (
            {_target_text(*b[:3]): b[3]} if b != a else {}), "cap": {_target_text(*c[:3]): c[3]}}
            for a, b, c in worldc.COMBO_DEF],
        "builtin": [{"control": worldc.CTL_NAMES[c], "target": _target_text(k, mk, i),
                     "max": mx, "min": mn, "curve": worldc.CURVE_NAMES[cv] if cv < len(worldc.CURVE_NAMES) else cv,
                     "smooth": worldc.CLASS_NAMES[cls]}
                    for c, k, mk, i, cls, cv, mn, mx in worldc.BUILTIN],
        "hard_limits": {k: v for k, v in worldc.GL.items() if k in ("GL_DFDBK_MAX", "GL_RSIZE_MAX", "GL_LEVEL_MAX",
                                                                    "GL_RESO_MAX", "GL_VMOD_MAX")},
    }


# ============================================================ worldc ===
def _need_world(body):
    w = body.get("world")
    if not isinstance(w, dict):
        raise ApiError(400, "\"world\": a World JSON object")
    return w


def split(msgs):
    out = []
    for m in msgs:
        keys, text = vw.split_msg(m)
        hm = vw.PATH_HEAD.match(m)
        out.append({"path": hm.group(1) if hm and m.startswith("$") else "$", "msg": text, "keys": keys,
                    "item": vw.classify(m), "text": m})
    return out


def check(w, user=False):
    """worldc check: the compiler, then the model over every scene x variation and the 3^4 grid"""
    try:
        blob, d = worldc.compile_world(w, user)
        if blob is not None:
            worldc.model_check(blob, d, d.map_src)
    except (worldc.BlobError, KeyError, TypeError, ValueError, AttributeError, IndexError) as e:   # a broken source
        return {"ok": False, "bytes": None, "errors": split([f"$: the compiler stopped: {e!r}"]), "warnings": []}
    errs = vw.dedupe(vw.suppress_cascades(d.errors, w))
    return {"ok": blob is not None and not errs, "bytes": len(blob) if blob is not None else None,
            "errors": split(errs), "warnings": split(d.warnings)}


def compiled(w, user=False):
    try:
        blob, d = worldc.compile_world(w, user)
    except (KeyError, TypeError, ValueError, AttributeError, IndexError) as e:
        raise ApiError(422, f"the compiler stopped: {e!r}", errors=[])
    if blob is None:
        raise ApiError(422, "the World does not compile", errors=split(d.errors))
    return blob, d


def compile_info(w, user=False):
    blob, d = compiled(w, user)
    ir = worldc.decode(blob)
    return {"ok": True, "bytes": len(blob), "user": user, "pool": len(ir["patterns"]), "maps": len(ir["maps"]),
            "progressions": len(ir["progs"]), "variations": len(ir["vars"]),
            "limits": {"cap": F["WF_MAX_LEN"] if user else F["WF_FACTORY_LEN"], "soft": F["WF_SOFT_LEN"],
                       "pool": F["WF_MAX_PAT"], "maps": F["WF_MAX_MAPS"], "targets": worldc.OV_MAX},
            "warnings": split(d.warnings)}


def _index(v, names, what):
    if isinstance(v, int) and not isinstance(v, bool) and 0 <= v < len(names):
        return v
    if isinstance(v, str) and v in names:
        return names.index(v)
    raise ApiError(400, f"{json.dumps(v)}: a {what} ({', '.join(names)})")


def model(w, scene=None, var=None, pos=None, batch=None):
    """the firmware's macro engine and guard (worldc.Model) at positions pos: each slot of the overlay. batch: a
    list of positions, evaluated on one compile ({"batch": [result, ...]})"""
    blob, d = compiled(w)
    ir = worldc.decode(blob)
    md = worldc.Model(ir)
    sc = _index(scene if scene is not None else ir["defaults"]["scene"], worldc.SCENE_KEYS, "scene")
    vn = [v["name"] for v in ir["vars"]]
    vi = _index(var if var is not None else ir["defaults"]["var"], vn, "variation")
    md.stage(sc, vi)
    if isinstance(batch, list):
        if len(batch) > 256:
            raise ApiError(400, "batch: at most 256 positions")
        return {"batch": [_evaluate(w, ir, md, d, sc, vn, vi, x) for x in batch]}
    return _evaluate(w, ir, md, d, sc, vn, vi, pos)


def _evaluate(w, ir, md, d, sc, vn, vi, pos):
    p = list(md.pos)                     # the World's defaults, 0..1000; pos: 0..1 by control name or index
    given = pos.items() if isinstance(pos, dict) else enumerate(pos[:F["WF_NCTL"]]) if isinstance(pos, list) else ()
    for k, x in given:
        if not isinstance(x, (int, float)) or isinstance(x, bool):
            raise ApiError(400, f"pos: {json.dumps(x)} is not a position 0..1")
        p[_index(k, worldc.CTL_NAMES, "control")] = max(0, min(1000, int(round(x * 1000))))
    tn = [t.get("name", f"track{i + 1}") for i, t in enumerate(w.get("tracks", []))] + ["t1", "t2", "t3", "t4"]
    tab = md.evaluate(p)
    src = [m[0] for m in d.map_src]
    nmap = len(ir["maps"])
    slots = []
    for s in tab:
        eff = md.effective(s)
        if s.kind >= worldc.OV_VCUT:             # (a smooth offset: Q8 steps of the engine's own 1/256)
            base, label, kind = 0, f"{tn[s.part]}.{'~bright' if s.kind == worldc.OV_VCUT else '~shape'}", "vmod"
            dmin, dmax, eff = s.dmin, s.dmax, round(s.tgt / 256, 2)
        else:
            base = md.base(s)
            ds = md.desc(s.kind, s.part, s.pid)
            dmin, dmax = ds["min"], ds["max"]
            if s.kind == worldc.OV_G:
                label, kind = f"g.{PR.G[s.pid]['name']}", "global"
            else:
                pname = PR.P[s.pid]["name"] if s.part == worldc.TRK_DRUM or s.pid < PR.P_E0 else ds["label"]
                label, kind = f"{tn[s.part]}.{pname}", "param"
        role = None
        if s.kind == worldc.OV_P and s.part < worldc.TRK_DRUM and s.pid >= PR.P_E0:
            r = [n for n, k in PR.engines[md.eng[s.part]]["roles"].items() if k == s.pid - PR.P_E0]
            role = "@" + r[0] if r else None
        span = dmax - dmin
        slots.append({
            "target": label, "kind": kind, "track": tn[s.part] if s.kind != worldc.OV_G else None, "role": role,
            "base": base, "offset": round(s.tgt / 256, 2), "raw": round(s.raw / 256, 2), "effective": eff,
            "norm": round((eff - dmin) / span, 4) if span else 0.0, "lo": s.lo, "hi": s.hi, "min": dmin, "max": dmax,
            "smooth": worldc.CLASS_NAMES[s.cls] if s.cls < len(worldc.CLASS_NAMES) else "medium",
            "held": eff != base + ((s.raw + 128) >> 8) if s.kind < worldc.OV_VCUT else s.tgt != s.raw,
            "maps": [src[i] if i < len(src) else f"built-in {worldc.CTL_NAMES[md.maps[i][0]]}" for i in s.maps],
            "controls": sorted({worldc.CTL_NAMES[md.maps[i][0]] for i in s.maps}),
            "fmt": md.desc(s.kind, s.part, s.pid).get("fmt") if s.kind < worldc.OV_VCUT else "VMOD"})
    maps = []
    for i, m in enumerate(md.maps):
        maps.append({"path": src[i] if i < nmap and i < len(src) else None, "control": worldc.CTL_NAMES[m[0]],
                     "builtin": i >= nmap, "offset": round(md.offset(m, p) / 256, 2), "min": m[6], "max": m[7]})
    rules = [{"index": i, "strength": round(md.strength(r, p) / 4096, 3)} for i, r in enumerate(ir["rules"])]
    return {"scene": worldc.SCENE_KEYS[sc], "var": vn[vi], "vars": vn, "pos": p, "over": md.over, "slots": slots,
            "mappings": maps, "rules": rules, "max_targets": worldc.OV_MAX}


def _rot(m, r):
    r %= 12
    return ((m << r) | (m >> (12 - r))) & 0xFFF if r else m & 0xFFF


def harmony(w):
    """each progression's chords as harmony.c names them, their tones, and the 27 Smart Keys (smartkeys.c sk_stage:
    white keys over the melody scale from the tonic, black keys over the chord's tones, folded into the range)"""
    d = worldc.Diag()
    c = worldc.Compiler(w, d)
    try:
        c.meta()
    except (AttributeError, TypeError, KeyError) as e:
        raise ApiError(422, f"the key does not parse: {e!r}")
    key, scale = c.root, c.scale
    sc = _rot(PR.scale_mask(scale), key)
    keys = {"trk": None, "mask": None, "white": 0, "black": 0, "tonic": 60 + (key + 6) % 12 - 6}
    try:
        c.tracks()
        c.keys_src()
        keys.update(c.keys)
    except (AttributeError, TypeError, KeyError, IndexError, ValueError):
        pass
    if keys["mask"] is None:
        keys["mask"] = PR.scale_mask(scale)
    sk = w.get("smart_keys") if isinstance(w.get("smart_keys"), dict) else {}
    kname = sk.get("track")
    g = w.get("guard") if isinstance(w.get("guard"), dict) else {}
    gn = ((g.get("notes") or {}).get(kname) or {}) if isinstance(g.get("notes"), dict) else {}
    rng = gn.get("range") or sk.get("range")
    lo, hi = GU_RANGE
    if isinstance(rng, list) and len(rng) == 2:
        a, b = (worldc.parse_note(x) if isinstance(x, str) else None for x in rng)
        if a is not None and b is not None and a <= b:
            lo, hi = a, b
    if hi < lo + 11:
        hi = min(lo + 11, 127)
    if lo > hi - 11:
        lo = hi - 11
    avoid = worldc.AVOID_NAMES.index(gn["avoid"]) if gn.get("avoid") in worldc.AVOID_NAMES else 0
    tonic = keys["tonic"]
    tonic -= (tonic + 12 - key) % 12
    mel = keys["mask"] & 0xFFF or 1
    flats = (key + HARM_PARENT[scale & 15]) % 12 in (5, 10, 3, 8, 1, 6)
    names = FLAT if flats else SHARP

    def white(wm, dd):
        cnt = bin(wm).count("1") or 1
        q = dd // cnt
        r = dd - q * cnt
        tones = [i for i in range(12) if wm >> i & 1] or [0]
        return tonic + 12 * q + tones[r]

    def above(bs, p):
        p += 1
        for _ in range(12):
            if bs >> (p % 12) & 1:
                return p
            p += 1
        return p

    def fold(v):
        while v > hi:
            v -= 12
        while v < lo:
            v += 12
        return max(0, min(127, v))

    progs = {}
    for pname, chords in (w.get("progressions") or {}).items():
        out, beats = [], 0
        for i, tok in enumerate(chords if isinstance(chords, list) else []):
            dd = worldc.Diag()
            c.d = dd
            r = c.chord(worldc.jp(worldc.jp("$.progressions", pname), i), tok)
            if r is None:
                out.append({"token": tok, "ok": False, "error": dd.errors[0] if dd.errors else "?"})
                continue
            root, q, bt = r
            beats += bt
            pc = (key + root) % 12
            ct = _rot(F["WF_QUAL_MASK"][q], pc)
            av = sc & ~ct & (_rot(ct, 1) | (_rot(ct, 11) if avoid == 2 else 0)) if avoid != 1 else 0
            safe = (sc & ~av) | ct
            wm, bs, nine = mel, ct, (pc + 2) % 12
            if keys["white"] == 1:
                s_ = mel & _rot(safe, 12 - key)
                wm = s_ or mel
            if keys["black"] == 1 and (sc & safe) >> nine & 1:
                bs |= 1 << nine
            kmap, wv, bv, wk = [], 0, -1000, 0
            for k in range(SK_NKEY):
                black = SK_BLACK >> ((SK_LO + k) % 12) & 1
                if black:
                    n = bv = above(bs, max(wv, bv))
                else:
                    n = wv = white(wm, wk - 4)
                    wk += 1
                kmap.append({"key": k, "black": bool(black), "note": fold(n), "name": worldc.note_name(fold(n)),
                             "chord": bool(ct >> (fold(n) % 12) & 1)})
            out.append({"token": tok, "ok": True, "name": names[pc] + CHORD_SUFFIX[q], "root": names[pc],
                        "quality": worldc.QUAL_NAMES[q], "beats": bt,
                        "tones": [names[(pc + x) % 12] for x in range(12) if F["WF_QUAL_MASK"][q] >> x & 1],
                        "safe": [names[x] for x in range(12) if safe >> x & 1], "keys": kmap})
        progs[pname] = {"chords": out, "beats": beats, "ok": beats in F["WF_PROG_BEATS"] and
                        all(x["ok"] for x in out) and 1 <= len(out) <= F["WF_MAX_CHORDS"]}
    used = {}
    for k, s in (w.get("scenes") or {}).items():
        if isinstance(s, dict) and isinstance(s.get("progression"), str):
            used.setdefault(s["progression"], []).append(k)
    return {"key": names[key], "scale": PR.scales[scale]["name"], "scale_tones": [names[x] for x in range(12)
                                                                                    if sc >> x & 1],
            "degrees": [names[(key + x) % 12] for x in worldc.bits(worldc.degree_mask(PR.scale_mask(scale)))],
            "progressions": progs, "used": used,
            "keys": {"track": kname, "tonic": worldc.note_name(tonic), "range": [worldc.note_name(lo),
                                                                                  worldc.note_name(hi)],
                     "melody": [names[(key + x) % 12] for x in range(12) if mel >> x & 1],
                     "white": worldc.WHITE_NAMES[keys["white"]], "black": worldc.BLACK_NAMES[keys["black"]]},
            "errors": split(d.errors)}


def pattern(w, name, prog=None):
    """one pattern as the compiler makes it: drum lanes, or synth steps with their notes (chord tokens unrolled
    over a progression: the one asked for, else the first scene's that plays it, else the first)"""
    d = worldc.Diag()
    c = worldc.Compiler(w, d)
    try:
        c.meta()
        c.tracks()
        if d.errors:
            raise ApiError(422, "the tracks have errors", errors=split(d.errors))
        c.patterns_src()
        c.progressions()
    except (AttributeError, TypeError, KeyError, IndexError, ValueError) as e:
        raise ApiError(422, f"the compiler stopped: {e!r}", errors=split(d.errors))
    path = worldc.jp("$.patterns", name)
    mine = [e for e in split(d.errors) if e["path"] == path or e["path"].startswith(path + ".") or
            e["path"].startswith(path + "[")]
    if name not in c.psrc:
        return {"name": name, "ok": False, "errors": mine or [{"path": path, "msg": "not compiled", "item": "patterns",
                                                               "text": f"{path}: not compiled", "keys": []}]}
    p = c.psrc[name]
    c.inst, c.pool, c.pool_ix, c.used_pat, c.prog = {}, [], {}, set(), []
    pi, pn = None, None
    if not p["drum"] and p["chordy"]:
        pn = prog if prog in c.prog_src else None
        if pn is None:
            for s in (w.get("scenes") or {}).values():
                if isinstance(s, dict) and name in json.dumps(s.get("patterns")) and s.get("progression") in c.prog_src:
                    pn = s["progression"]
                    break
        if pn is None and c.prog_src:
            pn = next(iter(c.prog_src))
        if pn is None:
            return {"name": name, "ok": False, "errors": [{"path": path, "msg": "chord tokens need a progression",
                                                            "item": "patterns", "text": "", "keys": []}]}
        pi = c.prog_use(pn)
    ix = c.resolve(path, name, pi)
    errs = [e for e in split(d.errors)]
    if ix == worldc.NONE or ix >= len(c.pool):
        return {"name": name, "ok": False, "errors": errs}
    e = c.pool[ix]
    if p["drum"]:
        steps = [{PR.lanes[ln]["id"]: worldc.DRUM_OUT.get(v, "x") for ln, v in s.items()} for s in e["steps"]]
    else:
        steps = [{"t": ["note", "tie", "rest"][s.time], "notes": list(s.notes), "names": [worldc.note_name(n)
                                                                                         for n in s.notes],
                  "accent": bool(s.flags & worldc.SF_ACCENT), "slide": bool(s.flags & worldc.SF_SLIDE),
                  "soft": bool(s.lvl), "ratchet": (s.rat & 3) + 1 if s.rat else 1} for s in e["steps"]]
    return {"name": name, "ok": True, "drum": p["drum"], "div": PR.divs[p["div"]], "track": w["tracks"][p["t"]]["name"],
            "progression": pn, "chordy": bool(p.get("chordy")), "src_len": len(p["fixed"]) if p["drum"] else
            len(p["toks"]), "len": len(steps), "steps": steps, "errors": errs}


# ========================================================== validation jobs ===
class Jobs:
    ESTIMATE = {"static": 1.5, "quick": 7.0, "full": 45.0}

    def __init__(self, store):
        self.store, self.jobs, self.lock, self.n = store, {}, threading.Lock(), 0

    def start(self, w, ref, mode, user):
        if mode not in self.ESTIMATE:
            raise ApiError(400, "mode: static, quick or full")
        with self.lock:
            self.n += 1
            jid = f"j{self.n}"
            self.jobs[jid] = job = {"id": jid, "state": "running", "mode": mode, "phase": "static checks",
                                    "started": time.time(), "elapsed": 0.0, "progress": 0.0, "static": None,
                                    "report": None, "error": None, "file": None}
        tmpdir = tempfile.mkdtemp(prefix="world-author-")
        path = None
        if ref:
            try:
                p, _ = self.store.path(ref)
                if p.exists() and json.loads(p.read_text(encoding="utf-8")) == w:
                    path = p                  # the saved file itself (the factory set's budget needs its place)
            except (ApiError, ValueError, OSError):
                path = None
        if path is None:
            path = Path(tmpdir) / f"{worldc.world_id_of(str(w.get('id') or 'world'))}.world.json"
            path.write_text(dumps(w), encoding="utf-8")
        job["file"] = rel(path)
        threading.Thread(target=self._run, args=(job, path, mode, user, tmpdir), daemon=True).start()
        return jid

    @staticmethod
    def _validate(path, flags):
        r = subprocess.run([sys.executable, str(ROOT / "tools" / "validate_world.py"), "--json", *flags, str(path)],
                           capture_output=True, text=True, cwd=ROOT)
        try:
            rep = json.loads(r.stdout)
        except ValueError:
            raise RuntimeError(f"validate-world exited {r.returncode}: {(r.stderr or r.stdout)[-600:]}")
        return rep["worlds"][0] if rep.get("worlds") else rep

    def _run(self, job, path, mode, user, tmpdir):
        flags = ["--user"] if user else []
        try:
            job["static"] = self._validate(path, flags + ["--static"])
            job["progress"] = 0.1
            if mode == "static":
                job["report"] = job["static"]
            elif not job["static"].get("bytes") or any(i["status"] == "fail" for i in job["static"]["items"]):
                job["report"] = job["static"]
                job["phase"] = "static checks failed: the sweep does not run"
            else:
                job["phase"] = f"rendering the macro space ({mode})"
                job["report"] = self._validate(path, flags + (["--full"] if mode == "full" else []))
            job["state"] = "done"
        except Exception as e:    # noqa: BLE001 (reported to the page)
            job["state"], job["error"] = "error", str(e)
        finally:
            job["progress"] = 1.0
            job["elapsed"] = round(time.time() - job["started"], 2)
            for p in Path(tmpdir).glob("*"):
                p.unlink()
            os.rmdir(tmpdir)

    def get(self, jid):
        job = self.jobs.get(jid)
        if job is None:
            raise ApiError(404, f"no job {jid}")
        if job["state"] == "running":
            job["elapsed"] = round(time.time() - job["started"], 2)
            est = self.ESTIMATE[job["mode"]] + (self.ESTIMATE["static"] if job["mode"] != "static" else 0)
            job["progress"] = max(job["progress"], min(0.95, job["elapsed"] / est))
            job["estimate"] = est
        return job


# ============================================================ the simulator ===
class Sim:
    """build/host-bin/flowstate-sim --world FILE, spawned once and kept: it reloads the file whenever it changes
    (every 500 ms it looks), so a save is heard at once. Another file: the simulator is started again with it"""

    def __init__(self, binary):
        self.bin = Path(binary)
        self.proc, self.file, self.log, self.started = None, None, None, None
        self.lock = threading.Lock()

    def status(self):
        running = self.proc is not None and self.proc.poll() is None
        st = {"binary": rel(self.bin), "missing": not (self.bin.is_file() and os.access(self.bin, os.X_OK)),
              "build": SIM_BUILD, "running": running, "pid": self.proc.pid if running else None,
              "file": rel(self.file) if self.file else None,
              "exit": self.proc.returncode if self.proc is not None and not running else None}
        if self.log and Path(self.log).exists():
            st["log"] = Path(self.log).read_text(errors="replace")[-1500:]
        return st

    def start(self, path):
        with self.lock:
            path = Path(path).resolve()
            st = self.status()
            if st["missing"]:
                return dict(st, error=f"no simulator at {rel(self.bin)}: build it with {SIM_BUILD}")
            if st["running"] and self.file == path:
                return dict(st, reused=True)
            self._stop()
            fd, self.log = tempfile.mkstemp(prefix="flowstate-sim-", suffix=".log")
            env = dict(os.environ, FLOWSTATE_ROOT=str(ROOT))
            self.proc = subprocess.Popen([str(self.bin), "--world", str(path)], stdout=fd, stderr=subprocess.STDOUT,
                                         stdin=subprocess.DEVNULL, cwd=ROOT, env=env, start_new_session=True)
            os.close(fd)
            self.file, self.started = path, time.time()
            return dict(self.status(), reused=False)

    def _stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None

    def stop(self):
        with self.lock:
            self._stop()
            self.file = None
            return self.status()


# ================================================================ the API ===
class App:
    def __init__(self, args):
        self.store = Store(args.worlds, args.extra, args.allow_factory)
        self.initial = None
        if args.path:
            p = Path(args.path)
            if p.is_file():
                key = self.store.key_of(p.parent)
                self.initial = f"{key}/{p.name}" if key else None
        self.jobs = Jobs(self.store)
        self.sim = Sim(args.sim)
        self._names = None

    def names(self):
        if self._names is None:
            self._names = names_json()
        return self._names

    # ---- handlers: (body or query) -> JSON
    def state(self, q):
        return {"root": str(ROOT), "dirs": [{"key": k, "path": rel(d[0]), "writable": d[1], "label": d[2]}
                                            for k, d in self.store.dirs.items()],
                "initial": self.initial, "allow_factory": self.store.allow_factory, "sim": self.sim.status()}

    def list(self, q):
        return {"dirs": self.store.listing()}

    def load(self, q):
        ref = (q.get("ref") or [None])[0]
        p, writable = self.store.path(ref)
        if not p.exists():
            raise ApiError(404, f"{ref}: no such World")
        text = p.read_text(encoding="utf-8")
        out = {"ref": ref, "writable": writable, "file": rel(p), "mtime": p.stat().st_mtime}
        try:
            out["world"] = json.loads(text)
        except ValueError as e:
            out.update(world=None, text=text, error=f"not JSON: {e}")
        return out

    def save(self, b):
        ref, w = b.get("ref"), _need_world(b)
        p, writable = self.store.path(ref)
        if not writable:
            raise ApiError(403, f"{ref}: the factory Worlds are read-only here. Save As a copy under worlds/user/ "
                                "(or start the tool with --allow-factory)")
        if b.get("new") and p.exists():
            raise ApiError(409, f"{ref} exists: choose another name, or save over it", exists=True)
        if b.get("base_mtime") and p.exists() and abs(p.stat().st_mtime - b["base_mtime"]) > 1e-3 and not b.get("force"):
            raise ApiError(409, f"{ref} changed on disk since it was loaded: reload it, or save anyway", changed=True)
        p.parent.mkdir(parents=True, exist_ok=True)
        text = dumps(w)
        write_atomic(p, text)
        return {"ref": ref, "file": rel(p), "bytes": len(text.encode()), "mtime": p.stat().st_mtime,
                "check": check(w), "sim": self.sim.status()}

    def new(self, q):
        return {"world": template((q.get("id") or ["my_world"])[0], (q.get("name") or ["MY WORLD"])[0])}

    def api_names(self, q):
        return self.names()

    def api_check(self, b):
        return check(_need_world(b), bool(b.get("user")))

    def api_compile(self, b):
        return compile_info(_need_world(b), bool(b.get("user")))

    def api_model(self, b):
        return model(_need_world(b), b.get("scene"), b.get("var"), b.get("pos"), b.get("batch"))

    def api_harmony(self, b):
        return harmony(_need_world(b))

    def api_pattern(self, b):
        if not isinstance(b.get("name"), str):
            raise ApiError(400, "\"name\": a pattern's name")
        return pattern(_need_world(b), b["name"], b.get("progression"))

    def validate(self, b):
        return {"job": self.jobs.start(_need_world(b), b.get("ref"), b.get("mode", "quick"), bool(b.get("user")))}

    def job(self, q):
        return self.jobs.get((q.get("id") or [""])[0])

    def export(self, b):
        w, user = _need_world(b), bool(b.get("user"))
        kinds = b.get("kinds") or ["wblob", "c", "json"]
        blob, d = compiled(w, user)
        wid = worldc.world_id_of(str(w.get("id") or "world"))
        out = EXPORT_DIR / wid
        out.mkdir(parents=True, exist_ok=True)
        res = {"dir": rel(out), "bytes": len(blob), "user": user, "files": []}
        stem = wid + ("_user" if user else "")
        if "wblob" in kinds:
            (out / f"{stem}.wblob").write_bytes(blob)
            res["files"].append({"kind": "wblob", "path": rel(out / f"{stem}.wblob"), "bytes": len(blob)})
            res["blob"] = base64.b64encode(blob).decode()
        if "c" in kinds:
            txt = worldc.c_array(blob, f"WORLD_{stem.upper()}")
            write_atomic(out / f"{stem}.h", txt)
            res["files"].append({"kind": "c", "path": rel(out / f"{stem}.h"), "bytes": len(txt)})
            res["c"] = txt
        if "json" in kinds:
            txt = dumps(w)
            write_atomic(out / f"{wid}.world.json", txt)
            res["files"].append({"kind": "json", "path": rel(out / f"{wid}.world.json"), "bytes": len(txt.encode())})
            res["json"] = txt
        return res

    def api_import(self, b):
        try:
            blob = base64.b64decode(b.get("blob") or "", validate=True)
            return {"world": worldc.import_user(blob)}
        except (ValueError, worldc.BlobError) as e:
            raise ApiError(422, f"not a World blob: {e}")

    def preview_get(self, q):
        return self.sim.status()

    def preview(self, b):
        if b.get("stop"):
            return self.sim.stop()
        p, _ = self.store.path(b.get("ref"))
        if not p.exists():
            raise ApiError(404, f"{b.get('ref')}: save the World first: the simulator plays the file")
        return self.sim.start(p)

    def routes(self):
        return {("GET", "state"): self.state, ("GET", "list"): self.list, ("GET", "load"): self.load,
                ("POST", "save"): self.save, ("GET", "new"): self.new, ("GET", "names"): self.api_names,
                ("POST", "check"): self.api_check, ("POST", "compile"): self.api_compile,
                ("POST", "model"): self.api_model, ("POST", "harmony"): self.api_harmony,
                ("POST", "pattern"): self.api_pattern, ("POST", "validate"): self.validate,
                ("GET", "job"): self.job, ("POST", "export"): self.export, ("POST", "import"): self.api_import,
                ("GET", "preview"): self.preview_get, ("POST", "preview"): self.preview}


CSP = ("default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data: blob:; "
       "connect-src 'self'; base-uri 'none'; form-action 'none'")


class Handler(BaseHTTPRequestHandler):
    server_version = "world-author/1"
    app = None
    routes = {}

    def log_message(self, fmt, *a):
        if os.environ.get("WORLD_AUTHOR_LOG"):
            sys.stderr.write("world-author: " + fmt % a + "\n")

    def send(self, status, body, ctype="application/json; charset=utf-8", extra=()):
        data = body if isinstance(body, bytes) else json.dumps(body, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def route(self, method):
        port = self.server.server_address[1]
        if self.headers.get("Host") not in (f"127.0.0.1:{port}", f"localhost:{port}"):
            return self.send(403, {"error": "this server answers 127.0.0.1 only"})
        u = urlparse(self.path)
        if method == "GET" and u.path in ("/", "/author.html"):
            return self.send(200, PAGE.read_bytes(), "text/html; charset=utf-8", [("Content-Security-Policy", CSP)])
        if not u.path.startswith("/api/"):
            return self.send(404, {"error": f"{u.path}: not found (the page is /, the API /api/...)"})
        fn = self.routes.get((method, u.path[5:]))
        if fn is None:
            return self.send(404 if not any(k[1] == u.path[5:] for k in self.routes) else 405,
                             {"error": f"{method} {u.path}: no such API"})
        try:
            if method == "POST":
                if not (self.headers.get("Content-Type") or "").startswith("application/json"):
                    raise ApiError(415, "POST bodies are application/json")
                n = int(self.headers.get("Content-Length") or 0)
                if n > MAX_BODY:
                    raise ApiError(413, "too large")
                try:
                    arg = json.loads(self.rfile.read(n) or b"{}")
                except ValueError as e:
                    raise ApiError(400, f"not JSON: {e}")
                if not isinstance(arg, dict):
                    raise ApiError(400, "a JSON object")
            else:
                arg = parse_qs(u.query)
            self.send(200, fn(arg))
        except ApiError as e:
            self.send(e.status, {"error": str(e), **e.extra})
        except Exception as e:     # noqa: BLE001 (a bug: the page shows it)
            traceback.print_exc()
            self.send(500, {"error": f"{type(e).__name__}: {e}"})


def main(argv=None):
    ap = argparse.ArgumentParser(prog="world-author", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", help="a World file to open, or a directory of Worlds to list too")
    ap.add_argument("--port", type=int, default=0, help="the port (default: a free one)")
    ap.add_argument("--no-browser", action="store_true", help="do not open the page")
    ap.add_argument("--allow-factory", action="store_true", help="let Save write worlds/factory")
    ap.add_argument("--worlds", default=str(ROOT / "worlds"), help="the worlds directory (factory/ and user/)")
    ap.add_argument("--sim", default=str(SIM_BIN), help="the simulator binary (default build/host-bin/flowstate-sim)")
    a = ap.parse_args(argv)
    a.extra = None
    if a.path:
        p = Path(a.path)
        if not p.exists():
            ap.error(f"{a.path}: no such file or directory")
        a.extra = p if p.is_dir() else p.parent
    app = App(a)
    Handler.app, Handler.routes = app, app.routes()
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    srv.daemon_threads = True
    url = f"http://127.0.0.1:{srv.server_address[1]}/"
    print(f"world-author: {url}  (Ctrl-C stops it)", flush=True)

    def stop(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    if not a.no_browser:
        if sys.platform == "darwin":
            subprocess.Popen(["open", url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            webbrowser.open(url)
    try:
        srv.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
        app.sim.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
