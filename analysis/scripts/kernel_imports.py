"""List imports of the loaded modules from kernel-side libraries, with names.

Usage: kernel_imports.py <recomp tools dir> <names.tsv> <eboot.bin> <prx>...
"""
import collections
import pathlib
import sys

sys.path.insert(0, sys.argv[1])
import ps4elf  # noqa: E402

names = {}
for line in open(sys.argv[2], encoding="utf-8"):
    nid, _, symbol = line.rstrip("\n").partition("\t")
    names[nid] = symbol

by_lib = collections.defaultdict(set)
for arg in sys.argv[3:]:
    m = ps4elf.module(pathlib.Path(arg))
    for s in m["symbols"]:
        if s["section"] or not s["identity"]:
            continue
        nid = s["name"].split("#")[0]
        by_lib[s["identity"][1][0]].add(names.get(nid, "?" + nid))
for lib in ("libkernel", "libScePosix", "libSceSysmodule", "libSceLibcInternalExt"):
    print("==", lib, len(by_lib[lib]))
    print(" ".join(sorted(by_lib[lib])))
print("== other libraries:", {k: len(v) for k, v in by_lib.items() if k not in ("libkernel", "libScePosix", "libSceSysmodule")})
