# Nozzle It All: move to the Orca 2.5.0 combined base

Decision (2026-10-16): move nozzle-engine to the combined Orca 2.5.0 tree and build the Android app, desktop app and web engine from it. Standing requirement: the NIA UI reaches parity with Orca's object, paint and settings tooling.

Branch: `engine/orca-2.5-base` in the nozzle-engine repo, worktree `/mnt/faststorage/nozzle-engine-2.5`, started from `base/orca-2.5-combined` (= Combined-Orca `combined/orca-2.5-nozzle`). The user's checkouts (`Test Slicer/engine` on test/prime-tower-pack-position, `engine-colourmix`) are untouched.

## Engine delta to carry over (nozzle-engine = Snapmaker cbf7bbb0b3 + 61 commits)
1. Headless build: CMake changes (CMAKE_CROSSCOMPILING, no CURL/GL/GLEW/glfw without SLIC3R_GUI, Blosc off for Android/wasm, tbbmalloc stand-in, `ANDROID_JNI_BRIDGE_DIR` subdirectory, NSIS paths). Upstream 2.5.0 already has SLIC3R_GUI; check overlap first.
2. Bridges: `nozzle/bridge/{android,native,wasm}` (~8,300 lines) + `nozzle/contract`, `tools/nozzle` (golden, equivalence, contract tests).
3. Colour mixing: DECIDED (Claude's recommendation earlier in this merge, adopted by the owner with "follow your recommendation", and confirmed again 2026-10-17): Orca 2.5.0's own mixed filaments are the engine's mixer. The Prusa-derived `Feature/FullSpectrum/VirtualExtruder` and `deps_src/prusa_fdm_mixer` are NOT ported into the engine. The bridge's `full_spectrum.cpp`/`color_mix.cpp` and the Android/desktop mixing calls are re-implemented against 2.5.0's `filament_is_mixed`/`filament_mixed_*` options. G-code for mixed prints will differ from the Prusa-based engine; goldens for those cases are regenerated, not matched.
4. Bambu support (H2D/H2S/P2S/X2D/H2C variants, `ExtruderVariants`, AMS-per-object): check how much 2.5.0 has natively.
5. Wipe tower items from upstream (outline, wipe_tower_type): likely native in 2.5.0.
6. Determinism fixes (uninitialised members, seedable random), thumbnails in filament colours, Klipper wipe-tower object, wasm size_t guards, arm64 `get_at` miscompile fix, option states, setting aliases.
7. Combined features the engine gains for free: Snapmaker fixes, paint penetration, bridge anchoring, SemanticPaint core.

## Steps
A. Headless CMake on the new base; native `nozzle-engine` CLI builds on gthost.
B. Port bridge + contract + tools; run contract_test and golden/equivalence against the new engine.
C. Re-implement the bridge's colour-mixing entry points on 2.5.0 mixed filaments; regenerate the mixing goldens.
D. Port remaining deltas (4-6) after checking what 2.5.0 already has.
E. Android (NDK) and wasm builds; re-pin NIA to the new engine.
F. UI parity in NIA (Compose): object list/manipulation, paint tools (support, seam, colour/MMU, fuzzy skin), per-object and per-region settings, full print/filament/printer settings with Orca's option states. Separate scoping pass against the bridge API.
G. AI auto-paint in Prepare (SemanticPaint via bridge + provider call from Kotlin).

## Status (2026-10-17)
- A done: headless `libslic3r` builds from the combined 2.5.0 tree.
- B partly done: native `nozzle-engine` builds (colour-mixing entry points excluded, `NOZZLE_BRIDGE_MIXING=OFF`).
  - `tools/nozzle/golden.sh`: all 383 bundled printer profiles slice the 20 mm cube, 0 failures. Hashes differ from the old goldens (expected: new base). Spot check on Elegoo Centauri Carbon COSMOS and Snapmaker U1: same layer count and filament weight as the old engine, print time within 2-3%.
  - `tools/nozzle/contract_test.py`: PASSES (328 aliased settings). Fixes: `print_option_states` re-ported mechanically from 2.5.0's `ConfigManipulation::toggle_print_fff_options` (the old port read per-filament floats for options that are plain floats in 2.5.0 and crashed; the new rules include belt-printer, fuzzy-skin, wall-direction changes); `setting_aliases.json` re-keyed to 2.5.0 names (`outer_wall_filament_id`/`inner_wall_filament_id`, `sparse_infill_filament_id`, `internal_solid_filament_id`, `filament_preheat_temperature_delta`) and the four aliases with no 2.5.0 counterpart removed (Snapmaker graphic-effect plate temperatures, Local Z dithering mode, Cura prime Z-hop).
  - Printer and filament option states and `print_config_checks` are re-ported from 2.5.0 (`TabPrinter::toggle_options`, `TabFilament::toggle_options`, `update_print_fff_config`): belt printer, IDEX/IQEX, wipe tower type 2, retract-after-wipe, input shaping, adaptive pressure advance, air filtration, per-variant bed-plate rows, belt raft/draft-shield reset, cone wipe tower, mixed-slot filament resets. All 383 profiles run `--option-states` and `--config-checks` without a crash (766 calls).
  - Carried over since (2026-10-17, tools/nozzle/contract_test.py): the Setting Overrides page rules (`update_filament_overrides_page`: `overrides` in `--option-states`, per filament key checkbox / field / visibility / inherited value; options the profile doesn't carry read as unset), the IDEX tool-layout dropdown (`choices.imex_tool_layout`: Left/Right on one gantry with a rear value normalised through `forced`, four corners on two), and the mixed-sub-layer / variable-layer-height warning (`variableLayer` in the request, notice `mixed_sublayer_variable_layer` from `--config-checks`; the caller says whether any object has a layer profile or height ranges). `enable_wrapping_detection` is offered only for Bambu printers (the GUI's device tables are not available headless). `OptionContext::expert_mode` (default true) gates the two expert-only belt rows.
- Variant handling now uses 2.5.0's native `normalize_filament_values_to_variants`; the engine's own `ExtruderVariants.cpp` backport was dropped. Not yet checked against a Bambu H2D profile.
- E (Android) built: `libslic3rengine.so` (arm64-v8a, NDK 29, 65 MB stripped) cross-compiles from the 2.5.0 base on gthost (`~/nia-android`, using NIA's `app/src/main/cpp` CMake project, read only). Headless CMake changes needed: no embedded Python, no shared-library copy step, no fontconfig on Android, OCCT 7.6 library names. The prepared Android prefix had CGAL 5.6.3; 2.5.0 needs 6.2, so CGAL 6.2's headers and CMake config (header-only) were copied from the native deps prefix into a COPY of the prefix under `~/nia-android/root/deps` (the original on MORG is untouched). Colour-mixing JNI calls are stubbed to report "not available" until step C. Not run on a device or emulator; the native build of the same pipeline passes the golden and contract checks. NIA's `engine/fork/ENGINE_PIN.json` and `prepare_engine_root.sh` still point at the old engine; re-pinning is not done.
- Not started: C (mixing), wasm build, F (UI parity), G (AI auto-paint).

## Status: step C (colour mixing on 2.5.0's mixed filaments) - done in the bridge

- `nozzle/bridge/native/mixed_filament.{hpp,cpp}`: stateless ops `display, add, update, remove, delete_physical, match, presets, slice_overrides`
  on `Slic3r::blend_color_multi`, `format_mixed_*` and `recommend_from_physical_filaments` / `lookup_standard_recipe`.
  CLI `nozzle-engine --mixed-filament <request.json>`; JNI `NativeEngine.nativeMixedFilament(String): String`.
- `slice_overrides` returns the per-filament config values (`filament_colour`, `filament_is_mixed`, `filament_mixed_*`,
  `enable_mixed_color_sublayer`) a caller passes as `set` lines; `grow_mixed_slots()` in `slic3r_engine.cpp` grows the profile's
  per-filament options to the slot count before slicing. Gradient rows force the sub-layer path.
- Thumbnails draw a mixed slot's blended colour (`thumbnail_render.cpp`).
- `tools/nozzle/mixed_filament_test.sh <nozzle-engine>` covers the ops, the 3:1 extrusion share and the thumbnail colour.
- `nativeFullSpectrum` / `nativeColorMix` and the Snapmaker/Prusa sources (`full_spectrum.cpp`, `color_mix.cpp`,
  `colourmix_test.sh`) are retired but left in the tree until the NIA side stops calling them.

## Status: wasm, deltas, paint and AI auto-paint (2026-10-17)

- wasm: `nozzle-engine.{js,wasm}` builds from this base with Emscripten 6.0.10 (the dependency prefix's toolchain; gthost's 3.1.74 cannot link it) and slices the 20 mm cube in node. Engine changes: `__EMSCRIPTEN__` as a Linux-like platform, Boost.Filesystem `copy_options`, no `-Werror` for wasm32's size_t range checks. Prefix fixes (not in the repo): CGAL 6.2.1, Boost.Interval hw_rounding and `boost/detail/fenv.hpp` accept Emscripten.
- Delta ported from the old nozzle-engine (items 5-6): Klipper `wipe_tower` object (placed with the rib offset, includes the final purge), uninitialised-member fixes, seedable `random()`, arm64 `get_at` hardening, wipe tower comment-tag dialect. Already native in 2.5.0: wipe_tower_type, tower outline / rib offset, ceil()/floor(), plate origin, Arachne parameter defaults. Not ported: `append_tcr` empty-template branch (2.5.0 always retracts), the old ExtruderVariants backport (replaced by `normalize_filament_values_to_variants`).
- Device (arm64, NDK 29): the slicing, multi-object, paint-session, Bambu bundle, profile-pack and colour-mixing instrumented classes pass. Four Compose UI tests fail for reasons outside the engine: two (`MultiMaterialDeviceTest`) assert wording the app changed in ab8b5a23, two (`TestModeDeviceTest`) look for test-grid text.
- `--paint` (paint_ops.cpp): brush, bucket fill, smart fill, erase and painted pieces on TriangleSelector; JNI `nativePaintOps`. `tools/nozzle/paint_ops_test.sh`.
- `--semantic-paint` (semantic_paint_op.cpp): segment, openai_request, reply_text, assign over SemanticPaint; JNI `nativeSemanticPaint`. `tools/nozzle/semantic_paint_test.sh`. The provider call is made by the app.

## Status: object tools beyond the plate ops (2026-10-17)

Scoping against the bridge: what libslic3r serves from a slice request alone (no state kept between calls) is done; the rest is named.

- Slice request lines (nozzle-engine CLI; the same fields are `ObjectExtras` / `ModelTransform` for the JNI and wasm callers, which don't parse the new lines yet):
  `object_part <n> <kind:shape:cx,cy,cz:sx,sy,sz[:key=value|...]>` (kind negative / modifier / blocker / enforcer / part, shape box / cylinder / sphere; the centre is from the mesh's bounding-box centre),
  `object_range <n> <z0> <z1> <key> <value>` (Orca's height range modifier, `ModelObject::layer_config_ranges`),
  `object_layers <n> <z,h,z,h,...>` (`ModelObject::layer_height_profile`),
  `object_scale <n> <sx> <sy> <sz> [axes]` (per-axis size on top of the uniform scale; `axes` mirrors, for example `xz`).
- `--plate {"op":"adaptive_layers"}`: Orca's adaptive layer height (`layer_height_profile_adaptive`) for a mesh under the profiles' settings.
- `tools/nozzle/object_tools_test.sh` covers all of it against the real slicer (filament use, layer counts, extents, mirrored centroid).
- Instances: Orca's instance count is a copy of the object sharing its mesh; NIA's Duplicate already makes that copy (a shared-mesh instance would only save memory), so there is no separate op.
- Not stateless, not done: editing the layer height profile by brush (Orca's layer editing sliders), cut by an arbitrary plane / connectors, mesh-shaped parts (only primitives), part rotation.
