#!/bin/bash
# capture.sh -- game frames for tools/depthtest/eval_depth.py, from the headless
# libretro runner and the test zips in build/native (run in WSL).
cd "$(dirname "$0")/../.." || exit 1
mkdir -p "${1:-build/depthtest/frames}"
R="build/vfx-retro-headless --core $HOME/dosbox-pure/dosbox_pure_libretro.so --option dosbox_pure_conf=inside"
O=${1:-build/depthtest/frames}
D2KEYS="--key 6,27,0.2 --key 10,13,0.2 --key 14,13,0.2 --key 18,13,0.2 --key 22,27,0.2"
# Descent 2, i-glasses interlaced: head sweep gives varied views
$R --content build/native/d2demo-iglasses.zip --option dosbox_pure_cycles=max --seconds 50 --sweep $D2KEYS \
   --shot 30,$O/d2igl-30.png --shot 34,$O/d2igl-34.png --shot 38,$O/d2igl-38.png --shot 42,$O/d2igl-42.png --shot 46,$O/d2igl-46.png >/dev/null 2>&1
# Descent 2, VFX1 pages
for s in 33 39 45; do
  $R --content build/native/d2demo-vfx1.zip --option dosbox_pure_cycles=max --seconds $s --sweep $D2KEYS --eyes $O/d2vfx-$s-eyes.png >/dev/null 2>&1
done
# MadSpace, VFX1 pages
for s in 58 66; do
  $R --content build/native/madspace-vfx1.zip --option dosbox_pure_cycles=max --seconds $s --sweep --key 25,13,0.2 --key 35,13,0.2 --key 50,13,0.2 --eyes $O/ms-$s-eyes.png >/dev/null 2>&1
done
# Magic Carpet, colour, mono
$R --content build/native/carpet-novfx.zip --option dosbox_pure_cycles=max --seconds 50 --key 10,27,0.2 \
   --mouse 14,-3000,-3000,0,0.1 --mouse 15,60,200,0,0.1 --mouse 25,0,0,1,0.3 \
   --shot 32,$O/mc-32.png --shot 38,$O/mc-38.png --shot 46,$O/mc-46.png >/dev/null 2>&1
# MechWarrior 2 intro, mono
$R --content build/native/mw2-plain.zip --option dosbox_pure_cycles=max --seconds 50 \
   --shot 22,$O/mw2-22.png --shot 30,$O/mw2-30.png --shot 40,$O/mw2-40.png >/dev/null 2>&1
echo done
