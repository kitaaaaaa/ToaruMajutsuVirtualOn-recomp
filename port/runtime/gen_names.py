"""Writes NAMES.TSV (NID, symbol) from shadPS4's aerolib.inl, for readable logs and name lookups.

Usage: gen_names.py <aerolib.inl> <out.tsv>
"""
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
pairs = re.findall(r'STUB\(\s*"([^"]+)"\s*,\s*([^\s\)]+)\s*\)', text)
with open(sys.argv[2], "w", newline="\n", encoding="utf-8") as out:
    for nid, symbol in pairs:
        out.write("%s\t%s\n" % (nid, symbol))
print(len(pairs), "names")
