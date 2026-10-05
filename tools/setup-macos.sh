#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Check and set up a Mac for building and testing SLOOP (docs/macos-development.md).
# Safe to run again: it fetches only what is missing or different from the pins.
#   tools/setup-macos.sh [--install-python-deps] [--no-docker]
#   --install-python-deps  pip install -r tools/requirements-dev.txt when a package is missing
#   --no-docker            host-only development: skip Rosetta, Docker, the toolchain and the SDK
#   JIELI_TOOLCHAIN        use this toolchain (only checked); else tools/get_toolchain.sh
#                          installs the pinned one in $JIELI_HOME (default ~/.jieli)
#   AC79_SDK               SDK directory for tools/fetch_sdk.py (default ~/fw-AC79_AIoT_SDK)
#   PYTHON                 the Python to use (default python3), as for build.sh
set -e
cd "$(dirname "$0")/.."
PYDEPS=0
DOCKER=1
for a; do
    case "$a" in
        --install-python-deps) PYDEPS=1 ;;
        --no-docker) DOCKER=0 ;;
        -h|--help) sed -n '3,11s/^# \{0,1\}//p' "$0"; exit 0 ;;
        *) echo "setup-macos.sh: unknown option $a (see --help)"; exit 2 ;;
    esac
done
fails=0
ok()   { echo "  ok    $*"; }
note() { echo "  note  $*"; }
warn() { echo "  warn  $1"; [ -z "$2" ] || echo "        $2"; }
bad()  { echo "  FAIL  $1"; [ -z "$2" ] || echo "        fix: $2"; fails=$((fails + 1)); }
stop() { bad "$@"; echo "setup-macos.sh: stopped; fix this and run it again"; exit 1; }

# ---- platform
[ "$(uname -s)" = Darwin ] || { echo "setup-macos.sh: this is for macOS; on Linux x86-64 see BUILDING.md"; exit 1; }
ARCH="$(uname -m)"
if [ "$(sysctl -n sysctl.proc_translated 2>/dev/null)" = 1 ]; then
    warn "this shell runs under Rosetta" "a native (arm64) terminal is preferred"
    ARCH=arm64
fi
case "$ARCH" in
    arm64) ok "macOS $(sw_vers -productVersion), Apple silicon" ;;
    x86_64) ok "macOS $(sw_vers -productVersion), Intel" ;;
    *) stop "unknown architecture $ARCH" ;;
esac
if CLT="$(xcode-select -p 2>/dev/null)"; then
    cc --version >/dev/null 2>&1 ||
        stop "cc does not run (Xcode licence not accepted?)" "run cc once and follow its message"
    ok "command-line tools ($CLT)"
else
    stop "no Xcode command-line tools (cc, git, python3)" "xcode-select --install"
fi
if [ $DOCKER = 0 ]; then
    note "--no-docker: Rosetta, Docker, toolchain and SDK skipped"
elif [ "$ARCH" = arm64 ]; then
    arch -x86_64 /usr/bin/true 2>/dev/null ||
        stop "no Rosetta (Docker runs the x86-64 toolchain with it)" \
             "softwareupdate --install-rosetta   (it asks you to accept Apple's licence)"
    ok "Rosetta"
fi

# ---- Python
PY="${PYTHON:-python3}"
command -v "$PY" >/dev/null 2>&1 ||
    stop "no $PY" "install Python 3.9 or newer (python.org, Homebrew or conda), or set PYTHON"
"$PY" -c 'import sys; sys.exit(sys.version_info < (3, 9))' ||
    stop "$PY is Python $("$PY" -V 2>&1 | cut -d' ' -f2); 3.9 or newer is needed" \
         "install a newer Python, or set PYTHON"
pymissing() {   # packages of tools/requirements-dev.txt that are not installed
    "$PY" -c 'import importlib.util as u, sys
m = [("Pillow", "PIL"), ("mido", "mido"), ("python-rtmidi", "rtmidi")]
m += [("numpy", "numpy")] if sys.version_info >= (3, 12) else []
print(" ".join(n for n, mod in m if u.find_spec(mod) is None))'
}
PIP="$PY -m pip install -r tools/requirements-dev.txt"
MISS=" $(pymissing) "
if [ "$MISS" != "  " ] && [ $PYDEPS = 1 ]; then
    echo "        $PIP"
    "$PY" -m pip install -r tools/requirements-dev.txt ||
        bad "pip install failed" \
            "see above; with Homebrew's Python use a virtual environment (docs/macos-development.md)"
    MISS=" $(pymissing) "
fi
PYV="$("$PY" -c 'import sys; print(sys.version.split()[0])')"
PIL=0
case "$MISS" in
    *" Pillow "*) bad "Python $PYV ($PY) has no Pillow" "$PIP   (or run this with --install-python-deps)" ;;
    *) ok "Python $PYV ($PY), Pillow $("$PY" -c 'import PIL; print(PIL.__version__)')"; PIL=1 ;;
esac
case "$MISS" in *" numpy "*)
    warn "no numpy: on Python 3.12+ the sample pitch detection is slower without it" "$PIP" ;;
esac
case "$MISS" in *" mido "*|*" python-rtmidi "*)
    note "no mido / python-rtmidi (optional: tools/fm1_install.py talks to the FM-1 with them)" ;;
esac

# ---- Node
if command -v node >/dev/null 2>&1; then
    ok "Node $(node --version) (web tests)"
else
    warn "no Node.js: tests/run_tests.sh skips the web tests (optional)" \
         "install Node 18 or newer (nodejs.org or Homebrew)"
fi

