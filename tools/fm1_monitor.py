#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""fm1_monitor: log the FM-1's USB serial console to CSV while you play (Phase 17, docs/hardware-calibration.md).

Every --interval ms it sends `flow` (what the World, the macros and the CPU guard are doing: firmware/src/console.c)
and, every --status-every polls, `status` (uptime, boots, tempo). One CSV row per poll, a live summary on the
terminal. It only reads: neither command writes memory or flash. Standard library only (os, termios, select).

  fm1_monitor.py [--port PATH] [--interval MS] [--out FILE.csv] [--duration S] [--tag TEXT] [--status-every N]
  fm1_monitor.py --dry-run TRANSCRIPT [--out FILE.csv]     replay a recorded console session, no device
  fm1_monitor.py --cmd "flr 0xE5F00 256" --cmd crash       send read-only console commands once, print the replies
  fm1_monitor.py --dump-flash 0xE5000 0x17000 before.bin   read flash 0x93000..0xFFFFF with `flr` into a file
  fm1_monitor.py --fit FILE.csv [--ceiling PCT] [--margin PCT]   fit the CPU guard's constants (calibration doc)

While it runs on a terminal, type a label and Enter to tag the rows that follow (e.g. "neon rain all macros 100");
an empty line clears the tag. The tag column is what --fit groups by.

Without --port it takes the one /dev/cu.usbmodem* (macOS) or /dev/ttyACM* (Linux) there is; with several it lists
them and stops (it never probes other gear). If the FM-1 resets or the cable is pulled it waits for the port,
reconnects, reads `crash` and carries on; those events go to the terminal and to FILE.csv.log.

