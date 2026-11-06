"""Lists and extracts files from a CRI CPK archive.

Usage: cpk.py list <file.cpk> [glob]
       cpk.py extract <file.cpk> <out_dir> <glob> [glob...]

Globs match the path inside the archive, such as gui/menu/*. Extracted files come from the game: keep them
out of the repository.
"""
import fnmatch
import os
import struct
import sys

TYPES = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f", 9: ">d"}


def decrypt(data):
    out = bytearray(data)
    m = 0x655F
    for i in range(len(out)):
        out[i] ^= m & 0xFF
        m = (m * 0x4115) & 0xFFFFFFFF
    return bytes(out)


def read_table(data):
    """An @UTF table as a list of {column: value}."""
    if data[:4] != b"@UTF":
        data = decrypt(data)
    if data[:4] != b"@UTF":
        raise ValueError("not an @UTF table")
    size = struct.unpack(">I", data[4:8])[0]
    t = data[8 : 8 + size]
    rows_at, strings_at, data_at, _name, columns, row_length, rows = struct.unpack(">IIIIHHI", t[:24])

    def string(offset):
        start = strings_at + offset
        return t[start : t.index(b"\0", start)].decode("utf-8", "replace")

    def value(kind, at):
        if kind == 0xA:
            return string(struct.unpack(">I", t[at : at + 4])[0]), at + 4
        if kind == 0xB:
            offset, length = struct.unpack(">II", t[at : at + 8])
            return t[data_at + offset : data_at + offset + length], at + 8
        fmt = TYPES[kind]
        return struct.unpack(fmt, t[at : at + struct.calcsize(fmt)])[0], at + struct.calcsize(fmt)

    fields = []
    at = 24
    for _ in range(columns):
        flags = t[at]
        name = string(struct.unpack(">I", t[at + 1 : at + 5])[0])
        at += 5
        storage, kind = flags & 0xF0, flags & 0x0F
        constant = None
        if storage in (0x30, 0x70):
            constant, at = value(kind, at)
        fields.append((name, storage, kind, constant))

    table = []
    for r in range(rows):
        at = rows_at + r * row_length
        row = {}
        for name, storage, kind, constant in fields:
            if storage == 0x50:
                row[name], at = value(kind, at)
            elif storage in (0x30, 0x70):
                row[name] = constant
            else:
                row[name] = None
        table.append(row)
    return table


def read_chunk(f, offset):
    f.seek(offset)
    head = f.read(16)
    size = struct.unpack("<Q", head[8:16])[0]
    return read_table(f.read(size))


def entries(f):
    """Every file with a directory: (path, offset, stored size, extracted size)."""
    header = read_chunk(f, 0)[0]
    toc = header.get("TocOffset")
    if not toc:
        raise ValueError("no TOC: this archive only has IDs, which this tool doesn't handle")
    content = header.get("ContentOffset") or 0
    base = min(toc, content) if content else toc
    out = []
    for row in read_chunk(f, toc):
        path = "/".join(p for p in ((row.get("DirName") or ""), row["FileName"]) if p).lstrip("/")
        out.append((path, base + row["FileOffset"], row["FileSize"], row.get("ExtractSize") or row["FileSize"]))
    return out


def main():
    command, archive = sys.argv[1:3]
    with open(archive, "rb") as f:
        files = entries(f)
        if command == "list":
            pattern = sys.argv[3] if len(sys.argv) > 3 else "*"
            for path, offset, size, extracted in files:
                if fnmatch.fnmatch(path, pattern):
                    print("%10d %10d %12x %s" % (size, extracted, offset, path))
            return
        out_dir = sys.argv[3]
        patterns = sys.argv[4:]
        count = compressed = 0
        for path, offset, size, extracted in files:
            if not any(fnmatch.fnmatch(path, p) for p in patterns):
                continue
            target = os.path.join(out_dir, *path.split("/"))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            f.seek(offset)
            data = f.read(size)
            with open(target, "wb") as o:
                o.write(data)
            count += 1
            compressed += data[:8] == b"CRILAYLA"
        print("%d files, %d of them CRILAYLA-compressed" % (count, compressed))


if __name__ == "__main__":
    main()
