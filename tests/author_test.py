#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Checks of the World authoring tool (Phase 15: tools/world_author.py, web/author.html; docs/authoring.md), run by
tests/run_tests.sh from the repo root (stdlib only; about 15 s):
  - the file format: the stable layout (canonical key order, the author's order of variations kept, idempotent);
  - the server, started on a free port with --no-browser over a temp copy of worlds/: every API endpoint (state, list,
    load, new, save: the file holds the posted JSON in the stable layout; factory writes, path traversal, stale saves
    refused; names, check of a broken World with JSON paths, compile, the model against tools/worldc.py's Model, batch,
    harmony and the Smart Keys, pattern, validate quick on a factory World as a job, export, import of a user World
    blob, preview with no simulator and with a stand-in simulator spawned once), and its refusals (Host header, content
    type, unknown routes, only web/author.html served);
  - the page: it parses, every API it calls exists, no URL outside its own server;
  - the page's pure helpers in node's vm (when node is installed), against tools/worldc.py.
"""
import base64
import html.parser
import http.client
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from argparse import Namespace
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import world_author as wa  # noqa: E402
import worldc  # noqa: E402

FAILS = []
COUNT = [0]
PAGE = ROOT / "web" / "author.html"


def check(cond, what):
    COUNT[0] += 1
    if not cond:
        FAILS.append(what)
        print(f"FAIL {what}")
    return cond


def factory(name):
    return json.loads((ROOT / "worlds" / "factory" / f"{name}.world.json").read_text())


# ------------------------------------------------------------------------------------------- the layout ---
def test_layout():
    for p in sorted((ROOT / "worlds" / "factory").glob("*.world.json")):
        w = json.loads(p.read_text())
        t = wa.dumps(w)
        check(json.loads(t) == w, f"{p.name}: the stable layout holds the same JSON")
        check(wa.dumps(json.loads(t)) == t, f"{p.name}: the layout is idempotent")
        check(list(json.loads(t)["variations"]) == list(w["variations"]), f"{p.name}: the variations keep their order")
        keys = list(json.loads(t))
        check(keys == [k for k in wa.TOP if k in w], f"{p.name}: the top-level keys in the guide's order: {keys}")
        sh = dict(random.Random(1).sample(list(w.items()), len(w)))
        sh["tempo"] = dict(reversed(list(w["tempo"].items())))
        check(wa.dumps(sh) == t, f"{p.name}: a shuffled source gives the same file")
        check(max(len(x) for x in t.splitlines() if '"steps"' not in x and '"notes"' not in x) <= 140,
              f"{p.name}: lines stay short")


# ------------------------------------------------------------------------------------------- the server ---
class Server:
    def __init__(self, worlds, *extra):
        self.p = subprocess.Popen([sys.executable, str(ROOT / "tools" / "world_author.py"), "--no-browser",
                                   "--worlds", str(worlds), *extra], stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        line = self.p.stdout.readline()
        m = re.search(r"http://127\.0\.0\.1:(\d+)/", line)
        if not m:
            raise SystemExit(f"the server did not start: {line} {self.p.stderr.read()[-2000:]}")
        self.port = int(m.group(1))

    def call(self, method, path, body=None, headers=None, raw=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=120)
        h = {"Host": f"127.0.0.1:{self.port}"}
        data = raw if raw is not None else json.dumps(body).encode() if body is not None else None
        if data is not None:
            h["Content-Type"] = "application/json"
        h.update(headers or {})
        c.request(method, path, body=data, headers=h)
        r = c.getresponse()
        txt = r.read()
        try:
            j = json.loads(txt)
        except ValueError:
            j = None
        return r.status, j, txt, dict(r.getheaders())

    def get(self, path):
        return self.call("GET", path)

    def post(self, path, body):
        return self.call("POST", path, body)

    def stop(self):
        self.p.terminate()
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()


def with_overrides(blob, recs):
    """a user blob with an OVERRIDES section (as tests/worldc_test.py: the device's world_encode writes them)"""
    L, nsec = struct.unpack_from("<H", blob, 6)[0], blob[16]
    pay = b"".join(struct.pack("<BBB", sc, i, v & 0xFF) for sc, i, v in recs)
    out = bytearray(blob[:20 + 4 * nsec]) + struct.pack("<BBH", worldc.F["WF_S_OVERRIDES"], len(recs), len(pay))
    out += blob[20 + 4 * nsec:L] + pay
    out[16] = nsec + 1
    struct.pack_into("<H", out, 6, len(out))
    struct.pack_into("<I", out, worldc.F["WF_CRC_AT"], worldc.blob_crc(bytes(out), len(out)))
    return bytes(out)


def model_reference(w, scene, var, pos):
    """tools/worldc.py's Model directly: {target: effective} for the slots (the API's labels)"""
    blob, _ = worldc.compile_world(w)
    ir = worldc.decode(blob)
    md = worldc.Model(ir)
    md.stage(worldc.SCENE_KEYS.index(scene), [v["name"] for v in ir["vars"]].index(var))
    p = list(md.pos)
    for k, x in pos.items():
        p[worldc.CTL_NAMES.index(k)] = int(round(x * 1000))
    tn = [t["name"] for t in w["tracks"]]
    out = {}
    for s in md.evaluate(p):
        if s.kind >= worldc.OV_VCUT:
            out[f"{tn[s.part]}.{'~bright' if s.kind == worldc.OV_VCUT else '~shape'}"] = round(s.tgt / 256, 2)
        elif s.kind == worldc.OV_G:
            out[f"g.{worldc.PR.G[s.pid]['name']}"] = md.effective(s)
        else:
            d = md.desc(s.kind, s.part, s.pid)
            nm = worldc.PR.P[s.pid]["name"] if s.part == 3 or s.pid < worldc.PR.P_E0 else d["label"]
            out[f"{tn[s.part]}.{nm}"] = md.effective(s)
    return out


def test_server(tmp):
    worlds = tmp / "worlds"
    shutil.copytree(ROOT / "worlds" / "factory", worlds / "factory")
    srv = Server(worlds, "--sim", str(tmp / "no-such-sim"))
    try:
        api_checks(srv, worlds, tmp)
    finally:
        srv.stop()
    # a stand-in simulator: records its arguments and waits (never a window, never a sound)
    fake = tmp / "fake-sim"
    fake.write_text(f"#!/bin/sh\necho \"$@\" > '{tmp}/fake-sim.args'\nexec sleep 30\n")
    fake.chmod(0o755)
    srv = Server(worlds, "--sim", str(fake))
    try:
        st, r, _, _ = srv.post("/api/preview", {"ref": "user/test_new.world.json"})
        check(st == 200 and r["running"] and r["pid"] and not r["reused"], f"preview spawns the simulator: {st} {r}")
        pid = r["pid"]
        st, r, _, _ = srv.post("/api/preview", {"ref": "user/test_new.world.json"})
        check(st == 200 and r["running"] and r["pid"] == pid and r["reused"], f"preview again: the same process: {r}")
        for _ in range(50):
            if (tmp / "fake-sim.args").exists():
                break
            time.sleep(0.05)
        args = (tmp / "fake-sim.args").read_text().split() if (tmp / "fake-sim.args").exists() else []
        check(args[:1] == ["--world"] and args[1:] == [str((worlds / "user" / "test_new.world.json").resolve())],
              f"the simulator plays the saved file: {args}")
        st, r, _, _ = srv.get("/api/preview")
        check(r["running"] and r["file"].endswith("test_new.world.json"), f"preview status: {r}")
        st, r, _, _ = srv.post("/api/preview", {"stop": True})
        check(st == 200 and not r["running"], f"preview stop: {r}")
        try:
            os.kill(pid, 0)
            alive = True
        except OSError:
            alive = False
        check(not alive, "the simulator process is gone after stop")
    finally:
        srv.stop()


def api_checks(srv, worlds, tmp):
    st, body, raw, hdr = srv.get("/")
    check(st == 200 and b"World Author" in raw and "default-src 'none'" in hdr.get("Content-Security-Policy", ""),
          f"GET / serves web/author.html with a CSP: {st}")
    for bad in ("/web/editor.html", "/../tools/worldc.py", "/author.html/../../tools/worldc.py", "/tools/worldc.py"):
        st, *_ = srv.get(bad)
        check(st == 404, f"GET {bad}: not served ({st})")
    st, *_ = srv.call("GET", "/api/state", headers={"Host": "evil.example:80"})
    check(st == 403, f"a foreign Host header is refused ({st})")
    st, *_ = srv.call("POST", "/api/check", raw=b"{}", headers={"Content-Type": "text/plain"})
    check(st == 415, f"a POST that is not JSON is refused ({st})")
    st, *_ = srv.get("/api/nope")
    check(st == 404, f"an unknown API: 404 ({st})")
    st, *_ = srv.get("/api/save")
    check(st == 405, f"GET of a POST API: 405 ({st})")
    st, r, *_ = srv.post("/api/check", {"world": [1]})
    check(st == 400, f"a world that is not an object: 400 ({st})")

    st, r, *_ = srv.get("/api/state")
    dirs = {d["key"]: d for d in r["dirs"]}
    check(st == 200 and not dirs["factory"]["writable"] and dirs["user"]["writable"], f"state: {r['dirs']}")
    check(r["sim"]["missing"] and "make -C host" in r["sim"]["build"], f"state reports the missing simulator: {r['sim']}")
    st, r, *_ = srv.get("/api/list")
    fac = [d for d in r["dirs"] if d["key"] == "factory"][0]
    want = sorted(json.loads(f.read_text())["name"] for f in (ROOT / "worlds" / "factory").glob("*.world.json"))
    check(sorted(f["name"] for f in fac["files"]) == want and "NEON RAIN" in want,
          f"list: the {len(want)} factory Worlds: {[f.get('name') for f in fac['files']]}")
    st, r, *_ = srv.get("/api/load?ref=factory/neon_rain.world.json")
    neon = factory("neon_rain")
    check(st == 200 and r["world"] == neon and r["writable"] is False, "load: a factory World, read-only")
    for ref in ("factory/../../../etc/passwd", "user/..%2F..%2Ftools%2Fworldc.py", "user/../factory/neon_rain.world.json",
                "other/x.world.json", "factory/neon_rain.json", "/etc/passwd", "user/.hidden.world.json%00"):
        st, r, *_ = srv.get(f"/api/load?ref={ref}")
        check(st in (400, 403), f"load refuses {ref}: {st}")
    st, r, *_ = srv.post("/api/save", {"ref": "user/../factory/x.world.json", "world": neon})
    check(st in (400, 403), f"save refuses a traversal ({st})")

    before = (worlds / "factory" / "neon_rain.world.json").read_bytes()
    w2 = json.loads(before)
    w2["name"] = "CHANGED"
    st, r, *_ = srv.post("/api/save", {"ref": "factory/neon_rain.world.json", "world": w2})
    check(st == 403 and "read-only" in r["error"], f"a factory World is not written ({st} {r})")
    check((worlds / "factory" / "neon_rain.world.json").read_bytes() == before, "the factory file is untouched")

    st, r, *_ = srv.get("/api/new?id=test_new&name=Test%20New")
    tpl = r["world"]
    check(st == 200 and tpl["id"] == "test_new" and tpl["name"] == "TEST NEW", f"new: {tpl.get('id')} {tpl.get('name')}")
    st, r, *_ = srv.post("/api/save", {"ref": "user/test_new.world.json", "world": tpl, "new": True})
    f = worlds / "user" / "test_new.world.json"
    check(st == 200 and f.exists() and r["check"]["ok"], f"save: a new World under worlds/user ({st} {r.get('error')})")
    text = f.read_text()
    check(json.loads(text) == tpl and text == wa.dumps(tpl), "save: the file holds the posted JSON in the stable layout")
    st, r, *_ = srv.post("/api/save", {"ref": "user/test_new.world.json", "world": tpl, "new": True})
    check(st == 409 and r.get("exists"), f"save as new over an existing file: 409 ({st})")
    st, r, *_ = srv.get("/api/load?ref=user/test_new.world.json")
    mt = r["mtime"]
    time.sleep(0.02)
    os.utime(f, (time.time() + 5, time.time() + 5))
    st, r, *_ = srv.post("/api/save", {"ref": "user/test_new.world.json", "world": tpl, "base_mtime": mt})
    check(st == 409 and r.get("changed"), f"a file changed on disk is not overwritten silently ({st})")
    st, r, *_ = srv.post("/api/save", {"ref": "user/test_new.world.json", "world": tpl, "base_mtime": mt, "force": True})
    check(st == 200, f"... unless forced ({st})")
    st, r, *_ = srv.post("/api/save", {"ref": "user/neon_copy.world.json", "world": neon, "new": True})
    check(st == 200 and (worlds / "user" / "neon_copy.world.json").read_text() == wa.dumps(neon) and
          list(json.loads((worlds / "user" / "neon_copy.world.json").read_text())["variations"]) ==
          list(neon["variations"]), "Save As: a factory World's copy under worlds/user, variations in order")

    st, n, *_ = srv.get("/api/names")
    check(st == 200 and len(n["engines"]) == 9 and all(e["presets"] for e in n["engines"]) and len(n["controls"]) == 16
          and len(n["guard"]) == len(wa.GUARD_FIELDS) and len(n["default_combos"]) == len(worldc.COMBO_DEF) and
          len(n["builtin"]) == len(worldc.BUILTIN) and len(n["lanes"]) == 16 and len(n["scales"]) == 16,
          "names: engines, presets, controls, GUARD defaults, combinations, built-in mappings, lanes, scales")
    check(any(x["label"] == "CUT" and x["role"] == "BRIGHT" for x in n["engines"][0]["edit"]) and
          any(p["name"] == "root" and p["fixed"] for p in n["P"]), "names: roles and flags")

    bad = json.loads(json.dumps(neon))
    bad["tracks"][0]["sound"]["preset"] = "WARM PADD"
    st, r, *_ = srv.post("/api/check", {"world": bad})
    paths = {e["path"]: e["item"] for e in r["errors"]}
    check(st == 200 and not r["ok"] and paths == {"$.tracks[0].sound.preset": "presets"},
          f"check: a bad preset with its JSON path and item: {paths}")
    bad5 = json.loads(json.dumps(neon))
    bad5["progressions"]["main"][0] = "i:5"
    bad5["macros"]["SPACE"][0]["max"] = 120
    st, r, *_ = srv.post("/api/check", {"world": bad5})
    paths = {e["path"]: e["item"] for e in r["errors"]}
    check(paths.get("$.progressions.main") == "harmony" and len(r["errors"]) == 1,
          f"check: a progression of 17 beats: {paths}")
    bad5["progressions"]["main"][0] = "i9:4"
    st, r, *_ = srv.post("/api/check", {"world": bad5})
    check(any(e["path"] == "$.macros.SPACE[0]" and e["item"] in ("macro ranges", "Guardrails") for e in r["errors"]),
          f"check: the model refuses a mapping out of range: {[(e['path'], e['item']) for e in r['errors']]}")
    st, r, *_ = srv.post("/api/check", {"world": neon})
    check(r["ok"] and r["bytes"] == len(worldc.compile_world(neon)[0]), f"check: a factory World is clean: {r['bytes']}")
    st, r, *_ = srv.post("/api/check", {"world": {"format": 3, "tracks": "x"}})
    check(st == 200 and not r["ok"] and r["errors"], "check: a World that is barely JSON gets errors, not a crash")

    st, r, *_ = srv.post("/api/compile", {"world": neon})
    check(st == 200 and r["bytes"] == len(worldc.compile_world(neon)[0]) and r["pool"] == 12 and
          r["limits"]["cap"] == worldc.F["WF_FACTORY_LEN"], f"compile: {r}")
    st, r, *_ = srv.post("/api/compile", {"world": neon, "user": True})
    check(r["limits"]["cap"] == worldc.F["WF_MAX_LEN"], "compile --user: the user limit")
    st, r, *_ = srv.post("/api/compile", {"world": bad})
    check(st == 422 and r["errors"], f"compile of a broken World: 422 with errors ({st})")

    pos = {"COLOR": 0.0, "SPACE": 1.0, "ENERGY": 0.8, "FILTER": 0.2}
    st, r, *_ = srv.post("/api/model", {"world": neon, "scene": "C", "var": "DREAMY", "pos": pos})
    got = {s["target"]: s["effective"] for s in r["slots"]}
    ref = model_reference(neon, "C", "DREAMY", pos)
    check(st == 200 and got == ref and len(got) > 8, f"model = tools/worldc.py's Model: {len(got)} targets "
                                                     f"{set(got.items()) ^ set(ref.items())}")
    s0 = r["slots"][0]
    check(all(k in s0 for k in ("base", "offset", "effective", "norm", "lo", "hi", "smooth", "maps", "controls")) and
          any(m.startswith("$.macros.") for s in r["slots"] for m in s["maps"]) and
          any(m.startswith("built-in FILTER") for s in r["slots"] for m in s["maps"]),
          "model: every slot with base, offset, effective, normalised, range, mappings (the World's and built-in)")
    check(r["pos"][worldc.CTL_NAMES.index("COLOR")] == 0 and r["pos"][worldc.CTL_NAMES.index("SPACE")] == 1000 and
          r["scene"] == "C" and r["var"] == "DREAMY", "model: the positions and the stage asked for")
    st, r, *_ = srv.post("/api/model", {"world": neon, "batch": [{}, {"COLOR": 1}, {"ENERGY": 0}]})
    check(st == 200 and len(r["batch"]) == 3 and r["batch"][0]["scene"] == "B", "model: a batch on one compile")
    def bright(k):
        return next((s["effective"] for s in r["batch"][k]["slots"] if s["target"] == "pad.~bright"), 0)
    check(bright(1) > bright(0) + 10, f"model: COLOR at 100 % brightens the pad ({bright(0)} -> {bright(1)})")
    st, r, *_ = srv.post("/api/model", {"world": neon, "scene": "E"})
    check(st == 400, f"model: a scene that does not exist: 400 ({st})")

    st, r, *_ = srv.post("/api/harmony", {"world": neon})
    main = r["progressions"]["main"]
    check(st == 200 and [c["name"] for c in main["chords"]] == ["Dmadd9", "Bbmaj7", "Gm7", "C"] and main["beats"] == 16
          and main["ok"] and r["used"]["main"] == ["B"], f"harmony: chord names (harmony.c): {[c.get('name') for c in main['chords']]}")
    keys = main["chords"][0]["keys"]
    check(len(keys) == 27 and keys[7]["note"] == 62 and not keys[7]["black"] and keys[1]["black"],
          "harmony: the C4 key plays the tonic D4, 27 keys")
    lo, hi = (worldc.parse_note(x) for x in r["keys"]["range"])
    check(all(lo <= k["note"] <= hi for c in main["chords"] for k in c["keys"]) and
          all(k["chord"] for c in main["chords"] for k in c["keys"] if k["black"]),
          "harmony: every key inside the range, every black key a chord tone (chord, not chord+9)")
    whites = [k["note"] % 12 for k in keys if not k["black"]]
    check(set(whites) <= {2, 5, 7, 9, 0}, f"harmony: the white keys walk D minor pentatonic: {set(whites)}")
    bad2 = json.loads(json.dumps(neon))
    bad2["progressions"]["x"] = ["i:3", "Q"]
    st, r, *_ = srv.post("/api/harmony", {"world": bad2})
    check(st == 200 and not r["progressions"]["x"]["ok"] and not r["progressions"]["x"]["chords"][1]["ok"],
          "harmony: a bad chord and a bad length are reported")

    st, r, *_ = srv.post("/api/pattern", {"world": neon, "name": "pad_main"})
    check(st == 200 and r["ok"] and r["chordy"] and r["progression"] == "main" and r["steps"][0]["notes"] == [50, 57, 62, 65],
          f"pattern: chord tokens over the scene's progression: {r.get('progression')} {r.get('steps', [{}])[0]}")
    st, r, *_ = srv.post("/api/pattern", {"world": neon, "name": "pad_main", "progression": "intro"})
    check(r["progression"] == "intro", "pattern: over another progression")
    st, r, *_ = srv.post("/api/pattern", {"world": neon, "name": "dr_pulse"})
    check(r["ok"] and r["drum"] and r["len"] == 32 and r["steps"][0] == {"kick": "x", "shaker": "s"}, f"pattern: drums {r.get('steps', [0])[:1]}")
    bad3 = json.loads(json.dumps(neon))
    bad3["patterns"]["bass_pedal"]["steps"] = "1 ? 3"
    st, r, *_ = srv.post("/api/pattern", {"world": bad3, "name": "bass_pedal"})
    check(not r["ok"] and r["errors"][0]["path"] == "$.patterns.bass_pedal.steps", f"pattern: a bad step: {r['errors'][:1]}")

    # validate: quick on a factory World (the saved file itself), static on a broken one: background jobs
    st, r, *_ = srv.post("/api/validate", {"world": neon, "ref": "factory/neon_rain.world.json", "mode": "quick"})
    jq = r["job"]
    st, r, *_ = srv.post("/api/validate", {"world": bad, "mode": "static"})
    js = r["job"]
    seen_running = False
    for jid in (js, jq):
        t0 = time.time()
        while True:
            st, j, *_ = srv.get(f"/api/job?id={jid}")
            if j["state"] != "running":
                break
            seen_running = seen_running or (0 <= j["progress"] < 1 and j["phase"])
            if time.time() - t0 > 120:
                break
            time.sleep(0.25)
        if jid == jq:
            rep = j["report"]
            check(j["state"] == "done" and rep["ok"] and len(rep["items"]) == 17 and rep["sweep"] and
                  rep["sweep"]["points"] > 300 and j["static"] and j["file"].endswith("factory/neon_rain.world.json"),
                  f"validate quick: NEON RAIN passes 17 items over the sweep ({j['state']} {j.get('error')})")
            check(any(i["item"] == "CPU budget" and i["data"].get("cpu_mean_max") for i in rep["items"]) and
                  any(i["item"] == "flash budget" and i["data"].get("bytes") == rep["bytes"] and i["data"].get("limit")
                      for i in rep["items"]), "validate: the CPU and flash estimates")
        else:
            check(j["state"] == "done" and not j["report"]["ok"] and j["report"]["sweep"] is None and
                  any(i["item"] == "presets" and i["status"] == "fail" for i in j["report"]["items"]),
                  f"validate static: a broken World fails its items ({j['state']})")
    check(seen_running, "validate: a job reports its phase and progress while it runs")
    st, r, *_ = srv.get("/api/job?id=j999")
    check(st == 404, "job: an unknown job: 404")

    w4 = json.loads(json.dumps(tpl))
    w4["id"] = "author_test_tmp"
    out = ROOT / "build" / "author" / "author_test_tmp"
    try:
        st, r, *_ = srv.post("/api/export", {"world": w4, "kinds": ["wblob", "c", "json"]})
        blob = worldc.compile_world(w4)[0]
        files = {x["kind"]: ROOT / x["path"] for x in r["files"]}
        check(st == 200 and files["wblob"].read_bytes() == blob and base64.b64decode(r["blob"]) == blob and
              files["c"].read_text() == worldc.c_array(blob, "WORLD_AUTHOR_TEST_TMP") and
              files["json"].read_text() == wa.dumps(w4), f"export: the blob, a C array, the JSON copy ({st})")
        st, r, *_ = srv.post("/api/export", {"world": w4, "user": True, "kinds": ["wblob"]})
        check(worldc.decode(base64.b64decode(r["blob"]))["flags"] & worldc.F["WF_F_USER"], "export --user: flag USER")
        st, r, *_ = srv.post("/api/export", {"world": bad})
        check(st == 422, "export of a broken World: 422")
    finally:
        shutil.rmtree(out, ignore_errors=True)

    ub = with_overrides(worldc.compile_world(tpl, True)[0], [(worldc.F["WF_OVR_SOUND"] | 2, worldc.PR.eng_id["ANALOG"], 3)])
    st, r, *_ = srv.post("/api/import", {"blob": base64.b64encode(ub).decode()})
    check(st == 200 and r["world"]["tracks"][2]["sound"]["engine"] == "ANALOG" and
          worldc.compile_world(r["world"])[0] is not None, f"import: a user World blob as a source ({st})")
    st, r, *_ = srv.post("/api/import", {"blob": "not base64!"})
    check(st == 422, f"import of garbage: 422 ({st})")

    st, r, *_ = srv.get("/api/preview")
    check(r["missing"] and not r["running"], "preview: the simulator is missing")
    st, r, *_ = srv.post("/api/preview", {"ref": "user/test_new.world.json"})
    check(st == 200 and r["missing"] and "make -C host" in r["error"], f"preview without a simulator says how to build it: {r}")
    st, r, *_ = srv.post("/api/preview", {"ref": "user/never_saved.world.json"})
    check(st == 404, f"preview of a file not saved: 404 ({st})")


# ------------------------------------------------------------------------------------------- the page ---
class Parser(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts, self.tags, self.cur = [], [], None

    def handle_starttag(self, tag, attrs):
        self.tags.append((tag, dict(attrs)))
        if tag == "script":
            self.cur = []

    def handle_endtag(self, tag):
        if tag == "script" and self.cur is not None:
            self.scripts.append("".join(self.cur))
            self.cur = None

    def handle_data(self, data):
        if self.cur is not None:
            self.cur.append(data)


def test_page(tmp):
    text = PAGE.read_text()
    p = Parser()
    p.feed(text)
    p.close()
    check(len(p.scripts) == 1 and "/*HELPERS-BEGIN*/" in p.scripts[0], "author.html parses: one inline script")
    check(not re.search(r"(?i:https?://|(?:src|href)\s*=\s*[\"']//|@import)|\burl\(", text), "author.html names no URL")
    check(not any(t in ("link", "iframe", "img", "object", "embed") or (t == "script" and "src" in a) for t, a in p.tags),
          "author.html loads nothing (no script src, link, img)")
    called = set(re.findall(r"\bapi\(\s*[\"`]([a-z]+)", p.scripts[0]))
    app = wa.App(Namespace(worlds=str(tmp / "worlds"), extra=None, allow_factory=False, path=None, sim="x"))
    routes = {name for _, name in app.routes()}
    check(called and called <= routes, f"every API the page calls exists: {called - routes}")
    check(routes <= called, f"the page uses every API: {routes - called}")
    if not shutil.which("node"):
        print("author: no node: the page's helpers are not run (they are in web/author.html between HELPERS-BEGIN/END)")
        return
    helpers_node(tmp, text)


def helpers_node(tmp, text):
    """the page's pure helpers in node's vm, against tools/worldc.py on the same inputs"""
    steps = ["1 2 3 | 4 5 6", "[c1 c3 c5]! - . c7~", "[1 5]_ c9*2 A#3 | . - - -", "[c1  c5' ] 1,", "x|y", "[c1 c3"]
    toks = ["1", "b3", "#4'", "5''", "1,", "c1", "c9", "c3,", "A#3", "Bb2", "C4", "[c1 c3 c5]", "c7~!", "c9*2", "1!!",
            "[1 2 3 4 5]", "x", "c2", "H3", ".", "-", "[c1 c3]_", "1*5", "C-1", "G9", "[]", "c1*2*3"]
    chords = ["i", "VI:4", "bVII7", "ii°:2", "IVmaj7", "imaj7", "Isus", "v9:8", "VIIsus4:2", "x", "I:0", "I:33", "iø",
              "III+", "I5", "#iv°7", "IM7", "iadd9:16"]
    curves = {"pts": {"points": [[0, 0], [0.5, 0.2], [1, 1]]}, "vals": [0, 0.05, 0.1, 0.2, 0.3, 0.45, 0.6, 0.8, 1],
              "lut": {"lut": [0, 10, 30, 60, 100, 140, 180, 220, 255]}}
    js = text[text.index("/*HELPERS-BEGIN*/"):text.index("/*HELPERS-END*/")]
    prog = tmp / "helpers.mjs"
    prog.write_text("import vm from 'node:vm';\nimport { readFileSync } from 'node:fs';\n"
                    "const inp = JSON.parse(readFileSync(process.argv[2], 'utf8'));\n"
                    "const H = vm.runInNewContext(readFileSync(process.argv[3], 'utf8') + `;({scanSteps, parseStep, parseSteps, "
                    "joinSteps, withSuffix, parseChord, progBeats, chordWithBeats, curveY, mapOffset, drumCells, drumString, "
                    "drumCycle, drumResize, drumLength, renameTarget, mask16, mask16Str, jp, noteName, parseNote, pruneEmpty})`);\n"
                    "const out = {};\n"
                    "out.scan = inp.steps.map((s) => H.scanSteps(s));\n"
                    "out.tok = inp.toks.map((t) => H.parseStep(t).ok);\n"
                    "out.chord = inp.chords.map((c) => { const p = H.parseChord(c); return p.ok ? p.beats : null; });\n"
                    "out.curve = {};\n"
                    "for (const c of ['lin', 'exp', 'log', 's', 'late', 'pts', 'vals', 'lut']) out.curve[c] = [0, 1, 2, 3, 4, 5, 6, 7, 8]"
                    ".map((k) => H.curveY(c, k / 8 + 0.03 * (k < 8), inp.curves));\n"
                    "out.misc = {\n"
                    "  beats: H.progBeats(['i9:8', 'VImaj7:8']), badBeats: H.progBeats(['i', 'Q']), cwb: H.chordWithBeats('VIIsus4:2', 4),\n"
                    "  join: H.joinSteps(['1', '2', '3', '4', '5', '6', '7', '8'], 4), join2: H.joinSteps(['1', '2', '3'], 4),\n"
                    "  barsErr: H.parseSteps('1 2 | 3', 4).error, len65: H.parseSteps(Array(65).fill('1').join(' '), 16).error,\n"
                    "  sfx: [H.withSuffix('c1', '!', true), H.withSuffix('c1!~', '!', false), H.withSuffix('.', '!', true), H.withSuffix('[1 5]_', '*3', true)],\n"
                    "  map: [H.mapOffset({min: -10, max: 20}, 0, 0.5), H.mapOffset({min: -10, max: 20}, 1, 0.5), H.mapOffset({min: -10, max: 20}, 0.5, 0.5),"
                    " H.mapOffset({min: -10, max: 20, curve: 'exp'}, 0.75, 0.5), H.mapOffset({max: 40}, 0.5, 0)],\n"
                    "  cells: H.drumCells('x...|x...').join(''), str32: H.drumString(H.drumCells('x'.repeat(32)), true),\n"
                    "  cyc: [H.drumCycle('.', 1), H.drumCycle('x', -1), H.drumCycle('4', 1), H.drumCycle('.', -1)],\n"
                    "  resize: H.drumResize({kick: 'x'.repeat(16)}, 32).kick, shrink: H.drumResize({kick: 'x...|x...|x...|x...'.replace(/\\|/g, '').repeat(2)}, 16).kick,\n"
                    "  dlen: H.drumLength({a: 'x...|x...x...x...|x...x...x...x...'}),\n"
                    "  ren: [H.renameTarget('pad+bass.rev', 'bass', 'sub'), H.renameTarget('bass.CUT', 'bass', 'sub'), H.renameTarget('g.dfdbk', 'pad', 'x')],\n"
                    "  m16: H.mask16Str(H.mask16('x.x.|x')), jp: [H.jp('$.a', 'b c'), H.jp('$.a', 3), H.jp('$.v', 'ORIGINAL')],\n"
                    "  notes: [H.noteName(60), H.noteName(21), H.parseNote('A#3'), H.parseNote('Bb2'), H.parseNote('C-1'), H.parseNote('H2')],\n"
                    "  prune: JSON.stringify(H.pruneEmpty({a: {}, b: [], c: {x: 1}, d: 0}))};\n"
                    "console.log(JSON.stringify(out));\n")
    (tmp / "helpers.js").write_text(js)
    (tmp / "helpers-in.json").write_text(json.dumps({"steps": steps, "toks": toks, "chords": chords, "curves": curves}))
    r = subprocess.run(["node", str(prog), str(tmp / "helpers-in.json"), str(tmp / "helpers.js")], capture_output=True,
                       text=True)
    if not check(r.returncode == 0, f"node runs the page's helpers: {r.stderr[-800:]}"):
        return
    out = json.loads(r.stdout)
    check(out["scan"] == [worldc.scan_steps(s) for s in steps], f"scanSteps = worldc scan_steps: {out['scan']}")
    d = worldc.Diag()
    c = worldc.Compiler(json.loads((ROOT / "worlds" / "test" / "minimal.world.json").read_text()), d)
    c.meta()
    c.tracks()
    ref = [c.step_token("$", t, 1, 0) is not None for t in toks]
    check(out["tok"] == ref, f"parseStep accepts what worldc step_token accepts: "
                             f"{[t for t, a, b in zip(toks, out['tok'], ref) if a != b]}")
    refc = []
    for ch in chords:
        x = c.chord("$", ch)
        refc.append(x[2] if x else None)
    check(out["chord"] == refc, f"parseChord = worldc chord (and its beats): {out['chord']} {refc}")
    md = worldc.Model.__new__(worldc.Model)
    md.ir = {"curves": []}
    for k, cname in enumerate(["lin", "exp", "log", "s", "late"]):
        want = [md.curve(k, int((i / 8 + 0.03 * (i < 8)) * 4096)) / 4096 for i in range(9)]
        check(all(abs(a - b) < 0.003 for a, b in zip(out["curve"][cname], want)), f"curveY {cname} = macro.c's: "
                                                                                    f"{out['curve'][cname]} {want}")
    for cname, cdef in curves.items():
        lut = c.lut("$", cdef)
        md.ir = {"curves": [lut]}
        want = [md.curve(worldc.F["WF_CURVE_CUSTOM"], int((i / 8 + 0.03 * (i < 8)) * 4096)) / 4096 for i in range(9)]
        check(all(abs(a - b) < 0.006 for a, b in zip(out["curve"][cname], want)), f"curveY custom {cname} = macro.c's")
    m = out["misc"]
    check(m["beats"] == 16 and m["badBeats"] is None and m["cwb"] == "VIIsus4:4", f"progBeats, chordWithBeats: {m}")
    check(m["join"] == "1 2 3 4 | 5 6 7 8" and m["join2"] == "1 2 3" and "bars of" in m["barsErr"] and
          "65 steps" in m["len65"], "joinSteps, parseSteps' bar and length errors")
    check(m["sfx"] == ["c1!", "c1~", ".", "[1 5]_*3"], f"withSuffix: {m['sfx']}")
    check(m["map"] == [-10, 20, 0, 5, 20], f"mapOffset: {m['map']}")
    check(m["cells"] == "x...x..." and m["str32"] == "x" * 16 + "|" + "x" * 16 and m["cyc"] == ["x", ".", ".", "4"] and
          m["resize"] == "x" * 16 + "|" + "." * 16 and m["shrink"] == "x...x...x...x..." and m["dlen"] == 32,
          f"the drum helpers: {m}")
    check(m["ren"] == ["pad+sub.rev", "sub.CUT", "g.dfdbk"], f"renameTarget: {m['ren']}")
    check(m["m16"] == "x.x.x..........." and m["jp"] == [worldc.jp("$.a", "b c"), "$.a[3]", "$.v.ORIGINAL"],
          f"mask16, jp: {m['m16']} {m['jp']}")
    check(m["notes"] == [worldc.note_name(60), worldc.note_name(21), worldc.parse_note("A#3"), worldc.parse_note("Bb2"),
                         worldc.parse_note("C-1"), None], f"noteName / parseNote: {m['notes']}")
    check(m["prune"] == '{"c":{"x":1},"d":0}', f"pruneEmpty: {m['prune']}")


def main():
    t0 = time.time()
    with tempfile.TemporaryDirectory(prefix="author-test-") as td:
        tmp = Path(td)
        for name, t in (("layout", test_layout), ("server", lambda: test_server(tmp)), ("page", lambda: test_page(tmp))):
            n = len(FAILS)
            t()
            print(f"{name:<8} {'ok' if len(FAILS) == n else 'FAIL'}")
    print(f"author: all {COUNT[0]} checks passed ({time.time() - t0:.1f} s)" if not FAILS else
          f"author: {len(FAILS)} of {COUNT[0]} checks failed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
