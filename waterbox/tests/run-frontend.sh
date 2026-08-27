#!/bin/bash
# The frontend half of the gate: load the Flycast package in Chimera (under
# Mono, on a private Xvfb display), boot a homebrew cartridge for a fixed
# number of frames with nothing pressed, and require the machine's whole RAM -
# all 128 bytes of it - to be byte-identical to the native reference. Then
# prove a machine-shaping setting arrives (format=PAL builds a 50Hz machine
# that still matches ITS native reference), and that the package's keybinds
# become the frontend's defaults.
#
# Usage: ./run-frontend.sh [--chimera-root <path>] [--frames N]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
wb="$(cd "$here/.." && pwd)"
root="$(cd "$wb/.." && pwd)"
frames=300
chimera_root=""
while [ $# -gt 0 ]; do
	case "$1" in
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done

if [ -z "$chimera_root" ]; then
	for candidate in "$root/../chimera" "$HOME/chimera"; do
		[ -d "$candidate" ] && { chimera_root="$candidate"; break; }
	done
fi
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || {
	echo "chimera checkout not found; pass --chimera-root <path>" >&2; exit 1; }
chimera_root="$(cd "$chimera_root" && pwd)"

emu_exe="$chimera_root/build/Chimera.exe"
package="$chimera_root/build/Cores/flycast.chimeraCore"
rn="$root/build/meson-native/run-native"
[ -f "$emu_exe" ] || { echo "Chimera not built: $emu_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "package not installed: $package (run ../build-package.sh)" >&2; exit 1; }
[ -x "$rn" ] || { echo "native reference not built" >&2; exit 1; }

work="$here/work"
mkdir -p "$work"

# What the frontend boots here is the gate's own program as a naked ELF, not
# the GD-ROM the core gate uses. A .gdi names its tracks in sibling files, and
# a project (or a command line) names ONE file - so a multi-track disc is a
# project with a directory behind it, which is a wizard question rather than
# something this leg can answer. The ELF exercises the same path from the
# frontend's side: mount, boot, run, read memory.
rom="$root/tests/roms/padread.elf"

export LD_LIBRARY_PATH="$chimera_root/build/dll:$chimera_root/build:/usr/lib/x86_64-linux-gnu"
export MONO_CRASH_NOFILE=1 MONO_WINFORMS_XIM_STYLE=disabled ALSOFT_DRIVERS=null
xvfb_pid=""
cleanup() { [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null; }
trap cleanup EXIT
if [ -z "${DISPLAY:-}" ]; then
	command -v Xvfb >/dev/null || { echo "Xvfb not found (apt install xvfb)" >&2; exit 1; }
	for n in 90 91 92 93 94 95 96; do
		if [ ! -e "/tmp/.X11-unix/X$n" ]; then
			Xvfb ":$n" -screen 0 640x480x24 -nolisten tcp & xvfb_pid=$!
			export DISPLAY=":$n"; break
		fi
	done
	sleep 1
fi

config="$work/config.ini"
if [ ! -f "$config" ]; then
	( cd "$chimera_root" && timeout 120 mono "$emu_exe" --headless "--config=$config" \
		"--lua=$here/exit.lua" ) > "$work/bootstrap.log" 2>&1
	[ -f "$config" ] || { echo "config bootstrap failed (see $work/bootstrap.log)" >&2; exit 1; }
fi
sed -i 's/"DispMethod": [0-9]/"DispMethod": 1/' "$config"

ok=0
failed=0
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}" pkg="${5:-$package}" therom="${6:-$rom}"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 mono "$emu_exe" --headless \
		"--config=$cfg" "--core=$pkg" \
		"--lua=$here/frontend-ram.lua" "$therom" ) > "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}

# a native reference run: same rom, same idle schedule, settings JSON in $2
native_ram() {
	local tag="$1" settings="$2"
	local wd="$work/native.$tag"
	rm -rf "$wd"
	mkdir -p "$wd"
	cp "$rom" "$wd/"
	printf '{"disc":["%s"]}' "$(basename "$rom")" > "$wd/slots"
	[ -n "$settings" ] && printf '%s' "$settings" > "$wd/settings"
	"$rn" "$wd" --frames "$frames" --dump-domain "System RAM" "$work/native.$tag.ram.bin" \
		> "$work/native.$tag.txt" 2>&1
}

settings_config() { python3 "$here/settings-config.py" "$config" "$1" "$2"; }

# --- the machine the frontend builds must be the one the gate signed off on ---
settings_config "$work/config.base.ini" '{}'
if ! native_ram "base" ""; then
	report "disc:frontend" FAIL "native runner error (see tests/work/native.base.txt)"
elif ! run_frontend "base" "$work/config.base.ini" "$frames" "$work/base.png"; then
	report "disc:frontend" FAIL "no OK meta (see tests/work/base.log)"
elif cmp -s "$work/native.base.ram.bin" "$work/base.ram.bin"; then
	report "disc:frontend" PASS "$frames frames, System RAM identical to the native reference"
else
	report "disc:frontend" FAIL "System RAM differs from the native reference"
fi

# --- a machine-shaping setting must reach the guest through the frontend ---
# The region is a Dreamcast's, and it lives in the machine's FLASH rather than
# in RAM: a game reads it to decide what it is running on. So this leg compares
# the flash the frontend's machine ended up with against the native one's.
settings_config "$work/config.jp.ini" '{"region": "japan"}'
if ! native_ram "jp" '{"region":"japan"}'; then
	report "settings:region" FAIL "native runner error (see tests/work/native.jp.txt)"
elif ! run_frontend "jp" "$work/config.jp.ini" "$frames"; then
	report "settings:region" FAIL "run did not report OK (see tests/work/jp.log)"
elif ! cmp -s "$work/native.jp.ram.bin" "$work/jp.ram.bin"; then
	report "settings:region" FAIL "System RAM differs from its native reference with region=japan"
else
	# RAM alone cannot show this: a rom whose memory does not depend on the
	# television still draws a TALLER PICTURE on a PAL machine, and that is
	# the machine having changed.
	# RAM alone cannot show this: the region lives in flash, so the two
	# machines must differ THERE and nowhere else.
	base_flash="$(sed -n 's/^flashhash=//p' "$work/base.meta.txt")"
	jp_flash="$(sed -n 's/^flashhash=//p' "$work/jp.meta.txt")"
	if [ -n "$base_flash" ] && [ -n "$jp_flash" ] && [ "$base_flash" != "$jp_flash" ]; then
		report "settings:region" PASS "region=japan matches its native reference and changes the machine's flash"
	else
		report "settings:region" FAIL "region=japan left the flash unchanged ($base_flash)"
	fi
fi

# --- the bindings the package ships must become the frontend's defaults ---
python3 "$here/forget-controller.py" "$work/config.base.ini" "$work/config.keys.ini" "Dreamcast Controller"
if run_frontend "keys" "$work/config.keys.ini" 1; then
	if python3 "$here/check-keybinds.py" "$work/config.keys.ini" \
		"$wb/default_keybinds.json" "Dreamcast Controller" > "$work/keys.txt" 2>&1; then
		report "keybinds" PASS "$(cat "$work/keys.txt")"
	else
		report "keybinds" FAIL "$(head -1 "$work/keys.txt")"
	fi
else
	report "keybinds" FAIL "run did not report OK (see tests/work/keys.log)"
fi

echo
echo "$ok ok, $failed failed"
[ "$failed" -eq 0 ]
