"""Disassemble the .eh_frame range containing each given address with LLVM.

Usage: dump_fde.py <eboot.bin> <eh_frame_functions.csv> <llvm-mc.exe> <address>...
"""
import bisect
import csv
import struct
import subprocess
import sys

eboot, fde_csv, mc = sys.argv[1:4]
BASE = 0x01000000
data = open(eboot, "rb").read()
nseg = struct.unpack("<H", data[0x18:0x1a])[0]
for i in range(nseg):
    props, off, fsz, _ = struct.unpack("<QQQQ", data[0x20 + i * 32:0x40 + i * 32])
    if props & 0x800 and fsz > 0x100000:
        mem = data[off:off + fsz]
        break
with open(fde_csv, newline="") as f:
    fde = [(int(r["start"], 16), int(r["end"], 16)) for r in csv.DictReader(f)]
starts = [s for s, _ in fde]

for arg in sys.argv[4:]:
    a = int(arg, 16) - BASE
    s, e = fde[bisect.bisect_right(starts, a) - 1]
    print("==== 0x%X range 0x%X-0x%X size=%d" % (BASE + a, BASE + s, BASE + e, e - s))
    text = " ".join("0x%02x" % x for x in mem[s:e])
    r = subprocess.run([mc, "--disassemble", "--triple=x86_64-unknown-unknown", "--mcpu=btver2", "--show-encoding",
                        "--output-asm-variant=1"], input=text, capture_output=True, text=True)
    pos = s
    for line in r.stdout.splitlines():
        t = line.strip()
        if not t or t.startswith("."):
            continue
        ins, _, enc = t.partition("# encoding: [")
        n = enc.count(",") + 1 if enc else 0
        print("%08X  %s" % (BASE + pos, ins.strip()))
        pos += n
    if r.stderr.strip():
        print("stderr:", r.stderr.strip()[:300])
