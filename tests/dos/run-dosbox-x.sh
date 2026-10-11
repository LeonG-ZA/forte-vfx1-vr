#!/bin/sh
#
# run-dosbox-x.sh -- end-to-end check of the INT 33h head-tracking path.
#
#   ./tests/dos/run-dosbox-x.sh /path/to/dosbox-x/src/dosbox-x
#
# Assembles the probe, boots DOSBox-X with head tracking on and the scripted
# pose source, runs the probe inside DOS, and reports what the DOS side saw.
#
# It runs the emulator twice: once with tracking on and once off. Motion in the
# first run and none in the second is the result that means something. A single
# run proves less, because a stray host mouse event would look the same.
#
# Needs nasm and a built DOSBox-X with integration/dosbox-x/apply.sh applied.

set -eu

DOSBOX=${1:-}
if [ -z "$DOSBOX" ] || [ ! -x "$DOSBOX" ]; then
    echo "usage: $0 /path/to/dosbox-x/src/dosbox-x" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/c"
nasm -f bin -o "$WORK/c/INT33.COM" "$HERE/int33probe.asm"

run_once() {
    tracking=$1
    out=$2
    cat > "$WORK/vfx.conf" <<CONF
[sdl]
autolock = false

[log]
logfile = $WORK/dosbox-$tracking.log

[vfx1]
headtracking = $tracking
posesource   = script

[autoexec]
mount c $WORK/c
c:
INT33.COM > OUT.TXT
exit
CONF
    # DOS console output goes to the emulated screen, not the host stdout, so
    # the probe redirects into a file on the mounted drive and we read that.
    rm -f "$WORK/c/OUT.TXT"
    SDL_VIDEODRIVER=dummy timeout 90 "$DOSBOX" -conf "$WORK/vfx.conf" \
        < /dev/null > "$WORK/host-$tracking.txt" 2>&1 || true
    if [ -f "$WORK/c/OUT.TXT" ]; then
        tr -d '\r' < "$WORK/c/OUT.TXT" > "$out"
    else
        : > "$out"
    fi
}

echo "=== run 1: headtracking = true ==="
run_once true "$WORK/on.txt"
cat "$WORK/on.txt"
grep -h "VFX1:" "$WORK/dosbox-true.log" 2>/dev/null || true

echo
echo "=== run 2: headtracking = false (control) ==="
run_once false "$WORK/off.txt"
cat "$WORK/off.txt"

field() {   # field NAME FILE -- pull "NAME=<int>" out of the probe output
    sed -n "s/.*$1=\(-\{0,1\}[0-9]\{1,\}\).*/\1/p" "$2" | head -1
}

on_absx=$(field ABSX "$WORK/on.txt")
on_absy=$(field ABSY "$WORK/on.txt")
off_absx=$(field ABSX "$WORK/off.txt")
off_absy=$(field ABSY "$WORK/off.txt")

echo "tracking on : travel X=${on_absx:-?} Y=${on_absy:-?}"
echo "tracking off: travel X=${off_absx:-?} Y=${off_absy:-?}"

if [ -z "$on_absx" ] || [ -z "$off_absx" ]; then
    echo "RESULT: inconclusive, the probe did not report" >&2
    exit 1
fi
if [ "$off_absx" -ne 0 ] || [ "$off_absy" -ne 0 ]; then
    echo "RESULT: FAIL, motion appeared with tracking off, so something else moved it" >&2
    exit 1
fi

# Both axes, measured as distance travelled rather than net displacement. The
# scripted sweep is a sine on each axis, so a net sum can land near zero however
# far the head actually went.
if [ "$on_absx" -lt 100 ]; then
    echo "RESULT: FAIL, yaw travelled only $on_absx mickeys" >&2
    exit 1
fi
if [ "$on_absy" -lt 100 ]; then
    echo "RESULT: FAIL, pitch travelled only $on_absy mickeys" >&2
    exit 1
fi
echo "RESULT: PASS, both axes reach DOS through INT 33h and nothing else moves them"
