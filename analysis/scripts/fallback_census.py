"""Which instructions the generated C leaves to the interpreter, and how the three types the generator
and interpreter don't name are generated.

Usage: fallback_census.py <gen dir>
"""
import collections
import glob
import os
import re
import sys

steps = collections.Counter()
hits = collections.Counter()
samples = {}
watch = re.compile(r"/\* (vpmovsxbq|vpmovzxwq|vaddsubpd)\b")
for path in glob.glob(os.path.join(sys.argv[1], "recomp_*.c")):
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r"\s*(?:L_\w+:\s*)?/\* ([a-z0-9.]+)", line)
            if not m:
                m = re.search(r"/\* ([a-z0-9.]+)[^*]*\*/", line)
            if "rc->step" in line and m:
                steps[m.group(1)] += 1
            w = watch.search(line)
            if w:
                kind = "interpreter" if "rc->step" in line else "in C"
                hits[(w.group(1), kind)] += 1
                samples.setdefault(w.group(1), line[:170])
print("interpreter fallbacks:", sum(steps.values()), steps.most_common(30))
print("watched:", dict(hits))
for k, v in samples.items():
    print(" ", k, "::", v.strip())
