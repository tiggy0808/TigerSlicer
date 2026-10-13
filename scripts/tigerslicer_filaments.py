#!/usr/bin/env python3
"""TigerSlicer's filament library: one profile per material, for every printer.

Writes resources/profiles/OrcaFilamentLibrary/filament/<material> @System.json from MATERIALS below
and lists them in OrcaFilamentLibrary.json. The bundle keeps its OrcaFilamentLibrary name because the
app treats that vendor as the printer-independent library.

With --migrate it also removes every brand filament profile (from the library and from every printer
vendor), points each printer's default filaments at the new profiles, and records every removed name in
the new profiles' "renamed_from", so saved presets and projects that name an old profile find the
matching material. The migration was run once, for 0.0.5; running it again finds nothing to remove.

Usage: scripts/tigerslicer_filaments.py [--migrate]   (from the repository root)
"""
import hashlib
import json
import os
import re
import shutil
import sys

PROFILES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "resources", "profiles")
LIBRARY = "OrcaFilamentLibrary"
LIB_DIR = os.path.join(PROFILES, LIBRARY, "filament")
SUFFIX = " @System"

# Unfilled materials. Columns:
#   base: library template to inherit (resources/profiles/OrcaFilamentLibrary/filament/base)
#   id: filament_id (the OGF* ids are the old generic ones, which printer integrations map to)
#   nozzle, nozzle_first, nozzle_low, nozzle_high: nozzle temperatures (°C)
#   bed: smooth/textured/engineering plate (°C); cool: cool plate (0 = not suitable)
#   chamber: chamber temperature for printers that heat it (°C)
#   fan_min, fan_max: part cooling (%); vol: max volumetric speed (mm³/s)
#   density (g/cm³), soft: softening temperature (°C), flow: flow ratio, cost (per kg)
BASE = {
    "PLA":   dict(base="fdm_filament_pla",  id="OGFL99", nozzle=220, nozzle_first=220, nozzle_low=190, nozzle_high=240, bed=60,  cool=35, chamber=0,  fan_min=100, fan_max=100, vol=15,  density=1.24, soft=45,  flow=0.98, cost=20),
    "PETG":  dict(base="fdm_filament_pet",  id="OGFG99", nozzle=245, nozzle_first=245, nozzle_low=220, nozzle_high=260, bed=75,  cool=60, chamber=0,  fan_min=30,  fan_max=60,  vol=12,  density=1.27, soft=70,  flow=0.96, cost=22),
    "PCTG":  dict(base="fdm_filament_pctg", id="OGFG97", nozzle=255, nozzle_first=255, nozzle_low=240, nozzle_high=270, bed=75,  cool=60, chamber=0,  fan_min=20,  fan_max=60,  vol=12,  density=1.23, soft=70,  flow=0.97, cost=25),
    "ABS":   dict(base="fdm_filament_abs",  id="OGFB99", nozzle=255, nozzle_first=260, nozzle_low=240, nozzle_high=270, bed=100, cool=0,  chamber=40, fan_min=10,  fan_max=40,  vol=16,  density=1.04, soft=100, flow=0.95, cost=20),
    "ASA":   dict(base="fdm_filament_asa",  id="OGFB98", nozzle=260, nozzle_first=260, nozzle_low=240, nozzle_high=280, bed=100, cool=0,  chamber=40, fan_min=10,  fan_max=40,  vol=14,  density=1.07, soft=100, flow=0.95, cost=24),
    "PC":    dict(base="fdm_filament_pc",   id="OGFC99", nozzle=270, nozzle_first=270, nozzle_low=260, nozzle_high=290, bed=110, cool=0,  chamber=50, fan_min=0,   fan_max=30,  vol=12,  density=1.20, soft=130, flow=0.94, cost=30),
    "PA6":   dict(base="fdm_filament_pa",   id="OGFN99", nozzle=270, nozzle_first=275, nozzle_low=250, nozzle_high=290, bed=90,  cool=0,  chamber=50, fan_min=0,   fan_max=30,  vol=10,  density=1.14, soft=108, flow=0.96, cost=35),
    "PA12":  dict(base="fdm_filament_pa",   id="TSFN12", nozzle=255, nozzle_first=260, nozzle_low=240, nozzle_high=280, bed=80,  cool=0,  chamber=40, fan_min=0,   fan_max=30,  vol=10,  density=1.01, soft=90,  flow=0.96, cost=40),
    "PA612": dict(base="fdm_filament_pa",   id="TSFN61", nozzle=260, nozzle_first=265, nozzle_low=240, nozzle_high=280, bed=80,  cool=0,  chamber=40, fan_min=0,   fan_max=30,  vol=10,  density=1.06, soft=100, flow=0.96, cost=40),
    "PPA":   dict(base="fdm_filament_ppa",  id="TSFN95", nozzle=300, nozzle_first=300, nozzle_low=290, nozzle_high=320, bed=100, cool=0,  chamber=60, fan_min=0,   fan_max=30,  vol=6,   density=1.17, soft=180, flow=0.96, cost=60),
    "PPS":   dict(base="fdm_filament_pps",  id="TSFS50", nozzle=320, nozzle_first=320, nozzle_low=310, nozzle_high=340, bed=110, cool=0,  chamber=60, fan_min=0,   fan_max=30,  vol=6,   density=1.35, soft=200, flow=0.96, cost=80),
    "PEI":   dict(base="fdm_filament_pps",  id="TSFE10", nozzle=380, nozzle_first=380, nozzle_low=360, nozzle_high=400, bed=120, cool=0,  chamber=90, fan_min=0,   fan_max=10,  vol=5,   density=1.34, soft=150, flow=0.96, cost=150),
    "PEKK":  dict(base="fdm_filament_pps",  id="TSFE20", nozzle=360, nozzle_first=360, nozzle_low=340, nozzle_high=380, bed=120, cool=0,  chamber=90, fan_min=0,   fan_max=0,   vol=5,   density=1.27, soft=150, flow=0.96, cost=200),
    "PEEK":  dict(base="fdm_filament_pps",  id="TSFE30", nozzle=400, nozzle_first=400, nozzle_low=380, nozzle_high=420, bed=120, cool=0,  chamber=90, fan_min=0,   fan_max=0,   vol=5,   density=1.30, soft=150, flow=0.96, cost=250),
    "PP":    dict(base="fdm_filament_pp",   id="OGFP97", nozzle=230, nozzle_first=235, nozzle_low=220, nozzle_high=250, bed=80,  cool=0,  chamber=0,  fan_min=20,  fan_max=50,  vol=10,  density=0.90, soft=90,  flow=0.98, cost=30),
}
# Carbon and glass fibre versions: hotter, slower, heavier, and they need a hardened nozzle.
FIBRE_DENSITY = {  # (CF, GF)
    "PLA": (1.29, 1.38), "PETG": (1.30, 1.38), "PCTG": (1.27, 1.35), "ABS": (1.10, 1.13), "ASA": (1.12, 1.16),
    "PC": (1.22, 1.30), "PA6": (1.20, 1.33), "PA12": (1.06, 1.20), "PA612": (1.11, 1.25), "PPA": (1.21, 1.30),
    "PPS": (1.26, 1.55), "PEI": (1.38, 1.50), "PEKK": (1.36, 1.50), "PEEK": (1.40, 1.50), "PP": (0.98, 1.05),
}
FIBRE_IDS = {  # (CF, GF)
    "PLA": ("OGFL98", "TSFL97"), "PETG": ("TSFG98", "TSFG96"), "PCTG": ("TSFG95", "TSFG94"), "ABS": ("TSFB97", "TSFB96"),
    "ASA": ("TSFB95", "TSFB94"), "PC": ("TSFC98", "TSFC97"), "PA6": ("OGFN98", "TSFN97"), "PA12": ("TSFN13", "TSFN14"),
    "PA612": ("TSFN62", "TSFN63"), "PPA": ("OGFN97", "OGFN96"), "PPS": ("TSFS51", "TSFS52"), "PEI": ("TSFE11", "TSFE12"),
    "PEKK": ("TSFE21", "TSFE22"), "PEEK": ("TSFE31", "TSFE32"), "PP": ("TSFP98", "TSFP96"),
}
# Materials without fibre versions.
OTHER = {
    "TPU 95A": dict(base="fdm_filament_tpu",  id="OGFU99", type="TPU",  nozzle=225, nozzle_first=225, nozzle_low=210, nozzle_high=240, bed=45, cool=35, chamber=0,  fan_min=100, fan_max=100, vol=3.6, density=1.21, soft=30, flow=1.0,  cost=30),
    "TPU 90A": dict(base="fdm_filament_tpu",  id="TSFU90", type="TPU",  nozzle=220, nozzle_first=220, nozzle_low=205, nozzle_high=235, bed=45, cool=35, chamber=0,  fan_min=100, fan_max=100, vol=2.5, density=1.18, soft=30, flow=1.0,  cost=32),
    "TPU 85A": dict(base="fdm_filament_tpu",  id="TSFU85", type="TPU",  nozzle=215, nozzle_first=215, nozzle_low=200, nozzle_high=230, bed=45, cool=35, chamber=0,  fan_min=100, fan_max=100, vol=1.8, density=1.16, soft=30, flow=1.0,  cost=35),
    "PVA":     dict(base="fdm_filament_pva",  id="OGFS99", type="PVA",  nozzle=210, nozzle_first=215, nozzle_low=190, nozzle_high=230, bed=60, cool=45, chamber=0,  fan_min=50,  fan_max=100, vol=10,  density=1.23, soft=55, flow=0.95, cost=60),
    "BVOH":    dict(base="fdm_filament_bvoh", id="OGFS97", type="BVOH", nozzle=215, nozzle_first=220, nozzle_low=190, nozzle_high=240, bed=60, cool=45, chamber=0,  fan_min=50,  fan_max=100, vol=8,   density=1.14, soft=60, flow=1.0,  cost=80),
    "HIPS":    dict(base="fdm_filament_hips", id="OGFS98", type="HIPS", nozzle=240, nozzle_first=240, nozzle_low=220, nozzle_high=260, bed=100, cool=0, chamber=40, fan_min=10,  fan_max=40,  vol=8,   density=1.04, soft=90, flow=1.0,  cost=25),
}


