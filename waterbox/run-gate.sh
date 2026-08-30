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
digests() { grep -E '^(frames|vsync|videoHash|audioHash|audioFrames|lagFrames|domain\[)'; }
# What a turbo run can be held to: everything except the whole-run video hash,
# which a run that skipped the first half cannot possibly match - the second
# half it did draw is compared instead.
turboDigests() { grep -E '^(frames|vsync|tailVideoHash|audioHash|audioFrames|lagFrames|domain\[)'; }

ok=0
failed=0
# SKIP counts as neither: a check that does not apply to this program is not a
# pass to brag about and not a failure to fix.
report() { printf "%-30s %-6s %s\n" "$1" "$2" "$3"; case "$2" in PASS) ok=$((ok+1)) ;; SKIP) ;; *) failed=$((failed+1)) ;; esac; }
printf "%-30s %-6s %s\n" "Check" "Result" "Detail"
printf "%-30s %-6s %s\n" "-----" "------" "------"

# name program frames [noturbo]
#
# noturbo: the program draws ONE frame and then spins forever (see
# tests/make-testprog.py). Turbo skips the drawing of the first half of a run,
# and for this one that is the only render there will ever be - so the picture
# stays black and there is nothing in the second half to compare it against.
# That is turbo doing exactly what it says; the other three redraw continuously
# and are where the leg earns its keep.
tests=(
	"counter counter.elf 300"
	"counterShort counter.elf 60"
	"padread padread.elf 120"
	"triangle triangle.elf 30 noturbo"
)

for t in "${tests[@]}"; do
	read -r name prog frames noturbo <<< "$t"

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
	# THE MACHINE MUST MAKE SOUND. Not "the right sound" - there is no reference
	# for that here - but that the AICA's samples reach the ABI at all. This
	# core shipped silent for its whole life because WriteSample was a stub, and
	# nothing noticed: an empty buffer hashes consistently and matches its own
	# reference perfectly. A sample count is the one thing that does not.
	produced="$(sed -n 's/^audioFrames=//p' "$work/box.txt")"
	if [ -z "$produced" ] || [ "$produced" -eq 0 ]; then
		report "$name:audio" FAIL "the machine produced no samples at all"
	else
		report "$name:audio" PASS "$produced sample pairs over $frames frames ($((produced / frames)) per frame at 44.1kHz)"
	fi

	# ...and every frame must be the SAME LENGTH as every other. The total above
	# is blind to the distribution, and the distribution is what a frontend
	# actually depends on: it shows frames at a fixed rate and plays their sound
	# at a fixed rate, so a frame carrying twice the samples of its neighbour is
	# heard as the pitch and the speed moving. Flycast ended its frame when the
	# GAME PRESENTED, which a game does when it likes - Street Fighter Zero 3
	# produced 737 sample pairs on some frames, 1474 on others and 2212 on the
	# rest, all in one fight. The boundary is the video hardware's vblank now
	# (patches/0011), which ticks whatever the game does.
	#
	# Held as "steady", not as "737": these programs set their own video modes
	# and one of them runs its display at twice the usual rate. What must not
	# happen is frames of DIFFERENT lengths within one run. Frame 0 is exempt -
	# it runs from reset to the first vblank, which is not a whole field.
	"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames "$frames" --audio-trace "$work/$name.trace" >/dev/null 2>&1
	steady="$(python3 - "$work/$name.trace" <<'PYSTEADY'
import sys
n = [int(l.split()[1]) for l in open(sys.argv[1])][1:]
if not n:
    print("no trace"); sys.exit()
