#!/usr/bin/env python3
"""Builds the gate's own Dreamcast programs, as ELFs the HLE bios can boot.

A core needs something to run before it can be gated, and a Dreamcast game is
somebody's copyrighted disc. So this repository ships its own programs instead,
assembled here from readable SH4 (see sh4asm.py) so that what the machine is
asked to do is auditable rather than trusted.

Flycast's HLE bios boots a naked .elf at 0x8C010000 (see reios.cpp), which is
what makes this possible with no bios, no disc and no SH4 toolchain.

  counter.elf  counts, forever, into RAM. System RAM becomes a direct function
               of how many instructions the machine executed, so the
               equivalence gate compares the one thing that must never differ.

  triangle.elf submits one polygon to the TA through the store queues and
               triggers a render, which is what makes the software renderer
               testable at all: without it the machine draws nothing and every
               frame hashes the same.

  padread.elf  reads the controller over the maple bus every iteration and
               sums what it sees into RAM. That makes system RAM a function of
               the INPUT as well, which is what lets the gate prove input
               reaches the machine rather than assuming it.

Usage: make-testprog.py <out dir>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sh4asm import assemble  # noqa: E402

LOAD_ADDR = 0x8C010000

# Every program starts by refusing interrupts. The HLE bios boots an ELF with
# interrupts enabled and no handlers installed, so the first vblank vectors
# through VBR into empty RAM and the machine dies on an illegal instruction.
# Real homebrew installs handlers; these programs decline the interrupts.
# MD=1 keeps them privileged, IMASK=15 blocks every maskable level.
PROLOGUE = """
        mov.l   #0x400000F0,r0
        ldc     r0,sr
"""

COUNTER_ASM = PROLOGUE + """
        mov.l   #0x8C011000,r1
        mov     #0,r0
loop:   add     #1,r0
        mov.l   r0,@r1
        bra     loop
        nop
"""

# The maple bus, by hand.
#
# A Dreamcast controller is not memory-mapped: the SH4 builds a command frame
# in RAM, points the maple DMA at it and starts it, and the answer lands in
# another piece of RAM. This program does exactly that, once per iteration.
#
#   frame at 0x8C012000 (physical 0x0C012000, 32-byte aligned as the DMA
#   requires):
#       word0  0x80000001  last transfer | one more word after the command | port A
#       word1  0x0C012100  where to write the answer (physical, area 3)
#       word2  0x01002009  len 1 | sender 0x00 | recipient 0x20 | GetCondition
#       word3  0x01000000  the function being asked about: controller
#
#   the answer at 0x8C012100:
#       word0  the response header
#       word1  the function code, echoed back
#       word2  the button state in the low 16 bits (ACTIVE LOW), with two
#              analog axes above it
#
# SB_MDAPRO comes first: the DMA refuses addresses outside a window that
# defaults to a slice of area 2, and 0x6155407F is the value that opens system
# RAM to it.
PADREAD_ASM = PROLOGUE + """
        mov.l   #0xA05F6C8C,r1
        mov.l   #0x6155407F,r2
        mov.l   r2,@r1

        mov.l   #0x8C012000,r1
        mov.l   #0x80000001,r2
        mov.l   r2,@r1
        add     #4,r1
        mov.l   #0x0C012100,r2
        mov.l   r2,@r1
        add     #4,r1
        mov.l   #0x01002009,r2
        mov.l   r2,@r1
        add     #4,r1
        mov.l   #0x01000000,r2
        mov.l   r2,@r1

loop:
        mov.l   #0xA05F6C04,r1
        mov.l   #0x0C012000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F6C10,r1
        mov     #0,r2
        mov.l   r2,@r1
        mov.l   #0xA05F6C14,r1
        mov     #1,r2
        mov.l   r2,@r1
        mov.l   #0xA05F6C18,r1
        mov     #1,r2
        mov.l   r2,@r1

