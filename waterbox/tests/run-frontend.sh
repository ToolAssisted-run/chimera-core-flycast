#!/bin/bash
# The frontend half of the gate: load the Flycast package in Chimera's window
# (run headless: no display is needed), boot a homebrew program for a fixed
# number of frames with nothing pressed, and require the machine's System RAM
# to be byte-identical to the native reference. Then prove a machine-shaping
# setting arrives (region=japan still matches ITS native reference, and the
# flash is another machine's), and that the package's keybinds become the
# machine's.
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

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/flycast.chimeraCore"
rn="$root/build/meson-native/run-native"
[ -x "$chimera_exe" ] || { echo "Chimera not built: $chimera_exe" >&2; exit 1; }
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

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"

ok=0
failed=0
skipped=0
# SKIP had no case of its own here, so it fell through to the default and
# counted as a FAILURE - which is why the arcade legs below were written to
# vanish without a word rather than say they had been skipped. Counted and
# printed now, like the core gate's.
report() { printf "%-28s %-9s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; SKIP) skipped=$((skipped+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-28s %-9s %s\n" "Check" "Result" "Detail"
printf "%-28s %-9s %s\n" "-----" "------" "------"

run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" shot="${4:-}" pkg="${5:-$package}" therom="${6:-$rom}" extra="${7:-}" system="${8:-DC}"
	local job="$work/job.$tag.txt"
	{
		echo "frames=$nframes"
		echo "system=$system"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot=$shot"
	} > "$job"
	rm -f "$work/$tag.ram.bin" "$work/$tag.meta.txt"
	[ -n "$shot" ] && rm -f "$shot"
	local project
	project="$(game_project "$tag" "$cfg" "$pkg" "$therom")" || { echo "no project made (see tests/work/project.$tag/make.log)" > "$work/$tag.log"; return 1; }
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 "$chimera_exe" --headless \
		"--config=$cfg" "--core=$pkg" $extra \
		"--lua=$here/frontend-ram.lua" "--project=$project" ) > "$work/$tag.log" 2>&1
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

# A game runs only in a project (Chimera, 2026-10-26): each run opens one made
# of its game with chimera-run --new-project, holding the settings
# settings_config was given for the config it runs with.
declare -A settings_of
settings_config() { python3 "$here/settings-config.py" "$1" "$2"; settings_of["$1"]="$2"; }
crun="$chimera_root/build/dll/chimera-run"
game_project() { # tag, config, package, game: prints the project's path
	local dir="$work/project.$1" settings="${settings_of[$2]:-}"
	local pkg="$3" game="$4"
	local fw=()
	rm -rf "$dir" && mkdir -p "$dir" && cp "$game" "$dir/"
	"$crun" --new-project "$dir/$1.chimeraProject" "$pkg" "$dir/$(basename "$game")" \
		--settings "${settings:-"{}"}" "${fw[@]}" > "$dir/make.log" 2>&1 && echo "$dir/$1.chimeraProject"
}

# The bindings the machine has (the window keeps only the keys a person
# changed; fresh settings have none, so these are the package's): <tag>
# <project> <controller>, checked against the package's default_keybinds.json
check_keys() {
	local tag="$1" project="$2" controller="$3"
	printf 'out=%s/%s.controls.json\nmeta=%s/%s.meta.txt\n' "$work" "$tag" "$work" "$tag" > "$work/job.$tag.txt"
	rm -f "$work/$tag.controls.json" "$work/$tag.meta.txt"
	( cd "$chimera_root" && MINIHAWK_JOB="$work/job.$tag.txt" timeout 900 "$chimera_exe" --headless \
		"--config=$work/settings.$tag.json" "--project=$project" \
		"--lua=$here/dump-controls.lua" ) > "$work/$tag.log" 2>&1
	if [ ! -f "$work/$tag.meta.txt" ] || ! grep -q "^status=OK" "$work/$tag.meta.txt"; then
		echo "run did not report OK (see tests/work/$tag.log)"; return 1
	fi
	python3 "$here/check-keybinds.py" "$work/$tag.controls.json" "$wb/default_keybinds.json" "$controller" 2>&1
}