lo, hi = min(n), max(n)
# one pair of slack: 44100 samples do not divide evenly into a field, so a
# steady machine still alternates between two adjacent counts
span = "%d" % lo if lo == hi else "%d or %d" % (lo, hi)
print("ok " + span if hi - lo <= 1 else "%d..%d" % (lo, hi))
PYSTEADY
)"
	case "$steady" in
		ok*) report "$name:audioSteady" PASS "every frame carries ${steady#ok } sample pairs, one field's worth" ;;
		*)   report "$name:audioSteady" FAIL "frame lengths wander over $steady sample pairs" ;;
	esac

	if [ "${noturbo:-}" = "noturbo" ]; then
		report "$name:turbo" SKIP "this program draws one frame and stops"
	else
	# Turbo: the core's drawing switched off for the first half of the run and
	# back on for the second. The machine, the sound, the lag count and every
	# picture of that second half must be what they would have been.
	"$nat/run-wbx" "$gst/core.wbx" "$wd" --frames "$frames" 2>/dev/null | turboDigests > "$work/tnorm.txt"
	if "$nat/run-wbx" "$gst/core.wbx" "$wd" --frames "$frames" --turbo 2>/dev/null | turboDigests > "$work/turbo.txt"; then
		if cmp -s "$work/tnorm.txt" "$work/turbo.txt"; then
			report "$name:turbo" PASS "$frames frames, half of them undrawn, same machine and same pictures"
		else
			report "$name:turbo" FAIL "$(diff "$work/tnorm.txt" "$work/turbo.txt" | tr '\n' ' ' | head -c 120)"
		fi
	else
		report "$name:turbo" FAIL "turbo runner error"
	fi
	fi

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

	# ---- the triggers -------------------------------------------------------
	# A Dreamcast's L and R are ANALOG, and padread.elf's answer word carries
	# them beside the buttons: bits 0..15 are the buttons, 16..23 the right
	# trigger, 24..31 the left. So the exact byte the machine received can be
	# read out of RAM and held against the exact value the frontend sent, which
	# is the only form of this check worth having - "something changed" would
	# have passed throughout the bug this leg exists for.
	#
	# That bug: mapleInputState::halfAxes is the whole 16-bit range and
	# maple_cfg.cpp shifts it down by 8 itself. The core handed it a value
	# already reduced to 0..255, so EVERY trigger read 0, fully pressed
	# included, and Unreal Tournament could not fire (github #10). The fullAxes
	# lines next to it do want the reduced range, which is why the two looked
	# alike.
	trigread() { # <axis> <value> <runner...>; echoes "R=xx L=xx"
		axis="$1"; value="$2"; shift 2
		"$@" "$wd" --frames 30 --hold-axis "$axis" "$value" \
			--dump-domain "System RAM" "$work/trig.ram" >/dev/null 2>&1 || { echo "runner failed"; return; }
		python3 -c "
import struct
w = struct.unpack('<I', open('$work/trig.ram','rb').read()[0x11004:0x11008])[0]
print('R=%02x L=%02x' % ((w >> 16) & 0xff, (w >> 24) & 0xff))
"
	}
	# axis 2 is Left Trigger and axis 3 Right Trigger, in waterbox.config order
	tl_off="$(trigread 2 -32768 "$nat/run-native")"
	tl_on="$(trigread 2 32767 "$nat/run-native")"
	tr_on="$(trigread 3 32767 "$nat/run-native")"
	tl_box="$(trigread 2 32767 "$nat/run-wbx" "$gst/core.wbx")"
	if [ "$tl_off" != "R=00 L=00" ]; then
		report "input:triggers" FAIL "released, the machine reads $tl_off"
	elif [ "$tl_on" != "R=00 L=ff" ]; then
		report "input:triggers" FAIL "left trigger held, the machine reads $tl_on"
	elif [ "$tr_on" != "R=ff L=00" ]; then
		report "input:triggers" FAIL "right trigger held, the machine reads $tr_on"
	elif [ "$tl_box" != "$tl_on" ]; then
		report "input:triggers" FAIL "native reads $tl_on, sandbox reads $tl_box"
	else
		report "input:triggers" PASS "each trigger reaches the machine as 00 released and ff held, and only its own"
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
		report "savedata:vmu" PASS "$(ls "$sd" | wc -l) formatted memory card(s) exported, one per connected controller"
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

