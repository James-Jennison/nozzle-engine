// Full Spectrum colour helpers for nozzle-engine: the wx-free colour arithmetic Snapmaker Orca's GUI uses for colour
// mixing, ported from Snapmaker Orca at commit cbf7bbb0b3 (AGPL-3.0, as is this file):
//   src/slic3r/Utils/ColorSpaceConvert.cpp   PivotRGB, PivotXYZ, RGB2XYZ, XYZ2Lab, RGB2Lab, DeltaE00 (verbatim maths,
//                                            including the float/double conversions)
//   src/slic3r/GUI/MixedGradientSelector.cpp blend_pair_filament_mixer
//   src/slic3r/GUI/MixedFilamentColorMapPanel.cpp blend_multi_filament_mixer
//   src/slic3r/GUI/MixedColorMatchHelpers.cpp parse_mixed_color, normalize_color_match_hex, try_parse_color_match_hex
// wxColour is replaced by FsColour (8-bit RGB plus the IsOk flag); wxString by std::string.
#pragma once

#include <libslic3r/filament_mixer.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace nozzle_fs {

// Stand-in for wxColour: 8-bit channels and wxColour::IsOk().
struct FsColour {
    int  r = 0, g = 0, b = 0;
    bool ok = false;
    FsColour() = default;
    FsColour(int r_, int g_, int b_) : r(r_), g(g_), b(b_), ok(true) {}
    int  Red() const { return r; }
    int  Green() const { return g; }
    int  Blue() const { return b; }
    bool IsOk() const { return ok; }
    // wxColour::GetAsString(wxC2S_HTML_SYNTAX)
    std::string hex() const {
        char buf[8];
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r & 0xFF, g & 0xFF, b & 0xFF);
        return buf;
    }
};

inline int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// wxColour(const std::string&) for the forms Nozzle uses: "#RRGGBB" and "#RRGGBBAA" (alpha dropped). Anything else is
// !IsOk(), as wxColour is for an unparsable string. (wx also accepts colour names and "rgb(...)"; Nozzle never sends them.)
inline FsColour colour_from_string(const std::string& s) {
    if (!(s.size() == 7 || s.size() == 9) || s[0] != '#') return FsColour();
    int v[6];
    for (int i = 0; i < 6; ++i) { v[i] = hex_digit(s[size_t(i) + 1]); if (v[i] < 0) return FsColour(); }
    if (s.size() == 9 && (hex_digit(s[7]) < 0 || hex_digit(s[8]) < 0)) return FsColour();
    return FsColour(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]);
}

// MixedColorMatchHelpers.cpp parse_mixed_color
inline FsColour parse_mixed_color(const std::string& value) {
    FsColour color = colour_from_string(value);
    if (!color.IsOk()) color = colour_from_string("#26A69A");
    return color;
}

