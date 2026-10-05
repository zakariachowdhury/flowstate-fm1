#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_monitor.py without hardware: the `flow` / `status` parsers, the CSV and the summary from a canned console
transcript, --fit on a synthetic CSV, the CLI's exit codes, and the live path against a pseudo-terminal that plays the
FM-1's console (never a real port: every run names its --port). Run from the repo root:
  python3 tests/fm1_monitor_test.py"""
import contextlib
import csv
import io
import os
import select
import subprocess
import sys
import tempfile
import threading
import tty
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_monitor as M  # noqa: E402

failed = 0


def ok(cond, what):
    global failed
    if not cond:
        failed += 1
        print("FAIL:", what)


def flow(mode="PLAY", world="NEON RAIN", wid="4EEE4454", scene="A", var="ORIGINAL", macro=(420, 310, 760, 550), band=1,
         bands=3, voices=5, drums=2, q8=80, est=1210, load=47, guard=0, max_us=2100, last_us=1800, late=0, shed=0):
    if mode == "SLOOP":
        world, wid, scene, var = "-", "00000000", "-", "-"
    return (f'flow mode={mode} world="{world}" id={wid} scene={scene} var="{var}" macro={",".join(map(str, macro))} '
            f"band={band} bands={bands} voices={voices} drums={drums} cpu_pct={q8 * 100 // 256} cpu_q8={q8} est={est} "
            f"load_pct={load} ceil_pct=85 guard={guard} max_us={max_us} last_us={last_us} late={late} shed={shed}")


STATUS = ("felucca SLOOP 2.1\nuptime_ms 12345\ncpu_pct 31\naudio_max_us 2100\nvoices_shed 0\nvoices_given_up 0\ntrack 1\n"
          "batt_raw 3000\nengine ANALOG\npreset FUNK BASS\nbpm 72\nplaying 1\nboots 1\nusb_resets 1\nflash 1")


def transcript():
    """what a terminal shows: echo of what was typed, replies, prompts, CR LF"""
    seq = [
        flow(),                                                                           # 1 NEON RAIN A
        flow(scene="B", macro=(1000, 1000, 1000, 1000), q8=150, est=1900, voices=8, max_us=3400),      # 2
        flow(scene="B", macro=(1000, 1000, 1000, 1000), q8=230, est=2500, guard=1, max_us=5200, shed=2, late=1),   # 3 holds
        flow(scene="B", q8=215, est=2300, guard=1, max_us=5200, shed=2, late=3),                  # 4 still holding
        flow(scene="C", q8=120, est=1500, guard=0, max_us=5200, shed=2, late=3),                  # 5 let go
        flow(world="MIDNIGHT DRIVE", wid="12345678", var="DREAMY", q8=240, est=2600, guard=1, max_us=5300, shed=2, late=3),   # 6
        flow()[:-8],                                                                      # 7 cut short: no sample
        "? (help)",                                                                       # 8 not a flow reply
        flow(mode="SLOOP", q8=60, est=0, max_us=900, late=3, shed=0),                      # 9 after a reset, SLOOP mode
    ]
    out = "\r\nFelucca SLOOP 2.1 console - 'help'\r\n> "
    for i, line in enumerate(seq):
        out += "flow\r\n" + line + "\r\n> "
        if i == 0:
            out += "status\r\n" + STATUS.replace("\n", "\r\n") + "\r\n> "
    return out, seq


def main():
    # --- parse_flow
    f = M.parse_flow(flow(world="MIDNIGHT DRIVE", var="DREAMY", guard=1, macro=(1, 22, 333, 1000)))
    ok(f and f["world"] == "MIDNIGHT DRIVE" and f["var"] == "DREAMY" and f["scene"] == "A", f"names with spaces: {f}")
    ok(f and (f["color"], f["motion"], f["space"], f["energy"]) == (1, 22, 333, 1000), "the four macros")
    ok(f and f["guard"] == 1 and f["cpu_q8"] == 80 and f["est"] == 1210 and f["max_us"] == 2100 and f["shed"] == 0, "numbers")
    ok(f and f["id"] == "4EEE4454" and f["mode"] == "PLAY" and f["bands"] == 3 and f["drums"] == 2, "id, mode, band")
    ok(M.parse_flow("> " + flow()) is not None, "a prompt before the reply")
    ok(M.parse_flow(flow()[:-8]) is None, "a reply cut short before shed is refused")
    ok(M.parse_flow(flow() + " junk") is None, "shed is not the last field")
    ok(M.parse_flow("flow") is None and M.parse_flow("> flow") is None and M.parse_flow("") is None, "an echo is no reply")
    ok(M.parse_flow(flow(mode="SLOOP"))["world"] == "-", "SLOOP mode: no World")
    # --- parse_status
    s = M.parse_status(STATUS.split("\n"))
    ok(s.get("uptime_ms") == 12345 and s.get("boots") == 1 and s.get("bpm") == 72 and s.get("preset") == "FUNK BASS", f"status {s}")
    ok(M.parse_status(["Felucca SLOOP 2.1 console - 'help'", "cpu_pct 3"]) == {}, "the console banner is not a status")
    # --- the transcript
    text, seq = transcript()
    cyc = M.parse_transcript(text)
    ok(len(cyc) == 7, f"7 samples (2 lines are no sample), got {len(cyc)}")
    ok(cyc[0][1].get("uptime_ms") == 12345 and not cyc[1][1], "status belongs to the flow before it")
    only = M.parse_transcript("> status\r\n" + STATUS.replace("\n", "\r\n") + "\r\n> status\r\n" + STATUS.replace("\n", "\r\n") + "\r\n> ")
    ok(len(only) == 2 and all(c[0] is None and c[1].get("cpu_pct") == 31 for c in only), "status-only transcript: a sample each")
    r = M.make_row(1.5, "", "", *only[0])
    ok(r["cpu_pct"] == 31 and r["max_us"] == 2100 and r["uptime_ms"] == 12345 and r["mode"] == "?", "a status-only row")
    # --- dry run: CSV and summary
    with tempfile.TemporaryDirectory() as d:
        tf, cf = os.path.join(d, "t.txt"), os.path.join(d, "o.csv")
        Path(tf).write_text(text, encoding="latin-1")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = M.main(["--dry-run", tf, "--out", cf, "--interval", "200", "--tag", "canned", "--quiet"])
        ok(rc == 0, f"dry run exits 0, got {rc}")
        rows = list(csv.DictReader(open(cf, newline="")))
        ok(open(cf).readline().strip().split(",") == M.COLUMNS, "CSV header")
        ok(len(rows) == 7, f"7 CSV rows, got {len(rows)}")
        ok([r["world"] for r in rows] == ["NEON RAIN"] * 5 + ["MIDNIGHT DRIVE", "-"], "World column")
        ok([r["scene"] for r in rows][:5] == list("ABBBC"), "scene column")
        ok((rows[1]["color"], rows[1]["motion"], rows[1]["space"], rows[1]["energy"]) == ("1000",) * 4, "macro columns")
        ok([r["guard"] for r in rows] == ["0", "0", "1", "1", "0", "1", "0"], "guard column")
        ok(rows[0]["uptime_ms"] == "12345" and rows[0]["boots"] == "1" and rows[1]["uptime_ms"] == "", "status columns on row 1 only")
        ok(rows[2]["cpu_q8"] == "230" and rows[2]["est"] == "2500" and rows[2]["max_us"] == "5200" and rows[2]["shed"] == "2", "numbers")
        ok([r["late"] for r in rows] == ["0", "0", "1", "3", "3", "3", "3"], "late column")
        ok(rows[3]["t_s"] == "0.600" and all(r["tag"] == "canned" for r in rows), "time and tag")
        out = buf.getvalue()
        ok("samples 7" in out and "resets seen 1" in out and "late halves (dropouts) 3" in out, f"summary: samples, the reset, late\n{out}")
        ok("2 engagement(s), holding in 3 of 7 samples" in out, f"summary: guard engagements\n{out}")
        ok("peak cpu_q8 240" in out and "MIDNIGHT DRIVE" in out, "summary: peak CPU and where")
        ok("peak guard estimate 2600" in out and "max half 5300 us" in out and "voices shed 2" in out, "summary: estimate, half, shed")
        # --- --fit on a synthetic CSV: cpu_q8 = 0.09 est + 4, the proposal follows
        fc = os.path.join(d, "fit.csv")
        with open(fc, "w", newline="") as fh:
            w = csv.DictWriter(fh, M.COLUMNS)
            w.writeheader()
            for i in range(120):
                est = 900 + 15 * i
                w.writerow(M.make_row(i * 0.25, "", "heavy" if i > 60 else "light",
                                      M.parse_flow(flow(est=est, q8=round(0.09 * est + 4), max_us=100 + i)), {}))
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            ok(M.main(["--fit", fc]) == 0, "--fit exits 0")
        out = buf.getvalue()
        est_c = (0.85 * 256 - 4) / 0.09
        ok(f"GL_CPU_FULL {round(est_c / 0.85 / 10) * 10}" in out, f"--fit GL_CPU_FULL\n{out}")
        ok(f"GL_CPU_BUDGET {int(est_c * 0.9 // 10 * 10)}" in out and "R2 1.000" in out, f"--fit GL_CPU_BUDGET\n{out}")
        ok("heavy" in out and "light" in out, "--fit lists the tags")
        # --- the CLI
        env = dict(os.environ)
        r = subprocess.run([sys.executable, str(ROOT / "tools/fm1_monitor.py"), "--dry-run", "-", "--quiet"], input=text,
                           capture_output=True, text=True, env=env)
        ok(r.returncode == 0 and "2 engagement(s)" in r.stdout, f"CLI dry run from stdin: {r.returncode} {r.stderr}")
        r = subprocess.run([sys.executable, str(ROOT / "tools/fm1_monitor.py"), "--dry-run", "-", "--quiet"], input="nothing here\n",
                           capture_output=True, text=True)
        ok(r.returncode == 2, f"an empty transcript exits 2, got {r.returncode}")
        r = subprocess.run([sys.executable, str(ROOT / "tools/fm1_monitor.py"), "--dry-run", os.path.join(d, "none")],
                           capture_output=True, text=True)
        ok(r.returncode == 2, f"a missing transcript exits 2, got {r.returncode}")
        r = subprocess.run([sys.executable, str(ROOT / "tools/fm1_monitor.py"), "--interval", "10"], capture_output=True, text=True)
        ok(r.returncode == 2, f"--interval 10 exits 2, got {r.returncode}")
        # --- no port / several ports: nothing is opened
        real = M.find_ports
        try:
            M.find_ports = lambda: []
            with contextlib.redirect_stderr(io.StringIO()):
                rc = M.main(["--out", os.path.join(d, "x.csv")])
            ok(M.resolve_port(None)[0] is None and rc == 3, "no port: exit 3")
            M.find_ports = lambda: ["/dev/cu.usbmodemA", "/dev/cu.usbmodemB"]
            p, why = M.resolve_port(None)
            ok(p is None and "usbmodemA" in why and "--port" in why, "two ports: listed, none opened")
            M.find_ports = lambda: ["/dev/cu.usbmodemA"]
            ok(M.resolve_port(None) == ("/dev/cu.usbmodemA", None) and M.resolve_port("/dev/x") == ("/dev/x", None), "one port, or --port")
        finally:
            M.find_ports = real
        ok(not os.path.exists(os.path.join(d, "x.csv")), "no CSV when there is no port")
        # --- live, against a pseudo-terminal playing the console
        return pty_test(d)


IMG = bytes((i * 7 + 3) & 255 for i in range(0x100000))        # the fake FM-1's flash


class FakeConsole:
    """a pseudo-terminal playing the FM-1's console: banner, echo, prompts, CR LF, `flow`, `status`, `flr`, `crash`"""

    def __init__(self):
        self.master, self.slave = os.openpty()
        tty.setraw(self.slave)               # (so the banner is not echoed back: the monitor sets this too)
        self.name = os.ttyname(self.slave)
        self.stop, self.seen, self.polls = threading.Event(), [], 0

    def __enter__(self):
        self.th = threading.Thread(target=self.serve, daemon=True)
        self.th.start()
        return self

    def __exit__(self, *a):
        self.stop.set()
        self.th.join(2)
        os.close(self.slave)
        os.close(self.master)

    def say(self, cmd, body):
        os.write(self.master, cmd.encode() + b"\r\n" + body.replace("\n", "\r\n").encode() + b"\r\n> ")

    def serve(self):
        os.write(self.master, b"\r\nFelucca SLOOP 2.1 console - 'help'\r\n> ")
        buf = b""
        while not self.stop.is_set():
            if not select.select([self.master], [], [], 0.05)[0]:
                continue
            try:
                buf += os.read(self.master, 256)
            except OSError:
                return
            while b"\r" in buf:
                cmd, buf = buf.split(b"\r", 1)
                cmd = cmd.decode()
                self.seen.append(cmd)
                w = cmd.split()
                if not w:
                    os.write(self.master, b"\r\n> ")
                elif w[0] == "flow":
                    self.polls += 1
                    self.say(cmd, flow(world="DUSTY CAFE", wid="0A0B0C0D", scene="D", q8=40 + self.polls,
                                       est=1000 + 10 * self.polls, max_us=1000 + self.polls))
                elif w[0] == "status":
                    self.say(cmd, STATUS)
                elif w[0] == "crash":
                    self.say(cmd, "no crash record")
                elif w[0] == "flr":
                    a, n = int(w[1], 0), int(w[2], 0)
                    self.say(cmd, "\n".join("%06X:" % (a + o) + "".join(" %02X" % b for b in IMG[a + o:a + min(o + 16, n)])
                                            for o in range(0, n, 16)))
                else:
                    self.say(cmd, "? (help)")


