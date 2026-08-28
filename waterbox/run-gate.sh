#!/bin/bash
# The core-level equivalence gate: the sandboxed core must produce
# byte-identical video, audio, lag and memory-domain digests to the native
# reference build (the same sources compiled natively), and must survive a
# whole-machine savestate round-trip around every frame.
#
# SCOPE. It runs this repository's own SH4 programs rather than a game, because
# a Dreamcast game is somebody's copyrighted disc. Between them they exercise
# what a movie depends on: hundreds of millions of SH4 instructions, the AICA,
# the timers, the memory system, the maple bus carrying a frontend's input, and
# - since the software renderer landed - the PVR actually drawing.
#
# Usage: ./run-gate.sh [-n <native build dir>] [-g <guest build dir>]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
nat="$root/build/meson-native"
gst="$root/build/meson-guest"
while getopts "n:g:" opt; do
	case "$opt" in
		n) nat="$OPTARG" ;;
		g) gst="$OPTARG" ;;
		*) exit 2 ;;
	esac
done

[ -x "$nat/run-native" ] && [ -x "$nat/run-wbx" ] || {
	echo "native build missing: meson setup build/meson-native && ninja -C build/meson-native" >&2; exit 1; }
[ -f "$gst/core.wbx" ] || {
	echo "guest build missing: sh waterbox/setup-guest.sh && ninja -C build/meson-guest core.wbx" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
digests() { grep -E '^(frames|vsync|videoHash|audioHash|lagFrames|domain\[)'; }

ok=0
failed=0
report() { printf "%-30s %-6s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-30s %-6s %s\n" "Check" "Result" "Detail"
printf "%-30s %-6s %s\n" "-----" "------" "------"

# name program frames
tests=(
	"counter counter.elf 300"
	"counterShort counter.elf 60"
	"padread padread.elf 120"
	"triangle triangle.elf 30"
)

for t in "${tests[@]}"; do
	read -r name prog frames <<< "$t"

	wd="$work/$name"
	mkdir -p "$wd"
	cp "$root/tests/roms/$prog" "$wd/" || { report "$name:equivalence" FAIL "no program $prog"; continue; }
	printf '{"disc":["%s"]}' "$prog" > "$wd/slots"
	printf '{}' > "$wd/settings"

	if ! "$nat/run-native" "$wd" --frames "$frames" 2>"$work/nat.err" | digests > "$work/nat.txt"; then
		report "$name:equivalence" FAIL "native runner error: $(head -1 "$work/nat.err")"; continue
	fi
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" --frames "$frames" 2>"$work/box.err" | digests > "$work/box.txt"; then
		report "$name:equivalence" FAIL "waterbox runner error: $(head -1 "$work/box.err")"; continue
	fi
	if cmp -s "$work/nat.txt" "$work/box.txt"; then
		report "$name:equivalence" PASS "$frames frames, native == waterboxed"
	else
		report "$name:equivalence" FAIL "$(diff "$work/nat.txt" "$work/box.txt" | tr '\n' ' ' | head -c 120)"
		continue
	fi

	# A hollow pass cannot sneak through: the machine must actually have
	# EXECUTED something. The program counts instructions into RAM, so a
	# shorter run must reach a different state than a longer one.
	if ! "$nat/run-native" "$wd" --frames $((frames / 2)) 2>/dev/null | digests > "$work/half.txt"; then
		report "$name:ran" FAIL "half-length native run failed"
	elif cmp -s "$work/nat.txt" "$work/half.txt"; then
		report "$name:ran" FAIL "half as many frames left the machine in the same state"
	else
		report "$name:ran" PASS "the SH4 executed: $frames frames differ from $((frames / 2))"
	fi

	# The sandbox snapshots the whole guest, so a savestate here is the whole
	# machine by construction; what this checks is that taking one every frame
	# and restoring it changes nothing.
	if ! "$nat/run-wbx" "$gst/core.wbx" "$wd" --frames "$frames" --rerecord 2>/dev/null | digests > "$work/rr.txt"; then
		report "$name:savestate" FAIL "rerecord run failed"
	elif cmp -s "$work/box.txt" "$work/rr.txt"; then
		report "$name:savestate" PASS "per-frame round-trip is lossless"
	else
		report "$name:savestate" FAIL "$(diff "$work/box.txt" "$work/rr.txt" | tr '\n' ' ' | head -c 120)"
	fi
done

# ---- input, through the maple bus -----------------------------------------
# padread.elf builds a maple frame, starts the DMA and sums the controller's
# answer into RAM every iteration, so a different input schedule MUST leave a
# different machine. This is what separates "the frontend sets a variable" from
# "the machine read its controller": the first attempt at this core wrote the
# desktop input layer's kcode[] instead of mapleInputState[], which links, runs,
# and changes nothing at all.
wd="$work/padread"
if [ -d "$wd" ]; then
	idle="$("$nat/run-native" "$wd" --frames 120 2>/dev/null | digests)"
	held="$("$nat/run-native" "$wd" --frames 120 --exercise 2>/dev/null | digests)"
	boxheld="$("$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 120 --exercise 2>/dev/null | digests)"
	if [ "$idle" = "$held" ]; then
		report "input:shaped" FAIL "input made no difference to the machine"
	elif [ "$held" != "$boxheld" ]; then
		report "input:shaped" FAIL "native and sandbox disagree with input held"
	else
		report "input:shaped" PASS "the machine read its pad: idle != held, native == waterboxed"
	fi

	# Lag detection is the frontend's question "did this frame look at the
	# input", answered by patches/0002 where maple serves a controller read.
	lag_pad="$("$nat/run-native" "$wd" --frames 30 2>/dev/null | sed -n 's/^lagFrames=//p')"
	lag_cnt="$("$nat/run-native" "$work/counter" --frames 30 2>/dev/null | sed -n 's/^lagFrames=//p')"
	if [ "$lag_pad" = "0" ] && [ "$lag_cnt" = "30" ]; then
		report "input:lag" PASS "polling reports 0 lag frames, not polling reports 30"
	else
		report "input:lag" FAIL "pad=$lag_pad counter=$lag_cnt (want 0 and 30)"
	fi

	# Every domain must be present, non-empty and hashed. VRAM and sound RAM
	# matter to a movie's watch window as much as system RAM does.
	doms="$("$nat/run-native" "$wd" --frames 5 2>/dev/null | grep -c '^domain\[')"
	if [ "$doms" = "4" ]; then
		report "domains" PASS "System RAM, VRAM, Sound RAM and Flash exposed"
	else
		report "domains" FAIL "$doms domains, want 4"
	fi
fi

# ---- the picture ----------------------------------------------------------
# triangle.elf submits one polygon to the TA through the store queues and
# triggers a render, so the frame it produces is a large flat shape rather than
# the blank one every other program leaves. Both halves of this matter: a
# renderer that draws nothing passes an equivalence test perfectly.
wd="$work/triangle"
if [ -d "$wd" ]; then
	blank="$("$nat/run-native" "$work/counter" --frames 5 2>/dev/null | sed -n 's/^videoHash=//p')"
	drawn="$("$nat/run-native" "$wd" --frames 5 2>/dev/null | sed -n 's/^videoHash=//p')"
	boxdrawn="$("$nat/run-wbx" "$gst/core.wbx" "$wd" --frames 5 2>/dev/null | sed -n 's/^videoHash=//p')"
	if [ "$blank" = "$drawn" ]; then
		report "render:drew" FAIL "a rendered frame hashes the same as a blank one"
	elif [ "$drawn" != "$boxdrawn" ]; then
		report "render:drew" FAIL "native and sandbox drew different pictures"
	else
		report "render:drew" PASS "the PVR drew, and the sandbox drew the same"
	fi

	# What was drawn, not just that something was: the triangle covers a known
	# span of a known colour, so a renderer that fills the screen or draws the
	# wrong shape is caught rather than congratulated.
	shot="$work/triangle.tga"
	"$nat/run-native" "$wd" --frames 5 --screenshot "$shot" >/dev/null 2>&1
	shape="$(python3 - "$shot" <<'PYSHAPE'
import struct, sys
d = open(sys.argv[1], "rb").read()
w, h = struct.unpack("<HH", d[12:16])
px = d[18:]
def span(y):
    xs = [x for x in range(w) if px[(y * w + x) * 4:(y * w + x) * 4 + 3].hex() == "ff0000"]
    return (min(xs), max(xs)) if xs else None
top, lower = span(100), span(200)
red = sum(1 for i in range(0, w * h * 4, 4) if px[i:i + 3].hex() == "ff0000")
blue = sum(1 for i in range(0, w * h * 4, 4) if px[i:i + 3].hex() == "0000ff")
# A red triangle on a blue background, at 640x480. Every number here is one the
# program asked for, so a renderer that draws the wrong thing is caught rather
# than congratulated:
#   the base spans x=100..499 at y=100 and narrows below it (the triangle);
#   its area is 400*300/2 (exactly - the rasteriser fills what it should);
#   everything else is the BACKGROUND PLANE, which is drawn from parameters in
#     video memory rather than from the display list;
#   and the picture is 480 lines, which needs FB_R_CTRL's video clock set.
ok = ((w, h) == (640, 480)
      and top == (100, 499) and lower is not None
      and (lower[1] - lower[0]) < (top[1] - top[0])
      and red == 60000
      and blue == w * h - red)
print("ok" if ok else f"bad size=({w},{h}) top={top} lower={lower} red={red} blue={blue}")
PYSHAPE
)"
	if [ "$shape" = "ok" ]; then
		report "render:shape" PASS "640x480: a red triangle of exactly 60000 pixels on a blue background plane"
	else
		report "render:shape" FAIL "$shape"
	fi
