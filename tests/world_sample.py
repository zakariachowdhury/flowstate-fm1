#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The factory Worlds that the per-World groups of tests/run_tests.sh play (Phase 18: a library of 30 is too many for
every group on every run):

  tests/world_sample.py [--line]    their ids (the file names: neon_rain), one per line (--line: on one line)
  tests/world_sample.py --why       one line for the log: which Worlds, and why

  WORLDS unset or "sample"   the four demo Worlds (NEON RAIN, MIDNIGHT DRIVE, FROZEN LAKE, DUSTY CAFE) and
                             WORLDS_EXTRA more (default 2), drawn with a seed from the hash of the factory set (the
                             file names and their bytes): the same Worlds while the library stays the same, others
                             after any change to it, so successive commits rotate through the whole library
  WORLDS=all                 every World in worlds/factory (the four first)
  WORLDS="id id ..."         the demo four (some checks name them) and those (ids or file names; an unknown one is
                             an error)

Every World of the library is still compiled, checked by `worldc check`, budgeted and built into the firmware on
every run (gen_worlds, validate-world's flash budget); the groups that read the firmware's built-in Worlds
(smartkeys_test, macro_test, guard_test) take the same choice through FACTORY_WORLDS (tests/world_pick.h)."""
import hashlib
import os
import random
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FACTORY = ROOT / "worlds" / "factory"
DEMO = ["neon_rain", "midnight_drive", "frozen_lake", "dusty_cafe"]   # the UI spec's four, in the tests' order


def library():
    ids = sorted(p.name[:-len(".world.json")] for p in FACTORY.glob("*.world.json"))
    return [w for w in DEMO if w in ids] + [w for w in ids if w not in DEMO]


def library_hash(ids):
    h = hashlib.sha256()
    for w in sorted(ids):
        h.update(w.encode() + b"\0" + (FACTORY / f"{w}.world.json").read_bytes() + b"\0")
    return h.hexdigest()


def choose(spec=None, extra=None):
    """(the ids, a line saying why)"""
    ids = library()
    spec = (os.environ.get("WORLDS", "") if spec is None else spec).strip() or "sample"
    if spec == "all":
        return ids, f"all {len(ids)} factory Worlds (WORLDS=all)"
    if spec == "sample":
        n = int(os.environ.get("WORLDS_EXTRA", "2") if extra is None else extra)
        h = library_hash(ids)
        rest = [w for w in ids if w not in DEMO]
        pick = sorted(random.Random(int(h[:16], 16)).sample(rest, min(n, len(rest))))
        out = [w for w in DEMO if w in ids] + pick
        return out, (f"{len(out)} of {len(ids)} factory Worlds: the demo four and {', '.join(pick) or 'no other'} "
                     f"(drawn from the library's hash {h[:8]}; WORLDS=all for every World)")
    out = [w for w in DEMO if w in ids]                # (the demo four always: some checks name them)
    for w in spec.replace(",", " ").split():
        w = Path(w).name
        w = w[:-len(".world.json")] if w.endswith(".world.json") else w
        if w not in ids:
            raise SystemExit(f"world_sample: WORLDS: {w!r} is not in worlds/factory")
        if w not in out:
            out.append(w)
    return out, f"{len(out)} of {len(ids)} factory Worlds: the demo four and WORLDS={spec}"


def main(argv):
    ids, why = choose()
    if "--why" in argv:
        print(why)
    elif "--line" in argv:
        print(" ".join(ids))
    else:
        print("\n".join(ids))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
