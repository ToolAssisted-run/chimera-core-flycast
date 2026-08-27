# chimera-core-flycast

Flycast - Dreamcast, and later NAOMI/Atomiswave - as a Chimera waterbox core.

Upstream is `flyinghead/flycast`, pinned as a submodule at `extern/flycast`.

This core had no precedent: nothing in BizHawk runs Flycast, and unlike every
other core in the bundle its emulator has **no software renderer at all**. It
has one now - skmp's reference rasteriser from libswirl, fed from Flycast's own
display lists - so a Dreamcast draws with no GPU anywhere in the picture. What
that took, and what remains, is in [`docs/PLAN.md`](docs/PLAN.md).

```sh
# the native reference: the same sources built for the host
meson setup build/meson-native && ninja -C build/meson-native

# the guest: those sources through miniBox's C++ toolchain, into core.wbx
sh waterbox/setup-guest.sh && ninja -C build/meson-guest core.wbx

./waterbox/build-package.sh -r <chimera checkout>   # -> flycast.chimeraCore
```

Gates:

```sh
./waterbox/run-gate.sh                              # the machine, against itself
./waterbox/tests/run-frontend.sh --chimera-root ..  # the package, inside Chimera
```

Both run on programs this repository assembles (`tests/sh4asm.py`) and a GD-ROM
it builds (`tests/make-testdisc.py`), because a Dreamcast game is somebody's
copyrighted disc.
