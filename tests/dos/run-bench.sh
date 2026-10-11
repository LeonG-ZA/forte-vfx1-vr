#!/bin/sh
#
# run-bench.sh -- calibrate bench.asm against native DOSBox-X.
#
#   ./tests/dos/run-bench.sh /path/to/native/dosbox-x [cycles ...]
#
# DOSBox measures emulated CPU speed in "cycles": emulated instructions per
# millisecond. With cycles fixed, the benchmark's CPU score is capped by that
# setting, so running it at several fixed values gives a score-per-cycle scale.
# Any other run of the same benchmark, including the WebAssembly build in a
# headset browser, can then be expressed as an equivalent cycle count.
#
# The interpreter core is forced (core=normal) because it is the only core a
# WebAssembly build has. Comparing against the native recompiler would flatter
# native and say nothing useful about the headset.

set -eu

DOSBOX=${1:-}
if [ -z "$DOSBOX" ] || [ ! -x "$DOSBOX" ]; then
    echo "usage: $0 /path/to/dosbox-x [cycles ...]" >&2
    exit 2
fi
shift
CYCLES=${*:-"3000 10000 20000 30000 50000 max"}

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/c"
nasm -f bin -o "$WORK/c/BENCH.COM" "$HERE/bench.asm"

field() {
    sed -n "s/.*$1=\([0-9]\{1,\}\).*/\1/p" "$2" | head -1
}

printf '%-10s %14s %12s %12s\n' cycles cpu_iter_per_s fill_fps cpu_per_cycle
for c in $CYCLES; do
    if [ "$c" = max ]; then setting="max"; else setting="fixed $c"; fi
    cat > "$WORK/b.conf" <<CONF
[cpu]
core   = normal
cycles = $setting

[autoexec]
mount c $WORK/c
c:
BENCH.COM > OUT.TXT
exit
CONF
    rm -f "$WORK/c/OUT.TXT"
    SDL_VIDEODRIVER=dummy timeout 120 "$DOSBOX" -conf "$WORK/b.conf" \
        < /dev/null > /dev/null 2>&1 || true
    tr -d '\r' < "$WORK/c/OUT.TXT" > "$WORK/out.txt" 2>/dev/null || : > "$WORK/out.txt"

    cpu=$(field CPU "$WORK/out.txt"); ct=$(field CPUTICKS "$WORK/out.txt")
    fill=$(field FILL "$WORK/out.txt"); ft=$(field FILLTICKS "$WORK/out.txt")
    if [ -z "$cpu" ] || [ -z "$ct" ] || [ "$ct" = 0 ]; then
        printf '%-10s %14s\n' "$c" "no result"
        continue
    fi
    awk -v c="$c" -v cpu="$cpu" -v ct="$ct" -v fill="$fill" -v ft="$ft" 'BEGIN {
        cps = cpu * 18.2065 / ct
        fps = fill * 18.2065 / ft
        per = (c == "max") ? "-" : sprintf("%.4f", cps / c)
        printf "%-10s %14.0f %12.1f %12s\n", c, cps, fps, per
    }'
done
