"""Exports every robot's bodies, weapons and low-detail models with hmd2glb.py, and writes MANIFEST.txt.

Usage: export_models.py <cpk_out/root+> <out dir> [VR000 VR001 ...]

Extract chara/VR* and motion/* with cpk.py first. The output comes from the game: keep it out of the repository.

Colours: C00 and C01 are colour types A and B, C02 and C03 the VOOT colours, C04 the special colour, and H the hyper
version. A weapon with no texture bank of its own uses the one the game's mdl_param or decken_model table pairs it with,
or else the nearest weapon's with the same material layout; the manifest records which. Loose weapon motions go to the
weapon they name and to any with the same skeleton; banks go to any weapon with a skeleton they fit.

The armed folder holds each body with the weapons the game shows by default, mounted as in MOUNTS, so they follow every
motion. A rigged weapon stays in its rest form there. MODEL_FORMAT_NOTES.md says where MOUNTS comes from.
"""
import concurrent.futures
import glob
import os
import re
import struct
import subprocess
import sys

import numpy as np

import hmd2glb as H

SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "hmd2glb.py")
LABELS = {"C00": "ColorA", "C01": "ColorB", "C02": "VOOT_A", "C03": "VOOT_B", "C04": "Special"}
BODY_BANKS = ("", "_Upper", "_menu_idling")
# The game's per-robot weapon setup, for the weapons on at the start: (slot in the colour's mdl_param row, bone ids).
# Several ids bind the weapon's parts one by one. Slots mounted at id 0 are left out: they're empty markers or effects
# the game places itself, and PPRIMA's floating units work that way, so it has no armed model. Blue Stalker has no setup
# of its own; it runs Cypher's.
MOUNTS = {
    "VR000": [(0, (200,))],
    "VR001": [(0, (200,)), (0, (201,))],
    "VR002": [(0, (200,)), (4, (202,))],
    "VR003": [(0, (200,))],
    "VR004": [(0, (6,)), (0, (11,))],
    "VR005": [(0, (200,))],
    "VR006": [(0, (200,)), (1, (102, 103))],
    "VR007": [(0, (200,))],
    "VR008": [(0, (7,)), (1, (12,))],
    "VR009": [(0, (200,)), (1, (201,))],
    "VR010": [(0, (200,))],
    "VR011": [(0, (200,)), (1, (202,))],
    "VR012": [(0, (200,))],
    "VR013": [(0, (200,))],
}


def table_pairs(path):
    """The (model, texture) pairs in a mdl_param or decken_model table, in order, without extensions. None for an empty slot."""
    names = re.findall(rb"[A-Za-z0-9_]+\.(?:hmd|htx)|(?<=\x02)-1", open(path, "rb").read()) if os.path.exists(path) else []
    pairs = [(names[i], names[i + 1]) for i in range(0, len(names) - 1, 2)]
    return [(m[:-4].decode(), t[:-4].decode()) if m != b"-1" and t != b"-1" else None for m, t in pairs]


def colour_rows(root, robot):
    """{body texture bank: the 16 weapon slots of its row} from the mdl_param tables. A row is a body and 16 pairs."""
    rows = {}
    for name in ("mdl_param_vr%03d" % int(robot[2:]), "mdl_param_vr%03d_h" % int(robot[2:])):
        pairs = table_pairs(os.path.join(root, "param", name + ".prm"))
        for i in range(0, len(pairs) - 16, 17):
            if pairs[i] and pairs[i][0].startswith(robot) and pairs[i][1] not in rows:
                rows[pairs[i][1]] = pairs[i + 1 : i + 17]
    return rows


def label(stem, bank):
    """The file name part for a texture bank: its colour number, colour name and hyper mark."""
    m = re.search(r"_(C\d\d)(H?)$", bank)
    if not m:
        return stem
    name = LABELS.get(m.group(1))
    return "%s_%s%s" % (stem, m.group(1) + m.group(2), "_" + name + ("_Hyper" if m.group(2) else "") if name else "")


def number(stem):
    m = re.search(r"_WP(\d+)", stem)
    return int(m.group(1)) if m else -1


def same_skeleton(a, b):
    n = min(len(a.nodes), len(b.nodes))
    return abs(len(a.nodes) - len(b.nodes)) <= 1 and all(np.abs(np.array(a.nodes[i]["t"]) - np.array(b.nodes[i]["t"])).max() < 1e-3 for i in range(n))


def fits_weapon(model, d, at):
    return H.u32(d, at + 0x44) <= len(model.nodes) and H.motion_fits(model, d, at)


