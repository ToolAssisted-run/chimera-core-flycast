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

- **M1 DONE** (2026-08-27): the machine runs headless, in the sandbox, and is
  identical to the native reference. `waterbox/run-gate.sh`: 7/7 - equivalence
  over 60 and 300 frames, proof the SH4 actually executed (a half-length run
  must differ), lossless per-frame savestate round-trips, and two native runs
  agreeing with each other.
  - 115 curated sources + 60 vendored, from `waterbox/sources.sh`.
  - It runs THIS repository's own program: `tests/make-testprog.py` hand-
    assembles eight SH4 instructions into an ELF, which the HLE bios boots at
    0x8C010000 with no bios, no disc and no SH4 toolchain. In 300 frames the
    machine executes about 564 million instructions.
- **M2 DONE** (2026-08-27): input reaches the machine, and three memory
  domains are exposed. `waterbox/run-gate.sh`: 13/13.
  - `tests/sh4asm.py` is a small SH4 assembler, because the second test program
    does something a list of hex opcodes cannot document: `padread.elf` builds
    a maple command frame, points the DMA at it, starts it, spins for the
    answer, and sums the controller's condition word into RAM - a real maple
    transaction, roughly 200 of them per frame.
  - That is what makes the input leg a proof rather than an assumption: with a
    different input schedule the machine MUST end up in a different state, and
    it does, identically native and sandboxed.
  - Lag detection through patches/0002: the pad reader reports 0 lag frames,
    the counter (which never asks) reports every frame.
  - Domains: System RAM (16MB), VRAM (16MB), Sound RAM (8MB).
- **M3 DONE** (2026-08-27): the Dreamcast draws. `waterbox/run-gate.sh`: 18/18,
  including a rendered triangle that is checked by SHAPE (a red span of
  100..499 at its base, narrowing below) rather than only by hash - because a
  renderer that draws nothing passes an equivalence test perfectly.
  - `waterbox/refsw/` is skmp's reference rasteriser, vendored BSD-3 and
    otherwise unmodified: the ISP's depth and stencil, the TSP's texturing and
    shading, the tile buffers.
  - What had to change is WHERE TRIANGLES COME FROM. refsw reads the CORE
    structures out of video memory, because that is what the hardware reads and
    libswirl fed it with a low-level TA. Flycast's TA is high level: it parses
    the TA FIFO into its own vertex lists and never writes CORE parameter
    blocks to VRAM. So refsw's VRAM walker is unused, `waterbox/refsw-renderer.cpp`
    walks Flycast's parsed lists instead, and ONE hook in refsw
    (`chimera_fpu_entry`, weakly declared) answers its "decode this tag from
    memory" with the triangle the host already has.
  - The two projects agree on the structures that matter - refsw's Vertex and
    Flycast's are the same fields in the same order, both descended from
    reicast - so a triangle crosses the seam without conversion. Their headers
    still cannot meet, since each defines all of them; `waterbox/refsw/bridge.h`
    is that seam, with the layouts static_asserted on both sides.
  - `tests/roms/triangle.elf` submits a polygon the way a game does: parameters
    staged in a store queue and flushed to the TA's FIFO with `pref`, then
    STARTRENDER. It also sets up the BACKGROUND PLANE (parameters in video
    memory, which is where the PVR reads them from) and the video clock, so the
    gate checks a 640x480 picture of a red triangle of exactly 60000 pixels on
    blue - every number one the program asked for.
- **M4 DONE** (2026-08-27): discs, memory cards and firmware.
  `waterbox/run-gate.sh`: 21/21.
  - `tests/make-testdisc.py` builds a GD-ROM from scratch - three tracks, an
    ISO9660 filesystem at LBA 45000, an IP.BIN bootstrap naming 1ST_READ.BIN -
    so the gate boots the way a game does (disc reader, drive, HLE bios
    locating the bootfile) without anyone's copyrighted disc.
  - The VMU's 128KB of flash leaves through the save-data channel, formatted:
    two cards, in the two slots of the controller in port A.
  - Firmware: the core looks for `dc_boot.bin` where the frontend mounts it and
    falls back to the HLE bios when a project has none. UNTESTED in the
    positive direction - a real bios is copyrighted and this repository has
    none - so what is proven is that its absence is handled.
