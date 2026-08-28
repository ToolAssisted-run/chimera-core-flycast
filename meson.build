# chimera-core-flycast: Flycast in miniBox's sandbox, as a Chimera core.
#
#   guest (core.wbx)   sh waterbox/setup-guest.sh
#                      ninja -C build/meson-guest core.wbx
#   native reference   meson setup build/meson-native
#                      ninja -C build/meson-native
#
# Both flavors compile the SAME curated source set (waterbox/sources.sh) with
# the SAME defines, and the equivalence gate compares them frame for frame.
# No GPU backend is built: a sandbox has none, and M1 draws nothing at all
# (see docs/PLAN.md - the renderer is its own milestone).
project('chimera-core-flycast', 'c', 'cpp',
  default_options: ['c_std=gnu11', 'cpp_std=c++17',
                    'optimization=2', 'debug=false', 'warning_level=0'])

fc = 'extern/flycast'
fc_abs = meson.current_source_dir() / fc
sh = find_program('sh')

# the submodule pin is pristine upstream; overlay the chimera patch set
# (idempotent - see docs/PLAN.md)
run_command(files('waterbox/apply-patches.sh'), check: true)

mb = get_option('minibox_dir')
if mb == ''
  mb_candidate = meson.current_source_dir() / '../chimera/extern/tools/chimera-common-minibox'
  if import('fs').is_dir(mb_candidate)
    mb = mb_candidate
  else
    error('pass -Dminibox_dir=<miniBox checkout> (or keep one at ../chimera/extern/tools/chimera-common-minibox)')
  endif
endif

# CHIMERA_WATERBOX marks the places upstream asks the host for something a
# sandbox does not have. NO_ASAN/NDEBUG are upstream's own; LIBRETRO is NOT
# set: this core drives Emulator directly rather than through the libretro
# shell, which exists to serve a GUI this core does not have.
defs = [
  '-DNDEBUG',
  '-DPAGE_SIZE=4096',
  '-D_7ZIP_ST', '-DZ7_ST',   # the lzma in libchdr: single-threaded, both spellings
  '-DZSTD_DISABLE_ASM=1',    # zstd's hand-written amd64 decode loops: the C ones
                             # are what both flavors must agree on
  '-DTINYGETTEXT_WO_ICONV',
  '-DNOWIDE_NO_LIB',
  '-DCHIMERA_WATERBOX',
  # stb_image keeps its error string and its flip flag in thread_local storage,
  # and a waterbox guest has no thread pointer at all: the sandbox checks for
  # %fs accesses and refuses a core that has any. There are no threads here for
  # them to be local to.
  '-DSTBI_NO_THREAD_LOCALS',
  # Upstream's own escape hatch: with NO_REND the renderer factory returns the
  # renderer that draws nothing, which is exactly M1. (Without it, and with
  # every GPU backend off, that factory's switch has an empty body and does not
  # compile - which is how this was found.)
  # patches/0003 selects the software renderer this core supplies; NO_REND
  # stays defined so nothing tries to build a GPU backend.
  '-DCHIMERA_SOFTWARE_RENDERER',
  '-DNO_REND', '-DCHIMERA_NO_PAGE_PROTECTION',
  '-DNO_NETWORK_LOGGER',   # patches/0001: the log listener that opens a socket
  # No recompilers, chosen the way upstream chooses them: FEAT_* are build-time
  # decisions, not settings, so pinning DYNAREC_NONE is what actually selects
  # the interpreters for the SH4, the ARM7 and the AICA's DSP. (Setting only
  # config::DynarecEnabled leaves the ARM7 asking a JIT backend that is not
  # there, which is how this was found: the machine got as far as the ARM7 and
  # tripped its own assertion.)
  '-DFEAT_SHREC=0x40000001',
  '-DFEAT_AREC=0x40000001',
  '-DFEAT_DSPREC=0x40000001',
  '-DFLYCAST_DATADIR="/"',
  '-DFLYCAST_SYSCONFDIR="/"',
  '-fno-strict-aliasing',
  '-Wno-deprecated-declarations',
]
add_project_arguments(defs, language: ['c', 'cpp'])

