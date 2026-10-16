#include "SemanticPaint.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <optional>
#include <limits>
#include <map>
#include <set>
#include <sstream>

#include <miniz.h>
#include <nlohmann/json.hpp>

namespace Slic3r { namespace SemanticPaint {

namespace {

struct FaceGeom
{
    Vec3d  normal{0., 0., 1.};
    double area{0.};
    Vec3d  centroid{0., 0., 0.};
};

std::vector<FaceGeom> face_geometry(const indexed_triangle_set &its)
{
    std::vector<FaceGeom> out(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const Vec3d a = its.vertices[its.indices[i](0)].cast<double>();
        const Vec3d b = its.vertices[its.indices[i](1)].cast<double>();
        const Vec3d c = its.vertices[its.indices[i](2)].cast<double>();
        const Vec3d n = (b - a).cross(c - a);
        const double len = n.norm();
        out[i].area     = 0.5 * len;
        out[i].normal   = len > 1e-12 ? Vec3d(n / len) : Vec3d(0., 0., 1.);
        out[i].centroid = (a + b + c) / 3.;
    }
    return out;
}

} // namespace

Segmentation segment_mesh(const indexed_triangle_set &its, const SegmentParams &params)
{
    Segmentation result;
    const size_t nf = its.indices.size();
    result.face_segment.assign(nf, -1);
    if (nf == 0)
        return result;

    const std::vector<FaceGeom> geom      = face_geometry(its);
    const std::vector<Vec3i32>  neighbors = its_face_neighbors(its);
    const double                cos_step  = std::cos(params.angle_deg * M_PI / 180.);
    const double                cos_drift = std::cos(params.max_drift_deg * M_PI / 180.);

    // 1. Region growing over the face adjacency graph.
    std::vector<int> label(nf, -1);
    int              next_label = 0;
    for (size_t seed = 0; seed < nf; ++seed) {
        if (label[seed] >= 0)
            continue;
        const int    id = next_label++;
        Vec3d        sum = geom[seed].normal * std::max(geom[seed].area, 1e-12);
        std::vector<int> stack{int(seed)};
        label[seed] = id;
        while (!stack.empty()) {
            const int f = stack.back();
            stack.pop_back();
            const Vec3d mean = sum.norm() > 1e-12 ? Vec3d(sum.normalized()) : geom[f].normal;
            for (int k = 0; k < 3; ++k) {
                const int g = neighbors[f](k);
                if (g < 0 || label[g] >= 0)
                    continue;
                if (geom[f].normal.dot(geom[g].normal) < cos_step || mean.dot(geom[g].normal) < cos_drift)
                    continue;
                label[g] = id;
                sum += geom[g].normal * std::max(geom[g].area, 1e-12);
                stack.push_back(g);
            }
        }
    }

    // 2. Areas and shared border lengths between segments.
    const size_t         nseg = size_t(next_label);
    std::vector<double>  area(nseg, 0.);
    for (size_t f = 0; f < nf; ++f)
        area[label[f]] += geom[f].area;
    const double total_area = std::accumulate(area.begin(), area.end(), 0.);

    std::vector<std::map<int, double>> border(nseg);
    for (size_t f = 0; f < nf; ++f)
        for (int k = 0; k < 3; ++k) {
            const int g = neighbors[f](k);
            if (g < 0 || label[g] == label[f] || size_t(g) < f)
                continue;
            const Vec3d a = its.vertices[its.indices[f](k)].cast<double>();
            const Vec3d b = its.vertices[its.indices[f]((k + 1) % 3)].cast<double>();
            const double len = (b - a).norm();
            border[label[f]][label[g]] += len;
            border[label[g]][label[f]] += len;
        }

    // 3. Merge the smallest segments into their longest-bordering neighbour.
    std::vector<int>             parent(nseg);
    std::iota(parent.begin(), parent.end(), 0);
    std::set<std::pair<double, int>> by_area;
    for (size_t s = 0; s < nseg; ++s)
        by_area.insert({area[s], int(s)});
    size_t alive = nseg;
    std::set<int> unmergeable;
    while (!by_area.empty()) {
        const auto smallest = std::find_if(by_area.begin(), by_area.end(), [&](const auto &e) { return !unmergeable.count(e.second); });
        if (smallest == by_area.end())
            break;
        const int    s    = smallest->second;
        const double frac = total_area > 0. ? area[s] / total_area : 0.;
        if (frac >= params.min_area_fraction && alive <= params.max_segments)
            break;
        int    best = -1;
        double best_len = 0.;
        for (const auto &[u, len] : border[s])
            if (len > best_len) {
                best_len = len;
                best     = u;
            }
        if (best < 0) {
            unmergeable.insert(s); // a floating shell: nothing to merge into
            continue;
        }
        by_area.erase({area[s], s});
        by_area.erase({area[best], best});
        area[best] += area[s];
        by_area.insert({area[best], best});
        for (const auto &[u, len] : border[s]) {
            if (u == best)
                continue;
            border[best][u] += len;
            border[u][best] += len;
            border[u].erase(s);
        }
        border[best].erase(s);
        border[s].clear();
        parent[s] = best;
        --alive;
    }
    auto root = [&](int s) {
        while (parent[s] != s)
            s = parent[s];
        return s;
    };

    // 4. Final ids ordered by area, largest first.
    std::map<int, double> root_area;
    for (size_t f = 0; f < nf; ++f)
        root_area[root(label[f])] += geom[f].area;
    std::vector<std::pair<double, int>> order;
    for (const auto &[r, a] : root_area)
        order.push_back({-a, r});
    std::sort(order.begin(), order.end());
    std::map<int, int> final_id;
    for (size_t i = 0; i < order.size(); ++i)
        final_id[order[i].second] = int(i);
    for (size_t f = 0; f < nf; ++f)
        result.face_segment[f] = final_id[root(label[f])];

    // 5. Statistics.
    result.bbox = BoundingBoxf3();
    for (const Vec3f &v : its.vertices)
        result.bbox.merge(v.cast<double>());
    const Vec3d bmin = result.bbox.min;
    Vec3d       size = result.bbox.size();
    for (int i = 0; i < 3; ++i)
        size(i) = std::max(size(i), 1e-9);

    result.segments.assign(order.size(), Segment());
    std::vector<Vec3d> centroid_sum(order.size(), Vec3d::Zero()), normal_sum(order.size(), Vec3d::Zero());
    for (size_t i = 0; i < order.size(); ++i) {
        result.segments[i].id       = int(i);
        result.segments[i].bbox_min = Vec3d::Constant(std::numeric_limits<double>::max());
        result.segments[i].bbox_max = Vec3d::Constant(std::numeric_limits<double>::lowest());
    }
    for (size_t f = 0; f < nf; ++f) {
        Segment &s = result.segments[result.face_segment[f]];
        ++s.faces;
        s.area += geom[f].area;
        centroid_sum[s.id] += geom[f].centroid * geom[f].area;
        normal_sum[s.id] += geom[f].normal * geom[f].area;
        for (int k = 0; k < 3; ++k) {
            const Vec3d p = ((its.vertices[its.indices[f](k)].cast<double>() - bmin).array() / size.array()).matrix();
            s.bbox_min    = s.bbox_min.cwiseMin(p);
            s.bbox_max    = s.bbox_max.cwiseMax(p);
        }
    }
    for (Segment &s : result.segments) {
        s.area_fraction = total_area > 0. ? s.area / total_area : 0.;
        if (s.area > 0.)
            s.centroid = ((centroid_sum[s.id] / s.area - bmin).array() / size.array()).matrix();
        s.normal = normal_sum[s.id].norm() > 1e-12 ? Vec3d(normal_sum[s.id].normalized()) : Vec3d(0., 0., 1.);
    }
    return result;
}

std::vector<View> default_views()
{
    return {
        {"front", Vec3d(0, 1, 0), Vec3d(0, 0, 1)},
        {"back", Vec3d(0, -1, 0), Vec3d(0, 0, 1)},
        {"left", Vec3d(1, 0, 0), Vec3d(0, 0, 1)},
        {"right", Vec3d(-1, 0, 0), Vec3d(0, 0, 1)},
        {"top", Vec3d(0, 0, -1), Vec3d(0, 1, 0)},
        {"isometric", Vec3d(1, 1, -1).normalized(), Vec3d(0, 0, 1)},
    };
}

namespace {

// 3x5 digit font, one row per 3 bits, top row first.
const uint8_t k_font[10][5] = {{7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
                               {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};

void segment_color(int id, unsigned char out[3])
{
    const double h = std::fmod(id * 0.61803398875, 1.0), s = 0.65, v = 0.95;
    const double c = v * s, x = c * (1. - std::fabs(std::fmod(h * 6., 2.) - 1.)), m = v - c;
    double       r = 0, g = 0, b = 0;
    switch (int(h * 6.) % 6) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    out[0] = (unsigned char) std::lround((r + m) * 255.);
    out[1] = (unsigned char) std::lround((g + m) * 255.);
    out[2] = (unsigned char) std::lround((b + m) * 255.);
}

void put_pixel(Image &img, int x, int y, unsigned char r, unsigned char g, unsigned char b)
{
    if (x < 0 || y < 0 || x >= img.width || y >= img.height)
        return;
    unsigned char *p = &img.rgb[(size_t(y) * img.width + x) * 3];
    p[0] = r; p[1] = g; p[2] = b;
}

void draw_number(Image &img, int number, int cx, int cy, int scale)
{
    const std::string digits = std::to_string(number);
    const int         w      = int(digits.size()) * 4 * scale - scale;
    const int         h      = 5 * scale;
    const int         x0     = cx - w / 2, y0 = cy - h / 2;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t d = 0; d < digits.size(); ++d)
            for (int row = 0; row < 5; ++row)
                for (int col = 0; col < 3; ++col)
                    if (k_font[digits[d] - '0'][row] & (4 >> col)) {
                        const int px = x0 + int(d) * 4 * scale + col * scale, py = y0 + row * scale;
                        for (int dy = 0; dy < scale; ++dy)
                            for (int dx = 0; dx < scale; ++dx) {
                                if (pass == 0)
                                    for (int ox = -1; ox <= 1; ++ox)
                                        for (int oy = -1; oy <= 1; ++oy)
                                            put_pixel(img, px + dx + ox, py + dy + oy, 0, 0, 0);
                                else
                                    put_pixel(img, px + dx, py + dy, 255, 255, 255);
                            }
                    }
}

} // namespace

Image render_view(const indexed_triangle_set &its, const Segmentation &seg, const View &view, int size)
{
    Image img;
    img.width = img.height = size;
    img.rgb.assign(size_t(size) * size * 3, 235); // light background
    if (its.indices.empty() || seg.face_segment.size() != its.indices.size())
        return img;

    const Vec3d f     = view.view_dir.normalized();
    const Vec3d right = f.cross(view.up).normalized();
    const Vec3d up    = right.cross(f);

    std::vector<Vec3d> proj(its.vertices.size()); // x, y on screen, z depth
    double minx = 1e300, maxx = -1e300, miny = 1e300, maxy = -1e300;
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        const Vec3d p = its.vertices[i].cast<double>();
        proj[i]       = Vec3d(p.dot(right), p.dot(up), p.dot(f));
        minx = std::min(minx, proj[i].x()); maxx = std::max(maxx, proj[i].x());
        miny = std::min(miny, proj[i].y()); maxy = std::max(maxy, proj[i].y());
    }
    const double margin = size * 0.06;
    const double range  = std::max({maxx - minx, maxy - miny, 1e-9});
    const double scale  = (size - 2 * margin) / range;
    const double cx = 0.5 * (minx + maxx), cy = 0.5 * (miny + maxy);
    auto sx = [&](double x) { return (x - cx) * scale + size * 0.5; };
    auto sy = [&](double y) { return size * 0.5 - (y - cy) * scale; };

