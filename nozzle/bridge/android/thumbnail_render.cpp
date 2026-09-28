#include "thumbnail_render.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

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

} // namespace

Slic3r::ThumbnailsGeneratorCallback make_thumbnail_callback(Slic3r::TriangleMesh mesh) {
    return [mesh = std::move(mesh)](const ThumbnailsParams& params) -> ThumbnailsList {
        ThumbnailsList result;
        const indexed_triangle_set& its = mesh.its;
        if (its.indices.empty()) {
            for (size_t i = 0; i < params.sizes.size(); ++i) result.emplace_back();
            return result;
        }

        // Project every vertex once; every requested size below just rescales the same points.
        std::vector<Vec2f> projected(its.vertices.size());
        std::vector<float> depth(its.vertices.size());
        float minx = FLT_MAX, maxx = -FLT_MAX, miny = FLT_MAX, maxy = -FLT_MAX;
        for (size_t i = 0; i < its.vertices.size(); ++i) {
            const Vec3f& v = its.vertices[i];
            Vec2f p = project_isometric(v);
            projected[i] = p;
            depth[i] = v.x() + v.y() + v.z();
            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
        }
        const float spanx = std::max(maxx - minx, 1e-3f);
        const float spany = std::max(maxy - miny, 1e-3f);
        const float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;

        const Vec3f light = Vec3f(0.35f, 0.35f, 0.87f).normalized();
        // This app's print-orange accent (matches the color used elsewhere for active-job
        // state), not an attempt to guess the real filament color - libslic3r's headless config
        // here has no reliable per-object color to draw from.
        constexpr float base_r = 242.f, base_g = 117.f, base_b = 78.f;

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
                unsigned char r = (unsigned char)std::clamp(base_r * shade, 0.f, 255.f);
                unsigned char g = (unsigned char)std::clamp(base_g * shade, 0.f, 255.f);
                unsigned char b = (unsigned char)std::clamp(base_b * shade, 0.f, 255.f);

                auto [x0, y0] = to_pixel(projected[tri(0)]);
                auto [x1, y1] = to_pixel(projected[tri(1)]);
                auto [x2, y2] = to_pixel(projected[tri(2)]);
                float d0 = depth[tri(0)], d1 = depth[tri(1)], d2 = depth[tri(2)];

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
            result.push_back(std::move(data));
        }
        return result;
    };
}

} // namespace engine