wait:   mov.l   @r1,r2
        tst     r2,r2
        bf      wait

        mov.l   #0x8C012108,r3
        mov.l   @r3,r4

        mov.l   #0x8C011004,r3
        mov.l   r4,@r3

        mov.l   #0x8C011000,r3
        mov.l   @r3,r5
        add     r4,r5
        mov.l   r5,@r3

        mov.l   #0x8C011008,r3
        mov.l   @r3,r6
        add     #1,r6
        mov.l   r6,@r3

        bra     loop
        nop
"""


# A triangle, submitted the way a Dreamcast game submits one.
#
# Nothing about this is memory-mapped either: polygon and vertex parameters go
# to the TA through the SH4's STORE QUEUES - 32 bytes staged at 0xE0000000 and
# flushed with `pref`, landing at the address QACR0 selects, which here is the
# TA's FIFO at 0x10000000. Then STARTRENDER, and the PVR draws what it was told.
#
# The blocks, in order:
#   polygon    PCW 0x80000000 (opaque polygon, packed colour, untextured)
#              ISP 0xE0000000 (depth compare: always), TSP, TCW
#   vertex x3  PCW 0xE0000000, then x, y, 1/w, u, v, base colour, offset
#              the last one is 0xF0000000: end of strip
#   end        PCW 0x00000000: end of list
#
# The triangle is red, covers most of the screen, and sits at a constant depth,
# so the picture it makes is a large flat shape - which is exactly what a gate
# wants: obviously right or obviously wrong, and identical in both builds.
def _sq_block(words):
    """Stage 32 bytes in the store queue and flush them to the TA."""
    out = []
    for i, w in enumerate(words):
        out.append(f"        mov.l   #{w:#010x},r2")
        out.append(f"        mov.l   r2,@({i * 4},r3)")
    out.append("        pref    @r3")
    return "\n".join(out)


F100 = 0x42C80000   # 100.0f
F500 = 0x43FA0000   # 500.0f
F300 = 0x43960000   # 300.0f
F400 = 0x43C80000   # 400.0f
FHALF = 0x3F000000  # 0.5f, the 1/w every vertex shares
RED = 0xFFFF0000

DRAW_ASM = PROLOGUE + """
        ; the store queues point at the TA FIFO (0x10000000)
        mov.l   #0xFF000038,r1
        mov     #0x10,r2
        mov.l   r2,@r1
        mov.l   #0xFF00003C,r1
        mov.l   r2,@r1

        ; where the TA puts what it builds, and how much of the screen it covers
        mov.l   #0xA05F8124,r1      ; TA_OL_BASE
        mov.l   #0x00100000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F812C,r1      ; TA_OL_LIMIT
        mov.l   #0x00140000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F8128,r1      ; TA_ISP_BASE
        mov.l   #0x00200000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F8130,r1      ; TA_ISP_LIMIT
        mov.l   #0x00280000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F813C,r1      ; TA_GLOB_TILE_CLIP: 20x15 tiles = 640x480
        mov.l   #0x000E0013,r2
        mov.l   r2,@r1
        mov.l   #0xA05F8140,r1      ; TA_ALLOC_CTRL
        mov.l   #0x00000001,r2
        mov.l   r2,@r1
        mov.l   #0xA05F8144,r1      ; TA_LIST_INIT
        mov.l   #0x80000000,r2
        mov.l   r2,@r1

        ; Everything the PVR reads for itself - the region array, the object
        ; pointer blocks, the background parameters - is addressed in the
        ; 32-BIT address space, which is the 0xA5xxxxxx window from the SH4 and
        ; which the emulator maps into video memory the same way on both sides
        ; (pvr_read32p goes through pvr_map32). Writing them through the linear
        ; 0xA4 window puts them where the PVR does not look.
        ;
        ; The region array, at its own address - one entry, marked last. Its
        ; opaque pointer must be the object-list base the TA was given, because
        ; that address is how Flycast finds the display list this render is
        ; for: a region array that says "no object lists" produces no context
        ; and no picture, however much geometry the TA accepted.
        mov.l   #0xA5180000,r1
        mov.l   #0x80000000,r2      ; control: tile 0,0 and last region
        mov.l   r2,@r1
        mov.l   #0x00100000,r2      ; opaque: TA_OL_BASE
        mov.l   r2,@(4,r1)
        mov.l   #0x80000000,r2      ; the other three lists are empty
        mov.l   r2,@(8,r1)
        mov.l   r2,@(12,r1)
        mov.l   r2,@(16,r1)

        ; The first word of the object pointer block, which is what actually
        ; identifies this render: the PVR follows the region array's pointer to
        ; the OPB and reads the parameter address from it. Real hardware's TA
        ; writes that word while it builds the lists; Flycast's TA is high level
        ; and builds no lists in memory at all, so nothing writes it - and a
        ; render whose lists cannot be identified is a render that never
        ; happens. The program writes it, because on hardware it would be there.
        mov.l   #0xA5100000,r1
        mov.l   #0x00100000,r2
        mov.l   r2,@r1

        ; the display list itself
        mov.l   #0xE0000000,r3