# ---- Docker, toolchain, SDK
if [ $DOCKER = 1 ]; then
    IMG="$("$PY" -B -c 'import sys; sys.path.insert(0, "tools"); import build; print(build.DOCKER_IMAGE)' \
           2>/dev/null)" || IMG=
    [ -n "$IMG" ] ||    # the pin is DOCKER_IMAGE in tools/build.py; this is a fallback
        IMG="debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251"
    DOCKER_OK=0
    if ! command -v docker >/dev/null 2>&1; then
        bad "no Docker" "install Docker Desktop (docker.com), or run this with --no-docker for host-only work"
    else
        if ! docker info >/dev/null 2>&1 && [ -d /Applications/Docker.app ]; then
            echo "        starting Docker Desktop ..."
            open -a Docker || true
            i=0
            while [ $i -lt 60 ] && ! docker info >/dev/null 2>&1; do sleep 2; i=$((i + 1)); done
        fi
        if docker info >/dev/null 2>&1; then
            ok "Docker $(docker version --format '{{.Server.Version}}' 2>/dev/null) running"
            DS="$HOME/Library/Group Containers/group.com.docker/settings-store.json"
            [ -f "$DS" ] || DS="${DS%-store.json}.json"     # older Docker Desktop
            if [ "$ARCH" = arm64 ] && grep -qi '"usevirtualizationframeworkrosetta": *false' "$DS" 2>/dev/null; then
                warn "Docker Desktop emulates amd64 with QEMU (slower)" \
                     "Docker Desktop > Settings > General: Use Rosetta for x86_64/amd64 emulation on Apple Silicon"
            fi
            if [ "$(docker image inspect --format '{{.Architecture}}' "$IMG" 2>/dev/null)" = amd64 ]; then
                ok "image $IMG"
                DOCKER_OK=1
            else
                echo "        docker pull --platform linux/amd64 $IMG"
                if docker pull -q --platform linux/amd64 "$IMG" >/dev/null; then
                    ok "image $IMG (pulled)"
                    DOCKER_OK=1
                else
                    bad "docker pull failed" "check the network (Docker Hub) and run this again"
                fi
            fi
        else
            bad "Docker is not running" "start Docker Desktop and run this again"
        fi
    fi

    TC=
    if [ -n "$JIELI_TOOLCHAIN" ]; then
        if [ -x "$JIELI_TOOLCHAIN/pi32v2/bin/clang" ]; then
            ok "toolchain $JIELI_TOOLCHAIN (JIELI_TOOLCHAIN; not checked against the pin)"
            TC="$JIELI_TOOLCHAIN"
        else
            bad "JIELI_TOOLCHAIN=$JIELI_TOOLCHAIN has no pi32v2/bin/clang" "unset it to install the pinned toolchain"
        fi
    elif OUT="$(sh tools/get_toolchain.sh)"; then
        ok "toolchain ${OUT#JIELI_TOOLCHAIN=}"
        TC="${JIELI_HOME:-$HOME/.jieli}/toolchain"
    else
        bad "toolchain not installed (see above)" "docs/macos-development.md, troubleshooting"
    fi
    if [ -n "$TC" ] && [ $DOCKER_OK = 1 ]; then
        TCR="$(cd "$TC" && pwd -P)"
        if V="$(docker run --rm --platform linux/amd64 -v "$PWD:/work:ro" -v "$TCR:/opt/jieli:ro" "$IMG" \
                sh -c 'test -f /work/build.sh || { echo "the source tree is not visible in the container"; exit 1; }
                       /opt/jieli/pi32v2/bin/clang --version' 2>&1)"; then
            ok "$(echo "$V" | head -1) runs in linux/amd64"
        else
            bad "the toolchain does not run in Docker: $(echo "$V" | head -2)" \
                "Docker Desktop > Settings > Resources > File sharing must include $PWD and $TCR"
        fi
    fi

    if OUT="$("$PY" -B tools/fetch_sdk.py 2>&1)"; then
        echo "$OUT" | grep '^  got ' || true
        ok "SDK files in ${OUT##*AC79_SDK=}"
    else
        echo "$OUT"
        bad "SDK files missing or different (see above)"
    fi
fi

# ---- generators
if ! "$PY" tools/build.py --help 2>/dev/null | grep -q -- --gen-only; then
    note "generators not checked: tools/build.py has no --gen-only"
elif [ $PIL = 0 ]; then
    note "generators not checked: no Pillow"
elif OUT="$("$PY" tools/build.py --gen-only 2>&1)"; then
    ok "generators: build/gen ($(ls build/gen | wc -l | tr -d ' ') files)"
else
    echo "$OUT" | tail -5
    bad "tools/build.py --gen-only failed (see above)"
fi

echo
if [ $fails -gt 0 ]; then
    echo "setup-macos.sh: $fails problem(s); fix them and run it again"
    exit 1
fi
echo "Ready. Next:"
[ -z "$TC" ] || [ "$TC" = "$HOME/.jieli/toolchain" ] || echo "  export JIELI_TOOLCHAIN=$TC"
[ -z "$AC79_SDK" ] || [ "$AC79_SDK" = "$HOME/fw-AC79_AIoT_SDK" ] || echo "  export AC79_SDK=$AC79_SDK"
[ -z "$PYTHON" ] || echo "  export PYTHON=$PYTHON"
if [ $DOCKER = 1 ]; then
    echo "  ./build.sh                         firmware and package in build/ (Docker)"
    echo "  ./tests/run_tests.sh               all host tests (after ./build.sh)"
fi
echo "  ./tests/run_tests.sh --host-only   the host tests that do not need the target build (no Docker)"