def materials():
    """Every library profile as (name, settings), in the order they are listed."""
    out = []
    for name, m in BASE.items():
        out.append((name, dict(m, type=name, hrc=3)))
        for i, fibre in enumerate(("CF", "GF")):
            out.append((f"{name}-{fibre}", dict(m, type=f"{name}-{fibre}", id=FIBRE_IDS[name][i], hrc=40,
                                               nozzle=m["nozzle"] + 10, nozzle_first=m["nozzle_first"] + 10,
                                               nozzle_low=m["nozzle_low"] + 10, nozzle_high=m["nozzle_high"] + 10,
                                               fan_max=min(m["fan_max"], 40), vol=round(m["vol"] * 0.8, 1),
                                               density=FIBRE_DENSITY[name][i], cost=round(m["cost"] * 1.6))))
    for name, m in OTHER.items():
        out.append((name, dict(m, hrc=3)))
    return out


def profile_json(name, m, renamed_from):
    s = lambda v: [str(v)]
    temp = lambda v: [str(int(v))]
    j = {
        "type": "filament",
        "name": name + SUFFIX,
        "inherits": m["base"],
        "from": "system",
        "setting_id": "TS" + hashlib.md5(name.encode()).hexdigest()[:14],
        "filament_id": m["id"],
        "instantiation": "true",
        "filament_vendor": ["Generic"],
        "filament_type": [m["type"]],
        "nozzle_temperature": temp(m["nozzle"]),
        "nozzle_temperature_initial_layer": temp(m["nozzle_first"]),
        "nozzle_temperature_range_low": temp(m["nozzle_low"]),
        "nozzle_temperature_range_high": temp(m["nozzle_high"]),
        "chamber_temperatures": temp(m["chamber"]),
        "fan_min_speed": s(m["fan_min"]),
        "fan_max_speed": s(m["fan_max"]),
        "filament_max_volumetric_speed": s(m["vol"]),
        "filament_density": s(m["density"]),
        "filament_flow_ratio": s(m["flow"]),
        "filament_cost": s(m["cost"]),
        "temperature_vitrification": temp(m["soft"]),
        "required_nozzle_HRC": s(m["hrc"]),
        "compatible_printers": [],
    }
    for plate in ("hot_plate_temp", "textured_plate_temp", "eng_plate_temp"):
        j[plate] = j[plate + "_initial_layer"] = temp(m["bed"])
    j["cool_plate_temp"] = j["cool_plate_temp_initial_layer"] = temp(m["cool"])
    if renamed_from:
        j["renamed_from"] = ";".join('"' + n.replace("\\", "\\\\").replace('"', '\\"') + '"' for n in sorted(renamed_from))
    return j


