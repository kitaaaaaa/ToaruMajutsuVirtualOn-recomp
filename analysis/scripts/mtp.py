"""Reads the game's MTPA message files (.mtp): Shift-JIS strings, each with an id.

Usage: mtp.py dump <mtp_dir> <out_dir>     one <name>.txt per file: id<TAB>text, newlines as \\n
       mtp.py stats <mtp_dir>              how much text there is, by kind of file
       mtp.py check <mtp_dir>              rebuilds every file unchanged and compares it to the original

Layout: "MTPA" sections of 32-byte headers. At 0x24 the entry count n. At 0x40, n u32 indexes; then n records
of (u64 id, u64 offset); then the strings, each a u32 byte length and the text, NUL-terminated and padded to 4.
Offsets count from the start of the strings. Dumps come from the game: keep them out of the repository.
"""
import collections
import glob
import os
import re
import struct
import sys


def read_raw(data):
    """(records [(id, encoded text)], offset just past the last string)."""
    if data[:4] != b"MTPA":
        raise ValueError("not an MTPA file")
    count = struct.unpack("<I", data[0x24:0x28])[0]
    table = 0x40 + 4 * count
    base = table + 16 * count
    entries = []
    end = base
    for i in range(count):
        ident, offset = struct.unpack("<QQ", data[table + 16 * i : table + 16 * i + 16])
        at = base + offset
        length = struct.unpack("<I", data[at : at + 4])[0]
        entries.append((ident, data[at + 4 : at + 4 + length]))
        end = max(end, at + 4 + length + (4 - length % 4))
    return entries, end


def read(data):
    """(records [(id, text)], offset just past the last string)."""
    entries, end = read_raw(data)
    return [(ident, text.decode("cp932")) for ident, text in entries], end


def build(data, texts):
    """The file with new strings: `texts` maps id to text, or to bytes already encoded; ids it doesn't name
    keep theirs. The record table and the sections after the strings (ENRS relocations, EOFC) don't depend on
    the strings' contents."""
    entries, end = read_raw(data)
    count = len(entries)
    table = 0x40 + 4 * count
    base = table + 16 * count
    out = bytearray(data[:base])
    strings = bytearray()
    for i, (ident, original) in enumerate(entries):
        struct.pack_into("<Q", out, table + 16 * i + 8, len(strings))
        encoded = texts.get(ident, original)
        if isinstance(encoded, str):
            encoded = encoded.encode("cp932")
        strings += struct.pack("<I", len(encoded)) + encoded + b"\0" * (4 - len(encoded) % 4)
    # The next section stays aligned to 16.
    old_next = data.index(b"ENRS", end)
    next_section = base + len(strings)
    padding = -next_section % 16
    # Sizes in the headers: the MTPA section (0x4) and its body (0x14), which include the strings.
    growth = next_section + padding - old_next
    struct.pack_into("<I", out, 0x04, struct.unpack("<I", data[0x04:0x08])[0] + growth)
    struct.pack_into("<I", out, 0x14, struct.unpack("<I", data[0x14:0x18])[0] + growth)
    return bytes(out + strings + b"\0" * padding + data[old_next:])


def is_japanese(text):
    return any(ord(c) > 0x2FFF for c in text)


def main():
    command, source = sys.argv[1:3]
    paths = sorted(glob.glob(os.path.join(source, "*.mtp")))
    if command == "dump":
        out = sys.argv[3]
        os.makedirs(out, exist_ok=True)
        for path in paths:
            with open(path, "rb") as f:
                try:
                    entries, _ = read(f.read())
                except (ValueError, UnicodeDecodeError, struct.error) as e:
                    print("skipped %s: %s" % (os.path.basename(path), e))
                    continue
            name = os.path.splitext(os.path.basename(path))[0]
            with open(os.path.join(out, name + ".txt"), "w", encoding="utf-8", newline="\n") as f:
                for ident, text in entries:
                    f.write("%d\t%s\n" % (ident, text.replace("\n", "\\n")))
        print("%d files dumped" % len(paths))
        return
    if command == "check":
        same = different = skipped = 0
        for path in paths:
            with open(path, "rb") as f:
                data = f.read()
            try:
                rebuilt = build(data, {})
            except (ValueError, UnicodeDecodeError, struct.error):
                skipped += 1
                continue
            if rebuilt == data:
                same += 1
            else:
                different += 1
                print("DIFFERENT", os.path.basename(path), len(data), len(rebuilt))
        print("rebuilt byte for byte: %d; different: %d; skipped: %d" % (same, different, skipped))
        return
    kinds = collections.Counter()
    lines = collections.Counter()
    failed = []
    for path in paths:
        name = os.path.basename(path)
        with open(path, "rb") as f:
            try:
                entries, _ = read(f.read())
            except (ValueError, UnicodeDecodeError, struct.error) as e:
                failed.append("%s: %s" % (name, e))
                continue
        kind = re.sub(r"^MTPA_", "", name)
        kind = re.match(r"(DLC_story|TRIAL|story|battle_talk|battle|tutorial|chara|voice|[a-zA-Z]+)", kind).group(1)
        for _, text in entries:
            if is_japanese(text):
                lines[kind] += 1
                kinds[kind] += sum(ord(c) > 0x2FFF for c in text)
    print("files %d, failed %d" % (len(paths), len(failed)))
    for f in failed[:10]:
        print("  FAILED", f)
    print("Japanese lines %d, characters %d" % (sum(lines.values()), sum(kinds.values())))
    for kind, chars in kinds.most_common():
        print("  %-14s %6d lines %8d characters" % (kind, lines[kind], chars))


if __name__ == "__main__":
    main()
