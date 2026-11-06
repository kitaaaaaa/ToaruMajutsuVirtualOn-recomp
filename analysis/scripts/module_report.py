"""Report a PS4 module's layout: segments, dynamic tags, TLS, relocation types, imports by library.

Usage: module_report.py <bloodborne-recomp tools dir> <module>...
"""
import collections
import pathlib
import struct
import sys

sys.path.insert(0, sys.argv[1])
import ps4elf  # noqa: E402

for arg in sys.argv[2:]:
    m = ps4elf.module(pathlib.Path(arg))
    print("==", pathlib.Path(arg).name, "e_type=%#x entry=%#x" % (m["header"][2 - 1], m["header"][4]))
    for p in m["ph"]:
        print("  ph type=%#010x flags=%d off=%#x va=%#x filesz=%#x memsz=%#x align=%#x" % (
            p["type"], p["flags"], p["offset"], p["vaddr"], p["filesz"], p["memsz"], p["align"]))
    tags = m["tags"]
    print("  tags:", " ".join("%#x=%#x" % (k, v) for k, v in sorted(tags.items())))
    print("  relocation types:", dict(collections.Counter(t for _, t, _, _ in m["relocs"])))
    syms = m["symbols"]
    exports = [s for s in syms if s["section"] and s["binding"] in (1, 2)]
    imports = [s for s in syms if not s["section"] and s["name"]]
    print("  symbols=%d exports=%d undefined=%d" % (len(syms), len(exports), len(imports)))
    libs = collections.Counter(s["identity"][1][0] if s["identity"] else "(plain)" for s in imports)
    print("  imports by library:", libs.most_common())
    print("  modules:", {k: v for k, v in m["modules"].items()})
    plain = [s["name"] for s in syms if s["name"] and not s["identity"]]
    print("  plain names:", plain[:20])
