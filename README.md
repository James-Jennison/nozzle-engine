# nozzle-engine

The headless slicing engine behind [Nozzle It All](https://github.com/James-Jennison/nozzle-it-all). It turns 3D models and printer, filament and print settings into G-code, and runs on the desktop (Linux x86_64), on Android (arm64-v8a, through JNI) and in the browser (WebAssembly).

There is no user interface here. Nozzle It All draws every screen, and generates its settings screens from the schema this engine exports (`nozzle-engine --schema`).

## Build

The scripts under `tools/nozzle/` build each platform:

| Script | Output |
|---|---|
| `build_desktop_engine.sh` | `nozzle-engine`, the desktop command-line engine |
| `build_android_engine.sh` | `libslic3rengine.so` for Nozzle It All's Android app |
| `build_wasm_engine.sh` | `nozzle-engine.js` and `.wasm` for the Web App |
| `equivalence.sh REF CAND` | compares two engines: identical settings schema and identical G-code on every bundled printer profile |

The bridges Nozzle It All calls are in `nozzle/bridge/`.

## How changes are made

- Features and fixes from other slicers come in as individual, reviewed commits with their source recorded. Upstream projects are never merged wholesale.
- New behaviour is off by default, so default settings keep producing the same G-code.
- Every change must build on all three platforms and pass the equivalence check.

## Credits and licence

Licensed under the GNU Affero General Public License v3.0 (see `LICENSE.txt`).

This engine is derived from Snapmaker Orca, which is based on OrcaSlicer by SoftFever. That in turn builds on Bambu Studio by Bambu Lab, PrusaSlicer by Prusa Research, and Slic3r by Alessandro Ranellucci and the RepRap community. The upstream project's README is kept at `doc/UPSTREAM_README.md`. Individual files keep their original copyright and attribution headers.