// MixedColorMatchHelpers.cpp normalize_color_match_hex
inline std::string normalize_color_match_hex(const std::string& value) {
    std::string normalized = value;
    const char* ws = " \t\r\n";
    const size_t start = normalized.find_first_not_of(ws);
    if (start == std::string::npos) normalized.clear();
    else normalized = normalized.substr(start, normalized.find_last_not_of(ws) - start + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    if (!normalized.empty() && normalized[0] != '#') normalized.insert(normalized.begin(), '#');
    if (normalized.length() == 9) normalized = normalized.substr(0, 7);
    return normalized;
}

// MixedColorMatchHelpers.cpp try_parse_color_match_hex
inline bool try_parse_color_match_hex(const std::string& value, FsColour& color_out) {
    const std::string normalized = normalize_color_match_hex(value);
    if (normalized.length() != 7) return false;
    for (size_t idx = 1; idx < normalized.length(); ++idx)
        if (!std::isxdigit(static_cast<unsigned char>(normalized[idx]))) return false;
    FsColour parsed = colour_from_string(normalized);
    if (!parsed.IsOk()) return false;
    color_out = parsed;
    return true;
}

// ---- ColorSpaceConvert.cpp (verbatim) ----
inline double PivotRGB(double n) { return (n > 0.04045 ? std::pow((n + 0.055) / 1.055, 2.4) : n / 12.92) * 100.0; }
inline double PivotXYZ(double n) {
    double i = std::cbrt(n);
    return n > 0.008856 ? i : 7.787 * n + 16.0 / 116.0;
}
inline void RGB2XYZ(float R, float G, float B, float* X, float* Y, float* Z) {
    R = PivotRGB(R);
    G = PivotRGB(G);
    B = PivotRGB(B);
    *X = 0.412453f * R + 0.357580f * G + 0.180423f * B;
    *Y = 0.212671f * R + 0.715160f * G + 0.072169f * B;
    *Z = 0.019334f * R + 0.119193f * G + 0.950227f * B;
}
inline void XYZ2Lab(float X, float Y, float Z, float* L, float* a, float* b) {
    double REF_X = 95.047;
    double REF_Y = 100.000;
    double REF_Z = 108.883;
    double x = PivotXYZ(X / REF_X);
    double y = PivotXYZ(Y / REF_Y);
    double z = PivotXYZ(Z / REF_Z);
    *L = 116.0 * y - 16.0;
    *a = 500.0 * (x - y);
    *b = 200.0 * (y - z);
}
inline void RGB2Lab(float R, float G, float B, float* L, float* a, float* b) {
    float X = 0.0f, Y = 0.0f, Z = 0.0f;
    RGB2XYZ(R, G, B, &X, &Y, &Z);
    XYZ2Lab(X, Y, Z, L, a, b);
}
inline float DeltaE00(float l1, float a1, float b1, float l2, float a2, float b2) {
    auto rad2deg = [](float rad) { return 360.0 * rad / (2.0 * M_PI); };
    auto deg2rad = [](float deg) { return (2.0 * M_PI * deg) / 360.0; };
    float avgL = (l1 + l2) / 2.0;
    float c1 = std::sqrt(std::pow(a1, 2) + std::pow(b1, 2));
    float c2 = std::sqrt(std::pow(a2, 2) + std::pow(b2, 2));
    float avgC = (c1 + c2) / 2.0;
    float g = (1.0 - std::sqrt(std::pow(avgC, 7) / (std::pow(avgC, 7) + std::pow(25.0, 7)))) / 2.0;
    float a1p = a1 * (1.0 + g);
    float a2p = a2 * (1.0 + g);
    float c1p = std::sqrt(std::pow(a1p, 2) + std::pow(b1, 2));
    float c2p = std::sqrt(std::pow(a2p, 2) + std::pow(b2, 2));
    float avgCp = (c1p + c2p) / 2.0;
    float h1p = rad2deg(std::atan2(b1, a1p));
    if (h1p < 0.0) h1p = h1p + 360.0;
    float h2p = rad2deg(std::atan2(b2, a2p));
    if (h2p < 0.0) h2p = h2p + 360;
    float avghp = std::abs(h1p - h2p) > 180.0 ? (h1p + h2p + 360.0) / 2.0 : (h1p + h2p) / 2.0;
    float t = 1.0 - 0.17 * std::cos(deg2rad(avghp - 30.0)) + 0.24 * std::cos(deg2rad(2.0 * avghp)) +
              0.32 * std::cos(deg2rad(3.0 * avghp + 6.0)) - 0.2 * std::cos(deg2rad(4.0 * avghp - 63.0));
    float deltahp = h2p - h1p;
    if (std::abs(deltahp) > 180.0) {
        if (h2p <= h1p) deltahp += 360.0;
        else deltahp -= 360.0;
    }
    float deltalp = l2 - l1;
    float deltacp = c2p - c1p;
    deltahp = 2.0 * std::sqrt(c1p * c2p) * std::sin(deg2rad(deltahp) / 2.0);
    float sl = 1.0 + ((0.015 * std::pow(avgL - 50.0, 2)) / std::sqrt(20.0 + std::pow(avgL - 50.0, 2)));
    float sc = 1.0 + 0.045 * avgCp;
    float sh = 1.0 + 0.015 * avgCp * t;
    float deltaro = 30.0 * std::exp(-(std::pow((avghp - 275.0) / 25.0, 2)));
    float rc = 2.0 * std::sqrt(std::pow(avgCp, 7) / (std::pow(avgCp, 7) + std::pow(25.0, 7)));
    float rt = -rc * std::sin(2.0 * deg2rad(deltaro));
    float kl = 1;
    float kc = 1;
    float kh = 1;
    float delta_e00 = std::sqrt(std::pow(deltalp / (kl * sl), 2) + std::pow(deltacp / (kc * sc), 2) + std::pow(deltahp / (kh * sh), 2) +
                                rt * (deltacp / (kc * sc)) * (deltahp / (kh * sh)));
    return delta_e00;
}

// ---- MixedGradientSelector.cpp blend_pair_filament_mixer ----
inline FsColour blend_pair_filament_mixer(const FsColour& left, const FsColour& right, float t) {
    const FsColour safe_left  = left.IsOk() ? left : colour_from_string("#26A69A");
    const FsColour safe_right = right.IsOk() ? right : colour_from_string("#26A69A");
    unsigned char out_r = static_cast<unsigned char>(safe_left.Red());
    unsigned char out_g = static_cast<unsigned char>(safe_left.Green());
    unsigned char out_b = static_cast<unsigned char>(safe_left.Blue());
    ::Slic3r::filament_mixer_lerp(static_cast<unsigned char>(safe_left.Red()), static_cast<unsigned char>(safe_left.Green()),
                                  static_cast<unsigned char>(safe_left.Blue()), static_cast<unsigned char>(safe_right.Red()),
                                  static_cast<unsigned char>(safe_right.Green()), static_cast<unsigned char>(safe_right.Blue()),
                                  std::clamp(t, 0.f, 1.f), &out_r, &out_g, &out_b);
    return FsColour(out_r, out_g, out_b);
}

// ---- MixedFilamentColorMapPanel.cpp blend_multi_filament_mixer ----
inline FsColour blend_multi_filament_mixer(const std::vector<FsColour>& colors, const std::vector<double>& weights) {
    if (colors.empty() || weights.empty()) return colour_from_string("#26A69A");
    unsigned char out_r = 0, out_g = 0, out_b = 0;
    double        accumulated_weight = 0.0;
    bool          has_color          = false;
    for (size_t i = 0; i < colors.size() && i < weights.size(); ++i) {
        const double weight = std::max(0.0, weights[i]);
        if (weight <= 0.0) continue;
        const FsColour      safe = colors[i].IsOk() ? colors[i] : colour_from_string("#26A69A");
        const unsigned char r    = static_cast<unsigned char>(safe.Red());
        const unsigned char g    = static_cast<unsigned char>(safe.Green());
        const unsigned char b    = static_cast<unsigned char>(safe.Blue());
        if (!has_color) {
            out_r = r; out_g = g; out_b = b;
            accumulated_weight = weight;
            has_color          = true;
            continue;
        }
        const double new_total = accumulated_weight + weight;
        if (new_total <= 0.0) continue;
        const float t = float(weight / new_total);
        ::Slic3r::filament_mixer_lerp(out_r, out_g, out_b, r, g, b, t, &out_r, &out_g, &out_b);
        accumulated_weight = new_total;
    }
    if (!has_color) return colour_from_string("#26A69A");
    return FsColour(out_r, out_g, out_b);
}

} // namespace nozzle_fs