def plan_robot(root, out, robot):
    """Export jobs for one robot, and notes on what couldn't be placed."""
    chara = os.path.join(root, "chara", robot)
    motion = os.path.join(root, "motion", robot)
    stems = sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(chara, "*.hmd")))
    banks = sorted(os.path.basename(p)[:-4] for p in glob.glob(os.path.join(chara, "*.htx")))
    models = {s: H.Model(os.path.join(chara, s + ".hmd")) for s in stems}
    layout = {s: tuple(models[s].material(a)[1] for a, _ in models[s].meshes) for s in stems}
    body_motions = [os.path.join(motion, robot + b + ".mbm") for b in BODY_BANKS]
    extra = [p for p in sorted(glob.glob(os.path.join(motion, "*.mbm"))) if p not in body_motions]
    loose = sorted(glob.glob(os.path.join(motion, "*.hmt")))

    def own_banks(stem):
        return [b for b in banks if b == stem or re.fullmatch(re.escape(stem) + r"_C\d\dH?", b)]

    number_ = int(robot[2:])
    paired = {}
    for name in ("mdl_param_vr%03d" % number_, "mdl_param_vr%03d_h" % number_, "decken_model_vr%03d" % number_):
        for pair in filter(None, table_pairs(os.path.join(root, "param", name + ".prm"))):
            m, bank = pair
            if bank in banks and bank not in paired.setdefault(m, []):
                paired[m].append(bank)
    rows = colour_rows(root, robot)
    jobs, notes = [], []
    placed = set()
    for stem in stems:
        model = models[stem]
        is_body = re.fullmatch(r"VR\d{3}_(\d\d|Z)", stem) is not None
        is_weapon = "_WP" in stem
        low = stem.endswith("_Z") or (is_weapon and stem.endswith("Z"))
        folder = os.path.join(out, robot, "low_lod" if low else "" if is_body else "weapons" if is_weapon else "other")
        use = own_banks(stem)
        note = ""
        if not use and paired.get(stem):
            use = paired[stem]
            note = "textures paired by the game's table: " + " ".join(use)
        elif not use:
            # Same layout first, then a bank of the weapon this one is a variant of; nearest number wins.
            owners = sorted({s for s in stems if s != stem and own_banks(s) and layout[s] == layout[stem]}, key=lambda s: (abs(number(s) - number(stem)), s))
            base = stem.rsplit("_", 1)[0]
            if base in banks or own_banks(base):
                owners.insert(0, base)
            if owners:
                use = own_banks(owners[0])
                note = "textures borrowed from " + owners[0]
            else:
                note = "no textures found"
        motions = []
        if is_body:
            motions = [p for p in body_motions if os.path.exists(p)]
        elif is_weapon:
            names = set()
            for path in extra:
                d, entries = H.read_bank(path)
                hits = [n for _, n, at in entries if fits_weapon(model, d, at)]
                if hits:
                    motions.append(path)
                    names |= set(hits)
                    placed |= {(path, n) for n in hits}
            for path in loose:
                name = os.path.basename(path)
                m = re.match(r"vr\d+_(wp\d+)_", name)
                if not m:
                    continue
                owner = "%s_%s" % (robot, m.group(1).upper())
                d = open(path, "rb").read()
                related = stem.startswith(owner) or (owner in models and same_skeleton(model, models[owner]))
                if related and H.u32(d, 0x44) in (len(model.nodes), len(model.nodes) - 1):
                    placed.add((path, name))
                    if name not in names:
                        motions.append(path)
        for bank in use or ["-"]:
            target = label(stem, bank) if bank != "-" else stem
            jobs.append((stem, os.path.join(chara, stem + ".hmd"), os.path.join(chara, bank + ".htx") if bank != "-" else "-", os.path.join(folder, target + ".glb"), motions, note))
            slots = rows.get(bank)
            if is_body and not low and slots and robot in MOUNTS:
                held = [(ids, slots[slot]) for slot, ids in MOUNTS[robot] if slots[slot] and slots[slot][1] in banks and slots[slot][0] in models]
                attach = ["+%s|%s|%s" % (",".join(map(str, ids)), os.path.join(chara, w + ".hmd"), os.path.join(chara, b + ".htx")) for ids, (w, b) in held]
                worn = " ".join("%s on id %s" % (w[6:], ",".join(map(str, ids))) for ids, (w, _) in held)
                jobs.append((stem, os.path.join(chara, stem + ".hmd"), os.path.join(chara, bank + ".htx"), os.path.join(out, robot, "armed", target + ".glb"), motions + attach, worn))
    for path in loose:
        if (path, os.path.basename(path)) not in placed:
            notes.append("%s: motion %s fits no weapon, not exported" % (robot, os.path.basename(path)))
    for path in extra:
        d, entries = H.read_bank(path)
        left = [n for _, n, _ in entries if (path, n) not in placed]
        if left:
            notes.append("%s: %d of %d motions in %s fit no weapon, not exported: %s" % (robot, len(left), len(entries), os.path.basename(path), " ".join(left[:12])))
    return jobs, notes


def run(job):
    stem, model, texture, target, motions, note = job
    os.makedirs(os.path.dirname(target), exist_ok=True)
    args = target + ".motions"
    with open(args, "w", encoding="utf8") as f:
        f.write("\n".join(motions))
    result = subprocess.run([sys.executable, SCRIPT, model, texture, target, "@" + args], capture_output=True, text=True)
    os.remove(args)
    lines = result.stdout.strip().splitlines()
    if result.returncode:
        return target, "FAILED " + result.stderr.strip().splitlines()[-1], note, []
    return target, lines[-1].split(": ")[-1], note, lines[:-1]


def main():
    root, out = sys.argv[1:3]
    robots = sys.argv[3:] or sorted(os.path.basename(p) for p in glob.glob(os.path.join(root, "chara", "VR*")))
    jobs, notes = [], []
    for robot in robots:
        j, n = plan_robot(root, out, robot)
        jobs += j
        notes += n
        print(robot, len(j), "files planned", flush=True)
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        results = list(pool.map(run, jobs))
    with open(os.path.join(out, "MANIFEST.txt"), "w", encoding="utf8") as f:
        failed = 0
        for target, summary, note, extra in results:
            failed += summary.startswith("FAILED")
            f.write("%s: %s%s\n" % (os.path.relpath(target, out), summary, " (" + note + ")" if note else ""))
            skipped = [e for e in extra if e.startswith("skipped")]
            if skipped:
                f.write("    %d motions for another skeleton skipped: %s\n" % (len(skipped), " ".join(e.split(": ")[-1] for e in skipped[:10])))
            for e in extra:
                if not e.startswith("skipped"):
                    f.write("    " + e + "\n")
        f.write("\nNot exported:\n")
        for n in notes:
            f.write(n + "\n")
    print(len(results), "files,", failed, "failed; see MANIFEST.txt")


if __name__ == "__main__":
    main()
