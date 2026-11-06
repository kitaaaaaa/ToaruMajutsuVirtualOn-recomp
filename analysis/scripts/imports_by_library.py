"""List the eboot's imports of chosen libraries, with names.

Usage: imports_by_library.py <recomp tools dir> <names.tsv> <eboot.bin> <library>...
"""
import pathlib
import sys

sys.path.insert(0, sys.argv[1])
import ps4elf  # noqa: E402

names = {}
for line in open(sys.argv[2], encoding="utf-8"):
    nid, _, symbol = line.rstrip("\n").partition("\t")
    names[nid] = symbol
m = ps4elf.module(pathlib.Path(sys.argv[3]))
wanted = sys.argv[4:]
out = {}
for s in m["symbols"]:
    if s["section"] or not s["identity"]:
        continue
    lib = s["identity"][1][0]
    if wanted and lib not in wanted:
        continue
    out.setdefault(lib, []).append(names.get(s["name"].split("#")[0], "?" + s["name"]))
for lib, items in sorted(out.items()):
    print("==", lib, len(items))
    print(" ".join(sorted(items)))
