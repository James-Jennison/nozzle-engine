#include "paint_ops.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nozzle_paint {
namespace {
using json = nlohmann::ordered_json;
using namespace Slic3r;
struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };
struct Failed : std::runtime_error { using std::runtime_error::runtime_error; };

indexed_triangle_set read_its(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw BadRequest("Cannot read model " + path);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 84) throw BadRequest("Not a binary STL: " + path);
    uint32_t n; std::memcpy(&n, bytes.data() + 80, 4);
    if (bytes.size() < 84 + size_t(n) * 50) throw BadRequest("Truncated STL: " + path);
    indexed_triangle_set its;
    its.vertices.reserve(size_t(n) * 3); its.indices.reserve(n);
    for (uint32_t t = 0; t < n; ++t) {
        float v[9]; std::memcpy(v, bytes.data() + 84 + size_t(t) * 50 + 12, 36);
        for (int k = 0; k < 3; ++k) its.vertices.emplace_back(v[k * 3], v[k * 3 + 1], v[k * 3 + 2]);
        its.indices.emplace_back(int(t * 3), int(t * 3 + 1), int(t * 3 + 2));
    }
    its_merge_vertices(its);
    if (its.indices.size() != n) throw Failed("The model has degenerate triangles; painting needs every triangle intact.");
    return its;
}

std::string text(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) throw BadRequest(std::string("The request needs \"") + key + "\"");
    return j[key].get<std::string>();
}

Vec3d vec3(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3) throw BadRequest(std::string("\"") + key + "\" must be [x, y, z].");
    return Vec3d(j[key][0].get<double>(), j[key][1].get<double>(), j[key][2].get<double>());
}

// The 3MF's per-triangle strings <-> TriangleSplittingData, the same encoding as FacetsAnnotation (Model.cpp; whose constructors are
// protected). One hexadecimal digit per 4 bits of the triangle's tree, last digit first.
using Data = TriangleSelector::TriangleSplittingData;

Data load_paint(const json& req, size_t triangles) {
    Data data;
    if (!req.contains("paint") || req["paint"].is_null()) return data;
    int last = -1;
    for (const auto& e : req["paint"]) {
        if (!e.is_array() || e.size() != 2 || !e[0].is_number_integer() || !e[1].is_string()) throw BadRequest("\"paint\" entries must be [triangle, \"HEX\"].");
        const int id = e[0].get<int>();
        const std::string s = e[1].get<std::string>();
        if (id <= last || id < 0 || size_t(id) >= triangles) throw BadRequest("\"paint\" triangle ids must increase and lie inside the model.");
        last = id;
        if (s.empty()) continue;
        data.triangles_to_split.emplace_back(id, int(data.bitstream.size()));
        const size_t start = data.bitstream.size();
        for (auto it = s.crbegin(); it != s.crend(); ++it) {
            int dec;
            if (*it >= '0' && *it <= '9') dec = *it - '0';
            else if (*it >= 'A' && *it <= 'F') dec = 10 + (*it - 'A');
            else throw BadRequest("Paint strings are upper-case hexadecimal.");
            for (int b = 0; b < 4; ++b) data.bitstream.push_back(bool(dec & (1 << b)));
        }
        if (!data.update_used_states(start)) {
            data.bitstream.resize(start);
            data.triangles_to_split.pop_back();
        }
    }
    return data;
}

std::string triangle_string(const Data& data, int idx) {
    std::string out;
    auto it = std::lower_bound(data.triangles_to_split.begin(), data.triangles_to_split.end(), idx,
                               [](const TriangleSelector::TriangleBitStreamMapping& l, const int r) { return l.triangle_idx < r; });
    if (it == data.triangles_to_split.end() || it->triangle_idx != idx) return out;
    int offset = it->bitstream_start_idx;
    const int end = ++it == data.triangles_to_split.end() ? int(data.bitstream.size()) : it->bitstream_start_idx;
    while (offset < end) {
        int code = 0;
        for (int i = 3; i >= 0; --i) code = (code << 1) | int(data.bitstream[offset + i]);
        offset += 4;
        out.insert(out.begin(), char(code < 10 ? code + '0' : (code - 10) + 'A'));
    }
    return out;
}

json paint_out(const Data& data) {
    json out = json::array();
    for (const auto& m : data.triangles_to_split) {
        const std::string s = triangle_string(data, m.triangle_idx);
        if (!s.empty()) out.push_back({m.triangle_idx, s});
    }
    return out;
}