    std::vector<float> zbuf(size_t(size) * size, std::numeric_limits<float>::max());
    std::vector<int>   idbuf(size_t(size) * size, -1);
    const Vec3d        light = (-f + 0.4 * up + 0.3 * right).normalized();

    for (size_t t = 0; t < its.indices.size(); ++t) {
        const Vec3d &a = proj[its.indices[t](0)], &b = proj[its.indices[t](1)], &c = proj[its.indices[t](2)];
        const double ax = sx(a.x()), ay = sy(a.y()), bx = sx(b.x()), by = sy(b.y()), cxs = sx(c.x()), cys = sy(c.y());
        const double det = (bx - ax) * (cys - ay) - (cxs - ax) * (by - ay);
        if (std::fabs(det) < 1e-9)
            continue;
        const int x0 = std::max(0, int(std::floor(std::min({ax, bx, cxs})))), x1 = std::min(size - 1, int(std::ceil(std::max({ax, bx, cxs}))));
        const int y0 = std::max(0, int(std::floor(std::min({ay, by, cys})))), y1 = std::min(size - 1, int(std::ceil(std::max({ay, by, cys}))));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double px = x + 0.5, py = y + 0.5;
                const double w1 = ((px - ax) * (cys - ay) - (cxs - ax) * (py - ay)) / det;
                const double w2 = ((bx - ax) * (py - ay) - (px - ax) * (by - ay)) / det;
                const double w0 = 1. - w1 - w2;
                if (w0 < -1e-6 || w1 < -1e-6 || w2 < -1e-6)
                    continue;
                const float z = float(w0 * a.z() + w1 * b.z() + w2 * c.z());
                const size_t idx = size_t(y) * size + x;
                if (z < zbuf[idx]) {
                    zbuf[idx]  = z;
                    idbuf[idx] = int(t);
                }
            }
    }