- **M5 DONE** (2026-08-27): the package runs inside Chimera.
  `waterbox/tests/run-frontend.sh`: 3/3 - the frontend's machine reaches the
  same System RAM as the native reference over 300 frames, the region setting
  changes the machine's flash, and the package's bindings (9 buttons and 4
  analog) become the frontend's defaults.
  - `waterbox.config` declares the machine: one Dreamcast controller with its
    stick and two ANALOG triggers, four settings that shape the machine (bios,
    clock, region, language, broadcast), and a fourth memory domain - the flash,
    which is where a Dreamcast keeps what KIND of machine it is.
  - Firmware is conditioned on the `bios` setting rather than declared
    optional, because this frontend has no such thing as optional firmware:
    every declared entry that applies is required, and variants are separate
    entries selected by a setting. "hle" and "real" are different machines and
    do not share movies.
- **M6 - beyond Dreamcast.** NAOMI and Atomiswave as additional machines in one
  package, the way gpgx serves four systems.

## Sharp edges hit

- **Two windows onto video memory, and only one of them is the PVR's.** The
  32-bit window (0xA5xxxxxx from the SH4) is what everything the PVR reads for
  ITSELF is addressed in - the region array, the object pointer blocks, the
  background parameters - and the emulator maps it into video memory the same
  way on both sides (`pvr_map32`). The linear-looking 0xA4 window puts data
  where the PVR does not look, and the picture simply does not appear.
- **Flycast's TA writes no object lists.** A render is found by following the
  region array to an object pointer block and reading the parameter address
  from its first word; real hardware's TA writes that word while it builds the
  lists, and a high-level TA builds no lists at all - so nothing writes it and
  the render is never identified. The gate's own program writes it, because on
  hardware it would be there.
- **ISP_BACKGND_D is at 0x88 and ISP_BACKGND_T at 0x8C.** Swapping them costs
  an afternoon: the background reads its parameters from a strip_base computed
  out of a depth value, finds zeros, and draws black - which looks exactly like
  "the background is not implemented yet".

- **A frontend mounts ONE file, under a name of its choosing.** Flycast decides
  what a file is from its extension, and the plain-rom path hands the core its
  content as `disc` with no extension at all - so nothing matched and every
  disc was "an unknown format". patches/0007 makes an UNKNOWN extension mean
  "look at the file" (every driver validates its own content anyway) and lets a
  driver decline while sniffing rather than ending the load. A named file still
  fails loudly: a corrupt .chd is a corrupt .chd.
  It also means a multi-track `.gdi` is a project with a directory behind it,
  not something the command line can express - so the frontend gate boots the
  single-file ELF and the core gate keeps the GD-ROM.
- **A patch that touches two files is a patch that can vanish.** `git apply
  --check` refuses a patch whose hunks are already applied, and refuses the
  WHOLE patch - so reverting one file by hand silently dropped the console-id
  pin from a two-file patch, and only the equivalence gate noticed. Patches are
  one file each now, and apply-patches.sh warns when one neither applies nor is
  applied.
- **Settings that arrive too late.** `loadGame()` RESETS every Flycast option
  and reloads them immediately before building the machine's flash, so a value
  assigned beforehand is thrown away and one assigned afterwards is too late
  for the region and language that live in that flash. `config::setTransient`
  is the mechanism for a front end with no config file - and the section is
  "config" with the key "Dreamcast.Region", not the section "Dreamcast", which
  the dotted name suggests.

- **The machine took its identity from the host.** Two of them, both caught by
  the gate as a handful of RAM bytes differing while the program's own counter
  matched exactly:
  - the real-time clock. A Dreamcast writes the time into RAM at boot and the
    bios and games read it, so a machine seeded from the wall clock is a
    different machine every run. patches/0005 lets the project pin it.
  - the CONSOLE ID, six bytes in flash that some games read. Upstream fills
    them with the C library's `rand()`, seeded from that same clock - and glibc
    and musl do not agree on `rand()`, so the same seed gave one console
    outside the sandbox and another inside it. The core derives those bytes
    itself now, with arithmetic it owns, so both flavors reach the same
    machine.
- **A path that every desktop shrugs off.** `getParentPath` returns `"./"` for a
  bare filename and `getSubPath` glued a separator onto it, so a `.gdi` in the
  working directory asked for `.//track01.bin`. A real file system normalises
  that away; a sandbox serving exactly the files it was given does not.

