"""List the code map's remaining open items for ContextDump.java.

Usage: leftover_sites.py <eboot.bin> <export dir> <out addresses.txt>
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
starts = [s for s, _ in fde]
insts = [(int(r["address"], 16) - BASE, int(r["length"]), r["mnemonic"]) for r in rows("instructions.csv")]
addrs = [a for a, _, _ in insts]
covered = bytearray(TEXT_SIZE)
for a, n, _ in insts:
    covered[a:a + n] = b"\1" * n


def in_fde(o):
    k = bisect.bisect_right(starts, o) - 1
    return k >= 0 and o < fde[k][1]


runs = []
i = 0
while i < TEXT_SIZE:
    if covered[i]:
        i += 1
        continue
    j = i
    while j < TEXT_SIZE and not covered[j]:
        j += 1
    if in_fde(i) and j - i >= 16 and not set(mem[i:j]) <= {0xCC, 0x90, 0x00}:
        runs.append((j - i, i))
    i = j
runs.sort(reverse=True)

kinds = Counter()
callees = Counter()
for n, a in runs:
    p = bisect.bisect_left(addrs, a) - 1
    pa, pn, pm = insts[p]
    kinds[pm] += 1
    if pm == "CALL" and mem[pa] == 0xE8:
        callees["0x%08X" % (BASE + pa + 5 + struct.unpack("<i", mem[pa + 1:pa + 5])[0])] += 1
print("runs >=16 bytes:", len(runs), "bytes:", sum(n for n, _ in runs))
print("before run:", kinds.most_common(10))
print("direct callees before run:", callees.most_common(10))

with open(out_list, "w") as f:
    for n, a in runs[:8]:
        f.write("0x%X gap len=%d\n" % (BASE + a, n))
    for r in rows("computed_jumps_unresolved.csv"):
        ins = r["instruction"]
        if ins.startswith("JMP") and ("RAX + RCX" in ins or (("[" not in ins) and "RAX" not in ins)):
            f.write("%s unresolved %s\n" % (r["address"], ins))
    for t in rows("jump_tables.csv"):
        if t["bound_source"] == "fde_scan":
            f.write("%s fde_scan table=%s\n" % (t["jump"], t["table"]))
    for r in rows("functions.csv"):
        if r["status"] == "no_fde" and r["thunk"] != "true":
            f.write("%s no_fde_function\n" % r["body_end"])