Exit codes: 0 done, 2 bad arguments or input, 3 no console port, 4 the port gave no usable reply.
"""
import argparse
import csv
import glob
import os
import re
import select
import statistics
import struct
import sys
import termios
import time

COLUMNS = ["t_s", "iso", "tag", "uptime_ms", "mode", "world", "id", "scene", "var", "color", "motion", "space",
           "energy", "band", "bands", "voices", "drums", "cpu_pct", "cpu_q8", "est", "load_pct", "ceil_pct", "guard",
           "max_us", "last_us", "late", "shed", "boots", "playing", "bpm"]
INT_FIELDS = ("band", "bands", "voices", "drums", "cpu_pct", "cpu_q8", "est", "load_pct", "ceil_pct", "guard",
              "max_us", "last_us", "late", "shed")
HALF_US = 5805                        # one DMA half buffer; the device sheds a voice above 85 % of it

FLOW_RE = re.compile(r"(?:^|\s)flow\s+(mode=.*)$")
KV_RE = re.compile(r'(\w+)=("[^"]*"|\S+)')
STATUS_KV = re.compile(r"^(\w+) (-?\d+)$")
STATUS_STR = re.compile(r"^(engine|preset) (.+)$")
BANNER = re.compile(r"^felucca \S")   # status' first line ("felucca SLOOP 2.1"); the console's own banner is "Felucca ..."


def parse_flow(line):
    """One `flow` reply line -> dict, or None when it is not one or was cut short (the console drops the rest of a
    reply when the host stops reading; `shed` is the last field, so a line without it is not trusted)."""
    m = FLOW_RE.search(line.strip())
    if not m:
        return None
    kv = {k: v.strip('"') for k, v in KV_RE.findall(m.group(1))}
    if "shed" not in kv or not m.group(1).rstrip().split()[-1].startswith("shed="):
        return None
    try:
        d = {k: int(kv[k]) for k in INT_FIELDS}
        macro = [int(x) for x in kv["macro"].split(",")]
        if len(macro) != 4:
            return None
        d.update(zip(("color", "motion", "space", "energy"), macro))
        d.update(mode=kv["mode"], world=kv["world"], id=kv["id"], scene=kv["scene"], var=kv["var"])
    except (KeyError, ValueError):
        return None
    return d


def parse_status(lines):
    """`status` reply lines -> dict (ints, plus engine / preset), {} when there is no status block."""
    d, on = {}, False
    for ln in lines:
        ln = ln.strip()
        if BANNER.match(ln):
            on = True
        elif on:
            m = STATUS_KV.match(ln) or STATUS_STR.match(ln)
            if m:
                d[m.group(1)] = int(m.group(2)) if m.group(2).lstrip("-").isdigit() else m.group(2)
    return d if on else {}


def split_lines(text):
    return [ln for ln in re.split(r"\r\n|\n|\r", text)]


def parse_transcript(text):
    """A recorded console session -> [(flow dict | None, status dict)] per poll. Prompts ("> ") and the echo of what
    was typed are ignored. A status block belongs to the `flow` line before it, else it is a poll of its own
    (an older firmware without `flow`)."""
    cycles, cur, block = [], None, []

    def close():
        nonlocal block
        if block and cur is not None and cur[1] is None:
            cur[1] = parse_status(block)
        block = []

    for raw in split_lines(text):
        line = raw.strip()
        line = line[1:].strip() if line.startswith(">") else line
        f = parse_flow(line)
        if f:
            close()
            cur = [f, None]
            cycles.append(cur)
        elif BANNER.match(line):
            close()
            if cur is None or cur[1] is not None or cur[0] is None:
                cur = [None, None]
                cycles.append(cur)
            block = [line]
        elif block and (STATUS_KV.match(line) or STATUS_STR.match(line)):
            block.append(line)
        elif block:
            close()
    close()
    return [(f, s or {}) for f, s in cycles]


def make_row(t_s, iso, tag, flow, status):
    row = {c: "" for c in COLUMNS}
    row.update(t_s=f"{t_s:.3f}", iso=iso, tag=tag)
    if flow:
        row.update({k: flow[k] for k in COLUMNS if k in flow})
    else:                                                     # status only: what it has
        row.update(mode="?", cpu_pct=status.get("cpu_pct", ""), max_us=status.get("audio_max_us", ""),
                   shed=status.get("voices_shed", ""))
    for k_src, k_dst in (("uptime_ms", "uptime_ms"), ("boots", "boots"), ("playing", "playing"), ("bpm", "bpm")):
        if k_src in status:
            row[k_dst] = status[k_src]
    return row


# ----------------------------------------------------------------------------------------------- summary ---
class Stats:
    """What the live line and the closing summary say: peaks, guard engagements, resets, shedding."""

    def __init__(self):
        self.n = self.resets = self.engagements = self.guard_rows = self.late = 0
        self.held_s = 0.0
        self.peak_cpu = self.peak_est = self.peak_us = None
        self.prev = None
        self.shed = 0

    def add(self, row):
        def num(k):
            return row[k] if isinstance(row[k], int) else None
        self.n += 1
        t = float(row["t_s"])
        guard, cpu, est, us, shed, late = (num(k) for k in ("guard", "cpu_q8", "est", "max_us", "shed", "late"))
        if self.prev is not None:
            p = self.prev
            if (us is not None and p["us"] is not None and us < p["us"]) or \
               (shed is not None and p["shed"] is not None and shed < p["shed"]):
                self.resets += 1                              # max_us and the shed count restart at boot
            if late is not None and p["late"] is not None and late > p["late"]:
                self.late += late - p["late"]                 # halves the DMA outran: audible dropouts
            if p["guard"] and guard is not None:
                self.held_s += t - p["t"]
        if guard and not (self.prev and self.prev["guard"]):
            self.engagements += 1
        self.guard_rows += bool(guard)
        if cpu is not None and (self.peak_cpu is None or cpu > self.peak_cpu[0]):
            self.peak_cpu = (cpu, row)
        if est is not None and (self.peak_est is None or est > self.peak_est[0]):
            self.peak_est = (est, row)
        if us is not None and (self.peak_us is None or us > self.peak_us[0]):
            self.peak_us = (us, row)
        if shed is not None:
            self.shed = max(self.shed, shed)
        self.prev = {"guard": guard, "us": us, "shed": shed, "t": t, "late": late}

    @staticmethod
    def where(row):
        w = f'{row["world"]} {row["scene"]}' if row["world"] not in ("", "-") else row["mode"]
        mac = "/".join(str(row[k]) for k in ("color", "motion", "space", "energy"))
        return f"{w} macros {mac}" + (f' [{row["tag"]}]' if row["tag"] else "")

    def line(self, row):
        c = row["cpu_pct"]
        return (f't={float(row["t_s"]):6.1f}s {row["mode"]:5} {row["world"][:14]:14} {row["scene"]:1} '
                f'cpu {c}% est {row["est"]} max_us {row["max_us"]} shed {row["shed"]} late {row["late"]} '
                f'guard {"HOLD" if row["guard"] == 1 else "off"} | peak cpu '
                f'{(self.peak_cpu[0] * 100 // 256) if self.peak_cpu else 0}% engagements {self.engagements}')

    def summary(self):
        out = [f"samples {self.n}   resets seen {self.resets}   voices shed {self.shed}   late halves (dropouts) {self.late}"]
        if self.peak_cpu:
            out.append(f"peak cpu_q8 {self.peak_cpu[0]} ({self.peak_cpu[0] * 100 / 256:.1f} %) at {self.where(self.peak_cpu[1])}")
        if self.peak_est:
            out.append(f"peak guard estimate {self.peak_est[0]} instr/sample at {self.where(self.peak_est[1])}")
        if self.peak_us:
            out.append(f"max half {self.peak_us[0]} us of {HALF_US} ({self.peak_us[0] * 100 / HALF_US:.0f} %; the "
                       f"device sheds a voice above 85 %) at {self.where(self.peak_us[1])}")
        out.append(f"CPU guard: {self.engagements} engagement(s), holding in {self.guard_rows} of {self.n} samples "
                   f"({self.held_s:.1f} s)")
        return "\n".join(out)


class Sink:
    """The CSV file (header first, flushed per row), the events log beside it, the terminal."""

    def __init__(self, path, quiet=False, print_every=5.0):
        self.path, self.stats, self.quiet, self.every, self.last = path, Stats(), quiet, print_every, -1e9
        self.f = open(path, "w", newline="", encoding="utf-8") if path else None
        self.w = csv.DictWriter(self.f, COLUMNS) if self.f else None
        self.log = open(path + ".log", "w", encoding="utf-8") if path else None
        if self.w:
            self.w.writeheader()
        self.tty = sys.stdout.isatty()

    def add(self, row):
        if self.w:
            self.w.writerow(row)
            self.f.flush()
        g = self.stats.prev["guard"] if self.stats.prev else 0
        self.stats.add(row)
        if self.quiet:
            return
        t = float(row["t_s"])
        if self.tty:
            print("\r\x1b[K" + self.stats.line(row), end="", flush=True)
        elif t - self.last >= self.every or (row["guard"] == 1 and not g):
            print(self.stats.line(row), flush=True)
            self.last = t

    def event(self, text):
        if self.tty:
            print("\r\x1b[K", end="")
        print(text, flush=True)
        if self.log:
            self.log.write(text + "\n")
            self.log.flush()

    def close(self):
        if self.tty:
            print()
        print(self.stats.summary())
        for f in (self.f, self.log):
            if f:
                f.close()


# ----------------------------------------------------------------------------------------------- the port ---
def find_ports():
    return sorted(glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*"))


class Console:
    """The CDC-ACM port as a raw 8N1 terminal (the device ignores the baud rate; it needs DTR, which opening sets)."""

    def __init__(self, path):
        self.path, self.fd = path, None

    def open(self):
        fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            a = termios.tcgetattr(fd)
            a[0] &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP | termios.INLCR |
                      termios.IGNCR | termios.ICRNL | termios.IXON | termios.IXOFF)
            a[1] &= ~termios.OPOST
            a[2] = (a[2] & ~(termios.CSIZE | termios.PARENB | termios.CSTOPB)) | termios.CS8 | termios.CLOCAL | termios.CREAD
            a[3] &= ~(termios.ECHO | termios.ECHONL | termios.ICANON | termios.ISIG | termios.IEXTEN)
            a[4] = a[5] = termios.B115200
            a[6][termios.VMIN], a[6][termios.VTIME] = 0, 0
            termios.tcsetattr(fd, termios.TCSANOW, a)
            try:
                import fcntl
                fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack("I", termios.TIOCM_DTR))
            except (OSError, ImportError, AttributeError):
                pass
        except termios.error:
            os.close(fd)
            raise
        self.fd = fd
        self.read(0.3)                                        # the banner the console prints when DTR rises

    def close(self):
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None

    def read(self, wait, until=None):
        """Bytes for up to `wait` s, or until the text ends with `until`. Raises OSError when the port is gone."""
        end, buf = time.monotonic() + wait, b""
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            r, _, _ = select.select([self.fd], [], [], left)
            if not r:
                break
            chunk = os.read(self.fd, 4096)
            if not chunk:
                raise OSError("port closed")
            buf += chunk
            if until and buf.endswith(until):
                break
        return buf.decode("latin-1")

    def ask(self, cmd, timeout=1.0):
        self.read(0)                                          # (stale bytes)
        os.write(self.fd, cmd.encode() + b"\r")
        return self.read(timeout, until=b"\r\n> ")


def resolve_port(port):
    """(path, None), or (None, why not)."""
    ports = [port] if port else find_ports()
    if not ports:
        return None, "no /dev/cu.usbmodem* or /dev/ttyACM* (is the FM-1 on, in normal mode, with the CDC console built in?)"
    if len(ports) > 1:
        return None, "more than one serial port, name the FM-1's with --port:\n  " + "\n  ".join(ports)
    return ports[0], None


SAFE_CMDS = ("help", "status", "dbg", "crash", "params", "memr", "flr", "flow")   # read-only; never `uboot`
FLR_LINE = re.compile(r"^([0-9A-F]{6}):((?: [0-9A-F]{2})+)\s*$")


def clean_reply(text, cmd):
    """a console reply without the echo of the command and the closing prompt"""
    lines = split_lines(text)
    lines = lines[1:] if lines and lines[0].strip().lstrip("> ") == cmd.strip() else lines
    return "\n".join(ln for ln in lines if ln.strip() not in ("", ">"))


def parse_flr(text, addr, n):
    """`flr ADDR N` reply -> n bytes, or None when a line is missing, out of order or cut short"""
    out = bytearray()
    for ln in split_lines(text):
        m = FLR_LINE.match(ln.strip()[1:].strip() if ln.strip().startswith(">") else ln.strip())
        if m:
            if int(m.group(1), 16) != addr + len(out):
                return None
            out += bytes(int(x, 16) for x in m.group(2).split())
    return bytes(out) if len(out) == n else None


def open_console(args):
    path, why = resolve_port(args.port)
    if why:
        print("fm1_monitor: " + why, file=sys.stderr)
        return None
    con = Console(path)
    try:
        con.open()
    except OSError as e:
        print(f"fm1_monitor: cannot open {path}: {e}", file=sys.stderr)
        return None
    return con


def run_cmds(args):
    for c in args.cmd:
        if c.split()[:1] and c.split()[0] not in SAFE_CMDS:
            print(f"fm1_monitor: only read-only commands: {' '.join(SAFE_CMDS)} (not {c!r})", file=sys.stderr)
            return 2
    con = open_console(args)
    if con is None:
        return 3
    try:
        for c in args.cmd:
            print("> " + c)
            print(clean_reply(con.ask(c, 3.0), c))
    except OSError as e:
        print(f"fm1_monitor: console lost: {e}", file=sys.stderr)
        return 4
    return 0


def dump_flash(args):
    import hashlib
    addr, n, out = int(args.dump_flash[0], 0), int(args.dump_flash[1], 0), args.dump_flash[2]
    if addr < 0x93000 or n <= 0 or addr + n > 0x100000:
        print("fm1_monitor: `flr` reads 0x93000..0xFFFFF", file=sys.stderr)
        return 2
    con = open_console(args)
    if con is None:
        return 3
    data = bytearray()
    try:
        while len(data) < n:
            a, k = addr + len(data), min(256, n - len(data))
            for _ in range(3):
                blk = parse_flr(con.ask(f"flr 0x{a:X} {k}", 2.0), a, k)
                if blk:
                    break
            else:
                print(f"fm1_monitor: no clean reply for flr 0x{a:X} {k}", file=sys.stderr)
                return 4
            data += blk
            print(f"\r0x{a + k:X} / 0x{addr + n:X}", end="", file=sys.stderr, flush=True)
    except OSError as e:
        print(f"\nfm1_monitor: console lost: {e}", file=sys.stderr)
        return 4
    with open(out, "wb") as f:
        f.write(data)
    print(f"\n{out}: {len(data)} bytes from 0x{addr:X}, sha256 {hashlib.sha256(data).hexdigest()}")
    return 0


def run_live(args, sink, path):
    con, t0, n, bad, tag = Console(path), time.monotonic(), 0, 0, args.tag
    try:
        con.open()
    except OSError as e:
        print(f"fm1_monitor: cannot open {path}: {e}", file=sys.stderr)
        return 3
    sink.event(f"# {time.strftime('%Y-%m-%d %H:%M:%S')} monitoring {path} every {args.interval} ms")
    interactive = sys.stdin.isatty()
    nxt, flow_ok = t0, None            # flow_ok False: the firmware answered `flow` with "? (help)"
    while args.duration is None or time.monotonic() - t0 < args.duration:
        if interactive and select.select([sys.stdin], [], [], 0)[0]:
            tag = sys.stdin.readline().strip()
            sink.event(f"# tag: {tag!r}")
        try:
            now = time.monotonic()
            text = "" if flow_ok is False else con.ask("flow", 1.0)
            flow = next((f for f in map(parse_flow, split_lines(text)) if f), None)
            if flow is None and "? (help)" in text:           # an older firmware: status only
                flow_ok = False
                sink.event("# this firmware has no `flow`: logging `status` only")
            status = {}
            if flow_ok is False or (args.status_every and n % args.status_every == 0):
                status = parse_status(split_lines(con.ask("status", 1.5)))
            if flow is None and not status:
                bad += 1
                if bad == 1 and n == 0:
                    sink.event("# no reply to `flow` (is this the FM-1 console?)")
                if bad >= 5 and n == 0:
                    return 4
                if bad >= 5:
                    raise OSError("no replies")
            else:
                bad = 0
                n += 1
                sink.add(make_row(now - t0, time.strftime("%Y-%m-%dT%H:%M:%S"), tag, flow, status))
        except OSError as e:
            sink.event(f"# {time.strftime('%H:%M:%S')} t={time.monotonic() - t0:.1f}s console lost ({e}); waiting for the port")
            con.close()
            if not reconnect(con, args, sink):
                sink.event("# gave up waiting for the port")
                return 4
            bad = 0
        nxt += args.interval / 1000.0
        time.sleep(max(0.0, nxt - time.monotonic()))
    con.close()
    return 0


def reconnect(con, args, sink):
    end = time.monotonic() + args.reconnect
    while time.monotonic() < end:
        path = args.port or next(iter(find_ports()), None)
        if path and os.path.exists(path):
            con.path = path
            try:
                con.open()
            except OSError:
                time.sleep(0.5)
                continue
            time.sleep(1.5)                                   # let the firmware finish booting
            sink.event(f"# {time.strftime('%H:%M:%S')} reconnected on {path}")
            for cmd in ("crash", "status"):
                try:
                    sink.event("# " + cmd + ":\n" + con.ask(cmd, 1.5).replace("\r", "").strip())
                except OSError:
                    pass
            return True
        time.sleep(0.5)
    return False


# ------------------------------------------------------------------------------------------------- the fit ---
def fit(path, ceiling, margin):
    """cpu_q8 against the guard's estimate over the rows of a monitor CSV (docs/hardware-calibration.md)."""
    with open(path, newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    pts, tags = [], {}
    for r in rows:
        try:
            est, q8 = int(r["est"]), int(r["cpu_q8"])
        except (KeyError, ValueError):
            continue
        t = tags.setdefault(r.get("tag", ""), {"n": 0, "cpu": 0, "est": 0, "us": 0, "guard": 0, "sum": 0})
        t["n"] += 1
        t["cpu"], t["est"], t["sum"] = max(t["cpu"], q8), max(t["est"], est), t["sum"] + q8
        t["us"] = max(t["us"], int(r["max_us"] or 0))
        t["guard"] += r.get("guard") == "1"
        if est > 0 and r.get("mode") in ("PLAY", "ADV"):
            pts.append((est, q8))
    out = [f"{len(rows)} rows, {len(pts)} with a World sounding"]
    if len(pts) < 30:
        out.append("too few rows to fit: play more (calibration doc, step 3)")
    else:
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        mx, my = statistics.fmean(xs), statistics.fmean(ys)
        sxx = sum((x - mx) ** 2 for x in xs)
        a = sum((x - mx) * (y - my) for x, y in pts) / sxx if sxx else 0.0
        b = my - a * mx
        ss_res = sum((y - (a * x + b)) ** 2 for x, y in pts)
        ss_tot = sum((y - my) ** 2 for y in ys)
        r2 = 1 - ss_res / ss_tot if ss_tot else 0.0
        out.append(f"cpu_q8 = {a:.5f} x est + {b:.1f}    R2 {r2:.3f}    est {min(xs)}..{max(xs)}, cpu_q8 {min(ys)}..{max(ys)}")
        if max(xs) - min(xs) < 600 or a <= 0:
            out.append("not enough spread in est (or no slope): the fit is not trustworthy")
        else:
            cq = ceiling / 100 * 256
            est_c = (cq - b) / a                             # the estimate at which the device reaches the ceiling
            full = round(est_c / (ceiling / 100) / 10) * 10
            budget = int(est_c * (1 - margin / 100) // 10 * 10)
            top = sorted(pts)[-max(5, len(pts) // 10):]
            ratio = [x * 256 / full / y for x, y in top if y]
            out += [f"the device reaches the {ceiling} % ceiling (cpu_q8 {cq:.0f}) at est {est_c:.0f}",
                    f"proposed  GL_CPU_FULL {full}   GL_CPU_BUDGET {budget}  ({margin:g} % under the ceiling)",
                    f"with it the guard's load / measured over the top rows: median {statistics.median(ratio):.2f}, "
                    f"worst {min(ratio):.2f}..{max(ratio):.2f}  (1.00 = exact; under 0.85 means the estimate misses "
                    f"load the guard then only sees through cpu_q8)"]
    out.append("")
    out.append(f'{"tag":34} {"rows":>5} {"mean%":>6} {"max%":>5} {"max est":>8} {"max_us":>7} {"guard rows":>10}')
    for tag, t in sorted(tags.items(), key=lambda kv: -kv[1]["cpu"]):
        out.append(f'{(tag or "(none)")[:34]:34} {t["n"]:>5} {t["sum"] / t["n"] * 100 / 256:>6.1f} '
                   f'{t["cpu"] * 100 / 256:>5.1f} {t["est"]:>8} {t["us"]:>7} {t["guard"]:>10}')
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", metavar="PATH", help="the FM-1 console (default: the one /dev/cu.usbmodem*)")
    ap.add_argument("--interval", type=int, default=250, metavar="MS", help="poll period (default 250, at least 50)")
    ap.add_argument("--out", metavar="FILE", help="CSV file (default fm1-YYYYmmdd-HHMMSS.csv; none with --dry-run)")
    ap.add_argument("--duration", type=float, metavar="S", help="stop after this many seconds (default: Ctrl-C)")
    ap.add_argument("--tag", default="", help="label for the rows until you type another")
    ap.add_argument("--status-every", type=int, default=20, metavar="N",
                    help="also send `status` every N polls (uptime, boots, tempo); 0 = never (default 20)")
    ap.add_argument("--reconnect", type=float, default=60.0, metavar="S", help="wait this long for the port to come back")
    ap.add_argument("--quiet", action="store_true", help="no live line (the summary is still printed)")
    ap.add_argument("--dry-run", metavar="TRANSCRIPT", help="replay a recorded console session ('-': stdin)")
    ap.add_argument("--cmd", action="append", metavar="COMMAND", help="send one read-only console command and print "
                    "the reply (repeatable): " + " ".join(SAFE_CMDS))
    ap.add_argument("--dump-flash", nargs=3, metavar=("ADDR", "LEN", "FILE"),
                    help="read LEN bytes of flash from ADDR (0x93000..0xFFFFF) into FILE; stop the transport first")
    ap.add_argument("--fit", metavar="CSV", help="fit GL_CPU_FULL and GL_CPU_BUDGET from a monitor CSV")
    ap.add_argument("--ceiling", type=float, default=85.0, metavar="PCT", help="the guard's ceiling for --fit (default 85)")
    ap.add_argument("--margin", type=float, default=10.0, metavar="PCT", help="BUDGET this far under the ceiling (default 10)")
    args = ap.parse_args(argv)
    if args.interval < 50 or args.status_every < 0:
        ap.error("--interval is at least 50 ms, --status-every at least 0")
    if args.fit:
        try:
            print(fit(args.fit, args.ceiling, args.margin))
        except OSError as e:
            print(f"fm1_monitor: {e}", file=sys.stderr)
            return 2
        return 0
    if args.cmd:
        return run_cmds(args)
    if args.dump_flash:
        return dump_flash(args)
    if args.dry_run:
        try:
            text = sys.stdin.read() if args.dry_run == "-" else open(args.dry_run, encoding="latin-1").read()
        except OSError as e:
            print(f"fm1_monitor: {e}", file=sys.stderr)
            return 2
        cycles = parse_transcript(text)
        if not cycles:
            print("fm1_monitor: no `flow` or `status` replies in the transcript", file=sys.stderr)
            return 2
        sink = Sink(args.out, args.quiet)
        for i, (flow, status) in enumerate(cycles):
            sink.add(make_row(i * args.interval / 1000.0, "", args.tag, flow, status))
        sink.close()
        return 0
    path, why = resolve_port(args.port)
    if why:
        print("fm1_monitor: " + why, file=sys.stderr)
        return 3
    out = args.out or time.strftime("fm1-%Y%m%d-%H%M%S.csv")
    sink = Sink(out, args.quiet)
    rc = 0
    try:
        rc = run_live(args, sink, path)
    except KeyboardInterrupt:
        pass
    sink.close()
    print(f"CSV: {out}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
