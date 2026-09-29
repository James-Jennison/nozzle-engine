#include "thumbnail_render.hpp"

#include <algorithm>
#include <cfloat>
#include <array>
#include <cctype>
#include <cmath>
#include <optional>
#include <string>

namespace engine {
using namespace Slic3r;

namespace {

struct Vec2f { float x, y; };

// A fixed isometric camera (looking toward the origin from the (1,1,1) octant, Z up - the
// print's own up axis). x+y+z, evaluated per vertex below, is a monotonic proxy for distance
// along that same view direction, so it doubles as the z-buffer's depth value - no separate
// view/projection matrix needed for a camera this simple.
Vec2f project_isometric(const Vec3f& p) {
    static constexpr float kCos30 = 0.8660254f;
    static constexpr float kSin30 = 0.5f;
    return { (p.x() - p.y()) * kCos30, (p.x() + p.y()) * kSin30 - p.z() };
}

using Rgb = std::array<float, 3>;

// This app's print-orange accent: the colour when the config names no filament colour at all.
constexpr Rgb kFallbackColour{242.f, 117.f, 78.f};

// "#RRGGBB" or "#RRGGBBAA" (alpha ignored); nullopt for anything else.
std::optional<Rgb> parse_colour(const std::string& text) {
    if (text.size() != 7 && text.size() != 9) return std::nullopt;
    if (text[0] != '#') return std::nullopt;
    Rgb rgb;
    for (int i = 0; i < 3; ++i) {
        const std::string byte = text.substr(1 + 2 * i, 2);
        if (!std::isxdigit((unsigned char)byte[0]) || !std::isxdigit((unsigned char)byte[1])) return std::nullopt;
        rgb[i] = float(std::stoi(byte, nullptr, 16));
    }
    return rgb;
}

// Filament `extruder` (1-based) to its filament_colour; a missing or unreadable entry falls back to filament 1's
// colour, then to kFallbackColour.
Rgb filament_colour(const DynamicPrintConfig& config, int extruder) {
    const auto* colours = config.option<ConfigOptionStrings>("filament_colour");
    if (colours == nullptr || colours->values.empty()) return kFallbackColour;
    const size_t index = size_t(std::max(extruder, 1) - 1);
    if (index < colours->values.size())
        if (auto rgb = parse_colour(colours->values[index])) return *rgb;
    return parse_colour(colours->values.front()).value_or(kFallbackColour);
}

struct ColouredPart {
    indexed_triangle_set its; // world/bed coordinates
    Rgb colour;
};

// Every printable part of every instance, split by colour: a part prints with its own filament, else its object's
// ("extruder" 0 means none set, so filament 1), and colour-painted facets with their painted filament. Modifiers,
// negative volumes and support blockers/enforcers are not printed, so are not drawn.
std::vector<ColouredPart> coloured_parts(const Model& model, const DynamicPrintConfig& config) {
    std::vector<ColouredPart> parts;
    for (const ModelObject* object : model.objects) {
        for (const ModelVolume* volume : object->volumes) {
            if (!volume->is_model_part()) continue;
            const int volume_extruder = std::max(volume->extruder_id(), 1);
            // Index 0 holds the unpainted facets, index n those painted with filament n.
            std::vector<indexed_triangle_set> by_state;
            if (volume->is_mm_painted())
                volume->mmu_segmentation_facets.get_facets(*volume, by_state);
            else
                by_state.push_back(volume->mesh().its);
            for (size_t state = 0; state < by_state.size(); ++state) {
                if (by_state[state].indices.empty()) continue;
                const Rgb colour = filament_colour(config, state == 0 ? volume_extruder : int(state));
                for (const ModelInstance* instance : object->instances) {
                    const Transform3d matrix = instance->get_matrix() * volume->get_matrix();
                    ColouredPart part{by_state[state], colour};
                    for (Vec3f& v : part.its.vertices) v = (matrix * v.cast<double>()).cast<float>();
                    parts.push_back(std::move(part));
                }
            }
        }
    }
    return parts;
}

} // namespace

Slic3r::ThumbnailsGeneratorCallback make_thumbnail_callback(const Slic3r::Model& model, const Slic3r::DynamicPrintConfig& config) {
    return [parts = coloured_parts(model, config)](const ThumbnailsParams& params) -> ThumbnailsList {
        ThumbnailsList result;
        size_t vertex_count = 0;
        for (const ColouredPart& part : parts)
            if (!part.its.indices.empty()) vertex_count += part.its.vertices.size();
        if (vertex_count == 0) {
            for (size_t i = 0; i < params.sizes.size(); ++i) result.emplace_back();
            return result;
        }

        // Project every vertex once; every requested size below just rescales the same points.
        std::vector<std::vector<Vec2f>> projected(parts.size());
        std::vector<std::vector<float>> depth(parts.size());
        float minx = FLT_MAX, maxx = -FLT_MAX, miny = FLT_MAX, maxy = -FLT_MAX;
        for (size_t k = 0; k < parts.size(); ++k) {
            const indexed_triangle_set& its = parts[k].its;
            projected[k].resize(its.vertices.size());
            depth[k].resize(its.vertices.size());
            for (size_t i = 0; i < its.vertices.size(); ++i) {
                const Vec3f& v = its.vertices[i];
                Vec2f p = project_isometric(v);
                projected[k][i] = p;
                depth[k][i] = v.x() + v.y() + v.z();
                minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
            }
        }
        const float spanx = std::max(maxx - minx, 1e-3f);
        const float spany = std::max(maxy - miny, 1e-3f);
        const float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;

        const Vec3f light = Vec3f(0.35f, 0.35f, 0.87f).normalized();

        for (const Vec2d& size : params.sizes) {
            unsigned int w = std::max(1, (int)std::lround(size.x()));
            unsigned int h = std::max(1, (int)std::lround(size.y()));
            ThumbnailData data;
            data.set(w, h);
            std::fill(data.pixels.begin(), data.pixels.end(), (unsigned char)0); // transparent

            std::vector<float> zbuffer(size_t(w) * h, -FLT_MAX);
            const float margin = 0.88f; // leaves a border so the silhouette doesn't touch the edge
            const float scale = margin * std::min(w / spanx, h / spany);
            auto to_pixel = [&](const Vec2f& p) -> std::pair<float, float> {
                float px = (p.x - cx) * scale + w * 0.5f;
                float py = h * 0.5f - (p.y - cy) * scale; // image rows run top-down, projected Y runs up
                return {px, py};
            };

            for (size_t k = 0; k < parts.size(); ++k) {
                const indexed_triangle_set& its = parts[k].its;
                const Rgb& base = parts[k].colour;
                for (const Vec3i32& tri : its.indices) {
                    const Vec3f& v0 = its.vertices[tri(0)];
                    const Vec3f& v1 = its.vertices[tri(1)];
                    const Vec3f& v2 = its.vertices[tri(2)];
                    Vec3f normal = (v1 - v0).cross(v2 - v0);
                    float nlen = normal.norm();
                    if (nlen < 1e-9f) continue;
                    normal /= nlen;
                    // abs() rather than a signed facing check: STL winding order isn't reliable
                    // enough to trust for backface culling, and the z-buffer below already handles
                    // hidden-surface removal correctly regardless of a face's winding direction.
                    float shade = std::clamp(0.35f + 0.65f * std::abs(normal.dot(light)), 0.35f, 1.0f);
                    // A small highlight on the lit faces, so a black or very dark filament still shows its shape.
                    float highlight = 32.f * (shade - 0.35f) / 0.65f;
                    unsigned char r = (unsigned char)std::clamp(base[0] * shade + highlight, 0.f, 255.f);
                    unsigned char g = (unsigned char)std::clamp(base[1] * shade + highlight, 0.f, 255.f);
                    unsigned char b = (unsigned char)std::clamp(base[2] * shade + highlight, 0.f, 255.f);

                    auto [x0, y0] = to_pixel(projected[k][tri(0)]);
                    auto [x1, y1] = to_pixel(projected[k][tri(1)]);
                    auto [x2, y2] = to_pixel(projected[k][tri(2)]);
                    float d0 = depth[k][tri(0)], d1 = depth[k][tri(1)], d2 = depth[k][tri(2)];

                    int minPx = std::max(0, (int)std::floor(std::min({x0, x1, x2})));
                    int maxPx = std::min((int)w - 1, (int)std::ceil(std::max({x0, x1, x2})));
                    int minPy = std::max(0, (int)std::floor(std::min({y0, y1, y2})));
                    int maxPy = std::min((int)h - 1, (int)std::ceil(std::max({y0, y1, y2})));
                    if (minPx > maxPx || minPy > maxPy) continue;

                    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
                    if (std::abs(area) < 1e-6f) continue;

                    for (int py = minPy; py <= maxPy; ++py) {
                        for (int px = minPx; px <= maxPx; ++px) {
                            float fx = px + 0.5f, fy = py + 0.5f;
                            float bw0 = ((x1 - fx) * (y2 - fy) - (x2 - fx) * (y1 - fy)) / area;
                            float bw1 = ((x2 - fx) * (y0 - fy) - (x0 - fx) * (y2 - fy)) / area;
                            float bw2 = 1.f - bw0 - bw1;
                            if (bw0 < -1e-4f || bw1 < -1e-4f || bw2 < -1e-4f) continue;
                            float d = bw0 * d0 + bw1 * d1 + bw2 * d2;
                            size_t idx = size_t(py) * w + px;
                            if (d <= zbuffer[idx]) continue;
                            zbuffer[idx] = d;
                            size_t px4 = idx * 4;
                            data.pixels[px4 + 0] = r; data.pixels[px4 + 1] = g;
                            data.pixels[px4 + 2] = b; data.pixels[px4 + 3] = 255;
                        }
                    }
                }
            }
            result.push_back(std::move(data));
        }
        return result;
    };
}

} // namespace engine
