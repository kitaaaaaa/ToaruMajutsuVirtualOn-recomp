"""Makes the stall reports in a run log readable: real function names for vox.exe, f_<guest address> for the
recompiled game. Threads that look the same are grouped.

Usage: stall_report.py [run.log]      (default port\\work\\run.log)
"""
import bisect
import os
import re
import subprocess
import sys

root = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
log = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "port", "work", "run.log")
game_map = os.path.join(root, "port", "work", "recomp", "librecomp.map")
exe = os.path.join(root, "port", "out", "vox.exe")
symbolizer = os.path.join(root, "tools", "llvm", "bin", "llvm-symbolizer.exe")
IMAGE_BASE = 0x140000000
GAME_BASE = 0x180000000
NOISE = ("bbcpu_hostcall", "bbcpu_call_host", "call_import", "call_other", "tail_other", "bbcpu_step", "bbcpu_run",
         "bbcpu_call_on", "construct_ptd", "thread_start", "__acrt", "replace_current", "std::", "Concurrency::", "_Cnd_", "_Mtx_")

symbols = []
if os.path.exists(game_map):
    for line in open(game_map, encoding="utf-8", errors="replace"):
        m = re.match(r"\s*[0-9a-f]{4}:[0-9a-f]{8}\s+(f_[0-9a-f]+)\s+([0-9a-f]{16})", line)
        if m:
            symbols.append((int(m.group(2), 16) - GAME_BASE, m.group(1)))
symbols = sorted(set(symbols))
addresses = [a for a, _ in symbols]

lines = open(log, encoding="utf-8", errors="replace").read().splitlines()
offsets = sorted({int(m.group(1), 16) for line in lines for m in [re.match(r"STALL  vox\.exe\+([0-9a-f]+)", line, re.I)] if m})
names = {}
if offsets:
    done = subprocess.run([symbolizer, "--obj=" + exe, "--functions=linkage", "--no-inlines"], input="\n".join(hex(IMAGE_BASE + o) for o in offsets), capture_output=True, text=True)
    for offset, block in zip(offsets, done.stdout.strip().split("\n\n")):
        names[offset] = re.sub(r"\(.*", "", block.split("\n")[0])[:40]


def label(frame):
    module, _, offset = frame.partition("+")
    value = int(offset, 16)
    low = module.lower()
    if low == "vox.exe":
        return names.get(value, "?")
    if low == "librecomp.dll":
        i = bisect.bisect_right(addresses, value) - 1
        return "%s+%x" % (symbols[i][1], value - addresses[i]) if i >= 0 else frame
    return "[" + re.sub(r"\.dll$", "", module, flags=re.I) + "]"


reports = 0
groups = None
for line in lines:
    if line.startswith("STALL begin"):
        reports += 1
        print("\n=== report %d: %s" % (reports, line[12:]))
        groups = {}
        thread = None
    elif line.startswith("STALL video") and groups is not None:
        print(line[6:])
    elif line.startswith("STALL thread") and groups is not None:
        m = re.match(r'STALL thread (\d+) "([^"]*)" cpu ([\d.]+) ms', line)
        thread = [m.group(1), m.group(2), float(m.group(3)), []]
        groups[m.group(1)] = thread
    elif line.startswith("STALL  ") and groups is not None and thread is not None:
        name = label(line[7:].strip())
        if not name.startswith(NOISE) and (not thread[3] or thread[3][-1] != name):
            thread[3].append(name)
    elif line.startswith("STALL end") and groups is not None:
        same = {}
        for ident, name, cpu, frames in sorted(groups.values(), key=lambda t: -t[2]):
            same.setdefault((name, " < ".join(frames[:12])), []).append((ident, cpu))
        for (name, chain), who in same.items():
            busy = max(c for _, c in who)
            print('%-3s "%s"%s\n     %s' % ("x%d" % len(who), name, "  CPU %.0f ms" % busy if busy > 1 else "", chain[:330]))
        groups = None
if not reports:
    print("no stall reports in", log)
