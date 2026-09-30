// Adapted from the owner's own orcaslicer-android-engine project (local, /mnt/faststorage/orcaslicer-android-engine).
// Licensed AGPL-3.0-or-later (same as OrcaSlicer). See THIRD_PARTY_NOTICES.md.
// Standalone on-device smoke test: exercises the exact same slicing
// pipeline as the JNI bridge (slic3r_engine.cpp), but as a plain
// executable pushed and run directly via `adb shell`, without needing a
// full Android app/JVM around it. Not shipped in the APK.
//
// This is also built as a plain host (x86_64) executable - see
// nozzle/bridge/native/CMakeLists.txt's slic3r_cli_test target - so its Full Spectrum/ColorMix/virtual-extruders
// modes (added alongside nativeFullSpectrum/nativeColorMix/nativeSliceMultiObjectMix/nativeSlicePaintSessionMix in
// slic3r_jni.cpp) can be exercised on gthost without an Android device, proving the Android bridge's mixing entry
// points return the same thing the desktop nozzle-engine CLI does for the same request, without adb.
//
//   slic3r_cli_test <input.stl> <output.gcode>          original bare-geometry smoke test (unchanged)
//   slic3r_cli_test --full-spectrum <request.json>      calls nozzle_fs::run_full_spectrum, prints its response
//   slic3r_cli_test --color-mix <request.json>          calls nozzle_cm::run_color_mix, prints its response
//   slic3r_cli_test --slice <request.txt>               calls engine::slice_multi_object; request.txt uses the same
//                                                        tab-separated line format native_cli.cpp's plain slice mode
//                                                        does (out/profile/set/object/virtual_extruders), so the
//                                                        exact same request file can be sliced by the desktop CLI
//                                                        and by this bridge-equivalent tool for comparison.
// Exit codes for --full-spectrum/--color-mix mirror the CLI: 0 success, 1 operation failed, 2 bad request (response
// is always {"error": "..."} on failure - see full_spectrum.hpp/color_mix.hpp). --slice: 0 success, 1 slice failed,
// 2 bad request.
#include <cstdio>
#include <exception>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

#include "slic3r_engine.hpp"
#include "full_spectrum.hpp"
#include "color_mix.hpp"

namespace {

struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };

// Same fd-1 hold/restore trick native_cli.cpp uses (see its own comment): libslic3r's Boost.Log static initialisers
// (Snapmaker Orca's PrintConfig.cpp) run before main and write to fd 1, which would otherwise interleave a trace line
// into the --full-spectrum/--color-mix JSON or --slice's own stdout - exactly the noise this tool exists to keep out,
// since its whole point is a byte-for-byte comparison against the desktop CLI's stdout for the same request.
int g_stdout = -1;
__attribute__((constructor(101))) void hold_stdout() {
    std::fflush(stdout);
    g_stdout = dup(STDOUT_FILENO);
    if (g_stdout >= 0) dup2(STDERR_FILENO, STDOUT_FILENO);
}
void restore_stdout() {
    std::fflush(stdout);
    if (g_stdout >= 0) dup2(g_stdout, STDOUT_FILENO);
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) throw BadRequest("Cannot read " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Same request line format/semantics as native_cli.cpp's parse() (see its own comment for the full field list);
// duplicated rather than shared because native_cli.cpp's Request/parse() are file-local (anonymous namespace) and
// this tool only needs the plain multi-object slice subset - no --plate/--schema/object_set/progress reporting.
struct Request {
    std::string out;
    std::vector<std::string> profiles;
    std::vector<std::pair<std::string, std::string>> overrides;
    std::vector<std::tuple<std::string, engine::ModelTransform, int>> objects;
    std::string virtual_extruders_path;
};

Request parse(const std::string& text) {
    Request r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream ls(line);
        std::string part;
        while (std::getline(ls, part, '\t')) f.push_back(part);
        if (f.empty()) continue;
        if (f[0] == "out" && f.size() == 2) r.out = f[1];
        else if (f[0] == "profile" && f.size() == 2) r.profiles.push_back(f[1]);
        else if (f[0] == "set" && f.size() == 3) r.overrides.emplace_back(f[1], f[2]);
        else if (f[0] == "object" && f.size() == 7) {
            engine::ModelTransform t;
            try {
                t.offset_x_mm = std::stod(f[2]); t.offset_y_mm = std::stod(f[3]); t.rotation_z_deg = std::stod(f[4]); t.scale = std::stod(f[5]);
                r.objects.emplace_back(f[1], t, std::stoi(f[6]));
            } catch (const std::logic_error&) { throw BadRequest("Bad number in object line: " + line); }
        } else if (f[0] == "virtual_extruders" && f.size() == 2) {
            if (!r.virtual_extruders_path.empty()) throw BadRequest("Only one virtual_extruders line is allowed.");
            r.virtual_extruders_path = f[1];
        } else throw BadRequest("Unrecognised request line: " + f[0]);
    }
    if (r.out.empty() || r.objects.empty() || r.profiles.empty()) throw BadRequest("The slice request is incomplete.");
    return r;
}

int run_full_spectrum_mode(const std::string& request_path) {
    std::string response;
    int code;
    try {
        code = nozzle_fs::run_full_spectrum(read_file(request_path), response);
    } catch (const BadRequest& ex) {
        restore_stdout();
        std::printf("{\"error\":\"%s\"}\n", ex.what());
        return 2;
    }
    restore_stdout();
    std::printf("%s\n", response.c_str());
    return code;
}

int run_color_mix_mode(const std::string& request_path) {
    std::string response;
    int code;
    try {
        code = nozzle_cm::run_color_mix(read_file(request_path), response);
    } catch (const BadRequest& ex) {
        restore_stdout();
        std::printf("{\"error\":\"%s\"}\n", ex.what());
        return 2;
    }
    restore_stdout();
    std::printf("%s\n", response.c_str());
    return code;
}

int run_slice_mode(const std::string& request_path) {
    try {
        Request r = parse(read_file(request_path));
        std::string virtual_extruders_json;
        if (!r.virtual_extruders_path.empty()) {
            virtual_extruders_json = read_file(r.virtual_extruders_path);
            std::string why;
            if (!nozzle_cm::check_virtual_extruders_file(virtual_extruders_json, why))
                throw BadRequest("Bad virtual extruders file " + r.virtual_extruders_path + ": " + why);
        }
        engine::slice_multi_object(r.objects, r.out, r.profiles, r.overrides, {}, virtual_extruders_json);
        restore_stdout();
        std::printf("OK: sliced -> %s\n", r.out.c_str());
        return 0;
    } catch (const BadRequest& ex) {
        std::fprintf(stderr, "BAD REQUEST: %s\n", ex.what());
        return 2;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAILED: %s\n", ex.what());
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--full-spectrum") return run_full_spectrum_mode(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--color-mix") return run_color_mix_mode(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--slice") return run_slice_mode(argv[2]);

    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <input.stl> <output.gcode>\n", argv[0]);
        std::fprintf(stderr, "       %s --full-spectrum <request.json>\n", argv[0]);
        std::fprintf(stderr, "       %s --color-mix <request.json>\n", argv[0]);
        std::fprintf(stderr, "       %s --slice <request.txt>\n", argv[0]);
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
        restore_stdout();
        std::printf("OK: sliced %s -> %s\n", argv[1], argv[2]);
        return 0;
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "FAILED: %s\n", ex.what());
        return 1;
    }
}
