#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Fetch the three JieLi AC79 SDK files that the package uses.

  tools/fetch_sdk.py [--sdk DIR]      (default: $AC79_SDK, else ~/fw-AC79_AIoT_SDK)

cpu/wl82/tools/{uboot.boot,cfg_tool.bin,cfg/eq_cfg_hw.bin} of tag AC79NN_SDK_V1.2.1_2023-12-13
(Apache-2.0) from gitee, at their SDK paths under DIR, so DIR works as AC79_SDK. The hashes are
SDK_SHA256 in tools/build.py. A file already there with the right hash is kept; a download is
written (atomically) only when its hash matches. Standard library only.
"""
import argparse
import hashlib
import os
import shlex
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from build import SDK_SHA256  # noqa: E402

TAG = "AC79NN_SDK_V1.2.1_2023-12-13"
REPO = "https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK"
SUB = "cpu/wl82/tools"


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def get(url, tries=3):
    for i in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                return r.read()
        except OSError as e:        # URLError, HTTPError, timeouts
            err = e
            if i + 1 < tries:
                time.sleep(2 * (i + 1))
    raise err


def put(path, data):
    """write via a temporary file in the same directory, then rename"""
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
        os.chmod(tmp, 0o644)
        os.replace(tmp, path)
    except BaseException:
        os.unlink(tmp)
        raise


def main():
    ap = argparse.ArgumentParser(description="Fetch the AC79 SDK files the package uses (see BUILDING.md).")
    ap.add_argument("--sdk", type=Path, default=Path(os.environ.get("AC79_SDK") or "~/fw-AC79_AIoT_SDK"),
                    help="SDK directory (default: $AC79_SDK, else ~/fw-AC79_AIoT_SDK)")
    root = ap.parse_args().sdk.expanduser()
    bad = 0
    for rel, sha in SDK_SHA256.items():
        path = root / SUB / rel
        if path.exists():
            have = sha256(path.read_bytes())
            if have == sha:
                print(f"  ok    {SUB}/{rel}")
            else:
                print(f"  FAIL  {path}: SHA-256 {have} is not {TAG}'s; the build only warns, but the package "
                      f"will not match the reference. Remove the file or use another --sdk / AC79_SDK.")
                bad += 1
            continue
        url = f"{REPO}/raw/{TAG}/{SUB}/{rel}"
        try:
            data = get(url)
        except OSError as e:
            print(f"  FAIL  {url}: {e}")
            bad += 1
            continue
        if sha256(data) != sha:
            print(f"  FAIL  {url}: SHA-256 {sha256(data)}, expected {sha}; not written")
            bad += 1
            continue
        put(path, data)
        print(f"  got   {SUB}/{rel} ({len(data)} B)")
    if bad:
        q = shlex.quote(str(root))
        print(f"fetch_sdk: {bad} file(s) missing or different in {root}.\n"
              f"Alternative: a shallow, sparse clone of the tag (into a new or empty directory):\n"
              f"  git clone --depth 1 --filter=blob:none --sparse --branch {TAG} {REPO}.git {q}\n"
              f"  git -C {q} sparse-checkout set {SUB}")
        return 1
    print(f"AC79_SDK={root}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