fi

# ---- the disc --------------------------------------------------------------
# A GD-ROM this repository builds from scratch (tests/make-testdisc.py): three
# tracks, an ISO9660 filesystem at LBA 45000, an IP.BIN bootstrap naming
# 1ST_READ.BIN. Booting it exercises the path a real game takes - the disc
# reader, the drive, the HLE bios locating the bootfile - none of which the ELF
# shortcut touches.
discdir="$work/disc"
if python3 "$root/tests/make-testdisc.py" "$discdir" "$root/tests/roms/counter.bin" >/dev/null 2>&1; then
	printf '{"disc":["gate.gdi"]}' > "$discdir/slots"
	printf '{}' > "$discdir/settings"

	if ! "$nat/run-native" "$discdir" --frames 30 --dump-domain "System RAM" "$work/disc.ram" 2>"$work/disc.err" | digests > "$work/disc.nat"; then
		report "disc:boots" FAIL "native runner error: $(head -1 "$work/disc.err")"
	elif ! python3 -c "
import struct, sys
d = open('$work/disc.ram','rb').read()
# the program the disc holds counts into 0x8C011000, and the bios loads it to
# 0x8C010000: both must be true, or something else was running
counter = struct.unpack('<I', d[0x11000:0x11004])[0]
loaded = d[0x10000:0x10002] == bytes.fromhex('03d0')
sys.exit(0 if counter > 1000 and loaded else 1)
"; then
		report "disc:boots" FAIL "the disc's program did not run"
	else
		report "disc:boots" PASS "booted 1ST_READ.BIN from a GD-ROM filesystem"
	fi

	if "$nat/run-wbx" "$gst/core.wbx" "$discdir" --frames 30 2>/dev/null | digests > "$work/disc.box" \
		&& cmp -s "$work/disc.nat" "$work/disc.box"; then
		report "disc:equivalence" PASS "30 frames from the disc, native == waterboxed"
	else
		report "disc:equivalence" FAIL "$(diff "$work/disc.nat" "$work/disc.box" 2>/dev/null | tr '\n' ' ' | head -c 100)"
	fi

	# ---- the memory cards --------------------------------------------------
	# A Dreamcast saves to a VMU, which Flycast keeps in a file next to itself.
	# A sandbox has nowhere to put one, so patches/0002 hands the flash to the
	# host and it leaves through the save-data channel - formatted, because a
	# blank 128KB of flash is not a memory card a game can write to.
	sd="$work/savedata"
	mkdir -p "$sd"
	"$nat/run-native" "$discdir" --frames 10 --savedata-out "$sd" >/dev/null 2>&1
	if python3 -c "
