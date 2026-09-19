#!/usr/bin/env python3
"""Zips for test-zipfile: what a rom set can look like, in the shapes that
matter to a reader - stored and deflated members, a member in a folder, a
member longer than one inflate chunk, an archive comment, and a zip64 member.
The member bytes are a fixed function of their name, so the test knows what to
expect without reading anything back from here."""
import sys, zipfile, zlib

def body(name, size):
    seed = zlib.crc32(name.encode()) & 0xffffffff
    out = bytearray(size)
    x = seed
    for i in range(size):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        # long members are mostly runs, so deflate has something to do; short
        # ones are noise, so a stored block inside a deflate stream is seen too
        out[i] = (x >> 16) & 0xff if size < 100000 or i % 64 == 0 else (i // 64 + seed) & 0xff
    return bytes(out)

MEMBERS = [
    ("epr-21001.ic22", 4096, zipfile.ZIP_STORED),
    ("mpr-21002.ic1", 200000, zipfile.ZIP_DEFLATED),
    ("sub/dir/mpr-21003.ic2", 777, zipfile.ZIP_DEFLATED),
    ("empty.bin", 0, zipfile.ZIP_STORED),
]

def zip64_by_hand(out):
    """Python only writes zip64 fields into the central directory when a size
    crosses 4GB, so the reader's directory-side zip64 path is fed one written
    here: every 32-bit field saturated, the truth in the extended-information
    field and the zip64 end-of-directory record."""
    import struct
    name = b"mpr-64001.ic3"
    data = body(name.decode(), 5000)
    crc = zlib.crc32(data) & 0xffffffff
    local = struct.pack("<IHHHHHIIIHH", 0x04034b50, 45, 0, 0, 0, 0x21, crc,
                        len(data), len(data), len(name), 0) + name
    cd_off = len(local) + len(data)
    extra = struct.pack("<HHQQQ", 0x0001, 24, len(data), len(data), 0)
    central = struct.pack("<IHHHHHHIIIHHHHHII", 0x02014b50, 45, 45, 0, 0, 0, 0x21, crc,
                          0xffffffff, 0xffffffff, len(name), len(extra), 0, 0, 0, 0,
                          0xffffffff) + name + extra
    z64_off = cd_off + len(central)
    z64 = struct.pack("<IQHHIIQQQQ", 0x06064b50, 44, 45, 45, 0, 0, 1, 1, len(central), cd_off)
    locator = struct.pack("<IIQI", 0x07064b50, 0, z64_off, 1)
    eocd = struct.pack("<IHHHHIIH", 0x06054b50, 0, 0, 0xffff, 0xffff, 0xffffffff, 0xffffffff, 0)
    with open(out, "wb") as f:
        f.write(local + data + central + z64 + locator + eocd)

def main(out):
    with zipfile.ZipFile(out, "w") as z:
        for name, size, method in MEMBERS:
            z.writestr(zipfile.ZipInfo(name), body(name, size), compress_type=method)
        # a member written through the zip64 path: sizes and offset in the
        # extended-information field, the 32-bit ones saturated
        with z.open(zipfile.ZipInfo("big64.bin"), "w", force_zip64=True) as f:
            f.write(body("big64.bin", 70000))
        z.comment = b"a comment of some length, as sets from some tools carry"

if __name__ == "__main__":
    main(sys.argv[1])
    if len(sys.argv) > 2:
        zip64_by_hand(sys.argv[2])