inc = include_directories(
  fc / 'core',
  fc / 'core/deps',
  fc / 'core/deps/stb',
  fc / 'core/deps/json',
  fc / 'core/deps/glm',
  fc / 'core/deps/xxHash',
  fc / 'core/deps/libchdr/include',
  fc / 'core/deps/libchdr/deps/zlib-1.3.1',
  fc / 'core/deps/libchdr/deps/lzma-24.05/include',
  fc / 'core/deps/libchdr/deps/zstd-1.5.6/lib',
  fc / 'core/deps/libelf/include',
  fc / 'core/deps/nowide/include',
  'waterbox',
  'waterbox/refsw',
)

sources_sh = files('waterbox/sources.sh')
core_sources = files(run_command(sources_sh, 'main', check: true).stdout().strip().split('\n'))
dep_sources = files(run_command(sources_sh, 'deps', check: true).stdout().strip().split('\n'))

deps_lib = static_library('flycast-deps', dep_sources, include_directories: inc)

# refsw is vendored UNMODIFIED (BSD-3, see waterbox/refsw/license/bsd), so
# everything it expects of its host arrives through a forced include rather
# than through edits: the VRAM accessors Flycast spells differently, the
# register fields Flycast leaves as plain u32, and libswirl's logging macros.
refsw_lib = static_library('refsw',
  files('waterbox/refsw/refsw_tile.cpp', 'waterbox/refsw/TexUtils.cpp'),
  include_directories: inc,
  cpp_args: ['-include', meson.current_source_dir() / 'waterbox/refsw/refsw-host.h'])

# What a sandbox answers differently from a desktop. Each file says what it
# refuses and why; none of them patch upstream.
driver_sources = files(
  'waterbox/cinterface.cpp',
  'waterbox/stubs/archive-stub.cpp',
  'waterbox/stubs/host-stubs.cpp',
  'waterbox/stubs/vmem-stub.cpp',
  'waterbox/stubs/posix-stubs.cpp',
  # the software renderer: skmp's reference rasteriser (BSD-3), and the walker
  # that feeds it from Flycast's parsed lists
  'waterbox/refsw-renderer.cpp',
  'waterbox/refsw/bridge.cpp',
)

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core', core_sources + driver_sources,
    name_suffix: 'wbx',
    include_directories: inc,
    link_with: [deps_lib, refsw_lib],
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax',
      '-Wl,-z,stack-size=8388608',
      '-T', mbdir / 'source/guest/linkscript.T',
      '-Wl,-u,pthread_once', '-Wl,-u,pthread_cond_wait',
      '-Wl,-u,pthread_cond_broadcast', '-Wl,-u,pthread_key_create',
      mbuild / 'source/guest/cxxglue.c.o',
      mbuild / 'source/guest/emulibc.c.o',
      '-L' + sr / 'lib', '-lstdc++', '-lgcc', '-lgcc_eh', '-lc'])
else
  # ---- the native reference and the sandbox driver --------------------------
  threads = dependency('threads')
  dl = meson.get_compiler('c').find_library('dl', required: false)
  m = meson.get_compiler('c').find_library('m', required: false)

  executable('run-native', core_sources + driver_sources + files('waterbox/run-native.c'),
    include_directories: [include_directories('waterbox/native-shim'), inc],
    c_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    cpp_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    link_with: [deps_lib, refsw_lib],
    dependencies: [threads, dl, m])

  mbhost = mb / 'build/meson-cpp/source/host'
  executable('run-wbx', files('waterbox/run-wbx.c'),
    c_args: ['-I' + mb / 'source/host'],
    link_args: [mbhost / 'libminiboxhost.so', '-Wl,-rpath,' + mbhost])
endif
