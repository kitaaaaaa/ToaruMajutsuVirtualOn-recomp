"""Convert jump_tables.csv (Ghidra addresses) to bbrecomp's BB_RECOMP_TABLES format (image offsets).

Usage: tables_tsv.py <jump_tables.csv> <out.tsv>
"""
import csv
import sys

BASE = 0x01000000
with open(sys.argv[1], newline="") as f:
    rows = list(csv.DictReader(f))
with open(sys.argv[2], "w", newline="\n") as out:
    for r in rows:
        out.write("%x\t%x\t%s\n" % (int(r["jump"], 16) - BASE, int(r["table"], 16) - BASE, r["entries"]))
print(len(rows), "tables")
