// Nozzle It All Desktop's slicing engine: a native command-line front end over the shared slicing pipeline
// (nozzle/bridge/android/slic3r_engine.cpp) that Android (JNI) and the Web App (nozzle/bridge/wasm/wasm_engine.cpp)
// also call, so all three platforms slice identically.
//
//   nozzle-engine <request.txt>      slice; prints "progress <0-100>" lines on stdout, exits 0 on success
//   nozzle-engine --version
//   nozzle-engine --schema           every print/filament/printer setting libslic3r defines, as JSON on stdout
//   nozzle-engine --full-spectrum <request.json>
//                                    Full Spectrum colour mixing (Snapmaker Orca's Color Mixing list and Color Mixing
//                                    Match, see full_spectrum.cpp): one JSON response on stdout; exit 0 success,
//                                    1 the operation failed, 2 bad request (response {"error": "..."})
//   nozzle-engine --plate <request.json>           plate tools (arrange, auto-orient, cut): see plate_ops.cpp
//   nozzle-engine --color-mix <request.json>
//                                    PrusaSlicer 2.9.6 colour mixing (virtual extruders; see color_mix.cpp): one JSON
//                                    response on stdout, exit codes as --full-spectrum
//
// The request file uses the Web App's line format (one field per line, tab-separated):
//   out\t<gcode path>
//   profile\t<path>              (repeatable, in load order: machine, process, filament...)
//   set\t<key>\t<value>          (repeatable config override)
//   object\t<model path>\t<x mm>\t<y mm>\t<rotation z deg>\t<scale>\t<tool index, 1-based, 0 = default>
//   object_set\t<object number, 1-based, in object line order>\t<key>\t<value>
//                                (repeatable) a per-object print setting, as Orca's object list sets them
//   virtual_extruders\t<path>    (optional, at most once) PrusaSlicer 2.9.6 virtual extruders, a JSON file in the
//                                Metadata/Prusa_Slicer_full_spectrum.json format; a tool index or painted state equal
//                                to a virtual id then prints by PrusaSlicer's layer cycle
// x/y are relative to the bed centre, as in the Web App.
//
// stdout carries only the progress lines: libslic3r's own log output (Boost.Log) is sent to stderr with everything else.
// Exit codes: 0 success, 1 slice failed (message on stderr), 2 bad usage/request, 3 cancelled (SIGINT/SIGTERM).
// Everything is local; nothing here performs network I/O.
#include "slic3r_engine.hpp"
#include "full_spectrum.hpp"
#include "color_mix.hpp"
#include "plate_ops.hpp"
#include "contract_cli.hpp"

#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Preset.hpp>

#include <atomic>
#include <cfloat>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <unistd.h>

namespace {

struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };

struct Request {
    std::string out;
    std::vector<std::string> profiles;
    std::vector<std::pair<std::string, std::string>> overrides;
    std::vector<std::tuple<std::string, engine::ModelTransform, int>> objects;
    std::vector<std::tuple<size_t, std::string, std::string>> object_settings;
    std::string virtual_extruders_path;
};

