#!/usr/bin/env python3
"""Builds the gate's own Dreamcast program: a few SH4 instructions in an ELF.

A core needs something to run before it can be gated, and a Dreamcast game is
somebody's copyrighted disc. So this repository ships its own program instead -
hand-assembled here, from opcodes, so that what the machine executes is
readable in this file rather than trusted from a binary.

Flycast's HLE bios boots a naked .elf at 0x8C010000 (see reios.cpp), which is
what makes this possible with no bios, no disc and no SH4 toolchain.

What it does: counts, forever, storing the count to a fixed address. That makes
system RAM a direct function of how many instructions the machine executed, so
the equivalence gate compares the one thing that must never differ - and a
sandbox that ran even one cycle differently says so immediately.

Usage: make-testprog.py <out.elf>
"""
import struct
import sys

LOAD_ADDR = 0x8C010000
COUNTER = 0x8C011000

# SH4, little-endian, 16-bit instructions.
#
#   0: D003   mov.l  @(3,pc),r0     ; r0 = SR value
#   2: 400E   ldc    r0,sr          ; mask every interrupt level
#   4: D103   mov.l  @(3,pc),r1     ; r1 = COUNTER
#   6: E000   mov    #0,r0
#   8: 7001   add    #1,r0          ; loop:
#   A: 2102   mov.l  r0,@r1
#   C: AFFC   bra    loop           ; pc+4 + (-4)*2 = 8
#   E: 0009   nop                   ; delay slot, always executed
#  10: .long  SR_VALUE
#  14: .long  COUNTER
#
# The LDC comes first and is the whole reason this program is more than three
# instructions: the HLE bios boots an ELF with interrupts ENABLED and no
# handlers installed, so the first vblank vectors through VBR into empty RAM
# and the machine dies on an illegal instruction. Real homebrew installs
# handlers; this one refuses the interrupts instead. MD=1 keeps it privileged,
# IMASK=15 blocks every maskable level.
SR_VALUE = 0x400000F0
CODE = [0xD003, 0x400E, 0xD103, 0xE000, 0x7001, 0x2102, 0xAFFC, 0x0009]
TEXT = (b"".join(struct.pack("<H", op) for op in CODE)
        + struct.pack("<I", SR_VALUE) + struct.pack("<I", COUNTER))

EM_SH = 42
ELF_HEADER_SIZE = 52
PROGRAM_HEADER_SIZE = 32


SECTION_HEADER_SIZE = 40


def build() -> bytes:
    entry = LOAD_ADDR
    offset = ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE
    # One null section header, because Flycast's libelf rejects a file whose
    # e_shstrndx is not less than e_shnum - and with no section table at all,
    # 0 is not less than 0. Nothing reads it; it exists to be counted.
    shoff = offset + len(TEXT)

    ehdr = struct.pack(
        "<4sBBBBB7sHHIIIIIHHHHHH",
        b"\x7fELF",
        1,          # ELFCLASS32
        1,          # ELFDATA2LSB
        1,          # EV_CURRENT
        0, 0, b"",  # osabi, abiversion, pad
        2,          # ET_EXEC
        EM_SH,
        1,          # version
        entry,
        ELF_HEADER_SIZE,   # phoff
        shoff,
        0x9,               # flags: SH4 (EF_SH4)
        ELF_HEADER_SIZE,
        PROGRAM_HEADER_SIZE, 1,   # phentsize, phnum
        SECTION_HEADER_SIZE, 1, 0,   # shentsize, shnum, shstrndx
    )

    phdr = struct.pack(
        "<IIIIIIII",
        1,              # PT_LOAD
        offset,
        LOAD_ADDR,      # vaddr
        LOAD_ADDR,      # paddr
        len(TEXT),      # filesz
        len(TEXT),      # memsz
        0x5,            # PF_R | PF_X
        4,              # align
    )

    return ehdr + phdr + TEXT + bytes(SECTION_HEADER_SIZE)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    with open(sys.argv[1], "wb") as f:
        f.write(build())
    print(f"{sys.argv[1]}: {len(build())} bytes, "
          f"{len(CODE)} instructions at {LOAD_ADDR:#x}, counter at {COUNTER:#x}")


if __name__ == "__main__":
    main()
