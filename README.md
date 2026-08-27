# chimera-core-flycast

Flycast - Dreamcast, and later NAOMI/Atomiswave - as a Chimera waterbox core.

Upstream is `flyinghead/flycast`, pinned as a submodule at `extern/flycast`.

This core has no precedent: nothing in BizHawk runs Flycast, and unlike every
other core in the bundle its emulator has **no software renderer**, so drawing
a frame without a GPU is a milestone of its own rather than a given. What that
means, what was measured, and the order the work goes in are in
[`docs/PLAN.md`](docs/PLAN.md).

Status: **M1, the machine running headless.** Nothing here runs a game yet.
