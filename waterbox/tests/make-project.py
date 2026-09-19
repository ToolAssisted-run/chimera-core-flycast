#!/usr/bin/env python3
"""Writes a .chimeraProject for one of this package's machines, the way the
wizard would: the files in their slots, a value for every setting the machine
has, the firmware its conditions call for, and an idle input log in the
machine's own control names.

usage: make-project.py <package> <out.chimeraProject> <frames> <machine value>
                       [<setting>=<value> ...] -- <slot=file> [<slot=file> ...]

The machine value is what the `machine` setting takes (dreamcast, naomi,
naomi2, atomiswave); the firmware sha1s are left empty, because a bios set is
pinned by nothing but its name (see waterbox.config) and the frontend takes
--firmware <id>=<path> for the file itself.
"""
import hashlib
import json
import os
import sys
import zipfile


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            h.update(chunk)
    return h.hexdigest().upper()


def holds(cond, settings):
    """the condition language of docs/project.md, the setting tests only"""
    if cond is None:
        return True
    if "all" in cond:
        return all(holds(c, settings) for c in cond["all"])
    if "any" in cond:
        return any(holds(c, settings) for c in cond["any"])
    if "not" in cond:
        return not holds(cond["not"], settings)
    if "setting" in cond:
        have = settings.get(cond["setting"])
        if "is" in cond:
            return have == cond["is"]
        return have in cond.get("in", [])
    return False


def main():
    package, out, frames, machine = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4]
    rest = sys.argv[5:]
    sep = rest.index("--")
    overrides, slotargs = rest[:sep], rest[sep + 1:]

    z = zipfile.ZipFile(package)
    cfg = json.loads(z.read("waterbox.config"))
    mcfg = next(m for m in cfg["machines"] if machine in m["when"])
    inputs = mcfg["input"]
    buttons = inputs["buttons"]
    axes = inputs.get("axes", [])

    def player_of(name):
        if len(name) > 2 and name[0] in "Pp" and name[1].isdigit():
            return int(name[1])
        return 0

    groups = max([player_of(a["name"]) for a in axes] + [player_of(b) for b in buttons] + [0]) + 1
    row, key = "", ""
    for g in range(groups):
        row += "|"
        key += "#"
        for a in axes:
            if player_of(a["name"]) == g:
                row += "%5d," % a.get("neutral", 0)
                key += a["name"] + "|"
        for b in buttons:
            if player_of(b) == g:
                row += "."
                key += b + "|"
    row += "|"
    log = "[Input]\nLogKey:" + key + "\n" + "\n".join([row] * frames) + "\n[/Input]\n"

    files = []
    for arg in slotargs:
        slot, path = arg.split("=", 1)
        files.append({"name": os.path.basename(path), "sha1": sha1(path), "slot": slot})

    # the machine's settings: those with no `when`, and those whose `when`
    # names this machine
    settings = {}
    for d in cfg.get("settings", []):
        when = d.get("when")
        if when is None or machine in when:
            if d.get("default") is not None:
                settings[d["name"]] = d["default"]
    settings[cfg["machineSetting"]] = machine
    for o in overrides:
        k, v = o.split("=", 1)
        if v in ("true", "false"):
            v = v == "true"
        elif v.lstrip("-").isdigit():
            v = int(v)
        settings[k] = v

    firmware = [{"id": d["id"], "sha1": d.get("sha1", "")}
                for d in cfg.get("firmware", []) if holds(d.get("requiredWhen"), settings)]

    project = {
        "id": "flycast-gate-%s" % machine,
        "title": "%s through Chimera" % mcfg["label"],
        "description": "written by waterbox/tests/make-project.py",
        "core": {"name": cfg["coreName"], "version": cfg["version"], "sha1": sha1(package)},
        "rerecords": 0,
        "files": files,
        "settings": settings,
        "firmware": firmware,
        "coreCache": [],
        "input": log,
        "markers": [],
        "branches": [],
        "headers": {
            "MovieVersion": "Chimera Project File v1.1",
            "Platform": mcfg["id"],
            "SHA1": files[0]["sha1"] if files else "",
            "LastInputFrame": str(frames - 1),
            "VsyncNumerator": str(cfg["video"]["vsyncNumerator"]),
            "VsyncDenominator": str(cfg["video"]["vsyncDenominator"]),
        },
    }
    with open(out, "w") as f:
        json.dump(project, f, indent="\t")
    return 0


if __name__ == "__main__":
    sys.exit(main())
