# chimera-core-flycast

Flycast - the Dreamcast, and the arcade boards built from it: NAOMI, NAOMI 2
and Atomiswave - as a Chimera waterbox core.

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

The arcade machines cannot be gated that way: a NAOMI has no HLE bios, and a
rom set is recognised by its name in Flycast's own table. Their legs run when
`FLYCAST_ARCADE_ROMS` names a folder holding `naomi.zip` and one game (a MAME
zip, or a decrypted `.dat`/`.bin`), and are skipped otherwise; what they said
on the machine that had the roms is in `docs/PLAN.md`.

## The arcade boards

One package, four machines: the `machine` setting picks Dreamcast, NAOMI,
NAOMI 2 or Atomiswave, and a project pins it. An arcade project is a MAME rom
set - the zip named as MAME names it (`vf4.zip`), a clone's parent zip beside
it, the `.chd` of a GD-ROM game beside its zip - or a decrypted dump of the
nullDC era (`.dat`, `.bin`, `.lst`). The bios set comes from the firmware page:
`naomi.zip`, `naomi2.zip` or `awbios.zip`, taken whole. The controller is the
JVS panel (Start, Button 1-8, Coin, Service, Test per player, plus the analog
channels, gun and rotary axes), with MAME's keyboard habits as the defaults.
The board's EEPROM and NVRAM leave through Export Save Data and go back through
the project's Board memory slot. Vertical games come out turned the way their
cabinet's monitor was mounted (the `rotation` setting). A set for another board
is refused with a message that names both.
