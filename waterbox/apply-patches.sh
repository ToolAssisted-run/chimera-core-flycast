#!/bin/sh
# Overlays the chimera patch set onto the pinned Flycast submodule. Idempotent:
# a patch that is already applied is skipped, so configuring twice is harmless.
#
# The submodule pin is pristine upstream; every difference this core needs is a
# file in patches/, which is what keeps "what did we change" answerable.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
fc="$here/../extern/flycast"
for p in "$here"/../patches/*.patch; do
	[ -f "$p" ] || continue
	if git -C "$fc" apply --check "$p" 2>/dev/null; then
		git -C "$fc" apply "$p"
		echo "applied $(basename "$p")"
	fi
done
