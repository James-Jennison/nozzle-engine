// Adapted from the owner's own orcaslicer-android-engine project (local, /mnt/faststorage/orcaslicer-android-engine).
// Licensed AGPL-3.0-or-later (same as OrcaSlicer). See THIRD_PARTY_NOTICES.md.
// Standalone on-device smoke test: exercises the exact same slicing
// pipeline as the JNI bridge (slic3r_engine.cpp), but as a plain
// executable pushed and run directly via `adb shell`, without needing a
// full Android app/JVM around it. Not shipped in the APK.
#include <cstdio>
#include <exception>

#include "slic3r_engine.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.stl> <output.gcode>\n", argv[0]);
        return 2;
    }

    try {
        // No real printer/filament/process profile is loaded here (this is
        // a bare geometry smoke test, not a real print), so factory
        // defaults are used as-is except for this one override: stock
        // defaults enable relative extruder addressing, which validate()
        // correctly rejects without a "G92 E0" reset in layer_gcode. A real
        // printer profile supplies that in its start/layer gcode; this is
        // the minimal fix for a config with no such profile at all.
        engine::slice_file(argv[1], argv[2], {}, {{"use_relative_e_distances", "0"}});
        std::printf("OK: sliced %s -> %s\n", argv[1], argv[2]);
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAILED: %s\n", ex.what());
        return 1;
    }
}
