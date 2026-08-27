#!/usr/bin/env python3
"""A very small SH4 assembler: enough to write the gate's own test programs.

The alternative was packing opcodes by hand into a list of hex numbers, which
is how this repository's first test program was written - and which is
unreadable the moment a program does anything more than count. A gate's test
program has to be auditable: someone reading it must be able to say what the
machine was asked to do.

Supported: the instructions the test programs use, and no more. Adding one is
a line in OPS; anything unsupported is an error rather than a wrong encoding.

Syntax is standard SH4 assembly, one instruction per line, with labels:

    start:  mov     #0,r0
            mov.l   @(addr,pc),r1     ; the assembler builds the literal pool
            mov.l   r0,@r1
            bra     start
            nop

Literals are written `mov.l #0x8C011000,r1` and the assembler allocates them in
a pool after the code, computing the PC-relative displacement itself. That is
the part most easily got wrong by hand: the SH4 rounds PC down to a longword
boundary and adds 4 before applying the displacement.
"""
import re
import struct
import sys


class AsmError(Exception):
    pass


def _reg(text: str) -> int:
    m = re.fullmatch(r"r(\d+)", text.strip(), re.I)
    if not m or not 0 <= int(m.group(1)) <= 15:
        raise AsmError(f"not a register: {text}")
    return int(m.group(1))


def _imm(text: str) -> int:
    text = text.strip()
    if not text.startswith("#"):
        raise AsmError(f"not an immediate: {text}")
    return int(text[1:], 0)


class Assembler:
    """Two passes: sizes and labels first, then encodings."""

    def __init__(self, base: int):
        self.base = base
        self.lines: list[tuple[str, list[str]]] = []
        self.labels: dict[str, int] = {}
        self.pool: list[int] = []          # longword literals, in order
        self.pool_of: dict[int, int] = {}  # value -> index

    # ---- pass 1 ---------------------------------------------------------
    def parse(self, text: str) -> None:
        pc = 0
        for raw in text.splitlines():
            line = raw.split(";")[0].strip()
            if not line:
                continue
            while True:
                m = re.match(r"^([A-Za-z_][A-Za-z_0-9]*):\s*", line)
                if not m:
                    break
                self.labels[m.group(1)] = pc
                line = line[m.end():].strip()
            if not line:
                continue
            parts = line.split(None, 1)
            mnem = parts[0].lower()
            args = [a.strip() for a in parts[1].split(",")] if len(parts) > 1 else []
            self.lines.append((mnem, args))
            pc += 2
        self.code_size = pc
        # the pool sits after the code, longword aligned
        self.pool_base = (pc + 3) & ~3

    def literal(self, value: int) -> int:
        value &= 0xFFFFFFFF
        if value not in self.pool_of:
            self.pool_of[value] = len(self.pool)
            self.pool.append(value)
        return self.pool_base + self.pool_of[value] * 4

    # ---- pass 2 ---------------------------------------------------------
    def assemble(self, text: str) -> bytes:
        self.parse(text)
        # A first encoding pass populates the literal pool; a second one then
        # sees its final size. The pool only grows, and its base never moves,
        # so two passes are enough.
        for _ in range(2):
            self.pool = []
            self.pool_of = {}
            words = [self._encode(pc * 2, m, a) for pc, (m, a) in enumerate(self.lines)]
        out = b"".join(struct.pack("<H", w) for w in words)
        out += bytes(self.pool_base - self.code_size)   # alignment padding
        out += b"".join(struct.pack("<I", v) for v in self.pool)
        return out

    def _pcrel_disp(self, pc: int, target: int) -> int:
        """Displacement in longwords for @(disp,pc), SH4 rules."""
        disp = (target - ((pc & ~3) + 4)) // 4
        if not 0 <= disp <= 0xFF:
            raise AsmError(f"literal out of range at {pc:#x}: disp {disp}")
        return disp

    def _branch_disp(self, pc: int, label: str, bits: int) -> int:
        if label not in self.labels:
            raise AsmError(f"unknown label: {label}")
        disp = (self.labels[label] - (pc + 4)) // 2
        limit = 1 << (bits - 1)
        if not -limit <= disp < limit:
            raise AsmError(f"branch out of range at {pc:#x}: {label}")
        return disp & ((1 << bits) - 1)

    def _encode(self, pc: int, mnem: str, args: list[str]) -> int:
        def n():
            return _reg(args[-1])

        if mnem == "nop":
            return 0x0009
        if mnem == "mov" and args[0].startswith("#"):
            v = _imm(args[0])
            if not -128 <= v <= 255:
                raise AsmError(f"mov #imm out of range: {v}")
            return 0xE000 | (n() << 8) | (v & 0xFF)
        if mnem == "mov" and len(args) == 2:
            return 0x6003 | (n() << 8) | (_reg(args[0]) << 4)
        if mnem == "mov.l" and args[0].startswith("#"):
            # pseudo-instruction: literal load through the pool
            target = self.literal(_imm(args[0]))
            return 0xD000 | (n() << 8) | self._pcrel_disp(pc, target)
        if mnem == "mov.l" and args[0].startswith("@") and not args[1].startswith("@"):
            src = args[0][1:]
            if src.startswith("("):   # @(label,pc)
                inner = src.strip("()").split(",")[0]
                target = self.labels[inner] if inner in self.labels else int(inner, 0)
                return 0xD000 | (n() << 8) | self._pcrel_disp(pc, target)
            return 0x6002 | (n() << 8) | (_reg(src) << 4)       # mov.l @rm,rn
        if mnem == "mov.l" and args[1].startswith("@"):
            return 0x2002 | (_reg(args[1][1:]) << 8) | (_reg(args[0]) << 4)
        if mnem == "add" and args[0].startswith("#"):
            return 0x7000 | (n() << 8) | (_imm(args[0]) & 0xFF)
        if mnem == "add":
            return 0x300C | (n() << 8) | (_reg(args[0]) << 4)
        if mnem == "and":
            return 0x2009 | (n() << 8) | (_reg(args[0]) << 4)
        if mnem == "xor":
            return 0x200A | (n() << 8) | (_reg(args[0]) << 4)
        if mnem == "tst":
            return 0x2008 | (n() << 8) | (_reg(args[0]) << 4)
        if mnem == "cmp/eq" and args[0].startswith("#"):
            return 0x8800 | (_imm(args[0]) & 0xFF)
        if mnem == "cmp/eq":
            return 0x3000 | (n() << 8) | (_reg(args[0]) << 4)
        if mnem == "shll2":
            return 0x4008 | (n() << 8)
        if mnem == "ldc" and args[1].lower() == "sr":
            return 0x400E | (_reg(args[0]) << 8)
        if mnem == "bra":
            return 0xA000 | self._branch_disp(pc, args[0], 12)
        if mnem == "bt":
            return 0x8900 | self._branch_disp(pc, args[0], 8)
        if mnem == "bf":
            return 0x8B00 | self._branch_disp(pc, args[0], 8)
        raise AsmError(f"unsupported instruction: {mnem} {','.join(args)}")


def assemble(text: str, base: int) -> bytes:
    return Assembler(base).assemble(text)


if __name__ == "__main__":
    sys.exit("this is a library; see make-testprog.py")