""" + _sq_block([0x80000000, 0xE0000000, 0x20800000, 0, 0, 0, 0, 0]) + """
""" + _sq_block([0xE0000000, F100, F100, FHALF, 0, 0, RED, 0]) + """
""" + _sq_block([0xE0000000, F500, F100, FHALF, 0, 0, RED, 0]) + """
""" + _sq_block([0xF0000000, F300, F400, FHALF, 0, 0, RED, 0]) + """
""" + _sq_block([0, 0, 0, 0, 0, 0, 0, 0]) + """

        ; THE BACKGROUND PLANE, which is not part of the display list: the PVR
        ; reads its parameters from wherever ISP_BACKGND_T points and draws it
        ; behind everything. Here that is a parameter block at PARAM_BASE+0x1000
        ; (VRAM 0xA5201000) holding ISP/TSP/TCW and
        ; three vertices of 16 bytes
        ; each - x, y, 1/w and a packed colour, which is what skip=1 means.
        ; Blue, at a depth further than the triangle's, so the picture is a red
        ; triangle on blue rather than a red triangle on whatever was there.
        mov.l   #0xA5201000,r1
        mov.l   #0xE0000000,r2      ; ISP: depth always
        mov.l   r2,@r1
        mov.l   #0x20800000,r2      ; TSP
        mov.l   r2,@(4,r1)
        mov     #0,r2               ; TCW: no texture
        mov.l   r2,@(8,r1)

        mov.l   #0x00000000,r2      ; v0: x=0
        mov.l   r2,@(12,r1)
        mov.l   #0x00000000,r2      ; v0: y=0
        mov.l   r2,@(16,r1)
        mov.l   #0x3A83126F,r2      ; v0: 1/w = 0.001, behind everything
        mov.l   r2,@(20,r1)
        mov.l   #0xFF0000FF,r2      ; v0: blue
        mov.l   r2,@(24,r1)

        mov.l   #0x44200000,r2      ; v1: x=640
        mov.l   r2,@(28,r1)
        mov.l   #0x00000000,r2      ; v1: y=0
        mov.l   r2,@(32,r1)
        mov.l   #0x3A83126F,r2
        mov.l   r2,@(36,r1)
        mov.l   #0xFF0000FF,r2
        mov.l   r2,@(40,r1)

        mov.l   #0x00000000,r2      ; v2: x=0
        mov.l   r2,@(44,r1)
        mov.l   #0x43F00000,r2      ; v2: y=480
        mov.l   r2,@(48,r1)
        mov.l   #0x3A83126F,r2
        mov.l   r2,@(52,r1)
        mov.l   #0xFF0000FF,r2
        mov.l   r2,@(56,r1)

        mov.l   #0xA05F8088,r1      ; ISP_BACKGND_D: the depth it clears to
        mov.l   #0x3A83126F,r2
        mov.l   r2,@r1
        mov.l   #0xA05F808C,r1      ; ISP_BACKGND_T: tag_address 0x400, skip 1
        mov.l   #0x01002000,r2
        mov.l   r2,@r1

        ; The video output. Without this the machine draws 240 lines, which is
        ; not a bug: FB_R_CTRL's vclk_div picks the video clock, and 0 is the
        ; 240-line one. A game that wants 480 says so, and so does this.
        mov.l   #0xA05F8044,r1      ; FB_R_CTRL: enable | 565 | vclk_div
        mov.l   #0x00800005,r2
        mov.l   r2,@r1

        ; where to render from and to, then go
        mov.l   #0xA05F8020,r1      ; PARAM_BASE
        mov.l   #0x00200000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F802C,r1      ; REGION_BASE
        mov.l   #0x00180000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F8060,r1      ; FB_W_SOF1
        mov.l   #0x00600000,r2
        mov.l   r2,@r1
        mov.l   #0xA05F804C,r1      ; FB_W_CTRL: 565
        mov.l   #0x00000001,r2
        mov.l   r2,@r1
        mov.l   #0xA05F8014,r1      ; STARTRENDER
        mov     #1,r2
        mov.l   r2,@r1

        ; and then wait, forever: one frame drawn is what this program is for