# ---- the 240p Test Suite ----------------------------------------------------
# Artemio Urbina's test suite (tests/own/240pSuite, GPLv2, redistributable - see
# tests/own/README.md). It is homebrew written to be run on REAL Dreamcasts to
# check that hardware behaves, which makes it the one thing in this repository
# that can say "a real program agrees" rather than "we agree with ourselves".
#
# Its Controller Test draws the pad's own readouts, so the trigger check above
# gets a second, independent witness: this one reads the number the MACHINE
# printed on screen, through a program nobody here wrote, instead of a word this
# repository's own assembler put in RAM. The two boxes are the L and R analog
# readouts, found by diffing the screen with a trigger held; nothing else on the
# screen moves, which is itself part of the claim.
suite="$root/tests/own/240pSuite/240pSuite.cdi"
if [ -f "$suite" ]; then
	sd240="$work/suite240p"
	mkdir -p "$sd240"
	cp "$suite" "$sd240/240pSuite.cdi"
	printf '{"disc":["240pSuite.cdi"]}' > "$sd240/slots"
	printf '{}' > "$sd240/settings"

	# down x3 to Hardware Tests, A, down to Controller Test, A. Wire indices are
	# waterbox.config's order, player 1: 1 is Down and 4 is A.
	nav240="--press 420:6:1 --press 450:6:1 --press 480:6:1 --press 520:6:4 --press 580:6:1 --press 630:6:4"
	# <out.tga> <extra harness args> <runner> [the runner's own leading args].
	# The work directory is POSITIONAL and comes first for both runners, so it
	# cannot simply be appended after a caller's options.
	shoot240() {
		out="$1"; extra="$2"; shift 2
		runner="$1"; shift
		"$runner" "$@" "$sd240" --frames 700 $nav240 $extra --screenshot "$out" >/dev/null 2>&1
	}
	shoot240 "$work/s240.rest.tga"  ""                     "$nat/run-native"
	shoot240 "$work/s240.left.tga"  "--hold-axis 2 32767"  "$nat/run-native"
	shoot240 "$work/s240.right.tga" "--hold-axis 3 32767"  "$nat/run-native"
	shoot240 "$work/s240.box.tga"   "--hold-axis 2 32767"  "$nat/run-wbx" "$gst/core.wbx"

	verdict="$(python3 - "$work" <<'PY240'
import struct, sys
work = sys.argv[1]

def load(name):
    d = open(f"{work}/s240.{name}.tga", "rb").read()
    w, h = struct.unpack("<HH", d[12:16])
    return w, h, d[18:]

def region(px, w, box):
    x0, y0, x1, y1 = box
    return b"".join(px[(y * w + x) * 4:(y * w + x) * 4 + 3]
                    for y in range(y0, y1) for x in range(x0, x1))

# the two readouts, with a little room around them
LBOX = (54, 44, 72, 52)
RBOX = (84, 44, 102, 52)
try:
    w, h, rest = load("rest")
    _, _, left = load("left")
    _, _, right = load("right")
    _, _, box = load("box")
except Exception as e:
    print(f"could not read the screenshots: {e}"); sys.exit()

if (w, h) != (320, 240):
    print(f"the suite drew {w}x{h}, not 320x240"); sys.exit()

restL, restR = region(rest, w, LBOX), region(rest, w, RBOX)
leftL, leftR = region(left, w, LBOX), region(left, w, RBOX)
rghtL, rghtR = region(right, w, LBOX), region(right, w, RBOX)

if restL != restR:
    print("at rest the two readouts already differ")
elif leftL == restL:
    print("holding the left trigger changed nothing the suite printed")
elif leftR != restR:
    print("holding the left trigger moved the RIGHT readout")
elif rghtR == restR:
    print("holding the right trigger changed nothing the suite printed")
elif rghtL != restL:
    print("holding the right trigger moved the LEFT readout")
elif leftL != rghtR:
    # both are fully pressed, so both must print the same number
    print("the two triggers do not reach the same value")
elif region(box, w, LBOX) != leftL:
    print("native and sandbox print different numbers")
else:
    print("ok")
PY240
)"
	if [ "$verdict" = "ok" ]; then
		report "suite240p:triggers" PASS "the 240p Test Suite's own controller readout follows each trigger, and only its own"
	else
		report "suite240p:triggers" FAIL "$verdict"
	fi

	# ---- four ports --------------------------------------------------------
	# A Dreamcast has four, and the Controller Test draws one quadrant per port:
	# A0 top left, B0 top right, C0 bottom left, D0 bottom right. With four
	# gamepads it names a device in all four - and holding A on ONE of them must
	# light that quadrant's A and no other, which is the whole claim. A port
	# nothing is plugged into cannot light anything.
	#
	# This is the check that would have caught the first attempt, where the
	# ports were assigned AFTER loadGame had already built the maple devices:
	# the settings said four gamepads and the machine had one.
	p4="$work/suite240p-4"
	mkdir -p "$p4"
	cp "$suite" "$p4/240pSuite.cdi"
	printf '{"disc":["240pSuite.cdi"]}' > "$p4/slots"
	printf '{"port1":"gamepad","port2":"gamepad","port3":"gamepad","port4":"gamepad"}' > "$p4/settings"

	"$nat/run-native" "$p4" --frames 700 $nav240 --screenshot "$work/p4.rest.tga" >/dev/null 2>&1
	for port in 0 1 2 3; do
		# wire 4 of each port is A; a port's block is twenty wires wide
		"$nat/run-native" "$p4" --frames 700 $nav240 --press "665:35:$((port * 20 + 4))" \
			--screenshot "$work/p4.$port.tga" >/dev/null 2>&1
	done
	"$nat/run-wbx" "$gst/core.wbx" "$p4" --frames 700 $nav240 --press 665:35:44 \
		--screenshot "$work/p4.box.tga" >/dev/null 2>&1

	verdict="$(python3 - "$work" <<'PYPORTS'
