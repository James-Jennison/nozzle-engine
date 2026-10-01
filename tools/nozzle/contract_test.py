#!/usr/bin/env python3
"""Checks the engine contract Nozzle It All builds its settings screens on: --option-states and --config-checks give
Orca's GUI behaviour on real printer profiles, and nozzle/contract/setting_aliases.json only names real settings.

Usage: contract_test.py ENGINE   (NOZZLE_ROOT = a Nozzle It All checkout, for the bundled printer profiles)
"""
import json, os, subprocess, sys, tempfile

ENGINE = sys.argv[1]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
NOZZLE = os.environ.get("NOZZLE_ROOT", "/mnt/faststorage/Nozzle It All")
PACKS = os.path.join(NOZZLE, "app/src/main/assets/slicer_profiles")
failures = []


def call(mode, profile, overrides=None, **extra):
    d = os.path.join(PACKS, profile)
    req = {"profiles": [os.path.join(d, f) for f in ("machine.json", "filament.json", "process.json")],
           "overrides": overrides or {}, "printerName": profile, **extra}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
        json.dump(req, f)
    try:
        out = subprocess.run([ENGINE, mode, f.name], capture_output=True, text=True, timeout=120)
    finally:
        os.unlink(f.name)
    if out.returncode != 0:
        failures.append(f"{mode} {profile} {overrides}: exit {out.returncode}: {out.stdout[-300:]}")
        return {"states": {}, "forced": {}, "conflicts": [], "notices": []}
    return json.loads(out.stdout)


def expect(label, cond):
    if not cond:
        failures.append(label)


# Process rules (ConfigManipulation::toggle_print_fff_options).
r = call("--option-states", "snapmaker_u1", {"wall_loops": "0", "sparse_infill_density": "0%", "enable_support": "1",
                                             "support_type": "tree(auto)", "support_style": "organic",
                                             "max_volumetric_extrusion_rate_slope": "5"}, scope="process")
s = r["states"]
expect("no walls greys out outer wall speed", s.get("outer_wall_speed", {}).get("enabled") is False)
expect("0% infill hides the infill pattern", s.get("sparse_infill_pattern", {}).get("visible") is False)
expect("organic tree support shows tip diameter", s.get("tree_support_tip_diameter", {}).get("visible") is True)
expect("organic tree support hides normal-tree branch angle", s.get("tree_support_branch_angle", {}).get("visible") is False)
expect("extrusion-rate smoothing forces arc fitting off", r["forced"].get("enable_arc_fitting") == "0")

# Printer and filament rules (TabPrinter / TabFilament::toggle_options), per extruder.
r = call("--option-states", "prusa_xl_5t", {"use_firmware_retraction": "1"}, scope="all")
s = r["states"]
expect("firmware retraction greys out retraction length on extruder 0", s.get("retract_length#0", {}).get("enabled") is False)
expect("five extruders get per-extruder states", all(f"retract_length#{i}" in s for i in range(5)))
expect("Marlin 2 shows junction deviation", s.get("machine_max_junction_deviation", {}).get("visible") is True)
expect("wipe with firmware retraction is reported as a conflict with two choices",
       any(c["id"] == "wipe_with_firmware_retraction" and len(c["choices"]) == 2 for c in r["conflicts"]))

# Fix-ups (ConfigManipulation::update_print_fff_config).
r = call("--config-checks", "generic_klipper", {"layer_height": "0", "spiral_mode": "1", "wall_loops": "3"})
ids = {n["id"] for n in r["notices"]} | {c["id"] for c in r["conflicts"]}
expect("zero layer height is reset to 0.2", any(n["id"] == "layer_height_too_small" and n["changes"].get("layer_height") == "0.2"
                                               for n in r["notices"]))
expect("spiral vase with 3 walls asks what to change", "spiral_mode_settings" in ids)
r = call("--config-checks", "generic_klipper", {"fuzzy_skin_mode": "extrusion", "wall_generator": "classic"})
expect("extrusion fuzzy skin without Arachne asks", any(c["id"] == "fuzzy_skin_needs_arachne" for c in r["conflicts"]))
r = call("--config-checks", "generic_klipper")
expect("a stock profile has no conflicts", r["conflicts"] == [])

# Which process presets a printer offers (Orca's is_compatible_with_printer): its name in compatible_printers, else
# compatible_printers_condition evaluated against the printer, else every printer.
with tempfile.TemporaryDirectory() as t:
    def write(name, data):
        path = os.path.join(t, name + ".json")
        json.dump(data, open(path, "w"))
        return path
    printer = write("printer", {"printer_notes": "PRINTER_VENDOR_PRUSA3D PRINTER_MODEL_COREONE HF_NOZZLE", "nozzle_diameter": ["0.4"]})
    presets = [write("listed", {"compatible_printers": ["Test Printer 0.4 nozzle"]}),
               write("other", {"compatible_printers": ["Some Other Printer"]}),
               write("cond_yes", {"compatible_printers": [], "compatible_printers_condition":
                                  "printer_notes=~/.*PRINTER_MODEL_COREONE[^_a-zA-Z0-9].*/ and nozzle_diameter[0]==0.4 and printer_notes=~/.*HF_NOZZLE.*/"}),
               write("cond_no", {"compatible_printers": [], "compatible_printers_condition": "nozzle_diameter[0]==0.6"}),
               write("cond_preset", {"compatible_printers_condition": "printer_preset==\"Test Printer 0.4 nozzle\" and num_extruders==1"}),
               write("everyone", {})]
    with open(os.path.join(t, "req.json"), "w") as f:
        json.dump({"printer": printer, "printerName": "Test Printer 0.4 nozzle", "type": "print", "presets": presets}, f)
    out = subprocess.run([ENGINE, "--compatible-presets", f.name], capture_output=True, text=True, timeout=60)
    got = json.loads(out.stdout).get("compatible") if out.returncode == 0 else out.stdout
    expect(f"compatible presets: list, condition and no-restriction rules (got {got})", got == [True, False, True, False, True, True])

# Aliases name only real settings and choice values.
schema = json.loads(subprocess.run([ENGINE, "--schema"], capture_output=True, text=True).stdout)
opts = {o["key"]: o for o in schema["options"]}
aliases = json.load(open(os.path.join(ROOT, "nozzle/contract/setting_aliases.json")))
for key in aliases["settings"]:
    expect(f"alias key {key} is in the schema", key in opts)
for kv in aliases["choices"]:
    key, _, value = kv.partition("=")
    expect(f"alias choice {kv} is in the schema", key in opts and value in [c["value"] for c in opts[key].get("choices", [])])

if failures:
    print("contract: FAILED")
    for f in failures:
        print("  -", f)
    sys.exit(1)
print(f"contract: all checks pass ({len(aliases['settings'])} aliased settings)")
