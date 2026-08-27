#!/bin/sh
# Overlays the chimera patch set onto the pinned Flycast submodule. Idempotent:
# a patch that is already applied is skipped, so configuring twice is harmless.
#
# The submodule pin is pristine upstream; every difference this core needs is a
# file in patches/, which is what keeps "what did we change" answerable.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
fc="$here/../extern/flycast"
# ONE FILE PER PATCH, deliberately. `git apply --check` refuses a patch whose
# hunks are already applied, and it refuses the WHOLE patch - so a patch
# touching two files, one of which was reverted by hand, is silently skipped
# and the build quietly loses a change. That cost an afternoon once: the
# console id stopped being pinned and only the equivalence gate noticed.
for p in "$here"/../patches/*.patch; do
	[ -f "$p" ] || continue
	if git -C "$fc" apply --check "$p" 2>/dev/null; then
		git -C "$fc" apply "$p"
		echo "applied $(basename "$p")"
	elif ! git -C "$fc" apply --check --reverse "$p" 2>/dev/null; then
		echo "WARNING: $(basename "$p") neither applies nor is applied" >&2
	fi
done