import struct, sys
work = sys.argv[1]

def load(name):
    d = open(f"{work}/p4.{name}.tga", "rb").read()
    w, h = struct.unpack("<HH", d[12:16])
    return w, h, d[18:]

def region(px, w, box):
    x0, y0, x1, y1 = box
    return b"".join(px[(y * w + x) * 4:(y * w + x) * 4 + 3]
                    for y in range(y0, y1) for x in range(x0, x1))

# where each port's A indicator is drawn, found by holding it and diffing
BOXES = [(89, 70, 97, 80), (249, 70, 257, 80),
         (89, 160, 97, 170), (249, 160, 257, 170)]
try:
    w, h, rest = load("rest")
    held = [load(str(p))[2] for p in range(4)]
    box = load("box")[2]
except Exception as e:
    print(f"could not read the screenshots: {e}"); sys.exit()

if (w, h) != (320, 240):
    print(f"the suite drew {w}x{h}, not 320x240"); sys.exit()

for p in range(4):
    for q in range(4):
        before, after = region(rest, w, BOXES[q]), region(held[p], w, BOXES[q])
        if p == q and before == after:
            print(f"holding A on port {p + 1} lit nothing there"); sys.exit()
        if p != q and before != after:
            print(f"holding A on port {p + 1} moved port {q + 1}"); sys.exit()
if region(box, w, BOXES[2]) != region(held[2], w, BOXES[2]):
    print("native and sandbox disagree about port 3"); sys.exit()
print("ok")
PYPORTS
)"
	if [ "$verdict" = "ok" ]; then
		report "suite240p:ports" PASS "four controllers, and each one's A lights its own quadrant and no other"
	else
		report "suite240p:ports" FAIL "$verdict"
	fi

	# ---- a port's setting is a DIFFERENT DEVICE, not a relabelled one -------
	# Four gamepads prove the ports are wired; they do not prove the setting
	# picks anything. This does: a twin stick has a SECOND D-PAD and a retail
	# controller does not, so the same wire held on the same port either reaches
	# the machine or does not exist, depending only on what the project said was
	# plugged in.
	pts="$work/suite240p-ts"
	mkdir -p "$pts"
	cp "$suite" "$pts/240pSuite.cdi"
	printf '{"disc":["240pSuite.cdi"]}' > "$pts/slots"
	tsverdict="ok"
	for dev in gamepad twinStick; do
		printf '{"port1":"gamepad","port2":"%s"}' "$dev" > "$pts/settings"
		"$nat/run-native" "$pts" --frames 700 $nav240 \
			--screenshot "$work/ts.$dev.idle.tga" >/dev/null 2>&1
		# wire 12 of port 2 (20 + 12) is P2 Up2, the second d-pad's up
		"$nat/run-native" "$pts" --frames 700 $nav240 --press 665:35:32 \
			--screenshot "$work/ts.$dev.held.tga" >/dev/null 2>&1
		if cmp -s "$work/ts.$dev.idle.tga" "$work/ts.$dev.held.tga"; then
			[ "$dev" = "twinStick" ] && tsverdict="a twin stick's second d-pad reached nothing"
		else
			[ "$dev" = "gamepad" ] && tsverdict="a plain controller reported a second d-pad"
		fi
	done
	if [ "$tsverdict" = "ok" ]; then
		report "suite240p:devices" PASS "a twin stick has a second d-pad where a controller has none, on the same wire and the same port"
	else
		report "suite240p:devices" FAIL "$tsverdict"
	fi

	# A memory card per connected controller, which is what makes four players
	# able to save - and ONLY for the devices that have a slot to put one in. A
	# mouse and a light gun have none on the real thing either. One card per
	# controller, not two: BOTH of port A's expansion slots default to a VMU,
	# which is the emulator's habit rather than the machine's.
	cardcheck() { # <settings json> <expected file list>
		sd="$work/savedata-cards"
		rm -rf "$sd"; mkdir -p "$sd"
		printf '%s' "$1" > "$p4/settings"
		"$nat/run-native" "$p4" --frames 60 --savedata-out "$sd" >/dev/null 2>&1
		got="$(ls "$sd" 2>/dev/null | tr '\n' ' ')"
		[ "$got" = "$2" ] || echo "$got"
	}
	four="$(cardcheck '{"port1":"gamepad","port2":"gamepad","port3":"gamepad","port4":"gamepad"}' \
		"vmu_A1.bin vmu_B1.bin vmu_C1.bin vmu_D1.bin ")"
	mixed="$(cardcheck '{"port1":"gamepad","port2":"twinStick","port3":"mouse","port4":"lightGun"}' \
		"vmu_A1.bin vmu_B1.bin ")"
	if [ -n "$four" ]; then
		report "suite240p:cards" FAIL "four controllers gave '$four', want one card each"
	elif [ -n "$mixed" ]; then
		report "suite240p:cards" FAIL "a mouse and a gun gave '$mixed', and neither has a slot for a card"
	else
		report "suite240p:cards" PASS "a card for every controller and none for a mouse or a gun"
	fi