def pty_test(d):
    with FakeConsole() as con:
        cf = os.path.join(d, "live.csv")
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = M.main(["--port", con.name, "--interval", "50", "--duration", "1.2", "--out", cf, "--quiet",
                         "--status-every", "5", "--tag", "live"])
        rows = list(csv.DictReader(open(cf, newline="")))
        ok(rc == 0, f"live run exits 0, got {rc}")
        ok(10 <= len(rows) <= 30, f"about 20 polls in 1.2 s at 50 ms, got {len(rows)}")
        ok(rows and rows[0]["world"] == "DUSTY CAFE" and rows[0]["scene"] == "D" and rows[0]["tag"] == "live", "live rows")
        ok(rows and [int(r["cpu_q8"]) for r in rows] == sorted(int(r["cpu_q8"]) for r in rows), "rows in order, one per reply")
        ok(rows and rows[0]["uptime_ms"] == "12345" and rows[1]["uptime_ms"] == "" and rows[5]["uptime_ms"] == "12345",
           "status every 5th poll")
        ok(set(con.seen) <= {"flow", "status"} and "flow" in con.seen and "status" in con.seen,
           f"only read-only commands were sent: {set(con.seen)}")
        ok(f"samples {len(rows)}" in buf.getvalue() and "CSV:" in buf.getvalue(), "the summary and the CSV name are printed")
        # --cmd: read-only commands, once; anything else is refused before a byte is sent
        con.seen.clear()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = M.main(["--port", con.name, "--cmd", "crash", "--cmd", "flr 0xE5F00 20"])
        out = buf.getvalue()
        ok(rc == 0 and "no crash record" in out, f"--cmd crash: {rc} {out!r}")
        want = "0E5F00:" + "".join(" %02X" % b for b in IMG[0xE5F00:0xE5F10]) + "\n0E5F10:" + "".join(" %02X" % b for b in IMG[0xE5F10:0xE5F14])
        ok(want in out, f"--cmd flr prints the bytes\n{out}")
        ok(con.seen[-2:] == ["crash", "flr 0xE5F00 20"], f"--cmd sent what was asked: {con.seen}")
        con.seen.clear()
        with contextlib.redirect_stderr(io.StringIO()):
            rc = M.main(["--port", con.name, "--cmd", "uboot yes"])
        ok(rc == 2 and not con.seen, f"`uboot` is refused (rc {rc}, sent {con.seen})")
        # --dump-flash: the bytes, in order, 256 at a time, with a sha256
        import hashlib
        for n in (0x300, 0x2F3):
            f = os.path.join(d, "dump%X.bin" % n)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf), contextlib.redirect_stderr(io.StringIO()):
                rc = M.main(["--port", con.name, "--dump-flash", "0xE5000", hex(n), f])
            ok(rc == 0 and open(f, "rb").read() == IMG[0xE5000:0xE5000 + n], f"--dump-flash {n:#x}: {rc}")
            ok(hashlib.sha256(IMG[0xE5000:0xE5000 + n]).hexdigest() in buf.getvalue(), "--dump-flash prints the sha256")
        with contextlib.redirect_stderr(io.StringIO()):
            ok(M.main(["--port", con.name, "--dump-flash", "0x1000", "16", os.path.join(d, "x.bin")]) == 2, "flr range: exit 2")
    ok(M.parse_flr("0E5F00: 01 02\r\n0E5F02: 03", 0xE5F00, 3) == b"\x01\x02\x03" and M.parse_flr("0E5F00: 01 02", 0xE5F00, 3) is None
       and M.parse_flr("0E5F10: 01", 0xE5F00, 1) is None, "parse_flr: whole, short, out of order")
    print("fm1_monitor_test:", "FAILED %d" % failed if failed else "all checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
