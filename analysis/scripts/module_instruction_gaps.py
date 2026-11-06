"""Which instructions in a module's code the interpreter doesn't name, by exact encoding.

Usage: module_instruction_gaps.py <encodings.exe> <llvm-mc.exe> <interp dir> <elf>...
"""
import collections
import os
import re
import subprocess
import sys

encodings, mc, interp_dir = sys.argv[1:4]
names = set()
for fn in ("interp.c", "interp_vec.c", "interp_x87.c"):
    text = open(os.path.join(interp_dir, fn), encoding="utf-8", errors="replace").read()
    names |= {x.lower() for x in re.findall(r"\bM\(([A-Z0-9_]+)\)", text)}
    names |= {x.lower() for x in re.findall(r"ZYDIS_MNEMONIC_([A-Z0-9_]+)", text)}


def normal(m):
    m = m.split(".")[0]
    alias = {"retq": "ret", "callq": "call", "jmpq": "jmp", "movabs": "mov", "cdqe": "cdqe"}
    return alias.get(m, m)


for elf in sys.argv[4:]:
    lines = subprocess.run([encodings, elf, "all", "2000"], capture_output=True, text=True).stdout.split()
    gaps = collections.defaultdict(list)
    total = 0
    for hexline in lines:
        code = " ".join("0x" + hexline[i:i + 2] for i in range(0, len(hexline), 2))
        r = subprocess.run([mc, "--disassemble", "--triple=x86_64-unknown-unknown", "--mcpu=btver2", "--output-asm-variant=1"],
                           input=code, capture_output=True, text=True)
        text = [s.strip() for s in r.stdout.splitlines() if s.strip() and not s.strip().startswith(".")]
        if not text:
            continue
        total += 1
        parts = text[-1].split()
        m = normal(parts[0])
        if m in ("lock", "rep", "repne") and len(parts) > 1:
            m = normal(parts[1])
        if m not in names and not re.match(r"^(j|set|cmov|v?cmp|nop|data16)", m):
            gaps[m].append((hexline, text[-1]))
    print("==", os.path.basename(elf), "encodings", total, "unnamed types", len(gaps))
    for m, items in sorted(gaps.items()):
        print("  %-14s %4d  e.g. %s  %s" % (m, len(items), items[0][0], items[0][1]))
