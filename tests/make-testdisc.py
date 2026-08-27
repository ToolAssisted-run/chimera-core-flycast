#!/usr/bin/env python3
"""Builds the gate's own GD-ROM: a real disc, made from scratch.

Every Dreamcast game is somebody's copyrighted disc, so this repository makes
its own instead - an actual GD-ROM image, with an actual ISO9660 filesystem and
an actual bootstrap, holding a program assembled by tests/sh4asm.py. Booting it
exercises the whole path a real game takes: the disc reader, the GD-ROM drive,
the HLE bios locating the bootfile, and the SH4 running what it loaded.

The layout is a Dreamcast's, not a CD's:

  track 1   data, LBA 0        the CD area, which a GD-ROM has and nothing on
  track 2   audio, LBA 600     it needs to read; a real disc's warning track
  track 3   data, LBA 45000    the GD area, where everything actually is

  45000  IP.BIN, 16 sectors: the bootstrap header, whose 0x60 field names the
         file to boot
  45016  the ISO9660 primary volume descriptor
  45017  the volume descriptor terminator
  45018  the root directory
  45019  1ST_READ.BIN, the program

Two details are the difference between a disc that boots and one that does not.
ISO9660 extents here are ABSOLUTE disc LBAs (45019, not 19), because the
filesystem sits at 45000 and the drive is addressed from the start of the disc.
And a GD-ROM's 1ST_READ.BIN is stored PLAIN: the scrambling everyone associates
with Dreamcast binaries belongs to CD-R images, and Flycast only descrambles
when the disc is not a GD-ROM (see reios.cpp's `descrambl`).

Usage: make-testdisc.py <out dir> <1ST_READ.BIN>
"""
import os
import struct
import sys

SECTOR = 2048
GD_START = 45000
IP_BIN_SECTORS = 16
PVD_LBA = GD_START + 16
TERM_LBA = GD_START + 17
ROOT_LBA = GD_START + 18
BOOT_LBA = GD_START + 19
BOOT_NAME = "1ST_READ.BIN"


def ip_bin() -> bytes:
    """The bootstrap header. Flycast reads 16 sectors of it to 0x8C008000 and
    takes the boot filename from offset 0x60."""
    ip = bytearray(b"\x20" * (IP_BIN_SECTORS * SECTOR))
    ip[0x00:0x10] = b"SEGA SEGAKATANA "
    ip[0x10:0x20] = b"SEGA ENTERPRISES"
    ip[0x20:0x30] = b"0000 CD-ROM1/1  "
    ip[0x30:0x38] = b"        "          # peripherals
    ip[0x40:0x4A] = b"CHIMERA-01"        # product number
    ip[0x4A:0x50] = b" V1.000"[:6]       # version
    ip[0x50:0x60] = b"20260827        "[:16]
    ip[0x60:0x70] = BOOT_NAME.ljust(16).encode()
    ip[0x70:0x80] = b"CHIMERA GATE    "
    ip[0x80:0x90] = b"CHIMERA TEST DISC"[:16]
    return bytes(ip)


def both_endian32(v: int) -> bytes:
    return struct.pack("<I", v) + struct.pack(">I", v)


def both_endian16(v: int) -> bytes:
    return struct.pack("<H", v) + struct.pack(">H", v)


def dir_record(name: bytes, lba: int, size: int, is_dir: bool) -> bytes:
    """One ISO9660 directory record."""
    length = 33 + len(name)
    if length % 2:
        length += 1                       # records are even-length
    rec = bytearray(length)
    rec[0] = length
    rec[1] = 0                            # extended attribute length
    rec[2:10] = both_endian32(lba)
    rec[10:18] = both_endian32(size)
    rec[18:25] = bytes([126, 8, 27, 0, 0, 0, 0])   # 2026-08-27, no offset
    rec[25] = 0x02 if is_dir else 0x00    # flags
    rec[26] = 0                           # file unit size
    rec[27] = 0                           # interleave gap
    rec[28:32] = both_endian16(1)         # volume sequence number
    rec[32] = len(name)
    rec[33:33 + len(name)] = name
    return bytes(rec)


