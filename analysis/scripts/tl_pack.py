"""Builds the runtime's translation overlays.

Usage: tl_pack.py <translation_dir> <cpk_dir> <out_dir>          from translation\\*.en
       tl_pack.py --vita <patch_dir> <cpk_dir> <out_dir>         from the user's own copy of a Vita rePatch

Each <name>.en holds id<TAB>text lines (newlines as \\n) for the message file <name>.mtp. With --vita, the
message files come from the .cpk archives under <patch_dir> instead. They share the PS4's format.

The result is <disc>.overlay for each archive: byte ranges the runtime lays over that archive as the game reads
it, so the archive itself never changes. A file that fits stays where it is, padded to its old size. One that
doesn't goes past the end of the archive, and the overlay also patches its table of contents entry. Overlays
contain game data, and with --vita someone else's work too: keep them out of the repository.

Overlay format: "VOXP", u32 count, u64 size of the archive as the game should see it,
count * (u64 archive offset, u32 size, u32 reserved), then the bytes.
"""
import glob
import os
import struct
import sys

import cpk
import mtp

ALIGN = 0x800


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


def vita_files(patch_dir):
    """Message files in a rePatch: its DLC over its Patch01 over its disc00, as the Vita would see them."""
    archives = sorted(glob.glob(os.path.join(patch_dir, "**", "*.cpk"), recursive=True))

    def rank(path):
        name = os.path.basename(path).lower()
        return 2 if name.startswith("dlc") else 1 if name.startswith("patch") else 0

    files = {}
    for path in sorted(archives, key=rank):
        with open(path, "rb") as f:
            for inner, offset, size, _ in cpk.entries(f):
                if inner.endswith(".mtp"):
                    f.seek(offset)
                    files[inner.rsplit("/", 1)[-1]] = f.read(size)
    return files


def from_vita(original, vita):
    """The PS4 file with a Vita file's text. Same ids (almost always): the Vita file as it is. Otherwise each
    id the PS4 file has takes the Vita text, because the PS4's tables are what its game expects."""
    ids = [i for i, _ in mtp.read_raw(original)[0]]
    entries = mtp.read_raw(vita)[0]
    if [i for i, _ in entries] == ids:
        return vita
    return mtp.build(original, dict(entries))


def main():
    arguments = sys.argv[1:]
    use_vita = arguments[0] == "--vita"
    if use_vita:
        arguments = arguments[1:]
    source, cpk_dir, out_dir = arguments
    if use_vita:
        vita = vita_files(source)
        wanted = {name: None for name in vita}
        print("%d message files in the patch" % len(vita))
    else:
        wanted = {os.path.splitext(os.path.basename(p))[0] + ".mtp": p for p in glob.glob(os.path.join(source, "*.en"))}
    os.makedirs(out_dir, exist_ok=True)
    problems = 0
    packed = set()
    moved = 0
    for disc_path in sorted(glob.glob(os.path.join(cpk_dir, "*.cpk"))):
        disc = os.path.splitext(os.path.basename(disc_path))[0]
        patches = []
        with open(disc_path, "rb") as f:
            real_size = os.path.getsize(disc_path)
            end = (real_size + ALIGN - 1) & ~(ALIGN - 1)
            base, fields = None, None
            for row, (path, offset, size, _) in enumerate(cpk.entries(f)):
                name = path.rsplit("/", 1)[-1]
                if name not in wanted:
                    continue
                f.seek(offset)
                original = f.read(size)
                try:
                    mtp.read_raw(original)
                except (ValueError, struct.error):
                    print("skipped %s: this game's own copy doesn't parse" % name)
                    packed.add(name)
                    continue
                try:
                    if use_vita:
                        built = from_vita(original, vita[name])
                    else:
                        texts = read_translation(wanted[name])
                        unknown = set(texts) - {i for i, _ in mtp.read_raw(original)[0]}
                        if unknown:
                            print("PROBLEM %s: ids not in the game: %s" % (name, sorted(unknown)[:8]))
                            problems += 1
                            continue
                        built = mtp.build(original, texts)
                except (ValueError, struct.error, UnicodeEncodeError) as e:
                    print("PROBLEM %s: %s" % (name, e))
                    problems += 1
                    continue
                packed.add(name)
                if len(built) <= size:
                    patches.append((offset, built + b"\0" * (size - len(built))))
                    continue
                # Too big for its place: past the end of the archive, with its TOC row pointing there.
                if fields is None:
                    base, fields = cpk.toc_layout(f)
                patches.append((end, built))
                position = fields["FileOffset"][1][row]
                patches.append((position, struct.pack(fields["FileOffset"][0], end - base)))
                for field in ("FileSize", "ExtractSize"):
                    if field in fields:
                        patches.append((fields[field][1][row], struct.pack(fields[field][0], len(built))))
                end = (end + len(built) + ALIGN - 1) & ~(ALIGN - 1)
                moved += 1
        target = os.path.join(out_dir, disc + ".overlay")
        if patches:
            patches.sort(key=lambda p: p[0])
            with open(target, "wb") as o:
                o.write(b"VOXP" + struct.pack("<IQ", len(patches), end))
                for offset, data in patches:
                    o.write(struct.pack("<QII", offset, len(data), 0))
                for _, data in patches:
                    o.write(data)
        elif os.path.exists(target):
            os.remove(target)
    for name in sorted(set(wanted) - packed):
        print("NOT PACKED", name)
    print("%d files packed, %d moved past the end of their archive, %d problems" % (len(packed), moved, problems))
    sys.exit(1 if problems else 0)


if __name__ == "__main__":
    main()
