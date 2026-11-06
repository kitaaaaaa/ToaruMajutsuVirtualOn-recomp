"""Print the signature and return statements of each named function in shadPS4's gnmdriver.cpp.

Usage: gnm_signatures.py <gnmdriver.cpp> <name>...
"""
import re
import sys

text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
for name in sys.argv[2:]:
    m = re.search(r"^([\w:\*]+(?: [\w:\*]+)?) PS4_SYSV_ABI " + re.escape(name) + r"\(([^)]*)\)\s*\{", text, re.M)
    if not m:
        print(name, "-- NOT FOUND")
        continue
    i = m.end()
    depth = 1
    while depth and i < len(text):
        depth += {"{": 1, "}": -1}.get(text[i], 0)
        i += 1
    body = text[m.end():i]
    rets = sorted(set(re.findall(r"return ([^;]+);", body)))
    sig = " ".join(m.group(2).split())
    print("%s %s(%s) lines=%d returns=%s" % (m.group(1), name, sig, body.count("\n"), rets[:4]))
