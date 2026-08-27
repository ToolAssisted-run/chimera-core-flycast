# Flycast as a Chimera core: plan and log

## What this is

A Dreamcast (and later NAOMI/Atomiswave) core for Chimera, built from upstream
`flyinghead/flycast` in miniBox's sandbox.

It is the first core here with **no precedent to copy**. Every core so far was
either a port of something BizHawk already ran, or something the author had
already built by hand; both meant the hard questions - what the machine is,
which sources it needs, how it renders headless, what makes it deterministic -
were answered before the porting started. None of them are answered here.

## The five questions, answered before writing code

**1. How does it draw a frame without a GPU?** This is the crux, and the answer
is unlike every other core in the bundle.

Flycast renders through OpenGL, Vulkan or DirectX. There is no software
rasteriser: the old `softrend` was deleted upstream in Feb 2020
(`99f04ec75 nuke softrend`), and the copy the libretro fork still carries is,
by its own header comment, "a rather weird very basic pvr softrend... does
depth and color, but no alpha, texture, or pixel processing". It is not wired
into any build, needs OpenMP and SSE intrinsics, and would not draw a playable
picture if it were. A sandbox has no GPU, and a software OpenGL that can run
Flycast's shaders means llvmpipe, which a waterbox guest cannot host.

So a PVR2 rasteriser has to come from somewhere. It exists:
**skmp's `refsw`**, the *reference* software renderer written for libswirl -
tiles, depth, textures, alpha, the pixel pipeline - about 80KB of C++ across
nine files, still maintained in `skmp/minicast`, and **BSD-3-Clause**
("Copyright 2003-2020 (c) the Reicast Team"), which is redistributable and
compatible with Flycast's GPL-2.0. Flycast IS a reicast descendant, so refsw
was written against ancestors of the same TA structures, but they have diverged
for five years and the port is real work, not a copy.

That makes the renderer a milestone of its own, and it is deliberately NOT
first: everything else about the machine can be brought up and gated against a
`norend` build before a single pixel is drawn.

**2. Can it build headless at all?** Almost. Upstream's own CMake has
`-DLIBRETRO=ON` with `USE_OPENGL=OFF USE_VULKAN=OFF USE_DX9=OFF USE_DX11=OFF
USE_OPENMP=OFF USE_ALSA=OFF`, which configures cleanly and describes 478
translation units - a smaller surface than PPSSPP's 675. Networking (picotcp,
miniupnpc), libzip and tinygettext are in that set and are candidates for the
curated list to drop.

Building it that way was tried, and it fails in exactly two ways, both
informative:

- three files (`core/network/dcnet.cpp`, `picoppp.cpp`,
  `core/input/dreampotato.cpp`) want `asio.hpp`. They are networking and a
  network gamepad; the curated list drops them.
- `core/hw/pvr/Renderer_if.cpp:447` does not compile: with every renderer
  disabled its factory `switch` has an EMPTY body. Upstream has no supported
  rendererless configuration, which is worth knowing before depending on one -
  so patch 0001 is the one that makes a software renderer a legal choice, and
  M1 uses it to select a stub.

**3. Does it need a real address space?** No. `addrspace::virtmemEnabled()` is
`ram_base != nullptr`, and Flycast falls back to software address translation
when the host cannot reserve the SH4's address space. That is the same escape
PPSSPP needed (`MASKED_PSP_MEMORY`, `NO_MMAP`) and the sandbox requires it.

**4. Threads?** The emulation loop itself is single-threaded
(`Emulator::runInternal` drives `getSh4Executor()->Run()`), and everything
threaded is optional: threaded rendering (a libretro option), the network
stack, the game scanner, audio. All are excluded or disabled.

**5. Does it need a BIOS?** Not to start: `core/reios` is an HLE BIOS. A real
`dc_boot.bin`/`dc_flash.bin` is better for accuracy and travels through
Chimera's firmware channel once the machine runs.

## What is uncertain, and will decide the schedule

- **Determinism.** Untested. A Dreamcast has a battery-backed clock, the AICA
  ARM runs alongside the SH4, and the GD-ROM has timing the game can observe.
  Stella taught the lesson that matters here: the native reference is where
  clock- and thread-dependence shows up, so the equivalence gate comes early,
  not late.
- **The SH4 dynarec.** Not needed at first - Flycast has an interpreter, which
  is what the gate should compare - but PPSSPP proved a JIT CAN live in the
  sandbox, so `rec-x64` is a later question rather than a closed one.
- **Savestates.** Flycast has `serialize.cpp`; whether its state is complete
  enough for per-frame round-tripping is what the gate will say.
- **refsw's fidelity.** It is a reference renderer: correct-ish and slow. How
  slow, and how much of the PVR2 it actually covers, is unknown until it runs.

## Milestones

- **M1 - the machine runs headless.** Curated source list (a `sources.sh` in
  the PPSSPP style), meson for guest and native reference, `cinterface.cpp`
  against the Chimera guest ABI, `norend`, HLE BIOS, no video. Proof: the SH4
  executes a known program to a fixed frame count identically native and
  waterboxed.
- **M2 - input, memory domains, savestates.** The gate grows the legs every
  other core has: input visibly shapes the machine, per-frame savestate
  round-trips are lossless, memory domains are exposed.
- **M3 - the renderer.** Port refsw onto Flycast's TA context. Video digests
  join the gate.
- **M4 - discs and firmware.** GD-ROM images (GDI/CDI/CHD) through the file
  slots, real BIOS through the firmware channel, VMU save data through the
  save-data channel.
- **M5 - the frontend leg.** The package inside Chimera: settings, keybinds,
  the file wizard.
- **M6 - beyond Dreamcast.** NAOMI and Atomiswave as additional machines in one
  package, the way gpgx serves four systems.

## Log

- **2026-08-27** Feasibility settled (this document). Repo created, upstream
  pinned at `c3763d8`. Headless CMake configure proven; the headless BUILD gets
  as far as the two findings above, which is the M1 starting line.
