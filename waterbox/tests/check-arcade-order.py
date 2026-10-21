#!/usr/bin/env python3
"""check-arcade-order.py <cinterface.cpp> <waterbox.config>

The arcade boards' panel goes over the wire by place: the core reads button i
of a player as the i-th name of its ARC_ enum (waterbox/cinterface.cpp), and
Chimera knows the buttons only by the names waterbox.config declares. The two
lists must be the same, for every arcade machine and every player, or a press
of Coin reaches the board as something else (chimera#337 moved Coin beside
Start in both).
"""
import json
import re
import sys

src = open(sys.argv[1]).read()
enum = src[src.index("ARC_UP, ARC_DOWN"):src.index("ARC_BTN_PER_PLAYER")]
names = {"UP": "Up", "DOWN": "Down", "LEFT": "Left", "RIGHT": "Right", "START": "Start", "COIN": "Coin",
         "SERVICE": "Service", "TEST": "Test", "RELOAD": "Reload"}
names.update({f"BTN{i}": f"Button {i}" for i in range(1, 9)})
want = [names[s] for s in re.findall(r"ARC_([A-Z0-9]+)", enum)]
bad = []
for m in json.load(open(sys.argv[2]))["machines"]:
    if m["id"] == "DC":
        continue
    b = m["input"]["buttons"]
    for p in range(4):
        got = b[p * len(want):(p + 1) * len(want)]
        if got != [f"P{p + 1} {n}" for n in want]:
            bad.append(f"{m['id']} P{p + 1}: {got}")
if bad:
    print("the declared arcade panel is not the core's order:\n" + "\n".join(bad[:4]), file=sys.stderr)
    sys.exit(1)
print(f"{len(want)} buttons a player, the core's order, on every arcade machine")
