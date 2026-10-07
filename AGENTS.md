# AGENTS.md - Flycast core for Chimera

This repository builds Flycast (Dreamcast, NAOMI, NAOMI 2, Atomiswave) as a
sandboxed guest for Chimera, a frontend for tool-assisted speedruns. It
produces one file, `flycast.chimeraCore`, which Chimera loads from its `Cores`
folder. The same sources are also built natively as a reference, and the gate
holds the two byte-identical. `.github/workflows/chimera.yml` is the
authoritative build recipe; `docs/BUILDING.md` explains it step by step.

## Layout

- `extern/flycast` - upstream Flycast, a pinned submodule. Never edited in place.
- `patches/` - 19 numbered patches applied to `extern/flycast`.
- `meson.build`, `meson_options.txt` - one build for both flavors (`minibox_dir`, `mesa_guest_dir`).
- `waterbox/sources.sh` - the curated list of upstream sources both flavors compile.
- `waterbox/cinterface.cpp` - the adapter between Flycast and the guest ABI.
- `waterbox/refsw/`, `waterbox/refsw-renderer.cpp` - the software rasteriser (vendored, BSD-3-Clause).
- `waterbox/gl-osmesa.cpp`, `waterbox/gl-bridged.cpp`, `waterbox/gl-entry-points.txt` - OpenGL in the sandbox, and the GPU bridge's guest half.
- `waterbox/run-native.c`, `waterbox/run-wbx.c`, `waterbox/gate-harness.h` - the native reference and the sandbox driver.
- `waterbox/apply-patches.sh`, `setup-mesa.sh`, `setup-guest.sh`, `build-package.sh`, `run-gate.sh` - the build and the core gate.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`, `package-licenses.json` - what the package declares to Chimera.
- `waterbox/tests/` - the frontend gate (`run-frontend.sh`) and its helpers.
- `tests/` - the gate's own programs (`roms/`, built by `make-testprog.py`, `make-testdisc.py`, `sh4asm.py`) and redistributable test content (`own/`).
- `docs/PLAN.md` - the design log: decisions, measurements, sharp edges.
- `build/` - every build output. Ignored by git.

## Set up the build environment

Ubuntu, as CI uses. Set the two paths first; both must be absolute.

```sh
chimera="$HOME/chimera"                        # a Chimera checkout
mb="$chimera/extern/chimera-common-minibox"    # miniBox, a submodule of it

sudo apt-get update
sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3 bison flex pkg-config python3-mako python3-packaging

git submodule update --init
git -C extern/flycast submodule update --init --recursive --depth 1 core/deps/libchdr core/deps/xbyak

[ -d "$chimera" ] || git clone https://github.com/ToolAssisted-run/chimera.git "$chimera"
git -C "$chimera" submodule update --init extern/chimera-common-minibox