// Same parser as nozzle/bridge/wasm/wasm_engine.cpp, so a request written for one engine works for the other.
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
        else if (f[0] == "set" && f.size() == 2) r.overrides.emplace_back(f[1], "");
        else if (f[0] == "object" && f.size() == 7) {
            engine::ModelTransform t;
            try {
                t.offset_x_mm = std::stod(f[2]); t.offset_y_mm = std::stod(f[3]); t.rotation_z_deg = std::stod(f[4]); t.scale = std::stod(f[5]);
                r.objects.emplace_back(f[1], t, std::stoi(f[6]));
            } catch (const std::logic_error&) { throw BadRequest("Bad number in object line: " + line); }
        } else if (f[0] == "object_set" && (f.size() == 4 || f.size() == 3)) {
            size_t n = 0;
            try { n = std::stoul(f[1]); } catch (const std::logic_error&) { throw BadRequest("Bad object number in object_set line: " + line); }
            r.object_settings.emplace_back(n, f[2], f.size() == 4 ? f[3] : "");
        } else if (f[0] == "virtual_extruders" && f.size() == 2) {
            if (!r.virtual_extruders_path.empty()) throw BadRequest("Only one virtual_extruders line is allowed.");
            r.virtual_extruders_path = f[1];
        } else throw BadRequest("Unrecognised request line: " + f[0]);
    }
    if (r.out.empty() || r.objects.empty() || r.profiles.empty()) throw BadRequest("The slice request is incomplete.");
    for (const auto& s : r.object_settings)
        if (std::get<0>(s) < 1 || std::get<0>(s) > r.objects.size()) throw BadRequest("object_set names an object that isn't in the request.");
    // The shared pipeline skips a profile path it cannot read; here a missing input is a request error instead.
    auto readable = [](const std::string& p) { std::ifstream f(p, std::ios::binary); return f.good(); };
    for (const auto& p : r.profiles) if (!readable(p)) throw BadRequest("Cannot read profile " + p);
    for (const auto& o : r.objects) if (!readable(std::get<0>(o))) throw BadRequest("Cannot read model " + std::get<0>(o));
    if (!r.virtual_extruders_path.empty() && !readable(r.virtual_extruders_path))
        throw BadRequest("Cannot read virtual extruders " + r.virtual_extruders_path);
    return r;
}

std::atomic<bool> g_cancel{false};
void on_signal(int) { g_cancel = true; }

// stdout is reserved for this program's own output (progress lines, --schema JSON, --version). libslic3r logs from its
// static initialisers (Snapmaker Orca's PrintConfig.cpp does), which run before main, so anything written to fd 1 during
// start-up is sent to stderr: this constructor runs before every default-priority static initialiser, and main takes
// the real stdout back from g_stdout.
int g_stdout = -1;
__attribute__((constructor(101))) void hold_stdout() {
    std::fflush(stdout);
    g_stdout = dup(STDOUT_FILENO);
    if (g_stdout >= 0) dup2(STDERR_FILENO, STDOUT_FILENO);
}
// Points fd 1 back at the real stdout (for --schema and --version, which print there directly).
void restore_stdout() {
    std::fflush(stdout); std::cout.flush();
    if (g_stdout >= 0) dup2(g_stdout, STDOUT_FILENO);
}

} // namespace

namespace {
std::string json_string(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; } else o += char(c);
        }
    }
    return o + "\"";
}

const char* type_name(Slic3r::ConfigOptionType t) {
    using namespace Slic3r;
    switch (t) {
        case coFloat: return "float"; case coFloats: return "floats"; case coInt: return "int"; case coInts: return "ints";
        case coString: return "string"; case coStrings: return "strings"; case coPercent: return "percent"; case coPercents: return "percents";
        case coFloatOrPercent: return "float_or_percent"; case coFloatsOrPercents: return "floats_or_percents";
        case coPoint: return "point"; case coPoints: return "points"; case coPoint3: return "point3";
        case coBool: return "bool"; case coBools: return "bools"; case coEnum: return "enum"; case coEnums: return "enums";
        default: return "other";
    }
}

/**
 * A default in the engine's serialized form. Lists of choices are spelled out from the definition's own name table:
 * a ConfigOptionEnumsGeneric built from an initializer list has no usable keys_map in Snapmaker Orca's Config.hpp (its
 * constructor initialises keys_map from itself), so its serialize() must not be called here.
 */
std::string default_text(const Slic3r::ConfigOptionDef& d) {
    using namespace Slic3r;
    if (d.type == coEnums) {
        const auto* opt = dynamic_cast<const ConfigOptionInts*>(d.default_value.get());
        if (opt == nullptr) return "";
        std::string out;
        for (size_t i = 0; i < opt->values.size(); ++i) {
            const int v = opt->values[i];
            std::string name;
            if (d.enum_keys_map != nullptr)
                for (const auto& [key, value] : *d.enum_keys_map) if (value == v) { name = key; break; }
            if (name.empty() && v >= 0 && size_t(v) < d.enum_values.size()) name = d.enum_values[v];
            out += (i ? "," : "") + name;
        }
        return out;
    }
    return d.default_value->serialize();
}

