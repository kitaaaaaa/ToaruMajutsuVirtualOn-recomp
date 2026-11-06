"""Count instructions using fs:/gs: segment prefixes and how many are the plain TCB load.

Usage: segment_census.py <eboot.bin> <instructions.csv>
"""
import csv
import struct
import sys
from collections import Counter

eboot, inst_csv = sys.argv[1:3]
BASE = 0x01000000
data = open(eboot, "rb").read()
nseg = struct.unpack("<H", data[0x18:0x1a])[0]
for i in range(nseg):
    props, off, fsz, _ = struct.unpack("<QQQQ", data[0x20 + i * 32:0x40 + i * 32])
    if props & 0x800 and fsz > 0x100000:
        mem = data[off:off + fsz]
        break

kinds = Counter()
samples = {}
with open(inst_csv, newline="") as f:
    for r in csv.DictReader(f):
        a = int(r["address"], 16) - BASE
        b = mem[a:a + int(r["length"])]
        k = 0
        while k < len(b) and b[k] in (0x66, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x67, 0xF0):
            k += 1
        if k < len(b) and b[k] in (0x64, 0x65):
            key = ("fs" if b[k] == 0x64 else "gs", r["mnemonic"], b[k + 1:].hex() if len(b) <= 10 else b[k + 1:k + 4].hex())
            kinds[key] += 1
            samples.setdefault(key, r["address"])
print("segment-prefixed instructions:", sum(kinds.values()), "forms:", len(kinds))
for key, c in kinds.most_common(25):
    print("  %6d %s %-8s %s e.g. %s" % (c, key[0], key[1], key[2], samples[key]))
