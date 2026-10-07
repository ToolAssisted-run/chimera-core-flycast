# Building the Flycast core

This repository builds Flycast (the Dreamcast, and the NAOMI, NAOMI 2 and
Atomiswave arcade boards) as a sandboxed guest for Chimera. The result is one
file, `flycast.chimeraCore`, which Chimera loads. The steps below are the ones
`.github/workflows/chimera.yml` runs on a fresh clone on a public Ubuntu
runner. Cores are built on Linux; the same package file runs on Linux and on
Windows, because the guest inside it is run by Chimera's sandbox (miniBox) on
either.

Names used below:

- `<chimera>` - a checkout of https://github.com/ToolAssisted-run/chimera.
- `<minibox>` - `<chimera>/extern/chimera-common-minibox`, the miniBox
  submodule: the sandbox host and the guest toolchain.

The commands use two shell variables for them. Both must be absolute paths
(CI passes `$PWD/chimera-checkout/...`). Commands run from the root of this
repository unless they say otherwise.

```sh
chimera=/absolute/path/to/chimera
mb="$chimera/extern/chimera-common-minibox"
```

## Requirements

**Operating system.** CI builds on GitHub's `ubuntu-latest` runner, x86-64.

**Packages for the core and the core gate** (workflow job `core-gate`):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3 bison flex pkg-config python3-mako python3-packaging
```

**Packages for the frontend gate** (workflow job `frontend-gate`, which also
builds Chimera itself):

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  meson ninja-build build-essential cmake pkg-config python3 bison flex \
  mono-complete xvfb \
  libgl1-mesa-dev libx11-dev libxext-dev libasound2-dev python3-mako python3-packaging
```

**Toolchains.**

- C and C++: the gcc and g++ that `build-essential` installs. The workflow
  pins no compiler version.
- .NET SDK 8.0, for the frontend gate only. CI uses `actions/setup-dotnet@v4`
  with `dotnet-version: '8.0'`. Chimera's README gives the manual equivalent:
  `curl -sSL https://dot.net/v1/dotnet-install.sh | bash -s -- --channel 8.0`.
  It also says distro-built SDKs omit the WindowsDesktop targets the frontend
  needs.
- Mono and Xvfb (`mono-complete`, `xvfb`), for the frontend gate only.
- No Rust.

**What the scripts fetch or build themselves.**

- `waterbox/setup-mesa.sh` downloads one file:
  `https://archive.mesa3d.org/mesa-24.0.9.tar.xz`. It checks it against the
  SHA-256 `51aa686ca4060e38711a9e8f60c8f1efaa516baf411946ed7f2c265cd582ca4c`
  and stops if it does not match. The tarball is kept in `build/deps/` (or
  `$CHIMERA_DEPS_DIR`) and unpacked into `build/mesa/`. The script calls
  `curl`, `sha256sum` and `tar`; the workflow installs none of them and uses
  the runner's.
- From that tarball it cross-builds Mesa for the guest: the softpipe driver
  (`-Dgallium-drivers=swrast`) behind the gallium OSMesa front end, static,
  with LLVM disabled. zlib and expat come from Mesa's own Meson wraps
  (`-Dforce_fallback_for=zlib,expat`). The output is `build/mesa/build-guest2/`.
- If the system has no `meson` with the Python modules `mako` and `packaging`,
  the same script makes a private Python venv at
  `$HOME/.cache/chimera-mesa-build-venv` (or `$MESA_BUILD_VENV`) and installs
  `meson ninja mako packaging` into it with pip. With the apt packages above
  the system meson is used and no venv is made.
- miniBox builds the guest C and C++ toolchain from the Chimera submodule.
- Everything else is source: this repository, the `extern/flycast` submodule
  and two of Flycast's own submodules (libchdr and xbyak).

## Get the sources

CI checks this repository out with `actions/checkout@v6` and
`submodules: true`, then fetches the two nested submodules the core compiles.
By hand:

```sh
git clone https://github.com/ToolAssisted-run/chimera-core-flycast.git
cd chimera-core-flycast
git submodule update --init
git -C extern/flycast submodule update --init --recursive --depth 1 core/deps/libchdr core/deps/xbyak
```

Flycast has many more submodules (SDL, Vulkan headers, breakpad, asio). They
belong to the desktop application and are never fetched; see
`waterbox/sources.sh`.

Chimera, with the miniBox submodule. CI checks out Chimera's `main` branch
(`CHIMERA_REF: main`) into `chimera-checkout/` inside this repository's
workspace:

```sh
git clone https://github.com/ToolAssisted-run/chimera.git "$chimera"
git -C "$chimera" submodule update --init extern/chimera-common-minibox
```

That is enough to build the core, the package and the core gate. The frontend
gate builds Chimera too, and for that CI checks Chimera out with
`submodules: recursive`:

```sh
git -C "$chimera" submodule update --init --recursive
```

Where the scripts look when they are not told:

| Script | Option | Otherwise |
| --- | --- | --- |
| `meson.build` | `-Dminibox_dir=<minibox>` | `../chimera/extern/chimera-common-minibox` beside this repository, else an error |
| `waterbox/setup-mesa.sh`, `waterbox/setup-guest.sh` | `-m <minibox>` | `$MINIBOX_DIR`, else `$HOME/chimera/extern/chimera-common-minibox` |
| `waterbox/build-package.sh` | `-r <chimera>`, `-m <minibox>` | Chimera: `../chimera`, else `$HOME/chimera`. miniBox: `$MINIBOX_DIR`, else `<chimera>/extern/chimera-common-minibox` |
| `waterbox/tests/run-frontend.sh` | `--chimera-root <chimera>` | `../chimera`, else `$HOME/chimera` |

The defaults do not all agree, so pass the paths, as CI does.

## Build miniBox

The host library, and the C++ guest toolchain Flycast needs:

```sh
[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

`build/meson-cpp/guest-sysroot` is the sysroot `setup-mesa.sh` and
`setup-guest.sh` require. `run-wbx` links
`build/meson-cpp/source/host/libminiboxhost.so`. CI keeps both build
directories in an `actions/cache@v4` cache; on your machine they simply stay.

## Build the core

### Patches

`extern/flycast` is pinned to pristine upstream. The 19 numbered patches in
`patches/` are applied to it by `waterbox/apply-patches.sh`. There is no
separate step: `meson.build` runs the script at configure time, so both
`meson setup build/meson-native` and `setup-guest.sh` apply them.

The script judges the series as a whole:

- It first applies every patch, in order, to a scratch copy of the touched
  files as the submodule's HEAD has them. If that fails it stops and names the
  patch: the submodule was moved without rebasing the patches.
- If the working tree is pristine it applies the series and prints
  `applied: <patch>` for each.
- If the working tree is exactly what the series leaves behind it prints
  `already applied: all 19 patches` and changes nothing.
- Anything in between is an error that names the files and prints the reset
  command.

### The guest Mesa

The OpenGL renderer runs inside the sandbox against a Mesa softpipe that is
linked into the core. Build it once:

```sh
bash waterbox/setup-mesa.sh -m "$mb"
```

Run it with `bash`, not `sh`: the script uses `set -o pipefail`.

- Options: `-m <miniBox dir>`, `-j N` (default: `nproc`).
- `MESA_TARBALL=<path>` uses a tarball already on the machine. It is still
  checked against the SHA-256.
- `MESA_BUILD_CONFIGURE_ONLY=1` stops after configuring. The script's header
  describes the compile it skips as about 15 minutes.
- If `build/mesa/build-guest2` already holds the archives and the osmesa
  `target.c.o`, the script prints `mesa: already built` and exits.
- Mesa's final shared osmesa library fails to link for the guest. That is
  expected. The core links the static archives and `target.c.o`, and the
  script checks that those exist and ends with `mesa: ready - ...`.

### The native reference

The same curated sources (`waterbox/sources.sh`) built for the host. It is
what the gates compare the sandboxed core against.

```sh
meson setup build/meson-native -Dminibox_dir="$mb"
ninja -C build/meson-native
```

This builds `run-native` (the reference), `run-wbx` (the driver that runs
`core.wbx` through the miniBox host library), `test-zipfile` and
`test-texfmt`. CI configures it without `-Dmesa_guest_dir`, so the reference
draws with the software rasteriser only. The frontend gate needs only
`ninja -C build/meson-native run-native`.

### The guest core

```sh
MINIBOX_DIR="$mb" sh waterbox/setup-guest.sh \
  -- -Dminibox_dir="$mb" -Dmesa_guest_dir="$PWD/build/mesa"