else
	for leg in triggers ports devices cards; do
		report "suite240p:$leg" SKIP "tests/own/240pSuite/240pSuite.cdi is not here"
	done
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

# ---- what a project PLUGS IN decides what a movie has columns for ----------
# This package declares the union of every device its four ports can hold -
# twenty controls and eleven axes per port - because a declaration is static and
# cannot know what a project chose. The core answers IsButtonActive and
# IsAxisActive once, after Init, and the engine builds the entry from what the
# machine HAS. A default Dreamcast is nine buttons and four axes, not eighty and
# forty-four.
#
# Every shape below is the DEVICE's, read off its own capability mask in
# maple_devs.cpp: a retail pad has no C, D, Z or second d-pad; an arcade stick
# has C and Z but no analog at all; a twin stick has the second d-pad and no
# analog; the PantherDC has everything including a second stick; a mouse and a
# gun are not controllers and have neither a d-pad nor a trigger.
chimera_root="${CHIMERA_ROOT:-$root/../../..}"
crun="$chimera_root/build/meson-linux/chimera-run"
cpkg="$chimera_root/build/Cores/flycast.chimeraCore"
if [ ! -x "$crun" ] || [ ! -f "$cpkg" ]; then
	report "ports:columns" SKIP "needs chimera-run and a built package (set CHIMERA_ROOT)"
else
	printf '[Input]\nLogKey:#\n' > "$work/none.txt"
	wrong=""
	check() { # <settings> <expected entry> <what it means>
		got="$("$crun" "$cpkg" "$root/tests/roms/counter.elf" "$work/none.txt" \
			--settings "$1" --frames 1 --record "$work/shape.txt" >/dev/null 2>&1 \
			&& head -1 "$work/shape.txt")"
		[ "$got" = "$2" ] || wrong="$wrong; $3 gave [${got:-nothing}] want [$2]"
	}
	check '{}' \
		'||    0,    0,    0,    0,.........|' "a retail pad: a stick, two triggers, nine buttons"
	check '{"port1":"arcadeStick"}' \
		'||...........|' "an arcade stick: C and Z, and no analog anywhere"
	check '{"port1":"twinStick"}' \
		'||..............|' "a twin stick: a second d-pad, and no analog"
	check '{"port1":"xl"}' \
		'||    0,    0,    0,    0,    0,    0,................|' "a PantherDC: every button and a second stick"
	check '{"port1":"mouse"}' \
		'||    0,    0,    0,...|' "a mouse: three buttons and three relative axes"
	check '{"port1":"lightGun"}' \
		'||    0,    0,........|' "a gun: a screen position, a trigger and a reload"
	check '{"port1":"gamepad","port2":"gamepad"}' \
		'||    0,    0,    0,    0,.........|    0,    0,    0,    0,.........|' "two pads"
	check '{"port1":"none"}' \
		'||' "nothing plugged in anywhere"
	if [ -z "$wrong" ]; then
		report "ports:columns" PASS "a movie carries the controls the machine has, and no others"
	else
		report "ports:columns" FAIL "${wrong#; }"
	fi
fi

echo
echo "$ok ok, $failed failed"
[ "$failed" -eq 0 ]
