#!/bin/bash
# The core-level equivalence gate: the sandboxed core must produce
# byte-identical video, audio, lag and memory-domain digests to the native
# reference build (the same sources compiled natively), and must survive a
# whole-machine savestate round-trip around every frame.
#
# SCOPE. The machine does not yet DRAW: Flycast has no software renderer, and
# porting one is its own milestone (docs/PLAN.md). It runs this repository's own
# SH4 programs rather than a game, because a Dreamcast game is somebody's
# copyrighted disc. What is proven here is everything the picture will later
# rest on: hundreds of millions of SH4 instructions, the AICA, the timers, the
# memory system, and the maple bus carrying a frontend's input to the machine -
# identical inside the sandbox and out.
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
	if [ "$doms" = "3" ]; then
		report "domains" PASS "System RAM, VRAM and Sound RAM exposed"
	else
		report "domains" FAIL "$doms domains, want 3"
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