ninja -C build/meson-guest
```

`setup-guest.sh` writes `build/guest-cross.ini` (machine-local paths) and runs
`meson setup build/meson-guest`; if that fails it runs it again with
`--reconfigure`. Arguments after `--` go to meson. The result is
`build/meson-guest/core.wbx`.

`-Dmesa_guest_dir` decides what the core can draw with:

- With it: Flycast's own OpenGL renderer is built and linked against the
  guest Mesa, and the guest half of Chimera's GPU bridge is generated
  (miniBox's `source/gl/gen-gl-bridge.py`, from `waterbox/gl-entry-points.txt`)
  and compiled in. The core then offers three renderers: `software`,
  `opengl` (softpipe, inside the sandbox) and `opengl-hw` (the same renderer
  with its calls leaving the sandbox to the host's GPU through the bridge).
- Without it: only the reference software rasteriser is built.

## Build the package

```sh
./waterbox/build-package.sh -m "$mb" -r "$chimera"
```

Options: `-m <miniBox dir>` and `-r <chimera root>`. There is no output
directory option.

What it does, in order:

1. Requires the guest Mesa at `build/mesa/build-guest2`. `MESA_GUEST_DIR=<path>`
   names one elsewhere. `MESA_GUEST_DIR=` (set, empty) builds a software-only
   core instead.
2. If `build/meson-guest/build.ninja` does not exist, runs `setup-guest.sh`.
   Then always `ninja -C build/meson-guest core.wbx`, and miniBox's
   `source/guest/check-wbx.sh` on the result.
3. Stages `core.wbx`, `waterbox.config`, `default_keybinds.json`,
   `file_slots.json`, the licence texts (`waterbox/package-licenses.json`)
   and a `build.json` that records the toolchain and the pins.
4. Stamps the version into the staged `waterbox.config`. CI passes
   `CORE_VERSION` (the commit). Without it the stamp is `<commit>+local`,
   with `-dirty` after the commit when `git diff --quiet HEAD` reports
   changes. The applied patch series counts as a change, so a hand build
   normally reads `<commit>-dirty+local`. `versionDate` is the commit's date
   in UTC.
5. Writes `<chimera>/build/Cores/flycast.chimeraCore`, replacing the one
   there. The zip is written twice and the two SHA-1s must match; it prints
   `package sha1 ...` and `packaged -> ...`.
6. Removes `<chimera>/build/CoreCache/flycast-*`.

The script does not need the native reference. From a built miniBox, the
shortest path to a package is `setup-mesa.sh` and then `build-package.sh`,
which is what the workflow's `frontend-gate` job does.

## Install it into Chimera

Chimera ships no cores and downloads nothing: it has no network code. A core
is a file a person puts in Chimera's `Cores` folder.

- **A Chimera source checkout.** The cores folder is `<chimera>/build/Cores/`,
  and `build-package.sh -r <chimera>` has already written the package there.
- **A release bundle.** Copy `flycast.chimeraCore` into the `Cores` folder
  beside `Chimera.exe`, or into the folder chosen with Change folder... in
  File > Core Manager.
- **Without building.** Download the package from this repository's Releases
  page, https://github.com/ToolAssisted-run/chimera-core-flycast/releases, and
  put it in the same folder. CI publishes a rolling `dev` release on every
  green push to `main` and a dated `nightly-YYYY-MM-DD` release from the
  scheduled run, when `main` moved since the last one.

File > Core Manager lists what is in the folder; Refresh List rescans it.

A package's version is the commit it was built from. A hand-built package
carries `+local` and is for testing; Chimera's publishing script refuses to
publish one. A published package is named `flycast-<version>.chimeraCore` and
the one built here `flycast.chimeraCore`; Chimera identifies a package by its
content, not by its file name.

## Run the gates

### The core gate

```sh
./waterbox/run-gate.sh
```

Options: `-n <native build dir>` (default `build/meson-native`) and
`-g <guest build dir>` (default `build/meson-guest`). It needs `run-native`,
`run-wbx` and `core.wbx`. It prints one line per check and ends with
`N ok, N failed, N skipped`; it fails when any check failed.

It proves that the sandboxed core produces byte-identical video, audio, lag
and memory-domain digests to the native reference, and that a savestate
round-trip around every frame changes nothing. It runs programs this
repository assembles (`tests/roms/`, built by `tests/make-testprog.py` and
`tests/make-testdisc.py`) and the 240p Test Suite (`tests/own/240pSuite/`,
GPL, redistributable), so it needs nothing provided.

Runs from a fresh clone:

| Checks | What they hold |
| --- | --- |
| `zip:reader`, `texfmt:order` | the zip reader and the texture decoders, with no machine |
| `<program>:equivalence`, `:ran`, `:audio`, `:audioSteady`, `:turbo`, `:savestate` | native == sandbox, the SH4 executed, sound is produced at a steady rate, turbo changes nothing, a state round-trip per frame is lossless |
| `progress:machine`, `native:determinism` | the machine moves; two native runs agree |
| `jit:selfModifyingCode`, `jit:equivalence` | the SH4 recompiler, native and sandboxed |
| `input:shaped`, `input:triggers`, `input:lag`, `domains` | input reaches the machine, lag is counted, the memory domains are exposed |
| `render:drew`, `render:shape`, `picture:onlyThePicture` | the picture; the last needs a core built with `-Dmesa_guest_dir` |
| `disc:selector`, `disc:boots`, `disc:equivalence` | the disc selector wraps; the gate's own GD-ROM boots and runs the same in the sandbox |
| `savedata:vmu`, `savedata:seeded` | memory cards through the save-data channel |
| `suite240p:*` | the 240p Test Suite's own readouts |

Skipped without content or without Chimera:

| Checks | Needs |
| --- | --- |
| `triangle:turbo` | nothing can make it run: that program draws one frame |
| `picture:transparentSorting` | `FLYCAST_GFX_DISC`, a Dreamcast disc (`.chd`, `.gdi`, `.cdi` or `.cue`); `FLYCAST_GFX_FRAMES` moves the frame |
| `picture:perPixel` | `FLYCAST_SORT_DISC`, and `FLYCAST_SORT_FRAMES` |
| `disc:swap` | `FLYCAST_GFX_DISC` and `FLYCAST_DISC2`, two Dreamcast discs |
| `naomi:*` | `FLYCAST_ARCADE_ROMS`, a folder holding `naomi.zip` and one game (a MAME zip, or a decrypted `.dat`/`.bin`) |
| `naomi2:*` | `FLYCAST_NAOMI2_ROMS`, a folder holding `naomi2.zip` and one game |
| `atomiswave:*` | `FLYCAST_AW_ROMS`, a folder holding `awbios.zip` and one game |
| `disc:columns`, `ports:columns`, `gl:rebuild-at-zero` | `<chimera>/build/meson-linux/chimera-run` and the installed `<chimera>/build/Cores/flycast.chimeraCore` |

The last three look for Chimera in `$CHIMERA_ROOT`, then `chimera-checkout/`
in this repository, `../../chimera`, `../chimera` and `$HOME/chimera`. CI's
`core-gate` job builds neither Chimera nor the package, so they are skipped
there. `gl:rebuild-at-zero` is also skipped when the machine gives the GPU
bridge no GL context.

### The frontend gate

It runs the package inside Chimera itself, headless under Mono. Build Chimera
first, as the workflow does:

```sh
cd "$chimera"
meson setup build/meson-linux --prefix "$PWD/build" --libdir dll
meson compile -C build/meson-linux
meson install -C build/meson-linux
dotnet build source/gui/Chimera.sln -c Release /nodeReuse:false -p:UseSharedCompilation=false
```

Then, from this repository, with the package installed and `run-native` built:

```sh
./waterbox/tests/run-frontend.sh --chimera-root "$chimera"
```

Options: `--chimera-root <path>` and `--frames N` (default 300). When
`DISPLAY` is not set it starts its own Xvfb. Logs and dumps go to
`waterbox/tests/work/`.

| Check | What it holds | Skipped without |
| --- | --- | --- |
| `disc:frontend` | `tests/roms/padread.elf` through Chimera: System RAM equals the native reference | - |
| `settings:region` | a machine-shaping setting reaches the guest through the frontend | - |
| `keybinds` | the package's key bindings become the frontend's defaults | - |
| `arcade:frontend`, `arcade:keybinds` | an arcade board opened by the frontend | `FLYCAST_ARCADE_ROMS` |

## Files the core needs at run time

Game files, BIOS and firmware are never in this repository or in the package.
The user provides them. `waterbox/file_slots.json` and the `firmware` list in
`waterbox/waterbox.config` are the declarations; this is what they say.

**Dreamcast** (`machine` = `dreamcast`, the default)

- Disc: 1 to 8 files. A `.gdi` index with its track files beside it, a
  `.cdi`, a `.chd`, a `.cue`, or a naked `.elf`.
- Firmware: none with `bios` = `hle`, the default. With `bios` = `real`, the
  project asks for `dc_boot.bin` (2097152 bytes) and `dc_flash.bin` (131072
  bytes).
- Save data, optional: up to 4 memory cards, read by name: `vmu_A1.bin`
  through `vmu_D1.bin` (and `vmu_A2.bin` through `vmu_D2.bin` for a second
  card), as Emulator > Export Save Data... writes them.

**NAOMI, NAOMI 2, Atomiswave** (`machine` = `naomi`, `naomi2`, `atomiswave`)

- Rom set: 1 to 3 files. The MAME zip under MAME's name, a clone's parent
  zip, the `.chd` of a GD-ROM game; or a decrypted dump (`.dat`, `.bin`,
  `.lst`).
- Firmware, required: the board's bios set, taken whole. `naomi.zip` for a
  NAOMI with the default `naomiBios`; the other `naomiBios` values each ask
  for their own set, listed in `waterbox.config`. `naomi2.zip` for a NAOMI 2.
  `awbios.zip` for an Atomiswave.
- Board memory, optional: `<rom set>.eeprom`, `<rom set>.nvmem`, and the
  cards `vmu_B1.bin` / `vmu_C1.bin`.

The `renderer` setting has three values. `software` (the default) and
`opengl` run inside the sandbox and are deterministic. `opengl-hw` sends the
OpenGL calls to the host's GPU through Chimera's GPU bridge; it is faster and
its picture is not deterministic.

## Troubleshooting

- **`guest sysroot not built at .../build/meson-cpp/guest-sysroot`**
  (`setup-mesa.sh`) or **`miniBox C++ guest toolchain missing`**
  (`setup-guest.sh`). Build miniBox's `meson-cpp` with `-Dguest_cpp=true`
  first.
- **`guest Mesa not built at .../build/mesa/build-guest2`**
  (`build-package.sh`). Run `bash waterbox/setup-mesa.sh` first, or set
  `MESA_GUEST_DIR=` (empty) for a software-only core.
- **`mesa: the tarball is not the release this core is pinned to`.** The file
  in `build/deps/` (or `$MESA_TARBALL`) does not have the pinned SHA-256.
  Remove it and run the script again.
- **`mesa: no usable meson - install python3-venv, or meson plus
  python3-mako`.** Install `python3-mako` and `python3-packaging` as CI does.
- **`setup-mesa.sh` fails at once under `sh`.** It needs `bash`.
- **Mesa prints a link error for the shared osmesa library.** Expected; see
  "The guest Mesa" above. What matters is the final `mesa: ready` line.
- **`pass -Dminibox_dir=<miniBox checkout>`** (meson). `meson.build` found no
  miniBox at `../chimera/extern/chimera-common-minibox`. Pass the option.
- **git rejects `core/deps/libchdr` or `core/deps/xbyak` as a pathspec.**
  They are submodules of Flycast, not of this repository. Use
  `git -C extern/flycast submodule update ...` as shown above.
- **`extern/flycast is partly patched`** (`apply-patches.sh`). A patched file
  was edited or reverted by hand. Turn any edits you want into a patch, then
  run the reset command the script prints.
- **`the series does not apply to the submodule's HEAD`.** The submodule pin
  was moved without rebasing the patches.
- **The package has no OpenGL renderer.** `build-package.sh` configures
  `build/meson-guest` only when it has no `build.ninja`. A guest directory
  configured earlier without `-Dmesa_guest_dir` keeps that choice. Remove
  `build/meson-guest`; `build-package.sh` then configures it with the guest
  Mesa. The core gate shows the same thing: `picture:onlyThePicture` reports
  SKIP, "built without -Dmesa_guest_dir".
- **A build cannot find `zconf.h`.** Running Flycast's own CMake in the same
  checkout renames zlib's tracked `zconf.h` to `zconf.h.included`
  (`docs/PLAN.md`). Restore it with git.
- **Gate legs say `needs chimera-run and a built flycast.chimeraCore`.** Set
  `CHIMERA_ROOT` to a Chimera checkout whose natives are built and where the
  package is installed.
- **The frontend gate stops with `Chimera not built`, `package not installed`
  or `native reference not built`.** Build Chimera, run `build-package.sh`,
  or build `run-native`, respectively.
- **`Xvfb not found (apt install xvfb)`.** The frontend gate starts its own X
  display when `DISPLAY` is not set, and needs `xvfb` for it.
- **`minibox-diag.log` appears.** The sandbox writes it when a guest faults.
  It is ignored by git.
