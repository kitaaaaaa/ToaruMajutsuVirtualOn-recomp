"""Export function ranges from eboot.bin's .eh_frame_hdr/.eh_frame to CSV.

Usage: ehframe_export.py <eboot.bin> <out.csv>
Addresses in the CSV are image-relative (add the Ghidra image base to compare).
"""
import csv
import struct
import sys
from collections import Counter

eboot, out_csv = sys.argv[1:3]
data = open(eboot, "rb").read()

nseg = struct.unpack("<H", data[0x18:0x1a])[0]
seg_off = seg_size = None
for i in range(nseg):
    props, off, fsz, _ = struct.unpack("<QQQQ", data[0x20 + i * 32:0x40 + i * 32])
    if props & 0x800 and fsz > 0x100000:
        seg_off, seg_size = off, fsz
        break
if seg_off is None:
    sys.exit("code segment not found")

elf = 0x20 + nseg * 32
phoff = struct.unpack("<Q", data[elf + 0x20:elf + 0x28])[0]
phnum = struct.unpack("<H", data[elf + 0x38:elf + 0x3a])[0]
hdr_va = None
for i in range(phnum):
    p = data[elf + phoff + i * 56:elf + phoff + (i + 1) * 56]
    ptype, _, _, va = struct.unpack("<IIQQ", p[:24])
    if ptype == 0x6474E550:
        hdr_va = va
if hdr_va is None:
    sys.exit("PT_GNU_EH_FRAME not found")

mem = data[seg_off:seg_off + seg_size]


def s32(o):
    return struct.unpack("<i", mem[o:o + 4])[0]


def u32(o):
    return struct.unpack("<I", mem[o:o + 4])[0]


def uleb(o):
    r = shift = 0
    while True:
        b = mem[o]
        o += 1
        r |= (b & 0x7f) << shift
        shift += 7
        if not b & 0x80:
            return r, o


ver, ptr_enc, cnt_enc, tbl_enc = mem[hdr_va:hdr_va + 4]
print("eh_frame_hdr va=0x%X version=%d enc=%02x/%02x/%02x" % (hdr_va, ver, ptr_enc, cnt_enc, tbl_enc))
if (ver, ptr_enc, cnt_enc, tbl_enc) != (1, 0x1b, 0x03, 0x3b):
    sys.exit("unexpected eh_frame_hdr encodings")
fde_count = u32(hdr_va + 8)
print("fde_count", fde_count)

cies = {}


def parse_cie(o):
    if o in cies:
        return cies[o]
    p = o + 8  # length + id
    version = mem[p]
    p += 1
    end = mem.index(b"\0", p)
    aug = mem[p:end].decode("ascii")
    p = end + 1
    _, p = uleb(p)  # code align
    _, p = uleb(p)  # data align
    if version == 1:
        p += 1
    else:
        _, p = uleb(p)
    fde_enc = 0
    lsda_enc = None
    if aug.startswith("z"):
        _, p = uleb(p)
        for ch in aug[1:]:
            if ch == "R":
                fde_enc = mem[p]
                p += 1
            elif ch == "L":
                lsda_enc = mem[p]
                p += 1
            elif ch == "P":
                penc = mem[p]
                if penc not in (0x1b, 0x03):
                    sys.exit("unsupported personality encoding %02x" % penc)
                p += 5
            else:
                sys.exit("unsupported augmentation %s" % aug)
    cies[o] = (aug, fde_enc, lsda_enc)
    return cies[o]


rows = []
augs = Counter()
for i in range(fde_count):
    t = hdr_va + 12 + i * 8
    tbl_loc = hdr_va + s32(t)
    fde = hdr_va + s32(t + 4)
    length = u32(fde)
    cie_ptr = (fde + 4) - u32(fde + 4)
    aug, fde_enc, lsda_enc = parse_cie(cie_ptr)
    if fde_enc != 0x1b:
        sys.exit("unsupported FDE encoding %02x" % fde_enc)
    begin = (fde + 8) + s32(fde + 8)
    size = u32(fde + 12)
    if begin != tbl_loc:
        sys.exit("table/FDE mismatch at %x" % fde)
    has_lsda = False
    if lsda_enc is not None:
        alen, p = uleb(fde + 16)
        if alen:
            has_lsda = any(mem[p:p + alen])
    augs[aug] += 1
    rows.append((begin, begin + size, int(has_lsda)))

rows.sort()
overlaps = sum(1 for a, b in zip(rows, rows[1:]) if a[1] > b[0])
print("cie_augmentations", dict(augs))
print("ranges=%d lowest=0x%X highest_end=0x%X overlaps=%d with_lsda=%d" % (
    len(rows), rows[0][0], rows[-1][1], overlaps, sum(r[2] for r in rows)))

with open(out_csv, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["start", "end", "has_lsda"])
    for s, e, l in rows:
        w.writerow(["0x%X" % s, "0x%X" % e, l])