    // Shade and collect label anchors.
    const size_t nseg = seg.segments.size();
    std::vector<double> sumx(nseg, 0.), sumy(nseg, 0.);
    std::vector<size_t> cnt(nseg, 0);
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int t = idbuf[size_t(y) * size + x];
            if (t < 0)
                continue;
            const int s = seg.face_segment[t];
            const Vec3d n = ((its.vertices[its.indices[t](1)] - its.vertices[its.indices[t](0)]).cast<double>())
                                .cross((its.vertices[its.indices[t](2)] - its.vertices[its.indices[t](0)]).cast<double>());
            const double shade = 0.55 + 0.45 * std::fabs(n.norm() > 1e-12 ? n.normalized().dot(light) : 0.);
            unsigned char col[3];
            segment_color(s, col);
            put_pixel(img, x, y, (unsigned char) (col[0] * shade), (unsigned char) (col[1] * shade), (unsigned char) (col[2] * shade));
            sumx[s] += x; sumy[s] += y; ++cnt[s];
        }
    std::vector<double> best_d(nseg, 1e300);
    std::vector<std::pair<int, int>> anchor(nseg, {-1, -1});
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const int t = idbuf[size_t(y) * size + x];
            if (t < 0)
                continue;
            const int s = seg.face_segment[t];
            if (cnt[s] < 60)
                continue;
            const double d = std::pow(x - sumx[s] / cnt[s], 2) + std::pow(y - sumy[s] / cnt[s], 2);
            if (d < best_d[s]) {
                best_d[s]  = d;
                anchor[s] = {x, y};
            }
        }
    const int label_scale = std::max(2, size / 128);
    for (size_t s = 0; s < nseg; ++s)
        if (anchor[s].first >= 0)
            draw_number(img, int(s), anchor[s].first, anchor[s].second, label_scale);
    return img;
}