# --- the machine the frontend builds must be the one the gate signed off on ---
settings_config "$work/settings.base.json" '{}'
if ! native_ram "base" ""; then
	report "disc:frontend" FAIL "native runner error (see tests/work/native.base.txt)"
elif ! run_frontend "base" "$work/settings.base.json" "$frames" "$work/base.png"; then
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
settings_config "$work/settings.jp.json" '{"region": "japan"}'
if ! native_ram "jp" '{"region":"japan"}'; then
	report "settings:region" FAIL "native runner error (see tests/work/native.jp.txt)"
elif ! run_frontend "jp" "$work/settings.jp.json" "$frames"; then
	report "settings:region" FAIL "run did not report OK (see tests/work/jp.log)"
elif ! cmp -s "$work/native.jp.ram.bin" "$work/jp.ram.bin"; then
	report "settings:region" FAIL "System RAM differs from its native reference with region=japan"
else
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

# --- an arcade board through the frontend, OFF THE RECORD (see run-gate.sh) ---
# The same machine the core gate ran, opened by the frontend as a directly
# given rom with the machine setting pinned and the bios set handed over on
# the firmware channel: its RAM must be the native reference's. Skipped
# without FLYCAST_ARCADE_ROMS, which is what CI does.
if [ -n "${FLYCAST_ARCADE_ROMS:-}" ] && [ -f "$FLYCAST_ARCADE_ROMS/naomi.zip" ]; then
	game="$(cd "$FLYCAST_ARCADE_ROMS" && ls *.zip *.dat *.bin 2>/dev/null | grep -v '^naomi\.zip$' | head -1)"
	aframes=${FLYCAST_ARCADE_FRAMES:-600}
	wd="$work/native.arcade"
	rm -rf "$wd"; mkdir -p "$wd"
	cp "$FLYCAST_ARCADE_ROMS/naomi.zip" "$FLYCAST_ARCADE_ROMS/$game" "$wd/"
	printf '{"romset":["%s"]}' "$game" > "$wd/slots"
	printf '{"machine":"naomi"}' > "$wd/settings"
	settings_config "$work/settings.arcade.json" '{"machine": "naomi"}'
	if ! "$rn" "$wd" --frames "$aframes" --dump-domain "System RAM" "$work/native.arcade.ram.bin" > "$work/native.arcade.txt" 2>&1; then
		report "arcade:frontend" FAIL "native runner error (see tests/work/native.arcade.txt)"
	elif ! frames="$aframes" run_frontend "arcade" "$work/settings.arcade.json" "$aframes" "$work/arcade.png" "$package" \
			"$FLYCAST_ARCADE_ROMS/$game" "--firmware=naomi.zip=$FLYCAST_ARCADE_ROMS/naomi.zip" NAOMI; then
		report "arcade:frontend" FAIL "no OK meta (see tests/work/arcade.log)"
	elif cmp -s "$work/native.arcade.ram.bin" "$work/arcade.ram.bin"; then
		report "arcade:frontend" PASS "$game, $aframes frames, System RAM identical to the native reference"
	else
		report "arcade:frontend" FAIL "System RAM differs from the native reference"
	fi
	# the panel's shipped bindings, adopted the way the Dreamcast's are below
	if out="$(check_keys "arcadekeys" "$work/project.arcade/arcade.chimeraProject" "Arcade Panel")"; then
		report "arcade:keybinds" PASS "$out"
	else
		report "arcade:keybinds" FAIL "$out"
	fi
else
	report "arcade:frontend" SKIP "set FLYCAST_ARCADE_ROMS to a folder with naomi.zip and a game: would prove a board opened by the frontend has the native reference's RAM"
	report "arcade:keybinds" SKIP "same content: would prove the Arcade Panel's shipped bindings are the machine's"
fi

# --- the bindings the package ships are the machine's when nobody changed them
# (the base run's project is the machine; its settings are the run's own)
if out="$(check_keys "keys" "$work/project.base/base.chimeraProject" "Dreamcast Controller")"; then
	report "keybinds" PASS "$out"
else
	report "keybinds" FAIL "$out"
fi

echo
echo "$ok ok, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