/**
 * The settings schema: for every option a preset can carry, its scope (process, filament, printer), type, labels, category,
 * help text, units, limits, detail level, choices and default, straight from libslic3r's PrintConfigDef. Nozzle generates
 * its settings screens from this instead of copying any slicer's UI.
 */
int dump_schema() {
    using namespace Slic3r;
    const std::pair<const char*, const std::vector<std::string>*> scopes[] = {
        {"process", &Preset::print_options()}, {"filament", &Preset::filament_options()}, {"printer", &Preset::printer_options()}};
    const char* modes[] = {"simple", "advanced", "expert", "develop"};
    std::cout << "{\"schema\":\"nozzle.settings\",\"version\":1,\"options\":[";
    bool first = true;
    for (const auto& [scope, keys] : scopes) {
        for (const std::string& key : *keys) {
            const ConfigOptionDef* d = print_config_def.get(key);
            if (d == nullptr) continue;
            std::cout << (first ? "\n" : ",\n") << "{\"key\":" << json_string(key) << ",\"scope\":\"" << scope << "\",\"type\":\"" << type_name(d->type) << "\"";
            first = false;
            if (!d->label.empty()) std::cout << ",\"label\":" << json_string(d->label);
            if (!d->full_label.empty() && d->full_label != d->label) std::cout << ",\"fullLabel\":" << json_string(d->full_label);
            if (!d->category.empty()) std::cout << ",\"category\":" << json_string(d->category);
            if (!d->tooltip.empty()) std::cout << ",\"tooltip\":" << json_string(d->tooltip);
            if (!d->sidetext.empty()) std::cout << ",\"units\":" << json_string(d->sidetext);
            if (d->min > -FLT_MAX) std::cout << ",\"min\":" << d->min;
            if (d->max < FLT_MAX) std::cout << ",\"max\":" << d->max;
            if (!d->ratio_over.empty()) std::cout << ",\"ratioOver\":" << json_string(d->ratio_over);
            int m = int(d->mode); std::cout << ",\"mode\":\"" << (m >= 0 && m < 4 ? modes[m] : "develop") << "\"";
            if (d->readonly) std::cout << ",\"readonly\":true";
            if (d->multiline) std::cout << ",\"multiline\":true";
            if (d->is_code) std::cout << ",\"code\":true";
            if (d->nullable) std::cout << ",\"nullable\":true";
            // Settable for one object (Orca's object list: SettingsFactory::get_options(false), region and object options).
            static const PrintObjectConfig object_options; static const PrintRegionConfig region_options;
            if (std::string(scope) == "process" && (object_options.option(key) != nullptr || region_options.option(key) != nullptr)) std::cout << ",\"perObject\":true";
            if (!d->enum_values.empty()) {
                std::cout << ",\"choices\":[";
                for (size_t i = 0; i < d->enum_values.size(); ++i)
                    std::cout << (i ? "," : "") << "{\"value\":" << json_string(d->enum_values[i]) << ",\"label\":"
                              << json_string(i < d->enum_labels.size() ? d->enum_labels[i] : d->enum_values[i]) << "}";
                std::cout << "]";
                if (d->gui_type == ConfigOptionDef::GUIType::f_enum_open || d->gui_type == ConfigOptionDef::GUIType::i_enum_open) std::cout << ",\"openChoices\":true";
            }
            if (d->default_value) std::cout << ",\"default\":" << json_string(default_text(*d));
            std::cout << "}";
        }
    }
    std::cout << "\n]}" << std::endl;
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--schema") { restore_stdout(); return dump_schema(); }
    if (argc == 2 && std::string(argv[1]) == "--version") {
        restore_stdout();
        std::cout << "nozzle-engine 1 (libslic3r, shared Android pipeline, native)" << std::endl;
        return 0;
    }
    if (argc == 3 && (std::string(argv[1]) == "--full-spectrum" || std::string(argv[1]) == "--color-mix" || std::string(argv[1]) == "--plate" || std::string(argv[1]) == "--option-states" || std::string(argv[1]) == "--config-checks" || std::string(argv[1]) == "--compatible-presets")) {
        const std::string mode = argv[1];
        const bool color_mix = mode == "--color-mix";
        std::string request, response;
        int code = 2;
        std::ifstream f(argv[2], std::ios::binary);
        if (f) {
            std::stringstream ss; ss << f.rdbuf();
            request = ss.str();
            code = mode == "--option-states" ? nozzle_contract::run_option_states(request, response)
                 : mode == "--config-checks" ? nozzle_contract::run_config_checks(request, response)
                 : mode == "--compatible-presets" ? nozzle_contract::run_compatible_presets(request, response)
                 : mode == "--plate" ? nozzle_plate::run_plate(request, response)
                 : color_mix ? nozzle_cm::run_color_mix(request, response) : nozzle_fs::run_full_spectrum(request, response);
        } else {
            response = "{\"error\":" + json_string(std::string("Cannot read request file ") + argv[2]) + "}";
        }
        restore_stdout();
        std::cout << response << std::endl;
        return code;
    }
    if (argc != 2) {
        std::cerr << "usage: nozzle-engine <request.txt> | --version | --schema | --full-spectrum <request.json> | --color-mix <request.json> | --plate <request.json> | --option-states <request.json> | --config-checks <request.json> | --compatible-presets <request.json>" << std::endl;
        return 2;
    }
    Request req;
    std::string virtual_extruders_json;
    try {
        std::ifstream f(argv[1], std::ios::binary);
        if (!f) throw BadRequest(std::string("Cannot read request file ") + argv[1]);
        std::stringstream ss; ss << f.rdbuf();
        req = parse(ss.str());
        if (!req.virtual_extruders_path.empty()) {
            std::ifstream vf(req.virtual_extruders_path, std::ios::binary);
            std::stringstream vs; vs << vf.rdbuf();
            virtual_extruders_json = vs.str();
            std::string why;
            if (!nozzle_cm::check_virtual_extruders_file(virtual_extruders_json, why))
                throw BadRequest("Bad virtual extruders file " + req.virtual_extruders_path + ": " + why);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 2;
    }

    // Keep stdout for progress lines only: the original stdout becomes a private stream, and fd 1 (where libslic3r's
    // log sink writes) is pointed at stderr.
    std::fflush(stdout);
    // The real stdout, held since start-up (fd 1 already points at stderr).
    int progress_fd = g_stdout >= 0 ? g_stdout : dup(STDOUT_FILENO);
    FILE* progress = progress_fd >= 0 ? fdopen(progress_fd, "w") : nullptr;
    if (!progress || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) { std::cerr << "Cannot set up the progress stream." << std::endl; return 1; }
    auto report = [progress](int p) { std::fprintf(progress, "progress %d\n", p); std::fflush(progress); };

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    engine::reset_cancel();

    // The slice runs on a worker thread; this thread reports progress and forwards a cancellation signal.
    report(0);
    std::atomic<bool> finished{false};
    int code = 0;
    std::string message;
    std::thread worker([&]() {
        try {
            std::vector<engine::ObjectExtras> extras(req.objects.size());
            for (const auto& [n, key, value] : req.object_settings) extras[n - 1].settings.emplace_back(key, value);
            engine::slice_multi_object(req.objects, req.out, req.profiles, req.overrides, extras, virtual_extruders_json);
        } catch (const engine::SliceCancelled&) {
            code = 3; message = "Slicing cancelled";
        } catch (const std::exception& e) {
            code = 1; message = e.what();
        } catch (...) {
            code = 1; message = "The engine stopped unexpectedly.";
        }
        finished = true;
    });
    int last = 0;
    bool cancel_sent = false;
    while (!finished) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (g_cancel && !cancel_sent) { engine::request_cancel(); cancel_sent = true; }
        int p = engine::slice_progress();
        if (p != last) { report(p); last = p; }
    }
    worker.join();
    if (code != 0) {
        std::cerr << (message.empty() ? "Slicing failed" : message) << std::endl;
        return code;
    }
    if (last != 100) report(100);
    return 0;
}
