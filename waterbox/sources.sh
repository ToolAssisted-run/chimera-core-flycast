#!/bin/sh
# The curated Flycast source set: what the Chimera core compiles of upstream,
# and nothing else. Both flavors of the build read it from here - the guest
# (core.wbx) and the native reference the equivalence gate compares against -
# so there is one answer to "which sources are this core", not two that drift.
#
# It is a script rather than a list because the set is described by SHAPE, not
# by enumeration: "all of the hardware except the modem and the broadband
# adapter", "the disc readers", "the HLE bios". A pin bump that adds a chip
# should be picked up; one that adds a GPU backend should not.
#
# What is deliberately absent, and why:
#   rend/vulkan, rend/dx9, rend/dx11              no GPU in a sandbox
#   rend/gles                                     compiled when this core is
#                                                 built with a guest Mesa: the
#                                                 GL renderer and the GL that
#                                                 answers it both run INSIDE
#                                                 the sandbox (see
#                                                 waterbox/gl-osmesa.cpp)
#   network/, hw/bba, hw/modem                    no sockets in a sandbox, and
#                                                 netplay is not a TAS feature
#   ui/, sdl/, windows/, achievements/, lua/      a frontend already exists
#   archive/                                      zipped discs; libzip and the
#                                                 7z sdk for a convenience a
#                                                 project file already provides
#   *_x64/_x86/_arm*.cpp under hw/                JIT backends for the ARM7 and
#                                                 the AICA DSP; M1 interprets
#   deps/libzip, deps/miniupnpc, deps/picotcp     what those pull in
#   input/dreampotato.cpp                         a network gamepad
#
# Prints paths relative to the repository root, one per line.
#
# Usage: sources.sh [main|deps]
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
fc="extern/flycast"
cd "$root"

# The machine: SH4, the AICA and its ARM7, the PVR, the GD-ROM, maple (the
# controller bus), the flash/eeprom chips, holly (the bus controller), and the
# NAOMI hardware - which is not optional even for a Dreamcast-only build,
# because the cart and maple code reference it.
hw_srcs() {
	find "$fc/core/hw" \( -name '*.cpp' -o -name '*.c' \) | grep -v \
		-e "^$fc/core/hw/bba/" \
		-e "^$fc/core/hw/modem/" \
		-e "^$fc/core/hw/naomi/netdimm" \
		-e "^$fc/core/hw/naomi/naomi_m3comm" \
		-e "_x64\.cpp$" -e "_x86\.cpp$" -e "_arm32\.cpp$" -e "_arm64\.cpp$" \
		-e "/rec-cpp/" -e "/rec_cpp"
}

# The emulator proper plus its support: disc images, the HLE bios, the
# texture cache and list sorter the renderer needs, save states, and the host
# abstraction (which the sandbox replaces piece by piece - see cinterface.cpp).
core_srcs() {
	for f in emulator.cpp nullDC.cpp serialize.cpp stdclass.cpp cheats.cpp; do
		echo "$fc/core/$f"
	done
	# the settings the machine is a function of, and the logger everything
	# reports through
	find "$fc/core/cfg" "$fc/core/log" \( -name '*.cpp' -o -name '*.c' \) \
		| grep -v -e "ConsoleListenerDroid" -e "ConsoleListenerWin"
	# savestates are rzip-compressed, which is the one piece of core/archive
	# that is not about opening someone's zipped rom set
	echo "$fc/core/archive/rzip.cpp"
	find "$fc/core/imgread" "$fc/core/reios" "$fc/core/input" \
		\( -name '*.cpp' -o -name '*.c' \) | grep -v -e "dreampotato"
	# rend/: everything that is not a GPU backend. TexCache and the sorter are
	# machine-side (they decode PVR textures and sort the TA lists); norend is
	# the renderer that draws nothing, which is what M1 runs.
	# transform_matrix.cpp is here for getScaledFramebufferSize: what size the
	# picture actually is, from the tile clip and the scaler registers. It was a
	# stub until the software renderer existed to care.
	for f in TexCache.cpp texconv.cpp sorter.cpp CustomTexture.cpp transform_matrix.cpp norend/norend.cpp; do
		echo "$fc/core/rend/$f"
	done
	# The OpenGL renderer, when there is an OpenGL for it to call. Mesa's
	# softpipe is compiled into the guest, so this is not a GPU and not a
	# bridge to one: it is a second software rasteriser, and the one Flycast's
	# authors actually test against.
	# ...minus opengl_driver.cpp, which is the ImGui backend: it draws a
	# desktop application's menus, not the machine's picture.
	if [ -n "${CHIMERA_GUEST_MESA:-}" ]; then
		for f in gles.cpp gldraw.cpp gltex.cpp quad.cpp postprocess.cpp naomi2.cpp; do
			echo "$fc/core/rend/gles/$f"
		done
		echo "$fc/core/deps/glad/src/gl.c"
		# the context the renderer asks about itself - version, driver name,
		# swap interval - with the window-system half left where it belongs
		echo "$fc/core/wsi/context.cpp"
		echo "$fc/core/wsi/gl_context.cpp"
	fi

	# oslib is the host abstraction; the sandbox has no audio device, no HTTP
	# client, no translations, and no resource bundle (that one reads fonts out
	# of a zip, for a UI this core does not have).
	find "$fc/core/oslib" \( -name '*.cpp' -o -name '*.c' \) \
		| grep -v -e "audiobackend" -e "audiostream" -e "http_client" -e "i18n" \
			-e "resources"
	# common.cpp is the POSIX host abstraction. posix_vmem is NOT here: it
	# reserves the SH4's address space with shm and mmap, which a sandbox has
	# no answer for - waterbox/stubs/vmem-stub.cpp refuses instead, and the
	# machine takes Flycast's software translation path.
	echo "$fc/core/linux/common.cpp"
	# the host CPU context, which the memory system needs even when nothing
	# ever faults
	echo "$fc/core/linux/context.cpp"
}

# Vendored libraries the above reference. libchdr (with its zlib, zstd and
# lzma) reads CHD discs; xxHash hashes textures and states; md5 identifies
# them; libelf loads a naked .elf, which is how a homebrew program boots
# without a disc; xbrz is the texture upscaler the texture cache can call.
deps_srcs() {
	find "$fc/core/deps/libchdr/src" -name '*.c'
	find "$fc/core/deps/libchdr/deps/lzma-24.05/src" -name '*.c'
	find "$fc/core/deps/libchdr/deps/zlib-1.3.1" -maxdepth 1 -name '*.c' | grep -v -e "example" -e "minigzip"
	find "$fc/core/deps/libchdr/deps/zstd-1.5.6/lib/common" \
		"$fc/core/deps/libchdr/deps/zstd-1.5.6/lib/decompress" \
		"$fc/core/deps/libchdr/deps/zstd-1.5.6/lib/compress" -name '*.c'
	find "$fc/core/deps/libelf/src" -name '*.c'
	echo "$fc/core/deps/xxHash/xxhash.c"
	echo "$fc/core/deps/md5/md5.cpp"
	echo "$fc/core/deps/xbrz/xbrz.cpp"
	echo "$fc/core/deps/chdpsr/cdipsr.cpp"
}

case "${1:-main}" in
	main) { hw_srcs; core_srcs; } | sort ;;
	deps) deps_srcs | sort ;;
	*) echo "usage: sources.sh [main|deps]" >&2; exit 2 ;;
esac