def material_for(filament_type, name):
    """The library profile that replaces an old profile of this type."""
    t = filament_type.upper().replace("_", "-").strip()
    n = name.upper()
    fibre = next(("-" + tag for tag in ("CF", "GF") for text in (t, n) if re.search(rf"(^|[^A-Z]){tag}\d*($|[^A-Z])", text)), "")
    if "TPU" in t or "FLEX" in t or "TPE" in t or "PEBA" in t:
        return "TPU 85A" if "85A" in n else "TPU 90A" if "90A" in n else "TPU 95A"
    if t in ("PVA",) or "SUPPORT" in n and "PVA" in n:
        return "PVA"
    if t == "BVOH":
        return "BVOH"
    if t == "HIPS" or t.endswith("-S") or t.startswith("S-") or "SUPPORT" in t or "BREAKAWAY" in t:
        return "HIPS" if t in ("HIPS",) else "PVA"
    for key, prefixes in (("PEEK", ("PEEK",)), ("PEKK", ("PEKK",)), ("PEI", ("PEI", "ULTEM", "PES", "PPSU", "PSU", "PI", "TPI")),
                          ("PPS", ("PPS",)), ("PPA", ("PPA", "HPP")), ("PA612", ("PA612",)), ("PA12", ("PA12", "PA11")),
                          ("PA6", ("PA", "ULTRAPA", "NYLON")), ("PCTG", ("PCTG",)), ("PC", ("PC",)),
                          ("ASA", ("ASA",)), ("ABS", ("ABS",)), ("PETG", ("PETG", "PET")), ("PP", ("PP", "PE"))):
        if any(t.startswith(p) for p in prefixes):
            return key + fibre if key in BASE else key
    return "PLA" + fibre if fibre else "PLA"