std::string encode_png(const Image &image)
{
    if (image.width <= 0 || image.height <= 0 || image.rgb.size() != size_t(image.width) * image.height * 3)
        return {};
    size_t len = 0;
    void  *buf = tdefl_write_image_to_png_file_in_memory_ex(image.rgb.data(), image.width, image.height, 3, &len, 6, MZ_FALSE);
    if (buf == nullptr)
        return {};
    std::string out(static_cast<const char *>(buf), len);
    mz_free(buf);
    return out;
}

std::string base64_encode(const std::string &bytes)
{
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8) | uint8_t(bytes[i + 2]);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
    }
    if (i + 1 == bytes.size()) {
        const uint32_t v = uint8_t(bytes[i]) << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == bytes.size()) {
        const uint32_t v = (uint8_t(bytes[i]) << 16) | (uint8_t(bytes[i + 1]) << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

std::string color_name(const std::string &hex)
{
    if (hex.size() < 7 || hex[0] != '#')
        return "unknown";
    auto comp = [&](int i) { return std::strtol(hex.substr(1 + 2 * i, 2).c_str(), nullptr, 16) / 255.0; };
    const double r = comp(0), g = comp(1), b = comp(2);
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    const double v = mx, s = mx > 0 ? d / mx : 0;
    if (v < 0.18)
        return "black";
    if (s < 0.15)
        return v > 0.88 ? "white" : (v > 0.6 ? "light gray" : (v > 0.35 ? "gray" : "dark gray"));
    double h = 0;
    if (d > 0) {
        if (mx == r) h = std::fmod((g - b) / d, 6.);
        else if (mx == g) h = (b - r) / d + 2.;
        else h = (r - g) / d + 4.;
        h *= 60.;
        if (h < 0) h += 360.;
    }
    std::string name;
    if (h < 15 || h >= 345) name = "red";
    else if (h < 40) name = (v < 0.6 ? "brown" : "orange");
    else if (h < 70) name = "yellow";
    else if (h < 160) name = "green";
    else if (h < 200) name = "cyan";
    else if (h < 255) name = "blue";
    else if (h < 290) name = "purple";
    else name = "pink";
    if (name != "brown" && v < 0.5) name = "dark " + name;
    else if (s < 0.45 && v > 0.8) name = "light " + name;
    return name;
}

namespace {

std::string axis_words(const Segment &s)
{
    auto band = [](double c, const char *lo, const char *mid, const char *hi) { return c < 0.33 ? lo : (c > 0.66 ? hi : mid); };
    std::ostringstream o;
    o << band(s.centroid.z(), "low", "middle", "high") << ", " << band(s.centroid.x(), "left", "center", "right") << ", "
      << band(s.centroid.y(), "front", "middle", "back");
    return o.str();
}

std::string facing_words(const Vec3d &n)
{
    if (n.z() > 0.7) return "facing up";
    if (n.z() < -0.7) return "facing down";
    const double ax = std::fabs(n.x()), ay = std::fabs(n.y());
    if (std::max(ax, ay) < 0.5) return "slanted";
    if (ay >= ax) return n.y() < 0 ? "facing front" : "facing back";
    return n.x() < 0 ? "facing left" : "facing right";
}

} // namespace

std::string build_prompt(const Segmentation &seg, const std::vector<FilamentInfo> &filaments, const std::string &object_name,
                         const std::vector<std::string> &view_names, const std::string &user_hint)
{
    std::ostringstream o;
    o << "You are helping colour a 3D-printable model for a multi-filament printer.\n";
    o << "The model" << (object_name.empty() ? std::string() : " (\"" + object_name + "\")")
      << " is shown in " << view_names.size() << " rendered views, in this order:";
    for (size_t i = 0; i < view_names.size(); ++i)
        o << (i ? ", " : " ") << view_names[i];
    o << ".\nIts surface has been split into numbered segments; each segment has its own tint and its number drawn on it.\n";
    o << "Coordinates below are relative to the model: x 0=left..1=right, y 0=front..1=back, z 0=bottom..1=top.\n\n";
    o << "Segments:\n";
    for (const Segment &s : seg.segments) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "  %d: %.1f%% of the surface, %s, %s; x %.2f-%.2f, y %.2f-%.2f, z %.2f-%.2f\n", s.id,
                      s.area_fraction * 100., axis_words(s).c_str(), facing_words(s.normal).c_str(), s.bbox_min.x(), s.bbox_max.x(),
                      s.bbox_min.y(), s.bbox_max.y(), s.bbox_min.z(), s.bbox_max.z());
        o << buf;
    }
    o << "\nAvailable filaments (slot: type, colour):\n";
    for (const FilamentInfo &f : filaments)
        o << "  " << f.index << ": " << (f.type.empty() ? "filament" : f.type) << ", " << f.color_hex << " (" << color_name(f.color_hex) << ")"
          << (f.name.empty() ? std::string() : ", " + f.name) << "\n";
    o << "\nTask: decide what the object is, then give every segment the filament slot that makes a sensible, attractive coloured "
         "print. Use only the slots listed above. Use 0 for a segment that should stay the base colour. "
         "Keep related parts (for example both eyes, or all petals) the same colour.\n";
    if (!user_hint.empty())
        o << "Extra instruction from the user: " << user_hint << "\n";
    o << "\nAnswer with JSON only, exactly in this shape: "
         "{\"object\":\"short description\",\"assignments\":[{\"segment\":0,\"filament\":1}],\"notes\":\"one sentence\"}\n";
    return o.str();
}