json op_stroke(const json& req) {
    const TriangleMesh mesh(read_its(text(req, "model")));
    const size_t triangles = mesh.its.indices.size();
    const Data existing = load_paint(req, triangles);
    TriangleSelector selector(mesh);
    selector.deserialize(existing, false);
    const AABBMesh aabb(mesh.its);
    const std::string mode = req.value("mode", std::string("brush"));
    if (mode != "brush" && mode != "fill" && mode != "smart") throw BadRequest("mode must be \"brush\", \"fill\" or \"smart\".");
    if (!req.contains("strokes") || !req["strokes"].is_array()) throw BadRequest("The request needs \"strokes\".");
    int hits = 0;
    for (const auto& s : req["strokes"]) {
        const Vec3d origin = vec3(s, "origin");
        Vec3d dir = vec3(s, "dir");
        if (dir.norm() < 1e-9) throw BadRequest("A stroke needs a direction.");
        dir.normalize();
        const int state = s.value("state", 1);
        if (state < 0 || state > int(EnforcerBlockerType::ExtruderMax)) throw BadRequest("state is outside Orca's range.");
        const AABBMesh::hit_result hit = aabb.query_ray_hit(origin, dir);
        if (!hit.is_hit()) continue;
        ++hits;
        const auto new_state = static_cast<EnforcerBlockerType>(state);
        if (mode == "fill") {
            // Orca's bucket fill: the connected surface of the same state, up to a sharp edge of "angle" degrees.
            const float angle = req.value("angle", 30.f);
            selector.bucket_fill_select_triangles(hit.position().cast<float>(), hit.face(), TriangleSelector::ClippingPlane(), angle, true, true);
            selector.seed_fill_apply_on_triangles(new_state);
        } else if (mode == "smart") {
            // Orca's smart fill: grows over faces whose neighbours turn less than "angle" degrees.
            const float angle = req.value("angle", 30.f);
            selector.seed_fill_select_triangles(hit.position().cast<float>(), hit.face(), Transform3d::Identity(), TriangleSelector::ClippingPlane(), angle, 0.f, Vec3f::UnitZ(), true);
            selector.seed_fill_apply_on_triangles(new_state);
        } else {
            const double radius = s.value("radius", 1.0);
            if (radius <= 0.0) throw BadRequest("A brush needs a radius.");
            std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::SinglePointCursor::cursor_factory(
                hit.position().cast<float>(), origin.cast<float>(), static_cast<float>(radius), TriangleSelector::CursorType::SPHERE,
                Transform3d::Identity(), TriangleSelector::ClippingPlane());
            selector.select_patch(hit.face(), std::move(cursor), new_state, Transform3d::Identity(), /*triangle_splitting=*/true);
        }
    }
    return {{"paint", paint_out(selector.serialize())}, {"hits", hits}};
}

json op_facets(const json& req) {
    const TriangleMesh mesh(read_its(text(req, "model")));
    const Data existing = load_paint(req, mesh.its.indices.size());
    TriangleSelector selector(mesh);
    selector.deserialize(existing, false);
    std::vector<indexed_triangle_set> per_state;
    selector.get_facets(per_state);
    json states = json::array();
    for (size_t state = 1; state < per_state.size(); ++state) {
        const indexed_triangle_set& its = per_state[state];
        if (its.indices.empty()) continue;
        json tris = json::array();
        for (const stl_triangle_vertex_indices& f : its.indices)
            for (int k = 0; k < 3; ++k)
                for (int a = 0; a < 3; ++a) tris.push_back(its.vertices[f[k]][a]);
        states.push_back({{"state", state}, {"triangles", tris}});
    }
    return {{"states", states}};
}
} // namespace

int run_paint(const std::string& request_text, std::string& response) {
    try {
        const json req = json::parse(request_text);
        const std::string op = text(req, "op");
        json out;
        if (op == "stroke") out = op_stroke(req);
        else if (op == "facets") out = op_facets(req);
        else throw BadRequest("Unknown op \"" + op + "\".");
        response = out.dump();
        return 0;
    } catch (const BadRequest& e) {
        response = json{{"error", e.what()}}.dump();
        return 2;
    } catch (const json::exception& e) {
        response = json{{"error", std::string("Bad request: ") + e.what()}}.dump();
        return 2;
    } catch (const std::exception& e) {
        response = json{{"error", e.what()}}.dump();
        return 1;
    }
}

} // namespace nozzle_paint
