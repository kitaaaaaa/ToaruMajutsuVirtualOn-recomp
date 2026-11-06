"""Compare the game's mnemonics with what bbrecomp generates in C and what bbcpu's interpreter runs,
and list C constructs that behave differently or don't exist on Windows.

Usage: bbcpu_fit.py <export dir> <bloodborne-recomp dir> <bbcpu dir>
"""
import csv
import os
import re
import sys
from collections import Counter

export, recomp, bbcpu = sys.argv[1:4]

with open(os.path.join(export, "instructions.csv"), newline="") as f:
    counts = Counter(r["mnemonic"].split(".")[0].upper() for r in csv.DictReader(f))

# Ghidra's spelling to Zydis's, where they differ.
ALIAS = {
    "JA": "JNBE", "JAE": "JNB", "JNC": "JNB", "JC": "JB", "JE": "JZ", "JNE": "JNZ", "JG": "JNLE", "JGE": "JNL",
    "JL": "JL", "JLE": "JLE", "JNA": "JBE", "CMOVA": "CMOVNBE", "CMOVAE": "CMOVNB", "CMOVNC": "CMOVNB", "CMOVC": "CMOVB",
    "CMOVE": "CMOVZ", "CMOVNE": "CMOVNZ", "CMOVG": "CMOVNLE", "CMOVGE": "CMOVNL", "CMOVBE": "CMOVBE", "CMOVLE": "CMOVLE",
    "SETA": "SETNBE", "SETAE": "SETNB", "SETNC": "SETNB", "SETC": "SETB", "SETE": "SETZ", "SETNE": "SETNZ", "SETG": "SETNLE",
    "SETGE": "SETNL", "RET": "RET", "MOVSXD": "MOVSXD", "CDQE": "CDQE", "CWDE": "CWDE", "CDQ": "CDQ", "CQO": "CQO",
    "MOVSQ": "MOVSQ", "MOVSD": "MOVSD", "STOSQ": "STOSQ", "STOSD": "STOSD", "PUSHFQ": "PUSHFQ",
}
ZYDIS_ONLY_NAME = re.compile(r"^VCMP[A-Z]+(SS|SD|PS|PD)$")


def normal(m):
    m = ALIAS.get(m, m)
    if ZYDIS_ONLY_NAME.match(m):
        return "VCMP" + m[-2:]
    return m


def names(path, pattern):
    text = open(path, encoding="utf-8", errors="replace").read()
    return {x.upper() for x in re.findall(pattern, text)}


gen = names(os.path.join(recomp, "tools", "recomp", "bbrecomp.c"), r"\bM\(([A-Z0-9_]+)\)")
interp = set()
for fn in ("interp.c", "interp_vec.c", "interp_x87.c"):
    p = os.path.join(bbcpu, "src", "cpu", fn)
    interp |= names(p, r"\bM\(([A-Z0-9_]+)\)") | names(p, r"ZYDIS_MNEMONIC_([A-Z0-9_]+)")
print("names seen: generator=%d interpreter=%d" % (len(gen), len(interp)))

total = sum(counts.values())
in_gen = in_interp = neither = 0
missing_gen = []
missing_all = []
for m, c in counts.most_common():
    n = normal(m)
    g, i = n in gen, n in interp
    in_gen += c if g else 0
    in_interp += c if i else 0
    if not g:
        missing_gen.append((m, c))
    if not g and not i:
        missing_all.append((m, c))
        neither += c
print("distinct=%d total=%d" % (len(counts), total))
print("instructions whose mnemonic the generator names: %d (%.2f%%), types=%d" % (in_gen, 100 * in_gen / total, len(counts) - len(missing_gen)))
print("instructions whose mnemonic the interpreter names: %d (%.2f%%)" % (in_interp, 100 * in_interp / total))
print("not named by the generator (%d):" % len(missing_gen), missing_gen)
print("named by neither (%d, %d instructions):" % (len(missing_all), neither), missing_all)

checks = {
    "long type (32-bit on Windows)": r"\b(?:unsigned\s+)?long\b(?!\s+long)",
    "__int128": r"__int128",
    "posix/mach calls": r"\b(?:mmap|munmap|mprotect|pthread_\w+|dlopen|dlsym|sigaction|getpagesize|sysconf|clock_gettime|strndup|getenv)\s*\(",
    "gcc/clang vector or attribute": r"__attribute__|__builtin_\w+",
    "inline asm": r"__asm__|\basm\s*\(",
}
for label, pat in checks.items():
    print("--", label)
    roots = [(os.path.join(recomp, "tools", "recomp"), ("bbrecomp.c", "scan.c")),
             (os.path.join(bbcpu, "src", "cpu"), ("bbcpu.h", "cpu.c", "cpu_internal.h", "decode.c", "interp.c", "interp_vec.c", "interp_x87.c", "native.c", "recomp.c", "trace.h")),
             (os.path.join(bbcpu, "src", "recomp"), ("rc.h",))]
    for root, files in roots:
        for fn in files:
            text = open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
            found = re.findall(pat, text)
            if found:
                print("   %-16s %4d  %s" % (fn, len(found), dict(Counter(x.strip() for x in found).most_common(6))))
