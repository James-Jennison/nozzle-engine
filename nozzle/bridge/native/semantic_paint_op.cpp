#include "semantic_paint_op.hpp"

#include "libslic3r/SemanticPaint.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace nozzle_sp {
namespace {
using json = nlohmann::ordered_json;
namespace SP = Slic3r::SemanticPaint;
struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };
struct Failed : std::runtime_error { using std::runtime_error::runtime_error; };

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw BadRequest("Cannot read " + path);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Binary STL -> indexed triangles with shared vertices; triangle i of the file stays face i.
indexed_triangle_set read_its(const std::string& path) {
    const std::string bytes = read_file(path);
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
    Slic3r::its_merge_vertices(its);
    if (its.indices.size() != n) throw Failed("The model has degenerate triangles; auto-paint needs every triangle intact.");
    return its;
}

std::string text(const json& j, const char* key) {
    if (!j.contains(key) || !j[key].is_string()) throw BadRequest(std::string("The request needs \"") + key + "\"");
    return j[key].get<std::string>();
}
std::string text_or(const json& j, const char* key, const std::string& fallback) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}

void write_file(const std::string& path, const std::string& bytes) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw Failed("Cannot write " + path);
    f.write(bytes.data(), std::streamsize(bytes.size()));
}

json op_segment(const json& req) {
    const indexed_triangle_set its = read_its(text(req, "model"));
    const std::string out_dir = text(req, "out_dir");
    const int size = req.value("size", 384);
    if (size < 64 || size > 2048) throw BadRequest("size must be between 64 and 2048.");
    std::vector<SP::FilamentInfo> filaments;
    if (!req.contains("filaments") || !req["filaments"].is_array() || req["filaments"].empty()) throw BadRequest("The request needs \"filaments\".");
    for (const auto& f : req["filaments"]) {
        SP::FilamentInfo info;
        info.index = f.value("index", int(filaments.size()) + 1);
        info.type = f.value("type", std::string());
        info.color_hex = f.value("color", std::string("#808080"));
        info.name = f.value("name", std::string());
        filaments.push_back(info);
    }
    const SP::Segmentation seg = SP::segment_mesh(its);
    if (seg.segments.empty()) throw Failed("The model has no surface to paint.");

    json views = json::array();
    std::vector<std::string> names, paths;
    for (const SP::View& v : SP::default_views()) {
        const std::string png = SP::encode_png(SP::render_view(its, seg, v, size));
        if (png.empty()) throw Failed("Could not render the " + v.name + " view.");
        const std::string path = out_dir + "/view-" + v.name + ".png";
        write_file(path, png);
        views.push_back({{"name", v.name}, {"path", path}});
        names.push_back(v.name); paths.push_back(path);
    }
    json segments = json::array();
    for (const SP::Segment& s : seg.segments)
        segments.push_back({{"id", s.id}, {"faces", s.faces}, {"area_fraction", s.area_fraction},
                            {"centroid", {s.centroid.x(), s.centroid.y(), s.centroid.z()}}, {"normal", {s.normal.x(), s.normal.y(), s.normal.z()}}});
    const std::string prompt = SP::build_prompt(seg, filaments, text_or(req, "object_name", "the model"), names, text_or(req, "hint", ""));
    return {{"segments", segments}, {"views", views}, {"prompt", prompt}, {"command_prompt", SP::command_prompt(prompt, paths)}};
}

json op_openai_request(const json& req) {
    std::vector<std::string> b64;
    if (req.contains("png_paths")) for (const auto& p : req["png_paths"]) b64.push_back(SP::base64_encode(read_file(p.get<std::string>())));
    return {{"body", SP::openai_request_json(text(req, "model_name"), text(req, "prompt"), b64)}};
}

json op_reply_text(const json& req) {
    const std::string kind = text(req, "kind"), in = text(req, "text");
    if (kind != "command" && kind != "openai") throw BadRequest("kind must be \"command\" or \"openai\".");
    const std::string out = kind == "command" ? SP::command_reply_text(in) : SP::openai_reply_text(in);
    if (out.empty()) throw Failed("The provider returned no answer.");
    return {{"text", out}};
}

json op_assign(const json& req) {
    const indexed_triangle_set its = read_its(text(req, "model"));
    const SP::Segmentation seg = SP::segment_mesh(its);
    const int count = req.value("filament_count", 0);
    if (count < 1) throw BadRequest("filament_count must be at least 1.");
    const auto seg_to_filament = SP::parse_assignments(text(req, "reply"), seg.segments.size(), size_t(count));
    if (!seg_to_filament) throw Failed("The answer did not say which filament goes on which segment.");
    const std::vector<int> faces = SP::face_filaments(seg, *seg_to_filament);
    json runs = json::array();
    for (size_t i = 0; i < faces.size();) {
        size_t j = i;
        while (j < faces.size() && faces[j] == faces[i]) ++j;
        runs.push_back({j - i, faces[i]});
        i = j;
    }
    return {{"segment_to_filament", *seg_to_filament}, {"runs", runs}, {"faces", faces.size()}};
}
} // namespace

int run_semantic_paint(const std::string& request_text, std::string& response) {
    try {
        const json req = json::parse(request_text);
        const std::string op = text(req, "op");
        json out;
        if (op == "segment") out = op_segment(req);
        else if (op == "openai_request") out = op_openai_request(req);
        else if (op == "reply_text") out = op_reply_text(req);
        else if (op == "assign") out = op_assign(req);
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

} // namespace nozzle_sp
