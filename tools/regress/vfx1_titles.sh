#!/bin/bash
# vfx1_titles.sh CORE TAG -- runs the VFX1 titles headless on one core build
# (build/windows/retro-headless.exe, game zips in build/native) and keeps the
# driver log, stats and screens in build/regress/TAG, to compare two builds.
cd "$(dirname "$0")/../.." || exit 1
core=$1; tag=$2
out=build/regress/$tag
mkdir -p "$out"
run() {
    name=$1; shift
    build/windows/retro-headless.exe --core "$core" --content "build/native/$name.zip" \
        --option dosbox_pure_conf=inside --option dosbox_pure_cpu_core=dynamic --option dosbox_pure_cycles=max \
        --png "$out/$name.png" "$@" 2>&1 | grep -E '^\{|first Sense' > "$out/$name.txt"
}
run descent-vfx1 --seconds 40 --sweep --eyes "$out/descent-eyes.png"
run d2demo-vfx1 --seconds 40 --sweep --eyes "$out/d2demo-eyes.png"
run madspace-vfx1 --seconds 40 --sweep --eyes "$out/madspace-eyes.png"
run gridfighter3d-vfx1 --seconds 30 --key 8,13,0.2 --key 14,32,0.2
run vfx1-utils --seconds 20 --key 10,13,0.2
run zar-vfx1 --seconds 30
