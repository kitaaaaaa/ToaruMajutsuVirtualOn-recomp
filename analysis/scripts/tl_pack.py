"""Builds the runtime's translation overlays from translation\\*.en and the game's own archives.

Usage: tl_pack.py <translation_dir> <cpk_dir> <out_dir>

Each <name>.en holds id<TAB>text lines (newlines as \\n) for the message file <name>.mtp. The result is
<disc>.overlay for each archive: byte ranges the runtime lays over that archive as the game reads it, so the
archive itself never changes. A translated file has to fit the original's size. Overlays contain game data:
keep them out of the repository.

Overlay format: "VOXP", u32 count, count * (u64 archive offset, u32 size, u32 reserved), then the bytes.
"""
import glob
import os
import struct
import sys

import cpk
import mtp


def read_translation(path):
    texts = {}
    with open(path, encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            ident, text = line.split("\t", 1)
            if int(ident) in texts:
                raise ValueError("%s:%d: id %s twice" % (path, number, ident))
            texts[int(ident)] = text.replace("\\n", "\n")
    return texts


def main():
    translation_dir, cpk_dir, out_dir = sys.argv[1:4]
    wanted = {os.path.splitext(os.path.basename(p))[0] + ".mtp": p for p in glob.glob(os.path.join(translation_dir, "*.en"))}
    os.makedirs(out_dir, exist_ok=True)
    problems = 0
    packed = set()
    for disc_path in sorted(glob.glob(os.path.join(cpk_dir, "*.cpk"))):
        disc = os.path.splitext(os.path.basename(disc_path))[0]
        patches = []
        with open(disc_path, "rb") as f:
            for path, offset, size, _ in cpk.entries(f):
                name = path.rsplit("/", 1)[-1]
                if name not in wanted:
                    continue
                f.seek(offset)
                original = f.read(size)
                entries, _end = mtp.read(original)
                known = {i for i, _t in entries}
                texts = read_translation(wanted[name])
                unknown = set(texts) - known
                if unknown:
                    print("PROBLEM %s: ids not in the game: %s" % (name, sorted(unknown)[:8]))
                    problems += 1
                    continue
                try:
                    built = mtp.build(original, texts)
                except UnicodeEncodeError as e:
                    print("PROBLEM %s: not Shift-JIS: %s" % (name, e))
                    problems += 1
                    continue
                if len(built) > size:
                    print("PROBLEM %s: %d bytes over the original's %d" % (name, len(built) - size, size))
                    problems += 1
                    continue
                print("%s: %d of %d lines, %d of %d bytes" % (name, len(texts), len(entries), len(built), size))
                patches.append((offset, built + b"\0" * (size - len(built))))
                packed.add(name)
        target = os.path.join(out_dir, disc + ".overlay")
        if patches:
            with open(target, "wb") as o:
                o.write(b"VOXP" + struct.pack("<I", len(patches)))
                for offset, data in patches:
                    o.write(struct.pack("<QII", offset, len(data), 0))
                for _, data in patches:
                    o.write(data)
        elif os.path.exists(target):
            os.remove(target)
    for name in sorted(set(wanted) - packed):
        print("NOT PACKED", name)
    print("%d files packed, %d problems" % (len(packed), problems))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
