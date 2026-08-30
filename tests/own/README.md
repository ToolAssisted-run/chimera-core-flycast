# Test content this repository ships

Everything under `tests/own/` is content the gate runs that somebody else
wrote, and that may be redistributed. It is tracked, so CI runs it; the
commercial discs a developer may keep locally never are (see `.gitignore`,
and `tests/roms/` for the programs this repository builds itself).

## 240pSuite

Artemio Urbina's **240p Test Suite** for the Dreamcast, version as shipped in
`240pSuite.cdi`, with the author's own `README.TXT` and `Changelog.txt` beside
it exactly as distributed.

- Copyright 2011-2022 Artemio Urbina
- **GPL-2.0-or-later** ("either version 2 of the License, or (at your option)
  any later version"), per `README.TXT`
- Source and documentation: <http://junkerhq.net/240p/>

**These files are NOT under this repository's MIT licence.** See
`../../LICENSE` for how the two sit together.

### Why it is here

It is homebrew written to be run on real Dreamcasts, to check that hardware
behaves. That makes it the one thing in this repository able to say "a program
somebody else wrote agrees" rather than "we agree with ourselves" - every other
test here is a program this repository assembles, checked against a reference
build of the same sources.

The gate uses its **Controller Test**, which draws the pad's own readouts on
screen: `suite240p:triggers` navigates to it, holds each analog trigger, and
reads the number the machine printed. That is a second, independent witness for
`input:triggers`, which reads the same value out of RAM through a program this
repository wrote. The two disagreeing would be worth knowing about.

It found more than it was brought in for: a 320x240 picture being copied 640
wide, which nothing else here runs at a resolution low enough to have shown.