def load(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def save(path, data, like=None):
    """Write JSON, keeping the indentation, line endings and final newline of the file it replaces."""
    indent, newline, final = 4, "\n", "\n"
    if like:
        with open(like, encoding="utf-8", newline="") as f:
            text = f.read()
        m = re.search(r"\n([ \t]+)\"", text)
        indent = m.group(1) if m else indent
        newline = "\r\n" if "\r\n" in text else "\n"
        final = newline if text.endswith("\n") else ""
    text = json.dumps(data, indent=indent, ensure_ascii=False).replace("\n", newline) + final
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def bump(version):
    parts = version.split(".")
    parts[-1] = str(int(parts[-1]) + 1).zfill(len(parts[-1]))
    return ".".join(parts)


def vendors():
    return sorted(f[:-5] for f in os.listdir(PROFILES) if f.endswith(".json") and f != "blacklist.json")


def collect_old_profiles():
    """name -> filament_type of every instantiable filament profile in every vendor."""
    profiles = {}
    for v in vendors():
        bundle = load(os.path.join(PROFILES, v + ".json"))
        for entry in bundle.get("filament_list", []):
            path = os.path.join(PROFILES, v, entry["sub_path"])
            if os.path.exists(path):
                profiles[(v, entry["name"])] = load(path)

    def resolve(v, name, depth=0):
        p = profiles.get((v, name)) or profiles.get((LIBRARY, name))
        if p is None or depth > 20:
            return {}
        out = resolve(v, p["inherits"], depth + 1) if p.get("inherits") else {}
        out.update(p)
        return out

    old = {}
    for (v, name), p in profiles.items():
        if p.get("instantiation") == "false":
            continue
        t = resolve(v, name).get("filament_type", ["PLA"])
        old.setdefault(name, t[0] if isinstance(t, list) else t)
    return old


def migrate(old):
    new_names = {name for name, _ in materials()}
    mapping = {name: material_for(t, name) for name, t in old.items()}
    to_new = lambda n: (mapping.get(n) or material_for("", n)) + SUFFIX

    for v in vendors():
        bundle_path = os.path.join(PROFILES, v + ".json")
        bundle = load(bundle_path)
        if v != LIBRARY:
            shutil.rmtree(os.path.join(PROFILES, v, "filament"), ignore_errors=True)
            bundle["filament_list"] = []
            for entry in bundle.get("machine_list", []):
                path = os.path.join(PROFILES, v, entry["sub_path"])
                machine = load(path)
                if machine.get("default_filament_profile"):
                    names = machine["default_filament_profile"]
                    names = names if isinstance(names, list) else [names]
                    machine["default_filament_profile"] = list(dict.fromkeys(to_new(n) for n in names))
                    save(path, machine, like=path)
            for entry in bundle.get("machine_model_list", []):
                path = os.path.join(PROFILES, v, entry["sub_path"])
                model = load(path)
                if model.get("default_materials"):
                    names = [n for n in model["default_materials"].split(";") if n.strip()]
                    model["default_materials"] = ";".join(dict.fromkeys(to_new(n.strip()) for n in names))
                    save(path, model, like=path)
        bundle["version"] = bump(bundle["version"])
        save(bundle_path, bundle, like=bundle_path)

    renamed = {}
    for old_name, new in mapping.items():
        if old_name != new + SUFFIX and new in new_names:
            renamed.setdefault(new, set()).add(old_name)
    return renamed


def write_library(renamed):
    bundle_path = os.path.join(PROFILES, LIBRARY + ".json")
    bundle = load(bundle_path)
    for entry in os.listdir(LIB_DIR):
        if entry != "base":
            path = os.path.join(LIB_DIR, entry)
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
    base_entries = [e for e in bundle["filament_list"] if e["sub_path"].startswith("filament/base/")]
    entries = []
    for name, m in materials():
        file_name = name + SUFFIX + ".json"
        save(os.path.join(LIB_DIR, file_name), profile_json(name, m, renamed.get(name, set())))
        entries.append({"name": name + SUFFIX, "sub_path": "filament/" + file_name})
    bundle["filament_list"] = base_entries + entries
    save(bundle_path, bundle, like=bundle_path)


def main():
    renamed = {}
    if "--migrate" in sys.argv:
        renamed = migrate(collect_old_profiles())
    else:
        # Keep the names recorded by the migration.
        for name, _ in materials():
            path = os.path.join(LIB_DIR, name + SUFFIX + ".json")
            if os.path.exists(path) and load(path).get("renamed_from"):
                renamed[name] = set(re.findall(r'"((?:[^"\\]|\\.)*)"', load(path)["renamed_from"]))
    write_library(renamed)
    print(f"{len(materials())} filament profiles written; {sum(len(v) for v in renamed.values())} old names mapped.")


if __name__ == "__main__":
    main()