[ -f "$mb/build/meson-linux/build.ninja" ] || meson setup "$mb/build/meson-linux" "$mb"
meson compile -C "$mb/build/meson-linux"
[ -f "$mb/build/meson-cpp/build.ninja" ] || meson setup "$mb/build/meson-cpp" "$mb" -Dguest_cpp=true
meson compile -C "$mb/build/meson-cpp"
```

## Build

Shortest path to a package (what the workflow's `frontend-gate` job runs):

```sh
bash waterbox/setup-mesa.sh -m "$mb"     # once: fetches Mesa 24.0.9 (SHA-256 pinned), builds build/mesa
./waterbox/build-package.sh -m "$mb" -r "$chimera"
```

For the core gate you also need the native reference, and the guest built by
hand (what the `core-gate` job runs):

```sh
meson setup build/meson-native -Dminibox_dir="$mb"
ninja -C build/meson-native
MINIBOX_DIR="$mb" sh waterbox/setup-guest.sh -- -Dminibox_dir="$mb" -Dmesa_guest_dir="$PWD/build/mesa"
ninja -C build/meson-guest
```

- `setup-mesa.sh` needs `bash` and network access the first time.
  `MESA_BUILD_CONFIGURE_ONLY=1` checks the recipe without compiling.
- The patches are applied by `meson.build` at configure time. Nothing to run.
- `build-package.sh` configures `build/meson-guest` only if it has no
  `build.ninja`. An existing directory keeps the options it was configured
  with, including a missing `-Dmesa_guest_dir`.
- A hand-built package is stamped `<commit>+local` (`-dirty` with changes in
  the tree, which includes the applied patches). CI stamps the commit.

## Install the core into Chimera

`build-package.sh -r "$chimera"` writes
`$chimera/build/Cores/flycast.chimeraCore`: the cores folder of a Chimera
source checkout, so nothing else is needed there. For a release bundle, copy
the file into the `Cores` folder beside `Chimera.exe` (or the folder set in
File > Core Manager > Change folder...). File > Core Manager lists the folder;
Refresh List rescans it. Chimera downloads nothing. The same file runs on
Linux and on Windows.

## Test before you commit

```sh
./waterbox/run-gate.sh                                        # the core gate
./waterbox/tests/run-frontend.sh --chimera-root "$chimera"    # the frontend gate
```

- The core gate must end `N ok, 0 failed, M skipped`. It needs
  `build/meson-native` and `build/meson-guest` and no content: it runs the
  programs in `tests/roms/` and the 240p Test Suite in `tests/own/`.
- Expected SKIPs without content: the legs behind `FLYCAST_GFX_DISC`,
  `FLYCAST_SORT_DISC`, `FLYCAST_DISC2`, `FLYCAST_ARCADE_ROMS`,
  `FLYCAST_NAOMI2_ROMS`, `FLYCAST_AW_ROMS`, and `triangle:turbo`.
- `disc:columns`, `ports:columns` and `gl:rebuild-at-zero` run only when
  `$CHIMERA_ROOT` (or `chimera-checkout/`, `../chimera`, `$HOME/chimera`) has
  `build/meson-linux/chimera-run` and the installed package. CI skips them.
  Run them when you touch inputs, discs or the GL renderer.
- The frontend gate needs Chimera built (natives and the .NET solution), the
  package installed, `run-native`, Mono and Xvfb. See `docs/BUILDING.md`.
  Run it when you touch `waterbox.config`, `default_keybinds.json`,
  `file_slots.json` or anything the frontend passes to the core.
- A leg that was skipped has proven nothing about your change.

## Rules of this repository

- **Upstream is patched, not edited.** `extern/flycast` stays at its pin.
  Changes to it are numbered patches in `patches/`
  (`NNNN-chimera-<what it does>.patch`, `git diff` format, paths relative to
  the submodule). `waterbox/apply-patches.sh` applies the whole series or
  none. `git status` shows `extern/flycast` as modified once it is applied;
  that is expected. Never commit inside the submodule. After adding or
  changing a patch, `waterbox/apply-patches.sh` must print
  `already applied: all N patches`.
- **Determinism is the product.** The guest must not read host time, host
  randomness or anything else that differs between runs, and a savestate
  must round-trip. The gate checks both; a change that breaks either is a
  bug. Only the `opengl-hw` renderer is declared non-deterministic
  (`waterbox.config`): its picture comes from the host's GPU.
- **Run the gate before committing.** A new check needs a negative control:
  show that it fails when the thing it checks is broken.
- **Never commit game files, BIOS or firmware.** `tests/roms/` holds only
  programs this repository builds; disc formats are ignored there. Content
  for local testing goes in `tests/roms-local/` and `tests/firmware-local/`.
  Redistributable content goes in `tests/own/` with its terms.
- **Never add network access** to the core. A sandbox has no sockets.
- **Scripts stay executable.** Every `.sh` under `waterbox/` is git mode
  100755; meson and CI run them directly.
- **Documentation prose is plain ASCII.**
- **Commit messages** follow the log: `type(scope): a sentence that says what
  is now true`, for example
  `fix(maple): a light gun has its memory card (chimera#181)`. The body is
  prose: the cause, the fix, what was measured, and the gate count. Issues
  live in the chimera repository and are cited as `chimera#N`. Assisted
  commits end with a `Co-Authored-By:` trailer.
- **Do not edit `.github/workflows`** unless the task is the workflow.

## Where to read more

- `docs/BUILDING.md` - every build step, option and gate leg.
- `docs/PLAN.md` - why each decision was made; grep it before changing a patch.
- `.github/workflows/chimera.yml` - the recipe CI runs.
- `tests/own/README.md`, `LICENSE` - terms of the test content and the package.
- In the Chimera checkout: `docs/porting-a-core.md` (how a core is put together, patch traps), `docs/gates.md` (how a green gate can be wrong), `docs/core-manager.md` (packages, versions, the cores folder).