import sys, glob
files = sorted(glob.glob('$sd/vmu_*.bin'))
if not files:
    sys.exit(1)
for path in files:
    d = open(path,'rb').read()
    if len(d) != 128 * 1024 or d[0x1FE00:0x1FE10] != bytes([0x55]) * 16:
        sys.exit(1)
sys.exit(0)
"; then
		report "savedata:vmu" PASS "$(ls "$sd" | wc -l) formatted memory cards exported"
	else
		report "savedata:vmu" FAIL "no formatted VMU came out of the save-data channel"
	fi

	# ...and back in. A project supplies save data by mounting it under the name
	# the export wrote, so a card marked with bytes the machine could not have
	# written must reach the machine and come back carrying them. Without this,
	# the import side is a slot declaration nobody proved.
	seed="$work/vmu-seed"
	back="$work/vmu-back"
	mkdir -p "$seed" "$back"
	cp "$wd"/* "$seed/" 2>/dev/null
	if [ -f "$sd/vmu_A1.bin" ] && python3 - "$sd/vmu_A1.bin" "$seed/vmu_A1.bin" <<'PYSEED'
import sys
d = bytearray(open(sys.argv[1], 'rb').read())
d[0x40:0x50] = b'CHIMERA-SEED-TST'
open(sys.argv[2], 'wb').write(bytes(d))
PYSEED
	then
		"$nat/run-native" "$seed" --frames 30 --savedata-out "$back" >/dev/null 2>&1
		if [ ! -f "$back/vmu_A1.bin" ]; then
			report "savedata:seeded" FAIL "nothing came back with a card mounted"
		elif ! cmp -s "$seed/vmu_A1.bin" "$back/vmu_A1.bin"; then
			report "savedata:seeded" FAIL "the mounted card is not what came back"
		else
			report "savedata:seeded" PASS "a card the project supplied reached the machine and returned unchanged"
		fi
	else
		report "savedata:seeded" FAIL "could not make a marked card to mount"
	fi
fi

# The Stella lesson: the native reference is the only place a real clock and
# real threads still tick, so it is where nondeterminism shows up. Two runs of
# the same program must agree.
wd="$work/counter"
if [ -d "$wd" ]; then
	a="$("$nat/run-native" "$wd" --frames 120 2>/dev/null | digests)"
	b="$("$nat/run-native" "$wd" --frames 120 2>/dev/null | digests)"
	if [ "$a" = "$b" ]; then
		report "native:determinism" PASS "two native runs agree"
	else
		report "native:determinism" FAIL "the native reference wanders between runs"
	fi
fi

echo
echo "$ok ok, $failed failed"
[ "$failed" -eq 0 ]
