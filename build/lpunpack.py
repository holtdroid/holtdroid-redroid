#!/usr/bin/env python3
import struct
import sys

SECTOR = 512
GEOMETRY_MAGIC = 0x616C4467
HEADER_MAGIC = 0x414C5030


def partitions(img, base):
    img.seek(base + 4096)
    (magic,) = struct.unpack("<I", img.read(4))
    if magic != GEOMETRY_MAGIC:
        sys.exit("not an LP super partition")
    meta = base + 4096 + 4096 * 2
    img.seek(meta)
    head = img.read(256)
    magic, _, _, header_size = struct.unpack_from("<IHHI", head)
    (tables_size,) = struct.unpack_from("<I", head, 44)
    if magic != HEADER_MAGIC:
        sys.exit("bad LP metadata header")
    tables = struct.unpack_from("<12I", head, 80)
    p_off, p_num, p_size, e_off, e_num, e_size = tables[:6]
    img.seek(meta + header_size)
    data = img.read(tables_size)
    extents = [struct.unpack_from("<QIQI", data, e_off + i * e_size) for i in range(e_num)]
    for i in range(p_num):
        name, _, first, count, _ = struct.unpack_from("<36sIIII", data, p_off + i * p_size)
        yield name.rstrip(b"\0").decode(), extents[first : first + count]


def super_offset(img):
    img.seek(SECTOR)
    gpt = img.read(92)
    if gpt[:8] != b"EFI PART":
        return 0
    entries, count, size = struct.unpack_from("<QII", gpt, 72)
    for i in range(count):
        img.seek(entries * SECTOR + i * size)
        first = struct.unpack_from("<Q", img.read(size), 32)[0]
        img.seek(first * SECTOR + 4096)
        if first and struct.unpack("<I", img.read(4))[0] == GEOMETRY_MAGIC:
            return first * SECTOR
    sys.exit("no LP super partition in the GPT")


def main():
    path, want, out = sys.argv[1:4]
    with open(path, "rb") as img:
        offset = super_offset(img)
        for name, extents in partitions(img, offset):
            if name != want:
                continue
            with open(out, "wb") as dst:
                for sectors, target, start, _ in extents:
                    if target != 0:
                        dst.write(b"\0" * sectors * SECTOR)
                        continue
                    img.seek(offset + start * SECTOR)
                    left = sectors * SECTOR
                    while left:
                        chunk = img.read(min(left, 1 << 24))
                        dst.write(chunk)
                        left -= len(chunk)
            return
    sys.exit(f"partition {want} not found")


if __name__ == "__main__":
    main()
