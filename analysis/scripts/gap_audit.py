"""Classify undecoded code inside .eh_frame ranges and list sites to inspect in Ghidra.

Usage: gap_audit.py <eboot.bin> <export dir> <out addresses.txt>
"""
import bisect
import csv
import os
import struct
import sys
from collections import Counter

eboot, export, out_list = sys.argv[1:4]
BASE = 0x01000000
data = open(eboot, "rb").read()
nseg = struct.unpack("<H", data[0x18:0x1a])[0]
for i in range(nseg):
    props, off, fsz, _ = struct.unpack("<QQQQ", data[0x20 + i * 32:0x40 + i * 32])
    if props & 0x800 and fsz > 0x100000:
        mem = data[off:off + fsz]
        break
TEXT_SIZE = 0x4E4A40


def rows(name):
    with open(os.path.join(export, name), newline="") as f:
        return list(csv.DictReader(f))


fde = [(int(r["start"], 16), int(r["end"], 16)) for r in rows("eh_frame_functions.csv")]
fde_starts = [s for s, _ in fde]


def fde_of(o):
    i = bisect.bisect_right(fde_starts, o) - 1
    return fde[i] if i >= 0 and o < fde[i][1] else None


insts = [(int(r["address"], 16) - BASE, int(r["length"]), r["mnemonic"]) for r in rows("instructions.csv")]
inst_addrs = [a for a, _, _ in insts]
covered = bytearray(TEXT_SIZE)
for a, n, _ in insts:
    covered[a:a + n] = b"\1" * n

runs = []
i = 0
while i < TEXT_SIZE:
    if covered[i]:
        i += 1
        continue
    j = i
    while j < TEXT_SIZE and not covered[j]:
        j += 1
    run = mem[i:j]
    if fde_of(i) and not set(run) <= {0xCC, 0x90, 0x00}:
        runs.append((i, j))
    i = j

prev_kind = Counter()
prev_bytes = Counter()
callee = Counter()
sites = []
for a, b in runs:
    k = bisect.bisect_left(inst_addrs, a) - 1
    pa, pn, pm = insts[k]
    prev_kind[pm] += 1
    prev_bytes[pm] += b - a
    if pm == "CALL" and mem[pa] == 0xE8:
        callee["0x%08X" % (BASE + pa + 5 + struct.unpack("<i", mem[pa + 1:pa + 5])[0])] += b - a
    sites.append(("gap", BASE + a, b - a, pm))

print("runs=%d bytes=%d" % (len(runs), sum(b - a for a, b in runs)))
print("instruction before run, by count:", prev_kind.most_common(12))
print("instruction before run, by bytes:", prev_bytes.most_common(12))
print("direct callees before runs (by bytes):", callee.most_common(12))

tables = rows("jump_tables.csv")
with open(out_list, "w") as f:
    for r in sorted(sites, key=lambda x: -x[2])[:14]:
        f.write("0x%X gap len=%d after %s\n" % (r[1], r[2], r[3]))
    for t in tables:
        if int(t["outside_fde"]) > 0:
            f.write("%s outside_fde table=%s entries=%s\n" % (t["jump"], t["table"], t["entries"]))
    for j in ("0x01059fb8", "0x0131ca46", "0x013d9a91", "0x014d8fdc", "0x014ab867"):
        f.write("%s suspect_bound\n" % j)
    for j in ("0x01143ccb", "0x013bc32d"):
        f.write("%s fde_scan\n" % j)
    picked = Counter()
    for r in rows("computed_jumps_unresolved.csv"):
        ins = r["instruction"]
        if ins.startswith("JMP R") and "[" not in ins:
            key = "RAX" if "RAX" in ins else "other"
            if picked[key] < 7:
                picked[key] += 1
                f.write("%s unresolved %s\n" % (r["address"], ins))