def root_directory(boot_size: int) -> bytes:
    """`.`, `..`, and the bootfile."""
    data = bytearray()
    data += dir_record(b"\x00", ROOT_LBA, SECTOR, True)     # .
    data += dir_record(b"\x01", ROOT_LBA, SECTOR, True)     # ..
    data += dir_record(f"{BOOT_NAME};1".encode(), BOOT_LBA, boot_size, False)
    return bytes(data).ljust(SECTOR, b"\x00")


def pvd(boot_size: int, total_sectors: int) -> bytes:
    v = bytearray(SECTOR)
    v[0] = 1                                   # primary volume descriptor
    v[1:6] = b"CD001"
    v[6] = 1                                   # version
    v[8:40] = b"CHIMERA".ljust(32)             # system id
    v[40:72] = b"CHIMERA_GATE".ljust(32)       # volume id
    v[80:88] = both_endian32(total_sectors)
    v[120:124] = both_endian16(1)              # volume set size
    v[124:128] = both_endian16(1)              # volume sequence number
    v[128:132] = both_endian16(SECTOR)         # logical block size
    v[132:140] = both_endian32(SECTOR)         # path table size
    v[140:144] = struct.pack("<I", 0)          # no path tables: the root
    v[148:152] = struct.pack(">I", 0)          # record below is enough
    v[156:190] = dir_record(b"\x00", ROOT_LBA, SECTOR, True)
    v[190:318] = b" " * 128                    # volume set id
    v[318:446] = b"CHIMERA".ljust(128)         # publisher
    v[446:574] = b" " * 128                    # data preparer
    v[574:702] = b" " * 128                    # application
    for off in (702, 739, 776, 813):           # file identifiers
        v[off:off + 37] = b" " * 37
    for off in (813, 830, 847, 864):           # dates, unset
        v[off:off + 17] = b"0" * 16 + b"\x00"
    v[881] = 1                                 # file structure version
    return bytes(v)


def terminator() -> bytes:
    v = bytearray(SECTOR)
    v[0] = 0xFF
    v[1:6] = b"CD001"
    v[6] = 1
    return bytes(v)


def build(outdir: str, boot_path: str) -> None:
    os.makedirs(outdir, exist_ok=True)
    with open(boot_path, "rb") as f:
        boot = f.read()
    boot_sectors = (len(boot) + SECTOR - 1) // SECTOR
    total = 19 + boot_sectors

    track3 = bytearray()
    track3 += ip_bin()
    track3 += pvd(len(boot), GD_START + total)
    track3 += terminator()
    track3 += root_directory(len(boot))
    track3 += boot.ljust(boot_sectors * SECTOR, b"\x00")

    # The CD area: a GD-ROM has one, and nothing here reads it. Two data
    # sectors and a second of silence keep the track count a GD-ROM's.
    track1 = bytes(SECTOR * 2)
    track2 = bytes(2352 * 300)

    with open(os.path.join(outdir, "track01.bin"), "wb") as f:
        f.write(track1)
    with open(os.path.join(outdir, "track02.raw"), "wb") as f:
        f.write(track2)
    with open(os.path.join(outdir, "track03.bin"), "wb") as f:
        f.write(track3)

    gdi = os.path.join(outdir, "gate.gdi")
    with open(gdi, "w") as f:
        f.write("3\n")
        f.write(f"1 0 4 {SECTOR} track01.bin 0\n")
        f.write("2 600 0 2352 track02.raw 0\n")
        f.write(f"3 {GD_START} 4 {SECTOR} track03.bin 0\n")

    print(f"{gdi}: {total} sectors in the GD area, "
          f"{BOOT_NAME} is {len(boot)} bytes at LBA {BOOT_LBA}")


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    build(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
