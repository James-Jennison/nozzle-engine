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

# Setting Overrides page (TabFilament::update_filament_overrides_page).
r = call("--option-states", "generic_klipper", {"filament_retraction_length": "0.8", "filament_z_hop": "nil"}, scope="filament")
o = r.get("overrides", {})
expect("an own retraction length is a checked override", o.get("filament_retraction_length", {}).get("checked") is True
       and o["filament_retraction_length"]["fieldEnabled"] is True)
expect("an unset z hop is unchecked and shows the printer's value", o.get("filament_z_hop", {}).get("checked") is False
       and o["filament_z_hop"]["fieldEnabled"] is False and o["filament_z_hop"]["inherited"] != "")
r = call("--option-states", "generic_klipper", {"filament_retraction_length": "0"}, scope="filament")
o = r.get("overrides", {})
expect("a zero filament retraction length locks the other retraction overrides",
       o.get("filament_z_hop", {}).get("checkboxEnabled") is False and o.get("filament_retraction_length", {}).get("checkboxEnabled") is True)
expect("the long-retraction rows are hidden unless the printer allows them per filament",
       o.get("filament_long_retractions_when_cut", {}).get("visible") is False)
expect("ironing overrides are always clickable", o.get("filament_ironing_flow", {}).get("checkboxEnabled") is True)

# IDEX tool layout dropdown (TabPrinter::toggle_options).
r = call("--option-states", "generic_klipper", {"is_imex": "1", "imex_gantry_count": "1", "imex_tool_layout": "rear-left"}, scope="printer")
expect("a single gantry offers Left and Right", [c["value"] for c in r.get("choices", {}).get("imex_tool_layout", [])] == ["front-left", "front-right"]
       and [c["label"] for c in r["choices"]["imex_tool_layout"]] == ["Left", "Right"])
expect("a stored rear layout is normalised to the front one on a single gantry", r["forced"].get("imex_tool_layout") == "front-left")
r = call("--option-states", "generic_klipper", {"is_imex": "1", "imex_gantry_count": "2", "imex_tool_layout": "rear-left"}, scope="printer")
expect("two gantries offer all four corners and keep the stored one", len(r.get("choices", {}).get("imex_tool_layout", [])) == 4
       and "imex_tool_layout" not in r["forced"])
r = call("--option-states", "generic_klipper", {}, scope="printer")
expect("no layout choices without IDEX", "imex_tool_layout" not in r.get("choices", {}))

# Mixed sub-layer + variable layer height warning.
r = call("--config-checks", "generic_klipper", {"enable_mixed_color_sublayer": "1"}, variableLayer=True)
expect("mixed sub-layers with a variable layer height warn", any(n["id"] == "mixed_sublayer_variable_layer" for n in r["notices"]))
r = call("--config-checks", "generic_klipper", {"enable_mixed_color_sublayer": "1"}, variableLayer=False)
expect("mixed sub-layers alone do not warn", not any(n["id"] == "mixed_sublayer_variable_layer" for n in r["notices"]))
r = call("--config-checks", "generic_klipper", {}, variableLayer=True)
expect("a variable layer height alone does not warn", not any(n["id"] == "mixed_sublayer_variable_layer" for n in r["notices"]))

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
