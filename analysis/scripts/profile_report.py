"""Summarize profile.tsv from VOX_PROFILE: thread CPU time, then where busy threads are, with callers.

Usage: profile_report.py <profile.tsv> <llvm-symbolizer.exe> <vox.exe>
"""
import collections
import re
import subprocess
import sys

path, symbolizer, exe = sys.argv[1:4]
show_all = len(sys.argv) > 4 and sys.argv[4] == "all"  # idle threads too, with game-code offsets
IMAGE_BASE = 0x140000000
threads = {}
samples = collections.defaultdict(list)
header = ""
for line in open(path, encoding="utf-8", errors="replace"):
    p = line.rstrip("\n").split("\t")
    if p[0] == "#rounds":
        header += " | " + " ".join(p[1:])
    elif p[0] == "#thread":
        threads[p[1]] = (p[2], float(p[3]), float(p[4]))
    else:
        samples[p[0]].append(p[1:])

offsets = set()
for rows in samples.values():
    for frames in rows:
        for f in frames:
            m = re.match(r"(?i)vox\.exe\+([0-9a-f]+)$", f)
            if m:
                offsets.add(int(m.group(1), 16))
offsets = sorted(offsets)
names = {}
if offsets:
    text = "\n".join(hex(IMAGE_BASE + o) for o in offsets)
    r = subprocess.run([symbolizer, "--obj=" + exe, "--functions=linkage", "--no-inlines"], input=text, capture_output=True, text=True)
    for o, b in zip(offsets, r.stdout.strip().split("\n\n")):
        names[o] = b.split("\n")[0]


def label(f):
    m = re.match(r"(?i)vox\.exe\+([0-9a-f]+)$", f)
    if m:
        n = names.get(int(m.group(1), 16), "?")
        return re.sub(r"\(.*", "", n)[:58]
    mod = f.split("+")[0]
    if mod.lower().startswith("librecomp"):
        return "game+" + f.split("+")[1] if show_all else "(game code)"
    return "[" + mod + "]"


print("rounds:", header)
print("-- CPU time in the window (ms), busiest first")
ordered = sorted(threads.items(), key=lambda kv: -(kv[1][1] + kv[1][2]))
for tid, (name, user, kernel) in ordered[:14]:
    print("  %7.0f user %7.0f kernel  %-30s thread %s" % (user, kernel, name[:30], tid))
print("-- what the busiest threads were doing (chains, innermost first, samples)")
for tid, (name, user, kernel) in ordered if show_all else ordered[:7]:
    if user + kernel < 200 and not show_all:
        continue
    chains = collections.Counter()
    for frames in samples.get(tid, []):
        chain = []
        for f in frames:
            l = label(f)
            if not chain or chain[-1] != l:
                chain.append(l)
        chains[" < ".join(chain if show_all else chain[:7])] += 1
    print("thread %s %s: %d samples" % (tid, name, len(samples.get(tid, []))))
    for c, n in chains.most_common(6):
        print("   %5d  %s" % (n, c if show_all else c[:230]))