std::string claude_request_json(const std::string &model, const std::string &prompt, const std::vector<std::string> &png_b64)
{
    nlohmann::json content = nlohmann::json::array();
    for (const std::string &b : png_b64)
        content.push_back({{"type", "image"}, {"source", {{"type", "base64"}, {"media_type", "image/png"}, {"data", b}}}});
    content.push_back({{"type", "text"}, {"text", prompt}});
    nlohmann::json body = {{"model", model}, {"max_tokens", 1500}, {"messages", nlohmann::json::array({{{"role", "user"}, {"content", content}}})}};
    return body.dump();
}

std::string ollama_request_json(const std::string &model, const std::string &prompt, const std::vector<std::string> &png_b64)
{
    nlohmann::json images = nlohmann::json::array();
    for (const std::string &b : png_b64)
        images.push_back(b);
    nlohmann::json body = {{"model", model},
                           {"stream", false},
                           {"format", "json"},
                           {"messages", nlohmann::json::array({{{"role", "user"}, {"content", prompt}, {"images", images}}})}};
    return body.dump();
}

std::string claude_cli_prompt(const std::string &prompt, const std::vector<std::string> &image_paths)
{
    std::ostringstream o;
    o << "Use the Read tool to open each of these image files (rendered views of the model, in order) before answering:\n";
    for (const std::string &p : image_paths)
        o << "  " << p << "\n";
    o << "\n" << prompt;
    return o.str();
}

