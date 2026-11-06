"""Audit Ghidra's exported code map against eboot.bin, .eh_frame and LLVM's decoder.

Usage: audit_export.py <eboot.bin> <export dir> <llvm-mc.exe>
"""
import bisect
import csv
import os
import re
import struct
import subprocess
import sys
from collections import Counter

eboot, export, mc = sys.argv[1:4]
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


def fde_of(off):
    i = bisect.bisect_right(fde_starts, off) - 1
    return fde[i] if i >= 0 and off < fde[i][1] else None


insts = [(int(r["address"], 16) - BASE, int(r["length"]), r["mnemonic"]) for r in rows("instructions.csv")]
starts = {a for a, _, _ in insts}
covered = bytearray(TEXT_SIZE)
for a, n, _ in insts:
    covered[a:a + n] = b"\1" * n
print("instructions", len(insts), "distinct_mnemonics", len({m for _, _, m in insts}))

# Undecoded bytes: padding or code nothing reached?
NOPS = re.compile(rb"^(?:\xcc|\x90|\x00|\x66*\x0f\x1f[\x00\x40\x44\x80\x84][\s\S]{0,7}|\x66*\x2e?\x0f\x1f[\s\S]{1,8}|\x66+\x90)+$")
inside_real = []
inside_pad = outside_pad = outside_real = 0
outside_real_runs = []
i = 0
while i < TEXT_SIZE:
    if covered[i]:
        i += 1
        continue
    j = i
    while j < TEXT_SIZE and not covered[j]:
        j += 1
    run = bytes(mem[i:j])
    pad = set(run) <= {0xCC, 0x90, 0x00} or NOPS.match(run) is not None
    in_fde = fde_of(i) is not None
    if in_fde and pad:
        inside_pad += j - i
    elif in_fde:
        inside_real.append((i, j))
    elif pad:
        outside_pad += j - i
    else:
        outside_real += j - i
        outside_real_runs.append((i, j))
    i = j
print("undecoded bytes: inside_fde_padding=%d inside_fde_nonpadding=%d (%d runs) outside_fde_padding=%d outside_fde_nonpadding=%d (%d runs)" % (
    inside_pad, sum(b - a for a, b in inside_real), len(inside_real), outside_pad, outside_real, len(outside_real_runs)))
for a, b in sorted(inside_real, key=lambda r: r[0] - r[1])[:15]:
    print("  inside  0x%08X len=%d %s" % (BASE + a, b - a, mem[a:a + 16].hex(" ")))
for a, b in sorted(outside_real_runs, key=lambda r: r[0] - r[1])[:10]:
    print("  outside 0x%08X len=%d %s" % (BASE + a, b - a, mem[a:a + 16].hex(" ")))

# Tables: would entries past the recovered bound also be valid targets in this function?
tables = rows("jump_tables.csv")
under_suspect = []
by_gap = Counter()
for t in tables:
    jump = int(t["jump"], 16) - BASE
    table = int(t["table"], 16) - BASE
    n = int(t["entries"])
    gap = int(t["gap_bytes"])
    rng = fde_of(jump)
    if table >= len(mem) or rng is None:
        by_gap["table_not_in_code_segment"] += 1
        continue
    if gap < 0:
        by_gap["last_table"] += 1
    elif gap == n * 4:
        by_gap["exact"] += 1
    else:
        by_gap["smaller_than_gap"] += 1
    extra = 0
    for k in range(n, n + 64):
        o = table + k * 4
        if gap >= 0 and k * 4 >= gap:
            break
        tgt = table + struct.unpack("<i", mem[o:o + 4])[0]
        if rng[0] <= tgt < rng[1] and tgt in starts:
            extra += 1
        else:
            break
    if extra:
        under_suspect.append((t["jump"], t["table"], n, gap, extra, t["bound_source"]))
print("table_size_vs_gap", dict(by_gap))
print("tables where the entries just past the bound still look valid:", len(under_suspect), Counter(u[5] for u in under_suspect))
for u in under_suspect[:15]:
    print("  jump=%s table=%s entries=%d gap=%d valid_after=%d src=%s" % u)
print("fde_scan tables:")
for t in tables:
    if t["bound_source"] == "fde_scan":
        print("  jump=%s table=%s entries=%s gap=%s" % (t["jump"], t["table"], t["entries"], t["gap_bytes"]))

# Functions with no unwind entry.
fns = rows("functions.csv")
print("functions by status", Counter((r["status"], r["thunk"]) for r in fns))
nf = [r for r in fns if r["status"] == "no_fde" and r["thunk"] != "true"]
print("non-thunk functions without FDE, in .text:", sum(int(r["entry"], 16) - BASE < TEXT_SIZE for r in nf))
for r in nf[:12]:
    print("  ", r["entry"], r["body_end"], r["name"])

# Unresolved computed jumps.
unres = rows("computed_jumps_unresolved.csv")
forms = Counter()
for r in unres:
    forms[re.sub(r"0x[0-9a-fA-F]+", "N", r["instruction"])] += 1
print("unresolved forms (top 15):")
for k, v in forms.most_common(15):
    print("  %5d %s" % (v, k))
in_fn = Counter()
for r in unres:
    a = int(r["address"], 16) - BASE
    in_fn["inside_fde" if fde_of(a) else "outside_fde"] += 1
print("unresolved location", dict(in_fn))

# Instruction lengths against LLVM: one input line per Ghidra instruction must give exactly one decode.
lines = [" ".join("0x%02x" % x for x in mem[a:a + n]) for a, n, _ in insts]
r = subprocess.run([mc, "--disassemble", "--triple=x86_64-unknown-unknown", "--mcpu=btver2"],
                   input="\n".join(lines), capture_output=True, text=True)
out = [s for s in r.stdout.splitlines() if s.strip() and not s.strip().startswith(".")]
print("llvm lines with no operands, top 12:", Counter(s.strip() for s in out if len(s.split()) == 1).most_common(12))
bad_lines = [int(m.group(1)) for m in re.finditer(r"<stdin>:(\d+):\d+: warning", r.stderr)]
print("llvm: input=%d decoded_lines=%d rc=%d invalid_warnings=%d" % (len(lines), len(out), r.returncode, len(bad_lines)))
badm = Counter()
for ln in bad_lines:
    a, n, m = insts[ln - 1]
    badm[m] += 1
print("llvm rejected, by Ghidra mnemonic:", badm.most_common(20))
for ln in bad_lines[:15]:
    a, n, m = insts[ln - 1]
    print("  0x%08X %-12s %s" % (BASE + a, m, mem[a:a + n].hex(" ")))
print("mnemonics, least common 40:", Counter(m for _, _, m in insts).most_common()[-40:])
