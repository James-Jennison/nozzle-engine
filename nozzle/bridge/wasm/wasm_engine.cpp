// Browser-facing C API for the Nozzle slicing engine. The Web Worker writes models and profiles into the in-memory
// filesystem, starts a slice, polls status for progress, and reads the G-code back. Everything runs inside the
// user's browser; nothing here performs network I/O.
//
// One slice at a time (the worker serialises requests). The slice runs on its own pthread so the worker's event loop
// stays free to report progress and deliver a cancellation request.
#include "slic3r_engine.hpp"

#include <emscripten/emscripten.h>
#include <atomic>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {
// 0 idle, 1 running, 2 done, 3 failed, 4 cancelled
std::atomic<int> g_state{0};
std::string g_message;

char* dup(const std::string& s) { char* p = static_cast<char*>(malloc(s.size() + 1)); std::memcpy(p, s.c_str(), s.size() + 1); return p; }

// Request format (one field per line, tab-separated, so no JSON parser is needed in C++):
//   out\t<gcode path>
//   profile\t<path>              (repeatable, in load order: machine, process, filament...)
//   set\t<key>\t<value>          (repeatable config override)
//   object\t<model path>\t<x mm>\t<y mm>\t<rotation z deg>\t<scale>\t<tool index, 1-based, 0 = default>
struct Request {
    std::string out;
    std::vector<std::string> profiles;
    std::vector<std::pair<std::string, std::string>> overrides;
    std::vector<std::tuple<std::string, engine::ModelTransform, int>> objects;
    std::vector<std::tuple<size_t, std::string, std::string>> object_settings;
};

Request parse(const char* text) {
    Request r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
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
            t.offset_x_mm = std::stod(f[2]); t.offset_y_mm = std::stod(f[3]); t.rotation_z_deg = std::stod(f[4]); t.scale = std::stod(f[5]);
            r.objects.emplace_back(f[1], t, std::stoi(f[6]));
        } else if (f[0] == "object_set" && (f.size() == 4 || f.size() == 3)) {
            r.object_settings.emplace_back(std::stoul(f[1]), f[2], f.size() == 4 ? f[3] : "");
        } else throw std::runtime_error("Unrecognised request line: " + f[0]);
    }
    if (r.out.empty() || r.objects.empty() || r.profiles.empty()) throw std::runtime_error("The slice request is incomplete.");
    for (const auto& s : r.object_settings)
        if (std::get<0>(s) < 1 || std::get<0>(s) > r.objects.size()) throw std::runtime_error("object_set names an object that isn't in the request.");
    return r;
}
} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* nz_version() { return "nozzle-engine 1 (libslic3r, shared Android pipeline)"; }

/** Returns 0 if the slice started, -1 if one is already running. */
EMSCRIPTEN_KEEPALIVE int nz_slice_start(const char* request) {
    int expected = 0;
    if (!g_state.compare_exchange_strong(expected, 1)) {
        if (expected == 1) return -1;
        g_state = 1;
    }
    std::string text(request);
    engine::reset_cancel();
    g_message.clear();
    std::thread([text]() {
        try {
            Request r = parse(text.c_str());
            std::vector<engine::ObjectExtras> extras(r.objects.size());
            for (const auto& [n, key, value] : r.object_settings) extras[n - 1].settings.emplace_back(key, value);
            engine::slice_multi_object(r.objects, r.out, r.profiles, r.overrides, extras);
            g_state = 2;
        } catch (const engine::SliceCancelled&) {
            g_state = 4;
        } catch (const std::exception& e) {
            g_message = e.what();
            g_state = 3;
        } catch (...) {
            g_message = "The engine stopped unexpectedly.";
            g_state = 3;
        }
    }).detach();
    return 0;
}

/** state * 1000 + percent, so one call reports both. */
EMSCRIPTEN_KEEPALIVE int nz_slice_status() { return g_state.load() * 1000 + engine::slice_progress(); }

/** The failure message of the last slice (caller frees). Resets the engine to idle once a finished slice is read. */
EMSCRIPTEN_KEEPALIVE char* nz_slice_result() {
    char* out = dup(g_message);
    int s = g_state.load();
    if (s >= 2) g_state = 0;
    return out;
}

EMSCRIPTEN_KEEPALIVE void nz_cancel() { engine::request_cancel(); }
EMSCRIPTEN_KEEPALIVE void nz_free(void* p) { free(p); }

} // extern "C"

int main() { return 0; }