- **A region array that says "no object lists".** Flycast finds the display
  list a render belongs to by reading the OPB pointer out of the region array
  in VRAM and looking up the TA context stored under that address. A test
  program whose region array marks every list empty submits geometry the TA
  happily accepts, triggers a render, and gets nothing at all - no context, no
  Process, no picture.
- **Nobody starts the renderer.** `rend_init_renderer()` is called by whoever
  owns the graphics context - the window, the D3D device, the GL surface. With
  no window, nothing calls it, the renderer pointer stays null, and the first
  render segfaults inside Flycast. A software renderer needs no device, so the
  core starts it itself.
- **The input global that does nothing.** `kcode[4]` in gamepad_device.h looks
  like where a frontend writes button state. It is not: it belongs to the
  desktop input layer, which reads real joysticks and then fills
  `mapleInputState[]`, and that is what the controller actually reads. Writing
  the wrong one links, runs, and leaves the machine byte-identical whether
  buttons are held or not - which is exactly what the gate's input leg exists
  to catch.
- **A second thread stepping the same SH4.** `config::ThreadedRendering`
  decides, despite its name, whether `Emulator::start()` launches an emulation
  THREAD - and settings assigned before `init()`/`loadGame()` are overwritten
  by both. The symptom was an instruction trace that interleaved two executions
  of the same three-instruction loop, with registers from the wrong one. The
  settings are now pinned immediately before `start()`.
- **A Dreamcast is big.** 16MB of RAM, 16MB of VRAM, 8MB for the AICA, 32MB for
  the Elan: about 72MB of machine before Flycast allocates anything of its own,
  and with no virtual memory those are ordinary allocations in the guest heap.
  Sized like an 8-bit core, an allocation failed quietly and the machine wrote
  through a null base; the sandbox caught it ("OUTSIDE every registered block")
  rather than letting it corrupt anything.
- **No virtual memory.** Upstream reserves 512MB with shm and mmap so the host
  MMU can do SH4 translation. `waterbox/stubs/vmem-stub.cpp` refuses, and
  Flycast's supported fallback (software translation, plain allocations) is
  what the sandbox runs.
- **`access()` is not a syscall here.** The guest has mounted files and no
  kernel, so the call stopped the machine outright. It is answered in terms of
  what the sandbox has: a file that opens is readable, one that does not is
  absent, and nothing is writable.
- **Thread-local storage.** stb_image keeps its error string in `thread_local`,
  and the sandbox refuses any core with TLS. `STBI_NO_THREAD_LOCALS` and one
  three-line shim for the flag setter it drops.
- **The test program needed to mask interrupts.** The HLE bios boots an ELF
  with interrupts enabled and no handlers installed, so the first vblank
  vectored through VBR into empty RAM and died on an illegal instruction.
  Real homebrew installs handlers; this program refuses the interrupts.
- **zlib's CMake renames a tracked header.** Running Flycast's own CMake in the
  same checkout moves `zconf.h` to `zconf.h.included`, and every later build
  fails to find it. Restore it with git if a stray configure has been run.

## Log

- **2026-08-27** Feasibility settled (this document). Repo created, upstream
  pinned at `c3763d8`. Headless CMake configure proven; the headless BUILD gets
  as far as the two findings above, which is the M1 starting line.
- **2026-08-27** M5 done: the package loads in Chimera, its settings reach the
  machine, and its bindings become the frontend's defaults.
- **2026-08-27** M4 done: a GD-ROM this repository builds from scratch boots
  through the HLE bios, memory cards leave through the save-data channel, and
  the machine stopped taking its identity from the host.
- **2026-08-27** M3 done: a red triangle, drawn by software, identical in the
  sandbox and out. Flycast can draw without a GPU for the first time.
- **2026-08-27** M2 done: the machine reads its controller over the maple bus,
  lag detection works, and VRAM and sound RAM joined system RAM as domains.
- **2026-08-27** M1 done: a Dreamcast runs inside the sandbox, byte-identical
  to the native reference, savestates included. No patch was needed for the
  renderer after all - upstream's own `NO_REND` selects the renderer that draws
  nothing - so the patch set is one file: the log listener that opens a socket.
