# Nozzle It All: move to the Orca 2.5.0 combined base

Decision (2026-10-16): move nozzle-engine to the combined Orca 2.5.0 tree and build the Android app, desktop app and web engine from it. Standing requirement: the NIA UI reaches parity with Orca's object, paint and settings tooling.

Branch: `engine/orca-2.5-base` in the nozzle-engine repo, worktree `/mnt/faststorage/nozzle-engine-2.5`, started from `base/orca-2.5-combined` (= Combined-Orca `combined/orca-2.5-nozzle`). The user's checkouts (`Test Slicer/engine` on test/prime-tower-pack-position, `engine-colourmix`) are untouched.

## Engine delta to carry over (nozzle-engine = Snapmaker cbf7bbb0b3 + 61 commits)
1. Headless build: CMake changes (CMAKE_CROSSCOMPILING, no CURL/GL/GLEW/glfw without SLIC3R_GUI, Blosc off for Android/wasm, tbbmalloc stand-in, `ANDROID_JNI_BRIDGE_DIR` subdirectory, NSIS paths). Upstream 2.5.0 already has SLIC3R_GUI; check overlap first.
2. Bridges: `nozzle/bridge/{android,native,wasm}` (~8,300 lines) + `nozzle/contract`, `tools/nozzle` (golden, equivalence, contract tests).
3. Colour mixing: DECIDED (owner, standing since the start of the merge): Orca 2.5.0's own mixed filaments are the engine's mixer. The Prusa-derived `Feature/FullSpectrum/VirtualExtruder` and `deps_src/prusa_fdm_mixer` are NOT ported into the engine. The bridge's `full_spectrum.cpp`/`color_mix.cpp` and the Android/desktop mixing calls are re-implemented against 2.5.0's `filament_is_mixed`/`filament_mixed_*` options. G-code for mixed prints will differ from the Prusa-based engine; goldens for those cases are regenerated, not matched.
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
