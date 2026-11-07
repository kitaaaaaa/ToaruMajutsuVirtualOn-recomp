"""Exports every robot's bodies, weapons and low-detail models with hmd2glb.py, and writes MANIFEST.txt.

Usage: export_models.py <cpk_out/root+> <out dir> [VR000 VR001 ...]

Extract chara/VR* and motion/* with cpk.py first. The output comes from the game: keep it out of the repository.

Colours: C00 and C01 are colour types A and B, C02 and C03 the VOOT colours, C04 the special colour, and H the hyper
version. A weapon with no texture bank of its own uses the one the game's mdl_param or decken_model table pairs it with,
or else the nearest weapon's with the same material layout; the manifest records which. Loose weapon motions go to the
weapon they name and to any with the same skeleton; banks go to any weapon with a skeleton they fit.

The armed folder holds each body with its weapons bound to the mount bones at its hands, the unskinned leaf bones below
each shoulder, so they follow every motion. Each colour's first weapon in mdl_param goes in the right hand; ARMED_LEFT
lists the robots that also hold their second in the left. A rigged weapon stays in its rest form there.
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
ARMED_LEFT = ("VR008", "VR009")


def table_pairs(path):
    """The (model, texture) pairs in a mdl_param or decken_model table, in order, without extensions."""
    names = re.findall(rb"[A-Za-z0-9_]+\.(?:hmd|htx)", open(path, "rb").read()) if os.path.exists(path) else []
    return [(names[i][:-4].decode(), names[i + 1][:-4].decode()) for i in range(0, len(names) - 1, 2)]


def colour_rows(root, robot):
    """{body texture bank: [(weapon, its bank), ...]} from the mdl_param tables, keeping what's on the disc."""
    number_ = int(robot[2:])
    rows = {}
    for name in ("mdl_param_vr%03d" % number_, "mdl_param_vr%03d_h" % number_):
        current = None
        for model, bank in table_pairs(os.path.join(root, "param", name + ".prm")):
            if re.fullmatch(r"VR\d+_\d\d", model):
                current = rows.setdefault(bank, []) if model.startswith(robot) and bank not in rows else None
            elif current is not None and model.startswith(robot) and "_WP99" not in model and (model, bank) not in current:
                if os.path.exists(os.path.join(root, "chara", robot, model + ".hmd")) and os.path.exists(os.path.join(root, "chara", robot, bank + ".htx")):
                    current.append((model, bank))
    return rows


def hand_mounts(model, robot):
    """The (right, left) mount bones: the unskinned leaf furthest down each arm."""
    swings = H.mirrored_shoulders(model, H.APOSE[robot][-1:])
    skinned = set(model.skin()[0])
    parents = {n["parent"] for n in model.nodes[1:]}
    world = model.rest_world()

    def mount(shoulder):
        below = {shoulder}
        for i in range(1, len(model.nodes)):
            if model.nodes[i]["parent"] in below:
                below.add(i)
        leaves = [i for i in below if i not in skinned and i not in parents]
        return max(leaves, key=lambda i: np.linalg.norm(world[i][:3, 3] - world[shoulder][:3, 3]))

    left, right = (mount(n) for n in sorted(swings, key=lambda n: -swings[n]))
    return right, left


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
        for m, bank in table_pairs(os.path.join(root, "param", name + ".prm")):
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
            weapons = rows.get(bank, [])
            if is_body and not low and weapons:
                right, left = hand_mounts(model, robot)
                held = [(right, weapons[0])] + ([(left, weapons[1])] if robot in ARMED_LEFT and len(weapons) > 1 else [])
                attach = ["+%d|%s|%s" % (mount, os.path.join(chara, w + ".hmd"), os.path.join(chara, b + ".htx")) for mount, (w, b) in held]
                worn = " ".join("%s on bone %d" % (w[6:], mount) for mount, (w, _) in held)
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