done:   mov.l   #0x8C011000,r1
        mov.l   @r1,r0
        add     #1,r0
        mov.l   r0,@r1
        bra     done
        nop
"""

PROGRAMS = {
    "counter.elf": COUNTER_ASM,
    "padread.elf": PADREAD_ASM,
    "triangle.elf": DRAW_ASM,
}

EM_SH = 42
ELF_HEADER_SIZE = 52
PROGRAM_HEADER_SIZE = 32
SECTION_HEADER_SIZE = 40


def build(text: bytes) -> bytes:
    offset = ELF_HEADER_SIZE + PROGRAM_HEADER_SIZE
    # One null section header, because Flycast's libelf rejects a file whose
    # e_shstrndx is not less than e_shnum - and with no section table at all,
    # 0 is not less than 0. Nothing reads it; it exists to be counted.
    shoff = offset + len(text)

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
        LOAD_ADDR,  # entry
        ELF_HEADER_SIZE,   # phoff
        shoff,
        0x9,               # flags: SH4 (EF_SH4)
        ELF_HEADER_SIZE,
        PROGRAM_HEADER_SIZE, 1,      # phentsize, phnum
        SECTION_HEADER_SIZE, 1, 0,   # shentsize, shnum, shstrndx
    )

    phdr = struct.pack(
        "<IIIIIIII",
        1,              # PT_LOAD
        offset,
        LOAD_ADDR,      # vaddr
        LOAD_ADDR,      # paddr
        len(text),      # filesz
        len(text),      # memsz
        0x5,            # PF_R | PF_X
        4,              # align
    )

    return ehdr + phdr + text + bytes(SECTION_HEADER_SIZE)


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    os.makedirs(outdir, exist_ok=True)
    for name, source in PROGRAMS.items():
        code = assemble(source, LOAD_ADDR)
        path = os.path.join(outdir, name)
        with open(path, "wb") as f:
            f.write(build(code))
        # The same code as a raw binary, which is what a DISC holds: the HLE
        # bios loads 1ST_READ.BIN to 0x8C010000 and jumps to it, with no ELF
        # header to read. tests/make-testdisc.py takes one of these.
        raw = os.path.join(outdir, name.replace(".elf", ".bin"))
        with open(raw, "wb") as f:
            f.write(code)
        print(f"{path}: {len(code)} bytes of SH4 at {LOAD_ADDR:#x}")


if __name__ == "__main__":
    main()
