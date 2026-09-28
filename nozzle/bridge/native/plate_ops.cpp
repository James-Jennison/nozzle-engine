// `nozzle-engine --plate <request.json>`: the plate tools of Orca's workspace, run by libslic3r itself through the shared
// bridge (nozzle/bridge/android/slic3r_engine.cpp):
//   {"op":"arrange","profiles":[paths],"overrides":{key:value},"objects":[{"model":stl,"x":mm,"y":mm,"rotation":deg,"scale":f}],
//    "distance":mm (0 = Orca's auto spacing),"rotate":bool,"alignY":bool}
//        -> {"objects":[{"dx":mm,"dy":mm,"rotation":deg,"plate":n}]}  (x/y relative to the bed centre, as for slicing)
//   {"op":"orient","model":stl}  -> {"matrix":[9 numbers, row-major]}
//   {"op":"cut","model":stl,"z":mm,"upper":stl out,"lower":stl out}  -> {"upper":triangles,"lower":triangles}
// Models are binary STL files in the mesh's own coordinates.
#include "plate_ops.hpp"
#include "slic3r_engine.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nozzle_plate {
namespace {
using json = nlohmann::ordered_json;
struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };

std::vector<float> read_stl(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw BadRequest("Cannot read model " + path);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 84) throw BadRequest("Not a binary STL: " + path);
    uint32_t n; std::memcpy(&n, bytes.data() + 80, 4);
    if (bytes.size() < 84 + size_t(n) * 50) throw BadRequest("Truncated STL: " + path);
    std::vector<float> soup(size_t(n) * 9);
    for (uint32_t t = 0; t < n; ++t) std::memcpy(&soup[size_t(t) * 9], bytes.data() + 84 + size_t(t) * 50 + 12, 36);
    return soup;
}

void write_stl(const std::string& path, const std::vector<float>& soup) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("Cannot write " + path);
    char header[80] = {0};
    f.write(header, 80);
    const uint32_t n = uint32_t(soup.size() / 9);
    f.write(reinterpret_cast<const char*>(&n), 4);
    const float normal[3] = {0, 0, 0}; const uint16_t attr = 0;
    for (uint32_t t = 0; t < n; ++t) {
        f.write(reinterpret_cast<const char*>(normal), 12);
        f.write(reinterpret_cast<const char*>(&soup[size_t(t) * 9]), 36);
        f.write(reinterpret_cast<const char*>(&attr), 2);
    }
}

std::string text(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) throw BadRequest(std::string("The request needs \"") + key + "\"");
    return j[key].get<std::string>();
}

json op_arrange(const json& req) {
    std::vector<engine::ArrangeItem> items;
    if (!req.contains("objects") || !req["objects"].is_array() || req["objects"].empty()) throw BadRequest("Nothing to arrange.");
    for (const auto& o : req["objects"]) {
        engine::ArrangeItem item;
        item.model_path = text(o, "model");
        item.transform.offset_x_mm = o.value("x", 0.0); item.transform.offset_y_mm = o.value("y", 0.0);
        item.transform.rotation_z_deg = o.value("rotation", 0.0); item.transform.scale = o.value("scale", 1.0);
        items.push_back(item);
    }
    std::vector<std::string> profiles;
    const json profile_list = req.value("profiles", json::array());
    for (const auto& p : profile_list) profiles.push_back(p.get<std::string>());
    std::vector<std::pair<std::string, std::string>> overrides;
    const json override_map = req.value("overrides", json::object()); // kept alive for items()
    for (const auto& [k, v] : override_map.items()) overrides.emplace_back(k, v.get<std::string>());
    engine::ArrangeOptions options;
    options.distance_mm = req.value("distance", 0.0);
    options.allow_rotations = req.value("rotate", false);
    options.align_to_y_axis = req.value("alignY", false);
    json out = json::array();
    for (const auto& r : engine::arrange_models(items, profiles, overrides, options))
        out.push_back({{"dx", r.dx_mm}, {"dy", r.dy_mm}, {"rotation", r.rotation_deg}, {"plate", r.plate}});
    return {{"objects", out}};
}

json op_orient(const json& req) { return {{"matrix", engine::orient_mesh_soup(read_stl(text(req, "model")))}}; }

json op_cut(const json& req) {
    if (!req.contains("z") || !req["z"].is_number()) throw BadRequest("The request needs \"z\"");
    const engine::CutResult r = engine::cut_mesh_soup(read_stl(text(req, "model")), req["z"].get<float>());
    write_stl(text(req, "upper"), r.upper);
    write_stl(text(req, "lower"), r.lower);
    return {{"upper", r.upper.size() / 9}, {"lower", r.lower.size() / 9}};
}
} // namespace

int run_plate(const std::string& request_text, std::string& response) {
    try {
        json req;
        try { req = json::parse(request_text); } catch (const std::exception& e) { throw BadRequest(std::string("The request is not valid JSON: ") + e.what()); }
        const std::string op = text(req, "op");
        json out;
        if (op == "arrange") out = op_arrange(req);
        else if (op == "orient") out = op_orient(req);
        else if (op == "cut") out = op_cut(req);
        else throw BadRequest("Unknown op: " + op);
        response = out.dump();
        return 0;
    } catch (const BadRequest& e) {
        response = json{{"error", e.what()}}.dump();
        return 2;
    } catch (const std::exception& e) {
        response = json{{"error", e.what()}}.dump();
        return 1;
    }
}
} // namespace nozzle_plate
