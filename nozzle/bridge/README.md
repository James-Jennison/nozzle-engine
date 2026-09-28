# Nozzle It All engine bridges

The C/C++ entry points Nozzle It All uses to drive libslic3r. Each directory is plugged into the engine's CMake project through `-DANDROID_JNI_BRIDGE_DIR=<dir>` (see the top-level CMakeLists.txt).

| Directory | Builds | Imported from Nozzle It All |
|---|---|---|
| `android/` | `libslic3rengine.so` (JNI) and the shared slicing pipeline (`slic3r_engine.cpp`) that the other two also compile | `app/src/main/cpp/bridge` |
| `native/` | `nozzle-engine`, the desktop CLI (`--schema`, `--full-spectrum`, `--color-mix`, `--plate`, slicing jobs) | `engine/native/bridge` |
| `wasm/` | `nozzle-engine.js/.wasm` for the Web App's worker | `engine/wasm/bridge` |

Imported from Nozzle It All `a657948` on 2026-09-27. The only change is that `native/` and `wasm/` find the shared sources at `../android`. Until Nozzle It All builds from this fork, `tools/nozzle/bridge_drift.sh` reports any differences between these copies and Nozzle's. Licence: see `LICENSE.txt` and the attribution headers in each file.
