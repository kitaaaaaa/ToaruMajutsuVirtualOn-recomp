"""Decodes a .htx texture (a DDS behind an HTEX/HTSF header) to a PNG, for looking at game art.

Usage: htx2png.py <in.htx> <out.png> [dark]

Handles DXT1, DXT3 and DXT5, top mip only. The output comes from the game: keep it out of the repository.
"""
import struct
import sys
import zlib


def colors(c0, c1, opaque):
    def rgb(c):
        return ((c >> 11 & 31) * 255 // 31, (c >> 5 & 63) * 255 // 63, (c & 31) * 255 // 31)

    a, b = rgb(c0), rgb(c1)
    if opaque or c0 > c1:
        return [a + (255,), b + (255,), tuple((2 * x + y) // 3 for x, y in zip(a, b)) + (255,), tuple((x + 2 * y) // 3 for x, y in zip(a, b)) + (255,)]
    return [a + (255,), b + (255,), tuple((x + y) // 2 for x, y in zip(a, b)) + (255,), (0, 0, 0, 0)]


def alpha_table(a0, a1):
    if a0 > a1:
        return [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    return [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]


def decode(data, width, height, fourcc):
    pixels = bytearray(width * height * 4)
    size = 8 if fourcc == b"DXT1" else 16
    at = 0
    for by in range(0, height, 4):
        for bx in range(0, width, 4):
            block = data[at : at + size]
            at += size
            if len(block) < size:
                return pixels
            alpha = [255] * 16
            if fourcc == b"DXT5":
                table = alpha_table(block[0], block[1])
                bits = int.from_bytes(block[2:8], "little")
                alpha = [table[bits >> (3 * i) & 7] for i in range(16)]
            elif fourcc == b"DXT3":
                bits = int.from_bytes(block[:8], "little")
                alpha = [(bits >> (4 * i) & 15) * 17 for i in range(16)]
            c = block[size - 8 :]
            c0, c1, indices = struct.unpack("<HHI", c)
            palette = colors(c0, c1, fourcc != b"DXT1")
            for i in range(16):
                x, y = bx + i % 4, by + i // 4
                if x < width and y < height:
                    r, g, b, a = palette[indices >> (2 * i) & 3]
                    pixels[(y * width + x) * 4 : (y * width + x) * 4 + 4] = bytes((r, g, b, a if fourcc == b"DXT1" else alpha[i]))
    return pixels


def write_png(path, pixels, width, height):
    raw = bytearray()
    for y in range(height):
        raw.append(0)
        raw += pixels[y * width * 4 : (y + 1) * width * 4]

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def main():
    source, target = sys.argv[1:3]
    d = open(source, "rb").read()
    at = d.index(b"DDS ")
    height, width = struct.unpack("<II", d[at + 12 : at + 20])
    fourcc = d[at + 84 : at + 88]
    print("%dx%d %s, header at %#x" % (width, height, fourcc.decode("latin-1"), at))
    pixels = decode(d[at + 128 :], width, height, fourcc)
    if len(sys.argv) > 3 and sys.argv[3] == "dark":  # a lot of the UI art is white on transparent
        for i in range(0, len(pixels), 4):
            a = pixels[i + 3]
            for c in range(3):
                pixels[i + c] = (pixels[i + c] * a + 32 * (255 - a)) // 255
            pixels[i + 3] = 255
    write_png(target, pixels, width, height)


if __name__ == "__main__":
    main()
