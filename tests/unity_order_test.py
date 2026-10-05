#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""The host unity root builds the firmware as the device does (design 1.3; run by tests/run_tests.sh from the
repo root): the firmware/src files that host/core.c includes are firmware/src/felucca.c's, in the same order,
apart from the hardware-only ones the host leaves out (hal_host.h stands in for them, or their feature is off on
the host). A file added to felucca.c (a new module, say harmony.c) and not to host/core.c fails here, and so
does one included at another place.
"""
import os
import re
import sys

SRC = "firmware/src"
HARDWARE_ONLY = {        # felucca.c includes them, host/core.c must not
    "lcd.c": "the SPI panel (hal_host.h: a framebuffer)",
    "midi_uart.c": "TRS MIDI (FELUCCA_UART 0 on the host)",
    "ota.c": "the update entry (FELUCCA_OTA 0)",
    "editor.c": "the web editor over USB (FELUCCA_OTA 0)",
    "console.c": "the USB console (FELUCCA_CDC 0)",
    "recovery.c": "the USB recovery (FELUCCA_OTA 0)",
    "main.c": "the boot and the main loop (host_boot, host_ui_frame; felucca_init via fw_main.h)",
}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def firmware_includes(path):
    """the "..." includes of path that are firmware/src files, in order"""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = INCLUDE.match(line)
            if m and os.path.isfile(os.path.join(SRC, os.path.basename(m.group(1)))):
                out.append(os.path.basename(m.group(1)))
    return out


def main():
    dev = firmware_includes(os.path.join(SRC, "felucca.c"))
    host = firmware_includes("host/core.c")
    fails = 0
    if len(dev) < 20 or "world.c" not in dev:
        print("unity_order: cannot read firmware/src/felucca.c's includes (%d found)" % len(dev))
        return 1
    for name in sorted(HARDWARE_ONLY):
        if name in host:
            print("unity_order: host/core.c includes %s, which is hardware only (%s)" % (name, HARDWARE_ONLY[name]))
            fails += 1
    want = [n for n in dev if n not in HARDWARE_ONLY]
    if host != want:
        print("unity_order: host/core.c's firmware includes differ from felucca.c's")
        width = max(len(want), len(host))
        for i in range(width):
            a = want[i] if i < len(want) else "-"
            b = host[i] if i < len(host) else "-"
            print("  %2d  felucca.c %-14s host/core.c %-14s%s" % (i + 1, a, b, "" if a == b else "  <-"))
        missing = [n for n in want if n not in host]
        extra = [n for n in host if n not in want]
        if missing:
            print("  missing in host/core.c: " + ", ".join(missing))
        if extra:
            print("  not in felucca.c: " + ", ".join(extra))
        fails += 1
    if fails:
        return 1
    print("unity_order: host/core.c includes felucca.c's %d firmware files in its order (%d hardware-only left out: %s)"
          % (len(host), len(dev) - len(want), ", ".join(n for n in dev if n in HARDWARE_ONLY)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