std::string claude_cli_reply_text(const std::string &body)
{
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || j.value("is_error", false) || !j.contains("result") || !j["result"].is_string())
        return {};
    return j["result"].get<std::string>();
}

std::string claude_reply_text(const std::string &body)
{
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("content") || !j["content"].is_array())
        return {};
    std::string out;
    for (const auto &c : j["content"])
        if (c.is_object() && c.value("type", "") == "text" && c.contains("text") && c["text"].is_string())
            out += c["text"].get<std::string>();
    return out;
}

std::string ollama_reply_text(const std::string &body)
{
    const nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("message") || !j["message"].is_object())
        return {};
    const auto &m = j["message"];
    return m.contains("content") && m["content"].is_string() ? m["content"].get<std::string>() : std::string();
}

namespace {

std::optional<int> as_int(const nlohmann::json &v)
{
    if (v.is_number_integer())
        return v.get<int>();
    if (v.is_number_float())
        return int(std::lround(v.get<double>()));
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        size_t            i = 0;
        while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i])))
            ++i;
        if (i < s.size())
            return std::atoi(s.c_str() + i);
    }
    return std::nullopt;
}

} // namespace

std::optional<std::vector<int>> parse_assignments(const std::string &text, size_t segment_count, size_t filament_count)
{
    for (size_t pos = text.find('{'); pos != std::string::npos; pos = text.find('{', pos + 1)) {
        // Find the matching close brace, ignoring braces inside strings.
        int  depth = 0;
        bool in_str = false, esc = false;
        size_t end = std::string::npos;
        for (size_t i = pos; i < text.size(); ++i) {
            const char c = text[i];
            if (in_str) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') in_str = false;
            } else if (c == '"') in_str = true;
            else if (c == '{') ++depth;
            else if (c == '}' && --depth == 0) { end = i; break; }
        }
        if (end == std::string::npos)
            continue;
        const nlohmann::json j = nlohmann::json::parse(text.substr(pos, end - pos + 1), nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("assignments") || !j["assignments"].is_array())
            continue;
        std::vector<int> out(segment_count, 0);
        for (const auto &a : j["assignments"]) {
            if (!a.is_object() || !a.contains("segment") || !a.contains("filament"))
                continue;
            const auto s = as_int(a["segment"]);
            const auto f = as_int(a["filament"]);
            if (!s || !f || *s < 0 || size_t(*s) >= segment_count || *f < 1 || size_t(*f) > filament_count)
                continue;
            out[*s] = *f;
        }
        return out;
    }
    return std::nullopt;
}

std::vector<int> face_filaments(const Segmentation &seg, const std::vector<int> &seg_to_filament)
{
    std::vector<int> out(seg.face_segment.size(), 0);
    for (size_t f = 0; f < out.size(); ++f) {
        const int s = seg.face_segment[f];
        if (s >= 0 && size_t(s) < seg_to_filament.size())
            out[f] = seg_to_filament[s];
    }
    return out;
}

}} // namespace Slic3r::SemanticPaint
