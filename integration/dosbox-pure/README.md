# DOSBox Pure integration: the VFX1 in a libretro core

The native OpenXR app (docs/ARCHITECTURE.md) runs DOS through DOSBox Pure, a
libretro core with an ARM64 dynamic recompiler. [apply.sh](apply.sh) adds the
same emulated Forte VFX1 the js-dos build has: libvfx1dev and the shared glue
in [integration/dosbox-common](../dosbox-common/vfx1_dosbox.cpp).

## Build and test on Linux or WSL

```bash
git clone https://github.com/schellingb/dosbox-pure.git ~/dosbox-pure
sh integration/dosbox-pure/apply.sh ~/dosbox-pure
make -C ~/dosbox-pure -j16
make build/vfx-retro-headless
```

Then run a DOS program through our own libretro frontend (src/retrohost), with
a scripted head sweep:

```bash
./build/vfx-retro-headless --core ~/dosbox-pure/dosbox_pure_libretro.so \
    --content build/web/SENSE.COM --seconds 30 --sweep --png build/retro-sense.png
```

## Descent

DOSBox Pure runs the autoexec lines of a DOSBOX.CONF inside the game zip when
the core option `dosbox_pure_conf` is `inside`:

```
[autoexec]
SET VIPPORT=260
CD DESCENT
dos32a DCNTSHR.EXE -vfx -vfxtrak
```

Headless on the dev PC, Descent shareware 1.4 in DOSBox Pure's dynamic core
ran 45 emulated seconds in 26 real ones, drew native page-flipped stereo that
vfxhost's extractor split into 790 complete eye pairs, and polled the VFX1
head tracker every frame.

## Notes

- DOSBox Pure restarts the machine after loading a dosbox.conf, so the glue's
  VFX1_Init() must tolerate being called twice.
- The host passes head orientation by calling the core's exported vfx1Pose().
