#!/bin/sh
#
# run-webxr.sh -- end-to-end check of the WebXR pose path.
#
#   ./tests/dos/run-webxr.sh /path/to/dosbox-x/src/dosbox-x [port]
#
# Boots DOSBox-X with posesource=webxr, connects a stand-in client that speaks
# what the browser page speaks, and runs the INT 33h probe inside DOS. Then
# does it again with nothing connecting.
#
# Motion in the first run and none in the second is the result that means
# something: it shows the poses arrived over the socket rather than from
# anywhere else. It also checks that the emulator serves the page over HTTP.
#
# What it cannot check is WebXR itself. Only a real headset does that.
#
# Needs nasm and python3, and a DOSBox-X built with apply.sh applied.

set -eu

DOSBOX=${1:-}
PORT=${2:-8099}
if [ -z "$DOSBOX" ] || [ ! -x "$DOSBOX" ]; then
    echo "usage: $0 /path/to/dosbox-x/src/dosbox-x [port]" >&2
    exit 2
fi

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
PAGE="$ROOT/integration/dosbox-x/overlay/share/vfx1-webxr.html"

WORK=$(mktemp -d)
cleanup() {
    [ -n "${DOSPID:-}" ] && kill "$DOSPID" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

mkdir -p "$WORK/c"
nasm -f bin -o "$WORK/c/INT33.COM" "$HERE/int33probe.asm"

write_conf() {
    cat > "$WORK/vfx.conf" <<CONF
[sdl]
autolock = false

[log]
logfile = $WORK/dosbox-$1.log

[vfx1]
headtracking = true
posesource   = webxr
webxr port   = $PORT
webxr page   = $PAGE

[autoexec]
mount c $WORK/c
c:
INT33.COM > OUT.TXT
exit
CONF
}

run_once() {
    label=$1
    with_client=$2
    out=$3

    write_conf "$label"
    rm -f "$WORK/c/OUT.TXT"

    SDL_VIDEODRIVER=dummy timeout 120 "$DOSBOX" -conf "$WORK/vfx.conf" \
        < /dev/null > "$WORK/host-$label.txt" 2>&1 &
    DOSPID=$!

    if [ "$with_client" = yes ]; then
        python3 "$HERE/webxr_client.py" --port "$PORT" --seconds 14 --hz 90 \
            > "$WORK/client-$label.txt" 2>&1 || true
    fi

    wait "$DOSPID" 2>/dev/null || true
    DOSPID=

    if [ -f "$WORK/c/OUT.TXT" ]; then
        tr -d '\r' < "$WORK/c/OUT.TXT" > "$out"
    else
        : > "$out"
    fi
}

field() {
    sed -n "s/.*$1=\(-\{0,1\}[0-9]\{1,\}\).*/\1/p" "$2" | head -1
}

echo "=== run 1: a client streaming poses ==="
run_once with yes "$WORK/on.txt"
cat "$WORK/on.txt"
cat "$WORK/client-with.txt" 2>/dev/null || true
grep -h "VFX1:" "$WORK/dosbox-with.log" 2>/dev/null || true

echo
echo "=== run 2: nothing connects (control) ==="
run_once without no "$WORK/off.txt"
cat "$WORK/off.txt"

echo
echo "=== the emulator serves the page over HTTP ==="
# An idle emulator: the same config with the autoexec exit removed, so it stays
# up long enough to answer a GET.
write_conf http
sed 's/^exit$/rem stay up/' "$WORK/vfx.conf" > "$WORK/vfx-idle.conf"

SDL_VIDEODRIVER=dummy timeout 40 "$DOSBOX" -conf "$WORK/vfx-idle.conf" \
    < /dev/null > "$WORK/host-http.txt" 2>&1 &
DOSPID=$!

PAGE_BYTES=0
i=0
while [ "$i" -lt 60 ]; do
    if PAGE_BYTES=$(python3 "$HERE/http_get.py" "$PORT" 2>/dev/null); then
        break
    fi
    i=$((i + 1))
    sleep 0.5
done

kill "$DOSPID" 2>/dev/null || true
wait "$DOSPID" 2>/dev/null || true
DOSPID=
echo "page served: ${PAGE_BYTES:-0} bytes"

echo
on_x=$(field ABSX "$WORK/on.txt")
on_y=$(field ABSY "$WORK/on.txt")
off_x=$(field ABSX "$WORK/off.txt")
off_y=$(field ABSY "$WORK/off.txt")
echo "client streaming : travel X=${on_x:-?} Y=${on_y:-?}"
echo "nothing connected: travel X=${off_x:-?} Y=${off_y:-?}"

if [ -z "${on_x:-}" ] || [ -z "${off_x:-}" ]; then
    echo "RESULT: inconclusive, the probe did not report" >&2
    exit 1
fi
if [ "$off_x" -ne 0 ] || [ "$off_y" -ne 0 ]; then
    echo "RESULT: FAIL, motion with nothing connected" >&2
    exit 1
fi
if [ "$on_x" -lt 100 ] || [ "$on_y" -lt 100 ]; then
    echo "RESULT: FAIL, too little motion from the streamed poses" >&2
    exit 1
fi
if [ "${PAGE_BYTES:-0}" -lt 1000 ]; then
    echo "RESULT: FAIL, the emulator did not serve the WebXR page" >&2
    exit 1
fi
echo "RESULT: PASS, poses arriving over the WebSocket reach DOS through INT 33h"
