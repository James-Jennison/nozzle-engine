// Full Spectrum (colour mixing) for nozzle-engine. A port of Snapmaker Orca's colour-mixing GUI logic, commit cbf7bbb0b3
// (AGPL-3.0, as is this file). The mixed-filament list itself is libslic3r's MixedFilamentManager, used unchanged; what is
// ported here is the GUI code around it, with wxColour replaced by FsColour and wxString by std::string, and the bitmap /
// widget code dropped. Maths, thresholds, step sizes, heap sizes and tie-breaking are kept identical.
//
// Ported from src/slic3r/GUI/MixedColorMatchHelpers.cpp:
//   normalize_color_match_weights, expand_color_match_recipe_weights, summarize_color_match_recipe, BlendLUT,
//   sRGB_to_CIELab, delta_e_lab, build_blend_lut, blend_weighted_lab_accurate, color_delta_e00,
//   build_best_color_match_recipe (lines 363-619), compute_color_match_recipe_display_color,
//   decode_color_match_gradient_weights, build_pair_color_match_candidate, build_multi_color_match_candidate,
//   color_match_weights_within_range, build_color_match_sequence, blend_sequence_filament_mixer,
//   batch_match_model_colors, assign_batch_virtual_filament_ids, merge_duplicate_recipe_mappings
// Ported from src/slic3r/GUI/MixedFilamentBatchDialog.cpp:
//   FULL_SPECTRUM_FALLBACK_COLORS, kMinComponentPercent/kMaxComponentPercent, kDeltaEGoodMax/kDeltaEFairMax,
//   load_palette_colors (target de-duplication), load_recommended_palette, the dialog constructor's default palette
//   selection, launch_background_match (the worker body, lines 2379-2675, run synchronously)
// Ported from src/slic3r/GUI/Plater.cpp:
//   the batch-match apply path of the "Color Mixing Match" button (lines ~2650-3000: recommended-palette write-back,
//   in-place recipe update of existing custom rows, add_batch_custom_filaments, assigned-id fixup),
//   Sidebar::init_color_mix_panel's "+" and "-" handlers (~6585-6640), Sidebar::update_color_mix_panel's display context
//   and row label (~6645-6780), the row Edit / Delete menu handlers (~6840-7031)
// Ported from src/slic3r/GUI/MixedFilamentDialog.cpp:
//   collect_result() for all four modes (3246-3414), validate_cycle_pattern (3155-3227), update_compatibility_warning /
//   get_ratio_warning_msg (2173-2351), the Match card's recipe hand-off (1224-1275), build_swatch_grid (2607-2886),
//   and the tri-picker weight clamps (1881-1903, 2004-2025)
// Ported from src/slic3r/GUI/MixedColorMatchHelpers.cpp (also): the material-compatibility check (1027-1359),
//   build_color_match_presets (201-268), build_mixed_filament_display_context (744-802)
// Ported from src/slic3r/GUI/MixedColorMatchPanel.cpp: launch_recipe_match / sync_recipe_preview (363-372, 494-505)
// Ported from src/libslic3r/PresetBundle.cpp: build_filament_id_remap (3955-4090, the no-physical-deletion case)
// Ported from src/slic3r/GUI/Plater.cpp (also): Sidebar::cleanup_unused_filaments_after_batch_match (8652-8990,
//   mixed rows only), the recommended-mode id remap of the batch apply (~2735-2750)
#include "full_spectrum.hpp"
#include "fs_color.hpp"

#include <libslic3r/libslic3r.h>
#include <libslic3r/MixedFilament.hpp>
#include <libslic3r/FilamentColorLibrary.hpp>
#include <libslic3r/Utils.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>

namespace nozzle_fs {

using namespace Slic3r;
using json = nlohmann::ordered_json;

struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };

// ---------------------------------------------------------------------------------------------------------------------
// MixedColorMatchHelpers.hpp / .cpp
// ---------------------------------------------------------------------------------------------------------------------

struct CIELab { double L, a, b; };

class BlendLUT {
public:
    BlendLUT() : m_n(0) {}
    explicit BlendLUT(size_t n) : m_n(n) {
        if (n < 2) { m_n = 0; return; }
        m_pair.resize(n);
        for (size_t a = 0; a < n; ++a) {
            m_pair[a].resize(n - a);
            for (size_t b = a; b < n; ++b) m_pair[a][b - a].resize(101);
        }
    }
    size_t size() const { return m_n; }
    bool   empty() const { return m_n < 2; }
    const CIELab& get(size_t a, size_t b, int percent) const {
        assert(a < b);
        return m_pair[a][b - a][percent];
    }

private:
    size_t m_n;
    std::vector<std::vector<std::vector<CIELab>>> m_pair;
    friend BlendLUT build_blend_lut(const std::vector<FsColour>& palette);
};

struct MixedColorMatchRecipeResult {
    bool         cancelled     = false;
    bool         valid         = false;
    unsigned int component_a   = 1;
    unsigned int component_b   = 2;
    int          mix_b_percent = 50;
    std::string  manual_pattern;
    std::string  gradient_component_ids;
    std::string  gradient_component_weights;
    FsColour     preview_color = colour_from_string("#26A69A");
    double       delta_e       = std::numeric_limits<double>::infinity();
};


// ---- Material compatibility (MixedColorMatchHelpers.cpp 1027-1314) ----
// The preset bundle is replaced by a per-slot filament_type list. filament_compatibility.json defaults to the table
// Snapmaker ships (resources/profiles/Snapmaker/filament/filament_compatibility.json at cbf7bbb0b3); a request may name
// another copy with "compatibility".

enum class FilamentCategory : uint8_t { PLA, PETG, TPU, PET, ABS, ASA, PC, PA, SUPPORT, UNKNOWN };
static constexpr const char* k_category_names[] = {"PLA", "PETG", "TPU", "PET", "ABS", "ASA", "PC", "PA", "SUPPORT"};
static constexpr size_t k_category_count = sizeof(k_category_names) / sizeof(k_category_names[0]);
static constexpr size_t k_compat_dim     = static_cast<size_t>(FilamentCategory::UNKNOWN) + 1;

static const char* k_shipped_compatibility_json = R"({
  "compatibility": {
    "PLA":     ["PC"],
    "PETG":    ["TPU", "PET", "ABS", "ASA", "PC"],
    "TPU":     ["PETG", "PET"],
    "PET":     ["PETG", "TPU", "ABS", "ASA", "PC"],
    "ABS":     ["PETG", "PET", "ASA", "PC", "PA"],
    "ASA":     ["PETG", "PET", "ABS", "PC", "PA"],
    "PC":      ["PLA", "PETG", "PET", "ABS", "ASA", "PA"],
    "PA":      ["ABS", "ASA", "PC"],
    "SUPPORT": []
  }
})";

static FilamentCategory filament_category_from_name(const std::string& name)
{
    for (size_t i = 0; i < k_category_count; ++i)
        if (name == k_category_names[i]) return static_cast<FilamentCategory>(i);
    return FilamentCategory::UNKNOWN;
}

static std::vector<std::vector<bool>> s_compat;

// load_filament_compatibility, reading `text` (the JSON file's contents) instead of a path.
static void load_filament_compatibility(const std::string& text)
{
    s_compat.assign(k_compat_dim, std::vector<bool>(k_compat_dim, false));
    for (size_t i = 0; i < k_category_count; ++i) s_compat[i][i] = true;
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        if (!j.contains("compatibility")) return;
        for (auto& [cat_a_str, partner_list] : j["compatibility"].items()) {
            FilamentCategory cat_a = filament_category_from_name(cat_a_str);
            if (cat_a == FilamentCategory::UNKNOWN || !partner_list.is_array()) continue;
            for (auto& cat_b_val : partner_list) {
                FilamentCategory cat_b = filament_category_from_name(cat_b_val.get<std::string>());
                if (cat_b == FilamentCategory::UNKNOWN) continue;
                s_compat[static_cast<size_t>(cat_a)][static_cast<size_t>(cat_b)] = true;
                s_compat[static_cast<size_t>(cat_b)][static_cast<size_t>(cat_a)] = true;
            }
        }
    } catch (const std::exception&) {
    }
}

static bool is_category_compatible(FilamentCategory a, FilamentCategory b)
{
    if (s_compat.empty()) load_filament_compatibility(k_shipped_compatibility_json);
    return s_compat[static_cast<size_t>(a)][static_cast<size_t>(b)];
}

static const std::unordered_map<std::string, FilamentCategory>& filament_type_category_map()
{
    static const std::unordered_map<std::string, FilamentCategory> m = {
        {"PLA", FilamentCategory::PLA},   {"PLA-CF", FilamentCategory::PLA}, {"ABS", FilamentCategory::ABS},
        {"ASA", FilamentCategory::ASA},   {"PETG", FilamentCategory::PETG},  {"PETG-CF", FilamentCategory::PETG},
        {"PCTG", FilamentCategory::PETG}, {"TPU", FilamentCategory::TPU},    {"PET", FilamentCategory::PET},
        {"PA", FilamentCategory::PA},     {"PA-CF", FilamentCategory::PA},   {"PC", FilamentCategory::PC},
        {"BVOH", FilamentCategory::SUPPORT}, {"PVA", FilamentCategory::SUPPORT},
    };
    return m;
}

static std::string normalize_filament_type(const std::string& type)
{
    std::string normalized = type;
    size_t start = normalized.find_first_not_of(" \t\r\n");
    size_t end   = normalized.find_last_not_of(" \t\r\n");
    if (start != std::string::npos && end != std::string::npos)
        normalized = normalized.substr(start, end - start + 1);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) { return std::toupper(c); });
    return normalized;
}

static FilamentCategory get_filament_category(const std::string& filament_type)
{
    const auto& m  = filament_type_category_map();
    auto        it = m.find(normalize_filament_type(filament_type));
    return it != m.end() ? it->second : FilamentCategory::UNKNOWN;
}

struct ResolvedFilamentCategory { unsigned int filament_id; FilamentCategory category; };

// resolve_filament_categories: 0-based ids; slots with no type are skipped.
static std::vector<ResolvedFilamentCategory> resolve_filament_categories(const std::vector<unsigned int>& filament_ids,
                                                                         const std::vector<std::string>&  types)
{
    std::vector<ResolvedFilamentCategory> result;
    for (unsigned int id : filament_ids) {
        if (id >= types.size() || types[id].empty()) continue;
        result.push_back({id, get_filament_category(types[id])});
    }
    return result;
}

bool is_filament_compatible(const std::vector<unsigned int>& filament_ids, const std::vector<std::string>& types)
{
    if (filament_ids.size() <= 1) return true;
    auto resolved = resolve_filament_categories(filament_ids, types);
    for (const auto& r : resolved)
        if (r.category == FilamentCategory::UNKNOWN) return false;
    if (resolved.size() <= 1) return true;
    if (std::all_of(resolved.begin() + 1, resolved.end(), [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return true;
    for (size_t i = 0; i < resolved.size(); ++i)
        for (size_t j = i + 1; j < resolved.size(); ++j)
            if (!is_category_compatible(resolved[i].category, resolved[j].category)) return false;
    return true;
}

// find_incompatible_filament_pair: 1-based ids of the first incompatible pair, {0,0} when all compatible.
std::pair<unsigned int, unsigned int> find_incompatible_filament_pair(const std::vector<unsigned int>& filament_ids,
                                                                      const std::vector<std::string>&  types)
{
    if (filament_ids.size() <= 1) return {0, 0};
    auto resolved = resolve_filament_categories(filament_ids, types);
    for (const auto& r : resolved)
        if (r.category == FilamentCategory::UNKNOWN)
            for (const auto& other : resolved)
                if (other.filament_id != r.filament_id) return {r.filament_id + 1, other.filament_id + 1};
    if (resolved.size() <= 1) return {0, 0};
    if (std::all_of(resolved.begin() + 1, resolved.end(), [&](const ResolvedFilamentCategory& r) { return r.category == resolved[0].category; }))
        return {0, 0};
    for (size_t i = 0; i < resolved.size(); ++i)
        for (size_t j = i + 1; j < resolved.size(); ++j)
            if (!is_category_compatible(resolved[i].category, resolved[j].category))
                return {resolved[i].filament_id + 1, resolved[j].filament_id + 1};
    return {0, 0};
}

static std::vector<std::vector<bool>> build_compatibility_matrix(size_t n, const std::vector<std::string>& types)
{
    std::vector<std::vector<bool>> m(n, std::vector<bool>(n, false));
    for (size_t i = 0; i < n; ++i) {
        m[i][i] = true;
        for (size_t j = i + 1; j < n; ++j) {
            bool ok = is_filament_compatible(std::vector<unsigned int>{(unsigned int) i, (unsigned int) j}, types);
            m[i][j] = m[j][i] = ok;
        }
    }
    return m;
}

std::vector<int> decode_color_match_gradient_weights(const std::string& value, size_t expected_components);
std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights);
FsColour blend_sequence_filament_mixer(const std::vector<FsColour>& palette, const std::vector<unsigned int>& sequence);
bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent);

std::vector<int> normalize_color_match_weights(const std::vector<int>& weights, size_t count)
{
    if (count == 0)
        return {};
    std::vector<int> out = weights;
    if (out.size() != count)
        out.assign(count, int(100 / count));

    int sum = 0;
    for (int& value : out) {
        value = std::max(0, value);
        sum += value;
    }
    if (sum <= 0 && count > 0) {
        out.assign(count, 0);
        out[0] = 100;
        return out;
    }

    std::vector<double> remainders(count, 0.0);
    int                 assigned = 0;
    for (size_t idx = 0; idx < count; ++idx) {
        const double exact = 100.0 * double(out[idx]) / double(sum);
        out[idx]           = int(std::floor(exact));
        remainders[idx]    = exact - double(out[idx]);
        assigned += out[idx];
    }

    int missing = std::max(0, 100 - assigned);
    while (missing > 0) {
        size_t best_idx       = 0;
        double best_remainder = -1.0;
        for (size_t idx = 0; idx < remainders.size(); ++idx) {
            if (remainders[idx] > best_remainder) {
                best_remainder = remainders[idx];
                best_idx       = idx;
            }
        }
        ++out[best_idx];
        remainders[best_idx] = 0.0;
        --missing;
    }
    return out;
}

std::vector<int> expand_color_match_recipe_weights(const MixedColorMatchRecipeResult& recipe, size_t num_physical)
{
    std::vector<int> weights(num_physical, 0);
    if (!recipe.valid || num_physical == 0)
        return weights;

    if (!recipe.gradient_component_ids.empty()) {
        const std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        const std::vector<int>          raw_weights =
            normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()), ids.size());
        if (ids.size() != raw_weights.size())
            return weights;
        for (size_t idx = 0; idx < ids.size(); ++idx) {
            if (ids[idx] >= 1 && ids[idx] <= num_physical)
                weights[ids[idx] - 1] = raw_weights[idx];
        }
        return weights;
    }

    if (recipe.component_a >= 1 && recipe.component_a <= num_physical)
        weights[recipe.component_a - 1] = std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100));
    if (recipe.component_b >= 1 && recipe.component_b <= num_physical)
        weights[recipe.component_b - 1] = std::max(0, std::clamp(recipe.mix_b_percent, 0, 100));
    return weights;
}

// The component ids and weights summarize_color_match_recipe prints (split out so the response can carry them as arrays).
static void color_match_recipe_components(const MixedColorMatchRecipeResult& recipe, std::vector<unsigned int>& ids, std::vector<int>& weights)
{
    ids.clear();
    weights.clear();
    if (!recipe.gradient_component_ids.empty()) {
        ids     = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        weights = normalize_color_match_weights(decode_color_match_gradient_weights(recipe.gradient_component_weights, ids.size()),
                                                ids.size());
    } else {
        ids     = {recipe.component_a, recipe.component_b};
        weights = {std::max(0, 100 - std::clamp(recipe.mix_b_percent, 0, 100)), std::max(0, std::clamp(recipe.mix_b_percent, 0, 100))};
    }
}

std::string summarize_color_match_recipe(const MixedColorMatchRecipeResult& recipe)
{
    if (!recipe.valid)
        return {};

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    color_match_recipe_components(recipe, ids, weights);
    if (ids.empty() || ids.size() != weights.size())
        return {};

    std::ostringstream out;
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << 'F' << ids[idx];
    }
    out << ' ';
    for (size_t idx = 0; idx < weights.size(); ++idx) {
        if (idx > 0)
            out << '/';
        out << weights[idx] << '%';
    }
    return out.str();
}

CIELab sRGB_to_CIELab(const FsColour& c)
{
    double r = c.Red() / 255.0;
    double g = c.Green() / 255.0;
    double b = c.Blue() / 255.0;
    float  lab[3];
    RGB2Lab(float(r), float(g), float(b), &lab[0], &lab[1], &lab[2]);
    return {double(lab[0]), double(lab[1]), double(lab[2])};
}

double delta_e_lab(const CIELab& a, const CIELab& b)
{
    return double(DeltaE00(float(a.L), float(a.a), float(a.b), float(b.L), float(b.a), float(b.b)));
}

BlendLUT build_blend_lut(const std::vector<FsColour>& palette)
{
    const size_t n = palette.size();
    BlendLUT     lut(n);
    if (lut.empty()) return lut;

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a; b < n; ++b) {
            for (int pct = 0; pct <= 100; ++pct) {
                FsColour blended          = blend_pair_filament_mixer(palette[a], palette[b], float(pct) / 100.f);
                lut.m_pair[a][b - a][pct] = sRGB_to_CIELab(blended);
            }
        }
    }
    return lut;
}

CIELab blend_weighted_lab_accurate(const std::vector<FsColour>& palette, const std::vector<unsigned int>& ids, const std::vector<int>& weights)
{
    if (ids.size() != weights.size() || ids.empty())
        return {50.0, 0.0, 0.0};

    std::vector<std::pair<unsigned int, int>> sorted;
    sorted.reserve(ids.size());
    for (size_t i = 0; i < ids.size(); ++i)
        sorted.emplace_back(ids[i], weights[i]);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<FsColour> colors;
    std::vector<double>   dweights;
    colors.reserve(sorted.size());
    dweights.reserve(sorted.size());
    for (const auto& [id, w] : sorted) {
        if (id == 0 || id > palette.size()) continue;
        colors.push_back(palette[id - 1]);
        dweights.push_back(double(std::max(0, w)));
    }

    FsColour blended = blend_multi_filament_mixer(colors, dweights);
    return sRGB_to_CIELab(blended);
}

double color_delta_e00(const FsColour& lhs, const FsColour& rhs)
{
    float lhs_l = 0.f, lhs_a = 0.f, lhs_b = 0.f;
    float rhs_l = 0.f, rhs_a = 0.f, rhs_b = 0.f;
    RGB2Lab(float(lhs.Red()) / 255.f, float(lhs.Green()) / 255.f, float(lhs.Blue()) / 255.f, &lhs_l, &lhs_a, &lhs_b);
    RGB2Lab(float(rhs.Red()) / 255.f, float(rhs.Green()) / 255.f, float(rhs.Blue()) / 255.f, &rhs_l, &rhs_a, &rhs_b);
    return double(DeltaE00(lhs_l, lhs_a, lhs_b, rhs_l, rhs_a, rhs_b));
}

// `types` stands in for the preset bundle's filament_type per palette slot (the compatibility check reads
// preset_bundle->filament_presets[i]); an empty string is a slot whose type cannot be resolved (skipped, as in the source).
MixedColorMatchRecipeResult build_best_color_match_recipe(const std::vector<std::string>& physical_colors,
                                                          const FsColour&                 target_color,
                                                          int                             min_component_percent,
                                                          int                             max_component_percent,
                                                          bool                            check_compatible,
                                                          const std::vector<std::string>& types)
{
    MixedColorMatchRecipeResult best;
    if (!target_color.IsOk() || physical_colors.size() < 2)
        return best;

    if (max_component_percent < 50 || max_component_percent > 100)
        return best;

    // ---- Step 1: build palette & pre-convert to Lab ----
    const size_t          n = physical_colors.size();
    std::vector<FsColour> palette;
    std::vector<CIELab>   palette_lab;
    palette.reserve(n);
    palette_lab.reserve(n);
    for (const std::string& hex : physical_colors) {
        FsColour c = parse_mixed_color(hex);
        palette.emplace_back(c);
        palette_lab.emplace_back(sRGB_to_CIELab(c));
    }
    const CIELab target_lab = sRGB_to_CIELab(target_color);

    const int loop_min_weight = std::max(1, std::clamp(min_component_percent, 0, 50));

    std::vector<std::vector<bool>> compat;
    if (check_compatible) {
        compat = build_compatibility_matrix(n, types);
    } else {
        compat.assign(n, std::vector<bool>(n, true));
    }

    auto encode_gradient_ids = [](const std::vector<unsigned int>& ids) -> std::string {
        return MixedFilamentManager::encode_gradient_component_ids(ids);
    };

    auto encode_gradient_weights = [](const std::vector<int>& weights) -> std::string {
        std::ostringstream ss;
        for (size_t i = 0; i < weights.size(); ++i) {
            if (i > 0) ss << '/';
            ss << weights[i];
        }
        return ss.str();
    };

    // ---- Step 2: build pair Blend LUT (polynomial mixing → Lab) ----
    const BlendLUT lut = build_blend_lut(palette);
    if (lut.empty()) return best;

    auto update_best_pair = [&](unsigned int a, unsigned int b, int pct, double de) {
        if (!best.valid || de + 1e-6 < best.delta_e) {
            best.valid         = true;
            best.component_a   = a;
            best.component_b   = b;
            best.mix_b_percent = pct;
            best.preview_color = blend_pair_filament_mixer(palette[a - 1], palette[b - 1], float(pct) / 100.f);
            best.delta_e       = de;
            best.gradient_component_ids.clear();
            best.gradient_component_weights.clear();
            best.manual_pattern.clear();
        }
    };

    // ---- Step 3: pair coarse scan (step=5%) ----
    constexpr int k_coarse_step = 5;
    constexpr int k_top_coarse  = 30;

    using HeapEntry = std::tuple<double, unsigned int, unsigned int, int>;
    auto cmp = [](const HeapEntry& x, const HeapEntry& y) { return std::get<0>(x) < std::get<0>(y); };
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> heap(cmp);

    for (size_t a = 0; a < n; ++a) {
        for (size_t b = a + 1; b < n; ++b) {
            if (!compat[a][b]) continue;
            for (int pct = std::max(loop_min_weight, 100 - max_component_percent);
                 pct <= std::min(100 - loop_min_weight, max_component_percent); pct += k_coarse_step) {
                const CIELab& blended_lab = lut.get(a, b, pct);
                double        de          = delta_e_lab(target_lab, blended_lab);
                update_best_pair(unsigned(a + 1), unsigned(b + 1), pct, de);
                if (heap.size() < k_top_coarse) {
                    heap.emplace(de, unsigned(a + 1), unsigned(b + 1), pct);
                } else if (de < std::get<0>(heap.top())) {
                    heap.pop();
                    heap.emplace(de, unsigned(a + 1), unsigned(b + 1), pct);
                }
            }
        }
    }

    // ---- Step 4: pair fine search (step=1%, top-N from coarse) ----
    while (!heap.empty()) {
        auto [de, a, b, coarse_pct] = heap.top();
        (void) de;
        heap.pop();
        int fine_min = std::max(std::max(loop_min_weight, 100 - max_component_percent), coarse_pct - k_coarse_step + 1);
        int fine_max = std::min(std::min(100 - loop_min_weight, max_component_percent), coarse_pct + k_coarse_step - 1);
        for (int pct = fine_min; pct <= fine_max; ++pct) {
            if ((pct - loop_min_weight) % k_coarse_step == 0) continue;
            const CIELab& blended_lab = lut.get(a - 1, b - 1, pct);
            update_best_pair(a, b, pct, delta_e_lab(target_lab, blended_lab));
        }
    }

    MixedColorMatchRecipeResult best_pair = best;

    // ---- Step 5: early termination ----
    if (best_pair.valid && best_pair.delta_e <= 0.5)
        return best_pair;

    // ---- Step 6: adaptive candidate pool (top-N by single-color ΔE) ----
    std::vector<std::pair<double, unsigned int>> ranked_ids;
    ranked_ids.reserve(n);
    for (size_t idx = 0; idx < n; ++idx)
        ranked_ids.emplace_back(delta_e_lab(target_lab, palette_lab[idx]), unsigned(idx + 1));
    std::sort(ranked_ids.begin(), ranked_ids.end(), [](const auto& x, const auto& y) {
        if (x.first != y.first) return x.first < y.first;
        return x.second < y.second;
    });

    const size_t              pool_size = std::min<size_t>(n, 8);
    std::vector<unsigned int> candidate_pool;
    candidate_pool.reserve(pool_size);
    for (size_t i = 0; i < pool_size; ++i)
        candidate_pool.emplace_back(ranked_ids[i].second);

    if (candidate_pool.size() < 3)
        return best;

    std::sort(candidate_pool.begin(), candidate_pool.end());

    // ---- Step 7: triple layered search ----
    constexpr int k_triple_coarse_step = 10;
    constexpr int k_top_triple         = 20;

    struct TripleEntry {
        double       de;
        unsigned int a, b, c;
        int          wa, wb;
        bool         operator<(const TripleEntry& o) const { return de < o.de; }
    };
    std::priority_queue<TripleEntry> triple_heap;

    for (size_t fi = 0; fi + 2 < candidate_pool.size(); ++fi) {
        for (size_t fj = fi + 1; fj + 1 < candidate_pool.size(); ++fj) {
            for (size_t fk = fj + 1; fk < candidate_pool.size(); ++fk) {
                unsigned int a = candidate_pool[fi], b = candidate_pool[fj], c = candidate_pool[fk];
                if (!compat[a - 1][b - 1] || !compat[b - 1][c - 1] || !compat[a - 1][c - 1]) continue;

                for (int wa = loop_min_weight; wa <= std::min(100 - 2 * loop_min_weight, max_component_percent); wa += k_triple_coarse_step) {
                    for (int wb = loop_min_weight; wb <= std::min(100 - wa - loop_min_weight, max_component_percent); wb += k_triple_coarse_step) {
                        int wc = 100 - wa - wb;
                        if (wc < loop_min_weight || wc > max_component_percent) continue;
                        CIELab blended = blend_weighted_lab_accurate(palette, {a, b, c}, {wa, wb, wc});
                        double de      = delta_e_lab(target_lab, blended);
                        if (!best.valid || de + 1e-6 < best.delta_e) {
                            best.valid                      = true;
                            best.component_a                = a;
                            best.component_b                = b;
                            best.mix_b_percent              = wa + wb > 0 ? int(std::lround(100.0 * double(wb) / double(wa + wb))) : 50;
                            best.gradient_component_ids     = encode_gradient_ids({a, b, c});
                            best.gradient_component_weights = encode_gradient_weights({wa, wb, wc});
                            best.preview_color = blend_multi_filament_mixer({palette[a - 1], palette[b - 1], palette[c - 1]},
                                                                            {double(wa), double(wb), double(wc)});
                            best.delta_e = de;
                            best.manual_pattern.clear();
                        }
                        if (triple_heap.size() < k_top_triple) {
                            triple_heap.push({de, a, b, c, wa, wb});
                        } else if (de < triple_heap.top().de) {
                            triple_heap.pop();
                            triple_heap.push({de, a, b, c, wa, wb});
                        }
                    }
                }
            }
        }
    }

    while (!triple_heap.empty()) {
        TripleEntry te = triple_heap.top();
        triple_heap.pop();
        int wa_min = std::max(loop_min_weight, te.wa - k_triple_coarse_step + 1);
        int wa_max = std::min(std::min(100 - 2 * loop_min_weight, max_component_percent), te.wa + k_triple_coarse_step - 1);
        for (int wa = wa_min; wa <= wa_max; ++wa) {
            if ((wa - loop_min_weight) % k_triple_coarse_step == 0) continue;
            int wb_min = std::max(loop_min_weight, te.wb - k_triple_coarse_step + 1);
            int wb_max = std::min(std::min(100 - wa - loop_min_weight, max_component_percent), te.wb + k_triple_coarse_step - 1);
            for (int wb = wb_min; wb <= wb_max; ++wb) {
                if ((wb - loop_min_weight) % k_triple_coarse_step == 0) continue;
                int wc = 100 - wa - wb;
                if (wc < loop_min_weight || wc > max_component_percent) continue;
                CIELab blended = blend_weighted_lab_accurate(palette, {te.a, te.b, te.c}, {wa, wb, wc});
                double de2     = delta_e_lab(target_lab, blended);
                if (!best.valid || de2 + 1e-6 < best.delta_e) {
                    best.valid                      = true;
                    best.component_a                = te.a;
                    best.component_b                = te.b;
                    best.mix_b_percent              = wa + wb > 0 ? int(std::lround(100.0 * double(wb) / double(wa + wb))) : 50;
                    best.gradient_component_ids     = encode_gradient_ids({te.a, te.b, te.c});
                    best.gradient_component_weights = encode_gradient_weights({wa, wb, wc});
                    best.preview_color = blend_multi_filament_mixer({palette[te.a - 1], palette[te.b - 1], palette[te.c - 1]},
                                                                    {double(wa), double(wb), double(wc)});
                    best.delta_e = de2;
                    best.manual_pattern.clear();
                }
            }
        }
    }

    // ---- final normalization: re-evaluate ΔE with consistent color_delta_e00 ----
    if (best.valid)
        best.delta_e = color_delta_e00(target_color, best.preview_color);
    if (best_pair.valid) {
        best_pair.delta_e = color_delta_e00(target_color, best_pair.preview_color);
        if (!best.valid || best_pair.delta_e + 1e-6 < best.delta_e)
            best = std::move(best_pair);
        else if (!best.gradient_component_ids.empty() && best_pair.delta_e <= best.delta_e + 0.5)
            best = std::move(best_pair);
    }

    return best;
}

FsColour compute_color_match_recipe_display_color(const MixedColorMatchRecipeResult& recipe, const MixedFilamentDisplayContext& context)
{
    if (!recipe.valid)
        return recipe.preview_color.IsOk() ? recipe.preview_color : colour_from_string("#26A69A");

    MixedFilament entry;
    entry.component_a                = recipe.component_a;
    entry.component_b                = recipe.component_b;
    entry.mix_b_percent              = recipe.mix_b_percent;
    entry.manual_pattern             = recipe.manual_pattern;
    entry.gradient_component_ids     = recipe.gradient_component_ids;
    entry.gradient_component_weights = recipe.gradient_component_weights;
    entry.distribution_mode          = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);

    return parse_mixed_color(compute_mixed_filament_display_color(entry, context));
}

std::vector<int> decode_color_match_gradient_weights(const std::string& value, size_t expected_components)
{
    std::vector<int> weights;
    if (value.empty() || expected_components == 0)
        return weights;

    std::string token;
    for (const char ch : value) {
        if (ch >= '0' && ch <= '9') {
            token.push_back(ch);
            continue;
        }
        if (!token.empty()) {
            weights.emplace_back(std::max(0, std::atoi(token.c_str())));
            token.clear();
        }
    }
    if (!token.empty())
        weights.emplace_back(std::max(0, std::atoi(token.c_str())));
    if (weights.size() != expected_components)
        weights.clear();
    return weights;
}

MixedColorMatchRecipeResult build_pair_color_match_candidate(
    const std::vector<FsColour>& palette, unsigned int component_a, unsigned int component_b, int mix_b_percent, int min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (component_a == 0 || component_b == 0 || component_a == component_b)
        return candidate;
    if (component_a > palette.size() || component_b > palette.size())
        return candidate;
    if (!color_match_weights_within_range({100 - std::clamp(mix_b_percent, 0, 100), std::clamp(mix_b_percent, 0, 100)},
                                          min_component_percent))
        return candidate;

    candidate.valid         = true;
    candidate.component_a   = component_a;
    candidate.component_b   = component_b;
    candidate.mix_b_percent = std::clamp(mix_b_percent, 0, 100);
    candidate.preview_color = blend_pair_filament_mixer(palette[component_a - 1], palette[component_b - 1],
                                                        float(candidate.mix_b_percent) / 100.f);
    return candidate;
}

MixedColorMatchRecipeResult build_multi_color_match_candidate(const std::vector<FsColour>&     palette,
                                                              const std::vector<unsigned int>& ids,
                                                              const std::vector<int>&          weights,
                                                              int                              min_component_percent)
{
    MixedColorMatchRecipeResult candidate;
    if (ids.size() < 3 || ids.size() != weights.size())
        return candidate;
    if (!color_match_weights_within_range(weights, min_component_percent))
        return candidate;

    std::vector<std::pair<int, unsigned int>> weighted_ids;
    weighted_ids.reserve(ids.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        if (ids[idx] == 0 || ids[idx] > palette.size())
            return candidate;
        if (weights[idx] <= 0)
            continue;
        weighted_ids.emplace_back(weights[idx], ids[idx]);
    }
    if (weighted_ids.size() < 3)
        return candidate;

    std::sort(weighted_ids.begin(), weighted_ids.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.first != rhs.first)
            return lhs.first > rhs.first;
        return lhs.second < rhs.second;
    });

    std::vector<unsigned int> ordered_ids;
    std::vector<int>          ordered_weights;
    ordered_ids.reserve(weighted_ids.size());
    ordered_weights.reserve(weighted_ids.size());
    for (const auto& [weight, filament_id] : weighted_ids) {
        ordered_ids.emplace_back(filament_id);
        ordered_weights.emplace_back(weight);
    }

    const std::vector<unsigned int> sequence = build_color_match_sequence(ordered_ids, ordered_weights);
    if (sequence.empty())
        return candidate;

    candidate.valid             = true;
    candidate.component_a       = ordered_ids[0];
    candidate.component_b       = ordered_ids[1];
    const int pair_weight_total = ordered_weights[0] + ordered_weights[1];
    candidate.mix_b_percent     = pair_weight_total > 0 ?
                                      std::clamp(int(std::lround(100.0 * double(ordered_weights[1]) / double(pair_weight_total))), 0, 100) :
                                      50;
    candidate.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ordered_ids);
    {
        std::ostringstream weights_ss;
        for (size_t weight_idx = 0; weight_idx < ordered_weights.size(); ++weight_idx) {
            if (weight_idx > 0)
                weights_ss << '/';
            weights_ss << ordered_weights[weight_idx];
        }
        candidate.gradient_component_weights = weights_ss.str();
    }
    candidate.preview_color = blend_sequence_filament_mixer(palette, sequence);
    return candidate;
}

bool color_match_weights_within_range(const std::vector<int>& weights, int min_component_percent)
{
    if (min_component_percent <= 0)
        return true;

    const int min_allowed       = std::clamp(min_component_percent, 0, 50);
    int       active_components = 0;
    for (const int weight : weights) {
        if (weight <= 0)
            continue;
        ++active_components;
        if (weight < min_allowed)
            return false;
    }
    return active_components >= 2;
}

std::vector<unsigned int> build_color_match_sequence(const std::vector<unsigned int>& ids, const std::vector<int>& weights)
{
    if (ids.empty() || ids.size() != weights.size())
        return {};

    constexpr int k_max_cycle = 48;

    std::vector<unsigned int> filtered_ids;
    std::vector<int>          counts;
    filtered_ids.reserve(ids.size());
    counts.reserve(weights.size());
    for (size_t idx = 0; idx < ids.size(); ++idx) {
        const int weight = std::max(0, weights[idx]);
        if (weight <= 0)
            continue;
        filtered_ids.emplace_back(ids[idx]);
        counts.emplace_back(std::max(1, int(std::round((double(weight) / 100.0) * k_max_cycle))));
    }

    if (filtered_ids.empty())
        return {};

    int cycle = std::accumulate(counts.begin(), counts.end(), 0);
    while (cycle > k_max_cycle) {
        auto it = std::max_element(counts.begin(), counts.end());
        if (it == counts.end() || *it <= 1)
            break;
        --(*it);
        --cycle;
    }

    if (cycle <= 0)
        return {};

    std::vector<unsigned int> sequence;
    sequence.reserve(size_t(cycle));
    std::vector<int> emitted(counts.size(), 0);
    for (int pos = 0; pos < cycle; ++pos) {
        size_t best_idx   = 0;
        double best_score = -1e9;
        for (size_t idx = 0; idx < counts.size(); ++idx) {
            const double target = double((pos + 1) * counts[idx]) / double(std::max(1, cycle));
            const double score  = target - double(emitted[idx]);
            if (score > best_score) {
                best_score = score;
                best_idx   = idx;
            }
        }
        ++emitted[best_idx];
        sequence.emplace_back(filtered_ids[best_idx]);
    }
    return sequence;
}

FsColour blend_sequence_filament_mixer(const std::vector<FsColour>& palette, const std::vector<unsigned int>& sequence)
{
    if (palette.empty() || sequence.empty())
        return colour_from_string("#26A69A");

    std::vector<int> counts(palette.size() + 1, 0);
    for (const unsigned int filament_id : sequence) {
        if (filament_id == 0 || filament_id > palette.size())
            continue;
        ++counts[filament_id];
    }

    std::vector<FsColour> colors;
    std::vector<double>   weights;
    colors.reserve(palette.size());
    weights.reserve(palette.size());
    for (size_t filament_id = 1; filament_id <= palette.size(); ++filament_id) {
        if (counts[filament_id] <= 0)
            continue;
        colors.emplace_back(palette[filament_id - 1]);
        weights.emplace_back(double(counts[filament_id]));
    }
    return blend_multi_filament_mixer(colors, weights);
}

// MixedColorMatchHelpers.cpp build_color_match_presets (~201-268)
std::vector<MixedColorMatchRecipeResult> build_color_match_presets(const std::vector<std::string>& physical_colors,
                                                                   int                             min_component_percent,
                                                                   const std::vector<std::string>& types)
{
    std::vector<MixedColorMatchRecipeResult> presets;
    if (physical_colors.size() < 2)
        return presets;

    std::vector<FsColour> palette;
    palette.reserve(physical_colors.size());
    for (const std::string& hex : physical_colors)
        palette.emplace_back(parse_mixed_color(hex));

    constexpr size_t                k_max_presets = 9999;
    std::unordered_set<std::string> seen_colors;
    auto add_candidate = [&presets, &seen_colors](MixedColorMatchRecipeResult candidate) {
        if (!candidate.valid)
            return;
        const std::string color_key = normalize_color_match_hex(candidate.preview_color.hex());
        if (color_key.empty() || !seen_colors.insert(color_key).second)
            return;
        presets.emplace_back(std::move(candidate));
    };

    auto compat = build_compatibility_matrix(palette.size(), types);
    for (size_t left_idx = 0; left_idx < palette.size() && presets.size() < k_max_presets; ++left_idx) {
        for (size_t right_idx = left_idx + 1; right_idx < palette.size() && presets.size() < k_max_presets; ++right_idx) {
            if (!compat[left_idx][right_idx]) continue;
            add_candidate(build_pair_color_match_candidate(palette, unsigned(left_idx + 1), unsigned(right_idx + 1), 50, min_component_percent));
        }
    }

    const size_t           triple_limit         = std::min<size_t>(palette.size(), 6);
    const std::vector<int> equal_triple_weights = normalize_color_match_weights({1, 1, 1}, 3);
    for (size_t first_idx = 0; first_idx + 2 < triple_limit && presets.size() < k_max_presets; ++first_idx) {
        for (size_t second_idx = first_idx + 1; second_idx + 1 < triple_limit && presets.size() < k_max_presets; ++second_idx) {
            for (size_t third_idx = second_idx + 1; third_idx < triple_limit && presets.size() < k_max_presets; ++third_idx) {
                if (!compat[first_idx][second_idx] || !compat[second_idx][third_idx] || !compat[first_idx][third_idx]) continue;
                const std::vector<unsigned int> ids = {unsigned(first_idx + 1), unsigned(second_idx + 1), unsigned(third_idx + 1)};
                add_candidate(build_multi_color_match_candidate(palette, ids, equal_triple_weights, min_component_percent));
                for (size_t dominant_idx = 0; dominant_idx < ids.size() && presets.size() < k_max_presets; ++dominant_idx) {
                    std::vector<int> dominant_weights(ids.size(), 25);
                    dominant_weights[dominant_idx] = 50;
                    add_candidate(build_multi_color_match_candidate(palette, ids, dominant_weights, min_component_percent));
                }
            }
        }
    }
    return presets;
}

// ---- Batch Match Mapping (MixedColorMatchHelpers.hpp) ----

struct ModelColorEntry {
    unsigned int              color_index;
    FsColour                  color;
    std::string               hex_value;
    std::vector<unsigned int> extruder_ids;
};

struct ColorMappingEntry {
    unsigned int                model_color_index = 0;
    FsColour                    source_color;
    MixedColorMatchRecipeResult recipe;
    unsigned int                target_filament_id = 0;
    FsColour                    matched_color;
    double                      delta_e        = std::numeric_limits<double>::infinity();
    bool                        is_pure_recipe = false;
    double                      pure_delta_e   = std::numeric_limits<double>::infinity();
    std::vector<unsigned int>   merged_model_indices;
    std::vector<unsigned int>   source_extruder_ids;
    bool                        in_place_edited = false;
};

struct BatchMatchResult {
    std::vector<unsigned int>      selected_physical_ids;
    std::vector<ColorMappingEntry> mappings;
    double                         avg_delta_e = std::numeric_limits<double>::infinity();
    bool                           success     = false;
    std::string                    error_message;
    int                            error_code          = 0;
    bool                           is_recommended_mode = false;
    std::vector<std::string>       recommended_physical_colors;
    std::vector<std::string>       recommended_physical_family_names;
};

void assign_batch_virtual_filament_ids(BatchMatchResult& result, size_t num_physical, size_t existing_mixed_count)
{
    if (num_physical < 1)
        return;
    unsigned int next_virtual_id = static_cast<unsigned int>(num_physical + existing_mixed_count + 1);

    for (auto& mapping : result.mappings) {
        if (mapping.is_pure_recipe) {
            if (mapping.recipe.component_a < 1 || mapping.recipe.component_a > num_physical) {
                mapping.target_filament_id = next_virtual_id++;
                continue;
            }
            mapping.target_filament_id = mapping.recipe.component_a;
        } else {
            mapping.target_filament_id = next_virtual_id++;
        }
    }
}

std::vector<ColorMappingEntry> merge_duplicate_recipe_mappings(const std::vector<ColorMappingEntry>& mappings)
{
    if (mappings.size() < 2) return mappings;

    auto fingerprint = [](const MixedColorMatchRecipeResult& r) {
        std::ostringstream oss;
        oss << r.component_a << '|' << r.component_b << '|' << r.mix_b_percent << '|' << r.manual_pattern << '|'
            << r.gradient_component_ids << '|' << r.gradient_component_weights;
        return oss.str();
    };

    std::vector<ColorMappingEntry> result;
    result.reserve(mappings.size());
    std::unordered_map<std::string, size_t> seen;

    for (const ColorMappingEntry& m : mappings) {
        if (m.is_pure_recipe) {
            result.push_back(m);
            continue;
        }
        const std::string key = fingerprint(m.recipe);
        auto              it  = seen.find(key);
        if (it != seen.end()) {
            ColorMappingEntry& survivor = result[it->second];
            survivor.source_extruder_ids.insert(survivor.source_extruder_ids.end(), m.source_extruder_ids.begin(), m.source_extruder_ids.end());
            survivor.merged_model_indices.insert(survivor.merged_model_indices.end(), m.merged_model_indices.begin(),
                                                 m.merged_model_indices.end());
        } else {
            seen.emplace(key, result.size());
            result.push_back(m);
        }
    }

    if (result.size() != mappings.size()) {
        for (ColorMappingEntry& e : result) {
            if (e.is_pure_recipe) continue;
            std::sort(e.source_extruder_ids.begin(), e.source_extruder_ids.end());
            e.source_extruder_ids.erase(std::unique(e.source_extruder_ids.begin(), e.source_extruder_ids.end()), e.source_extruder_ids.end());
            std::sort(e.merged_model_indices.begin(), e.merged_model_indices.end());
            e.merged_model_indices.erase(std::unique(e.merged_model_indices.begin(), e.merged_model_indices.end()),
                                         e.merged_model_indices.end());
        }
    }
    return result;
}

// Cancellation and progress callbacks are dropped (the match runs synchronously).
BatchMatchResult batch_match_model_colors(const std::vector<ModelColorEntry>& model_colors,
                                          const std::vector<std::string>&     physical_colors,
                                          int                                 min_component_percent,
                                          int                                 max_component_percent)
{
    BatchMatchResult result;
    result.success = true;

    if (min_component_percent < 0 || min_component_percent > 50) {
        result.success       = false;
        result.error_message = "min_component_percent must be in [0, 50]";
        return result;
    }
    if (max_component_percent < 50 || max_component_percent > 100) {
        result.success       = false;
        result.error_message = "max_component_percent must be in [50, 100]";
        return result;
    }
    if (model_colors.empty()) {
        result.success       = false;
        result.error_message = "No model colors to match";
        return result;
    }
    if (physical_colors.size() < 2) {
        result.success       = false;
        result.error_message = "Need at least 2 physical filaments";
        return result;
    }

    for (size_t i = 0; i < model_colors.size(); ++i) {
        const auto&                 entry = model_colors[i];
        MixedColorMatchRecipeResult recipe =
            build_best_color_match_recipe(physical_colors, entry.color, min_component_percent, max_component_percent,
                                          /*check_compatible=*/false, {});

        if (!recipe.valid)
            continue;

        ColorMappingEntry mapping;
        mapping.model_color_index    = entry.color_index;
        mapping.source_color         = entry.color;
        mapping.recipe               = recipe;
        mapping.delta_e              = recipe.delta_e;
        mapping.matched_color        = recipe.preview_color;
        mapping.is_pure_recipe       = (recipe.mix_b_percent == 0);
        mapping.pure_delta_e         = recipe.delta_e;
        mapping.merged_model_indices = {entry.color_index};
        mapping.source_extruder_ids  = entry.extruder_ids;
        result.mappings.push_back(mapping);
    }

    if (result.mappings.empty()) {
        result.success       = false;
        result.error_message = "No valid recipes found for any model color";
        result.error_code    = 1;
        return result;
    }

    double sum_de = 0.0;
    for (const auto& m : result.mappings)
        sum_de += m.delta_e;
    result.avg_delta_e = sum_de / double(result.mappings.size());

    result.selected_physical_ids.clear();
    for (size_t i = 1; i <= physical_colors.size(); ++i)
        result.selected_physical_ids.push_back(static_cast<unsigned int>(i));
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// MixedFilamentBatchDialog.cpp
// ---------------------------------------------------------------------------------------------------------------------

static const std::vector<std::string> FULL_SPECTRUM_FALLBACK_COLORS = {
    "#08ABFB", // semi-translucent cyan
    "#D93B90", // semi-translucent magenta
    "#F9ED3D", // semi-translucent yellow
    "#9199A4", // semi-translucent gray
};
static constexpr int    kMinComponentPercent = 0;   // recommended mode floor (near-pure allowed)
static constexpr int    kMaxComponentPercent = 70;  // recommended mode cap
static constexpr size_t kMaxColors           = 64;  // target-list cap (load_palette_colors)
static constexpr double kDeltaEGoodMax       = 4.0; // <4.0 → Good
static constexpr double kDeltaEFairMax       = 8.0; // <8.0 → Fair, else Poor

// MixedColorMatchHelpers.cpp full_spectrum_preset_name() for the only shipped fallback SKU; the family identity
// (GetFilamentMatchName) strips the nozzle suffix, so it is the same for every nozzle.
static std::string default_full_spectrum_family_name()
{
    return GetFilamentMatchName("Snapmaker PLA Full Spectrum @U1 0.4 nozzle");
}

// load_recommended_palette. `library_loaded` is FilamentColorLibrary::EnsureLoaded()'s result.
static std::vector<FullSpectrumPaletteEntry> load_recommended_palette(bool library_loaded, bool& used_fallback)
{
    used_fallback = false;
    std::vector<FullSpectrumPaletteEntry> palette;
    if (library_loaded)
        palette = BuildFullSpectrumPalette(FilamentColorLibrary::Instance().GetAllFilamentInfos());

    for (auto it = palette.begin(); it != palette.end();) {
        FsColour parsed;
        if (try_parse_color_match_hex(it->hex, parsed))
            ++it;
        else
            it = palette.erase(it);
    }

    if (palette.size() < static_cast<size_t>(kFullSpectrumSlotCount)) {
        static const char* fallback_names[kFullSpectrumSlotCount] = {"Cyan", "Magenta", "Yellow", "Gray"};
        used_fallback = true;
        palette.clear();
        for (size_t i = 0; i < FULL_SPECTRUM_FALLBACK_COLORS.size() && i < static_cast<size_t>(kFullSpectrumSlotCount); ++i) {
            FullSpectrumPaletteEntry entry;
            entry.hex         = FULL_SPECTRUM_FALLBACK_COLORS[i];
            entry.en_name     = fallback_names[i];
            entry.family_name = default_full_spectrum_family_name();
            palette.push_back(std::move(entry));
        }
    }
    return palette;
}

enum MatchingMethod { RECOMMENDED, MANUAL };

// The worker body of launch_background_match, run synchronously. `manual_selections` are the manual combos' 0-based
// selections (m_filament_combo[i]->GetSelection()); `recommended_selections` the palette dropdowns' indices.
static BatchMatchResult run_batch_match(MatchingMethod                               matching_method,
                                        const std::vector<ModelColorEntry>&          model_colors,
                                        const std::vector<std::string>&              m_physical_colors,
                                        const std::vector<int>&                      manual_selections,
                                        const std::vector<FullSpectrumPaletteEntry>& m_recommended_palette,
                                        const std::vector<int>&                      m_recommended_selections,
                                        size_t                                       existing_mixed_count,
                                        int                                          match_min_override,
                                        int                                          match_max_override)
{
    std::vector<std::string>  active_colors;
    std::vector<unsigned int> manual_full_ids;
    if (matching_method == MANUAL) {
        for (int sel : manual_selections) {
            if (sel >= 0 && sel < static_cast<int>(m_physical_colors.size())) {
                active_colors.push_back(m_physical_colors[sel]);
                manual_full_ids.push_back(static_cast<unsigned int>(sel + 1));
            }
        }
        if (active_colors.size() < 2) {
            active_colors = m_physical_colors;
            manual_full_ids.clear();
            for (size_t i = 0; i < m_physical_colors.size(); ++i)
                manual_full_ids.push_back(static_cast<unsigned int>(i + 1));
        }
    }
    const auto manual_colors = std::move(active_colors);
    const auto all_physical  = m_physical_colors;

    std::vector<std::string> preset_colors;
    std::vector<std::string> preset_family_names;
    if (matching_method == RECOMMENDED) {
        for (int i = 0; i < kFullSpectrumSlotCount && i < int(m_recommended_selections.size()); ++i) {
            const int sel = m_recommended_selections[i];
            if (sel >= 0 && sel < static_cast<int>(m_recommended_palette.size())) {
                preset_colors.push_back(m_recommended_palette[sel].hex);
                preset_family_names.push_back(m_recommended_palette[sel].family_name);
            }
        }
        if (preset_colors.size() < static_cast<size_t>(kFullSpectrumSlotCount)) {
            preset_colors = FULL_SPECTRUM_FALLBACK_COLORS;
            preset_family_names.assign(preset_colors.size(), default_full_spectrum_family_name());
        }
    }

    // ---- worker lambda body ----
    std::vector<std::string> physical_colors;
    if (matching_method == MANUAL) {
        physical_colors = manual_colors;
    } else {
        if (preset_colors.size() >= static_cast<size_t>(kFullSpectrumSlotCount))
            physical_colors = preset_colors;
        else
            physical_colors = all_physical;
    }

    BatchMatchResult result;
    result.success = true;
    std::vector<ModelColorEntry> unmatched_colors;

    const size_t              num_physical = physical_colors.size();
    std::vector<FsColour>     existing_palette;
    std::vector<unsigned int> existing_ids;
    for (size_t i = 0; i < physical_colors.size(); ++i) {
        FsColour c;
        if (try_parse_color_match_hex(physical_colors[i], c)) {
            existing_palette.push_back(c);
            existing_ids.push_back(static_cast<unsigned int>(i + 1));
        }
    }

    // Pass 1: a model colour within ΔE<1 of an existing filament maps directly to it.
    constexpr double K_REUSE_THRESHOLD = 1.0;
    for (const auto& mc : model_colors) {
        double best_de  = std::numeric_limits<double>::max();
        size_t best_idx = 0;
        for (size_t j = 0; j < existing_palette.size(); ++j) {
            double de = color_delta_e00(mc.color, existing_palette[j]);
            if (de < best_de) { best_de = de; best_idx = j; }
        }
        if (best_de < K_REUSE_THRESHOLD && best_idx < existing_ids.size()) {
            ColorMappingEntry mapping;
            mapping.model_color_index    = mc.color_index;
            mapping.source_color         = mc.color;
            mapping.target_filament_id   = existing_ids[best_idx];
            mapping.matched_color        = existing_palette[best_idx];
            mapping.delta_e              = best_de;
            mapping.is_pure_recipe       = (existing_ids[best_idx] <= static_cast<unsigned int>(num_physical));
            mapping.pure_delta_e         = best_de;
            mapping.merged_model_indices = {mc.color_index};
            mapping.source_extruder_ids  = mc.extruder_ids;
            mapping.recipe.valid         = true;
            mapping.recipe.component_a   = existing_ids[best_idx];
            mapping.recipe.mix_b_percent = 0;
            mapping.recipe.preview_color = existing_palette[best_idx];
            mapping.recipe.delta_e       = best_de;
            result.mappings.push_back(mapping);
        } else {
            unmatched_colors.push_back(mc);
        }
    }

    // Pass 2: batch-match the remaining colours (check_compatible=false in both modes).
    const int match_min = match_min_override >= 0 ? match_min_override : kMinComponentPercent;
    const int match_max = match_max_override >= 0 ? match_max_override : ((matching_method == MANUAL) ? 100 : kMaxComponentPercent);
    if (!unmatched_colors.empty()) {
        auto sub_result = batch_match_model_colors(unmatched_colors, physical_colors, match_min, match_max);
        if (sub_result.success) {
            assign_batch_virtual_filament_ids(sub_result, physical_colors.size(), existing_mixed_count);
            result.mappings.insert(result.mappings.end(), sub_result.mappings.begin(), sub_result.mappings.end());
        } else {
            result.success       = false;
            result.error_message = sub_result.error_message;
            result.error_code    = sub_result.error_code;
        }
    }

    // Manual mode: remap subset-relative ids back to the project's physical ids.
    const bool need_manual_remap = (matching_method == MANUAL && !manual_full_ids.empty() &&
                                    manual_full_ids.size() == physical_colors.size() && physical_colors.size() != all_physical.size());
    if (need_manual_remap) {
        std::vector<unsigned int> remap(physical_colors.size() + 1, 0);
        for (size_t i = 0; i < manual_full_ids.size(); ++i)
            remap[i + 1] = manual_full_ids[i];

        auto remap_id = [&](unsigned int& id) {
            if (id > 0 && id < remap.size() && remap[id] > 0)
                id = remap[id];
        };

        for (auto& mapping : result.mappings) {
            remap_id(mapping.target_filament_id);
            remap_id(mapping.recipe.component_a);
            remap_id(mapping.recipe.component_b);
            if (!mapping.recipe.gradient_component_ids.empty()) {
                auto ids = MixedFilamentManager::decode_gradient_component_ids(mapping.recipe.gradient_component_ids);
                for (auto& id : ids) remap_id(id);
                mapping.recipe.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
            }
        }

        const unsigned int full_phys = static_cast<unsigned int>(all_physical.size());
        for (auto& mapping : result.mappings) {
            if (mapping.is_pure_recipe)
                mapping.is_pure_recipe = (mapping.recipe.component_a <= full_phys);
        }

        unsigned int next_vid = static_cast<unsigned int>(all_physical.size() + existing_mixed_count + 1);
        for (auto& mapping : result.mappings) {
            if (!mapping.is_pure_recipe)
                mapping.target_filament_id = next_vid++;
        }
    }

    result.mappings = merge_duplicate_recipe_mappings(result.mappings);

    if (result.success && result.mappings.empty()) {
        result.success       = false;
        result.error_message = "No valid recipes found for any model color";
        result.error_code    = 1;
    }
    if (result.success) {
        double sum_de = 0.0;
        for (const auto& m : result.mappings) sum_de += m.delta_e;
        result.avg_delta_e = sum_de / double(result.mappings.size());
        if (need_manual_remap) {
            result.selected_physical_ids = manual_full_ids;
        } else {
            for (size_t i = 1; i <= physical_colors.size(); ++i)
                result.selected_physical_ids.push_back(static_cast<unsigned int>(i));
        }
        if (matching_method == RECOMMENDED) {
            result.is_recommended_mode         = true;
            result.recommended_physical_colors = physical_colors;
            if (physical_colors.size() == preset_family_names.size())
                result.recommended_physical_family_names = preset_family_names;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Plater.cpp: the Color Mixing panel and the batch-match apply path
// ---------------------------------------------------------------------------------------------------------------------

struct PanelSettings {
    std::vector<double> nozzle_diameters; // printer nozzle_diameter (per extruder; the last value repeats)
    float               lower_bound            = 0.04f;
    float               upper_bound            = 0.16f;
    bool                local_z_mode           = false;
    bool                component_bias_enabled = false;
    // Print settings read only by build_mixed_filament_display_context (the Match-mode preview).
    float               layer_height                = 0.2f;
    int                 wall_loops                  = 1;
    float               preferred_a_height          = 0.f;
    float               preferred_b_height          = 0.f;
    bool                local_z_direct_multicolor   = false;
};

// Sidebar::update_color_mix_panel's display context.
static MixedFilamentDisplayContext panel_display_context(const std::vector<std::string>& colors, const PanelSettings& s)
{
    const size_t             num_physical    = colors.size();
    std::vector<std::string> physical_colors = colors;
    physical_colors.resize(num_physical, "#26A69A");

    std::vector<double> nozzle_diameters(num_physical, 0.4);
    const size_t        opt_count = s.nozzle_diameters.size();
    if (opt_count > 0)
        for (size_t i = 0; i < num_physical; ++i)
            nozzle_diameters[i] = std::max(0.05, s.nozzle_diameters[std::min(i, opt_count - 1)]);

    float lower_bound = std::max(0.01f, s.lower_bound);
    float upper_bound = std::max(lower_bound, s.upper_bound);

    const MixedFilamentPreviewSettings preview_settings{0.2f, lower_bound, upper_bound, 0.f, 0.f, s.local_z_mode, false, 1};
    return MixedFilamentDisplayContext{num_physical, physical_colors, nozzle_diameters, preview_settings, s.component_bias_enabled};
}

// The row label Sidebar::update_color_mix_panel builds for the Color Mixing list.
static std::string panel_row_label(const MixedFilament& mf, size_t num_physical)
{
    char buf[64];
    const std::string         normalized_pattern_cm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
    std::vector<unsigned int> gradient_ids          = MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, 0);
    const bool z_gradient_tile = mf.gradient_enabled && mf.component_a != mf.component_b && normalized_pattern_cm.empty() && gradient_ids.size() < 3;
    std::string lbl;
    if (!normalized_pattern_cm.empty()) {
        // MixedColorMatchHelpers.cpp summarize_cycle_pattern_text
        const auto groups = MixedFilamentManager::split_pattern_groups(normalized_pattern_cm);
        std::map<unsigned int, int> counts;
        int                         total = 0;
        for (const auto& group : groups) {
            const auto tokens = MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical);
            for (const auto& token : tokens) {
                unsigned int eid = MixedFilamentManager::physical_filament_from_token(token, mf, num_physical);
                if (eid >= 1 && eid <= static_cast<unsigned>(num_physical)) {
                    counts[eid]++;
                    total++;
                }
            }
        }
        if (num_physical == 0 || groups.empty() || total <= 0 || counts.empty())
            return {};
        std::vector<std::pair<unsigned int, int>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<int> pcts(sorted.size());
        int              sum_pct = 0;
        for (size_t i = 0; i < sorted.size(); ++i) {
            pcts[i] = int((static_cast<long long>(sorted[i].second) * 100) / total);
            sum_pct += pcts[i];
        }
        if (sum_pct < 100) {
            std::vector<std::pair<size_t, int>> rem;
            rem.reserve(sorted.size());
            for (size_t i = 0; i < sorted.size(); ++i)
                rem.emplace_back(i, int((static_cast<long long>(sorted[i].second) * 100) % total));
            std::sort(rem.begin(), rem.end(), [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
            for (int extra = 100 - sum_pct; extra > 0; --extra) {
                pcts[rem.front().first]++;
                rem.erase(rem.begin());
            }
        }
        std::ostringstream out;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0) out << '+';
            out << 'F' << sorted[i].first << ' ' << pcts[i] << '%';
        }
        lbl = out.str();
    } else if (gradient_ids.size() >= 3) {
        const size_t     n = gradient_ids.size();
        std::vector<int> weights;
        {
            std::string token;
            for (const char c : mf.gradient_component_weights) {
                if (c >= '0' && c <= '9') { token.push_back(c); continue; }
                if (!token.empty()) { weights.emplace_back(std::max(0, std::atoi(token.c_str()))); token.clear(); }
            }
            if (!token.empty()) weights.emplace_back(std::max(0, std::atoi(token.c_str())));
            if (weights.size() != n) weights.assign(n, int(100 / n));
        }
        int sum = 0;
        for (int v : weights) sum += v;
        if (sum <= 0) { weights.assign(n, 0); weights[0] = 100; sum = 100; }
        for (size_t k = 0; k < n; ++k) {
            const unsigned int fid = gradient_ids[k];
            const int          pct = int(std::round(100.0 * weights[k] / sum));
            if (k > 0) lbl += "+";
            std::snprintf(buf, sizeof buf, "F%u %d%%", fid, pct);
            lbl += buf;
        }
    } else if (z_gradient_tile) {
        const unsigned from_id = mf.gradient_start >= mf.gradient_end ? mf.component_a : mf.component_b;
        const unsigned to_id   = mf.gradient_start >= mf.gradient_end ? mf.component_b : mf.component_a;
        std::snprintf(buf, sizeof buf, "F%u->F%u", from_id, to_id);
        lbl = buf;
    } else {
        const int pct_b = std::clamp(mf.mix_b_percent, 0, 100);
        const int pct_a = 100 - pct_b;
        std::snprintf(buf, sizeof buf, "F%u %d%%+F%u %d%%", mf.component_a, pct_a, mf.component_b, pct_b);
        lbl = buf;
        if (mf.distribution_mode != int(MixedFilament::Simple))
            for (unsigned int fid : gradient_ids) {
                std::snprintf(buf, sizeof buf, "+F%u", fid);
                lbl += buf;
            }
    }
    return lbl;
}

// PresetBundle::update_multi_material_filament_presets' mixed-filament reload: auto_generate, drop custom rows, load the
// project's definitions against the current physical colours.
static void load_manager(MixedFilamentManager& mgr, const std::string& definitions, const std::vector<std::string>& colors)
{
    mgr.auto_generate(colors);
    mgr.clear_custom_entries();
    mgr.load_custom_entries(definitions, colors);
}

// Index into mixed_filaments() of the row the Color Mixing panel shows as virtual id `id` (non-deleted rows, in order).
static int panel_index_from_id(const MixedFilamentManager& mgr, size_t num_physical, long long id)
{
    if (id <= static_cast<long long>(num_physical)) return -1;
    size_t visible = 0;
    const auto& mfs = mgr.mixed_filaments();
    for (size_t i = 0; i < mfs.size(); ++i) {
        if (mfs[i].deleted) continue;
        if (static_cast<long long>(num_physical + visible + 1) == id) return int(i);
        ++visible;
    }
    return -1;
}

static json panel_rows(MixedFilamentManager& mgr, const std::vector<std::string>& colors, const PanelSettings& settings)
{
    const MixedFilamentDisplayContext ctx = panel_display_context(colors, settings);
    mgr.set_display_context(ctx);
    json rows  = json::array();
    int  index = 0;
    for (MixedFilament& mf : mgr.mixed_filaments()) {
        if (mf.deleted) continue;
        const std::string synced_color = compute_mixed_filament_display_color(mf, ctx);
        if (mf.display_color != synced_color) mf.display_color = synced_color;
        json r;
        r["id"]                = int(colors.size()) + index + 1;
        r["a"]                 = mf.component_a;
        r["b"]                 = mf.component_b;
        r["mix_b_percent"]     = mf.mix_b_percent;
        r["gradient_ids"]      = mf.gradient_component_ids;
        r["gradient_weights"]  = mf.gradient_component_weights;
        r["manual_pattern"]    = mf.manual_pattern;
        r["distribution_mode"] = mf.distribution_mode;
        r["enabled"]           = mf.enabled;
        r["custom"]            = mf.custom;
        r["ui_mode"]           = mf.ui_mode;
        r["stable_id"]         = mf.stable_id;
        r["display"]           = mf.display_color;
        r["label"]             = panel_row_label(mf, colors.size());
        rows.push_back(std::move(r));
        ++index;
    }
    return rows;
}

// ---------------------------------------------------------------------------------------------------------------------
// Request handling
// ---------------------------------------------------------------------------------------------------------------------

static std::string require_colour(const json& v, const char* what)
{
    if (!v.is_string()) throw BadRequest(std::string(what) + " must be a \"#RRGGBB\" string");
    FsColour c;
    if (!try_parse_color_match_hex(v.get<std::string>(), c)) throw BadRequest(std::string(what) + " is not a colour: " + v.get<std::string>());
    return c.hex();
}

// "physical": ["#RRGGBB", ...] or [{"color": "#RRGGBB", "type": "PLA"}, ...]
static std::vector<std::string> read_physical(const json& req)
{
    if (!req.contains("physical") || !req["physical"].is_array()) throw BadRequest("\"physical\" must be an array of colours");
    std::vector<std::string> out;
    for (const json& p : req["physical"]) {
        if (p.is_object()) {
            if (!p.contains("color")) throw BadRequest("a physical entry has no \"color\"");
            out.push_back(require_colour(p["color"], "physical colour"));
        } else {
            out.push_back(require_colour(p, "physical colour"));
        }
    }
    if (out.size() < 2) throw BadRequest("Colour mixing needs at least 2 physical filaments");
    if (out.size() > MixedFilamentManager::kMaxPhysicalFilaments) throw BadRequest("Too many physical filaments");
    return out;
}

static std::string read_definitions(const json& req)
{
    if (!req.contains("definitions") || req["definitions"].is_null()) return {};
    if (!req["definitions"].is_string()) throw BadRequest("\"definitions\" must be a string");
    return req["definitions"].get<std::string>();
}

static int read_int(const json& req, const char* key, int fallback)
{
    if (!req.contains(key) || req[key].is_null()) return fallback;
    if (!req[key].is_number_integer()) throw BadRequest(std::string("\"") + key + "\" must be an integer");
    return req[key].get<int>();
}

static PanelSettings read_settings(const json& req)
{
    PanelSettings s;
    if (req.contains("nozzle_diameter")) {
        const json& v = req["nozzle_diameter"];
        if (v.is_number()) s.nozzle_diameters = {v.get<double>()};
        else if (v.is_array()) for (const json& d : v) { if (!d.is_number()) throw BadRequest("bad nozzle_diameter"); s.nozzle_diameters.push_back(d.get<double>()); }
        else throw BadRequest("bad nozzle_diameter");
    }
    auto num = [&](const char* key, float& out) {
        if (!req.contains(key)) return;
        if (!req[key].is_number()) throw BadRequest(std::string("\"") + key + "\" must be a number");
        out = req[key].get<float>();
    };
    auto flag = [&](const char* key, bool& out) {
        if (!req.contains(key)) return;
        if (req[key].is_boolean()) out = req[key].get<bool>();
        else if (req[key].is_number_integer()) out = req[key].get<int>() != 0;
        else throw BadRequest(std::string("\"") + key + "\" must be a boolean");
    };
    num("mixed_filament_height_lower_bound", s.lower_bound);
    num("mixed_filament_height_upper_bound", s.upper_bound);
    flag("dithering_local_z_mode", s.local_z_mode);
    flag("mixed_filament_component_bias_enabled", s.component_bias_enabled);
    num("layer_height", s.layer_height);
    num("mixed_color_layer_height_a", s.preferred_a_height);
    num("mixed_color_layer_height_b", s.preferred_b_height);
    flag("dithering_local_z_direct_multicolor", s.local_z_direct_multicolor);
    if (req.contains("wall_loops")) s.wall_loops = read_int(req, "wall_loops", 1);
    return s;
}

// Per-slot filament_type from "physical": [{"color", "type"}]; "" where no type is given.
static std::vector<std::string> read_types(const json& req)
{
    std::vector<std::string> out;
    for (const json& p : req["physical"])
        out.push_back(p.is_object() && p.contains("type") && p["type"].is_string() ? p["type"].get<std::string>() : std::string());
    return out;
}

static void read_compatibility(const json& req)
{
    if (req.contains("compatibility") && req["compatibility"].is_string()) {
        std::ifstream in(req["compatibility"].get<std::string>(), std::ios::binary);
        if (!in) throw BadRequest("Cannot read compatibility file " + req["compatibility"].get<std::string>());
        std::stringstream ss;
        ss << in.rdbuf();
        load_filament_compatibility(ss.str());
    } else {
        load_filament_compatibility(k_shipped_compatibility_json);
    }
}

// MixedColorMatchHelpers.cpp build_mixed_filament_display_context (744-802), with the print settings from the request.
static MixedFilamentDisplayContext print_display_context(const std::vector<std::string>& physical_colors, const PanelSettings& s)
{
    MixedFilamentDisplayContext context;
    context.num_physical    = physical_colors.size();
    context.physical_colors = physical_colors;
    context.nozzle_diameters.assign(context.num_physical, 0.4);
    const size_t opt_count = s.nozzle_diameters.size();
    if (opt_count > 0)
        for (size_t i = 0; i < context.num_physical; ++i)
            context.nozzle_diameters[i] = std::max(0.05, s.nozzle_diameters[std::min(i, opt_count - 1)]);
    context.preview_settings.mixed_lower_bound    = std::max(0.01, double(s.lower_bound));
    context.preview_settings.mixed_upper_bound    = std::max(context.preview_settings.mixed_lower_bound, double(s.upper_bound));
    context.preview_settings.preferred_a_height   = std::max(0.0, double(s.preferred_a_height));
    context.preview_settings.preferred_b_height   = std::max(0.0, double(s.preferred_b_height));
    context.preview_settings.nominal_layer_height = std::max(0.01, double(s.layer_height));
    context.preview_settings.wall_loops           = std::max<size_t>(1, size_t(std::max(1, s.wall_loops)));
    context.preview_settings.local_z_mode         = s.local_z_mode;
    context.preview_settings.local_z_direct_multicolor = s.local_z_direct_multicolor && context.preview_settings.preferred_a_height <= EPSILON &&
                                                         context.preview_settings.preferred_b_height <= EPSILON;
    context.component_bias_enabled = s.component_bias_enabled;
    return context;
}

// ---------------------------------------------------------------------------------------------------------------------
// Virtual-id remapping
// ---------------------------------------------------------------------------------------------------------------------

// PresetBundle.cpp build_filament_id_remap (3955-4090), for the case Nozzle has: no physical filament is being deleted
// (deleting_filament == false). `new_mixed` is PresetBundle::mixed_filaments after the change. Index = old 1-based id,
// value = new id (0 = removed).
static std::vector<unsigned int> build_filament_id_remap(const std::vector<MixedFilament>& old_mixed, const std::vector<MixedFilament>& new_mixed,
                                                         size_t old_num_filaments, size_t new_num_filaments, size_t deleted_mixed_idx,
                                                         const std::vector<unsigned int>& kept_physical_ids = {})
{
    const bool         deleting_filament = false;
    const unsigned int deleted_1based    = 0u;
    size_t old_enabled_mixed = 0;
    for (const auto& mf : old_mixed)
        if (mf.enabled) ++old_enabled_mixed;

    const size_t              old_total_filaments = old_num_filaments + old_enabled_mixed;
    std::vector<unsigned int> remap(old_total_filaments + 1, 0);

    std::vector<unsigned int> kept_sorted;
    if (!deleting_filament && !kept_physical_ids.empty()) {
        kept_sorted = kept_physical_ids;
        std::sort(kept_sorted.begin(), kept_sorted.end());
        kept_sorted.erase(std::unique(kept_sorted.begin(), kept_sorted.end()), kept_sorted.end());
    }
    for (unsigned int old_id = 1; old_id <= unsigned(old_num_filaments); ++old_id) {
        unsigned int mapped = 0;
        if (!kept_sorted.empty()) {
            auto it = std::lower_bound(kept_sorted.begin(), kept_sorted.end(), old_id);
            mapped  = (it != kept_sorted.end() && *it == old_id) ? static_cast<unsigned int>(it - kept_sorted.begin() + 1) : 0;
        } else if (old_id <= unsigned(new_num_filaments)) {
            mapped = old_id;
        }
        remap[old_id] = mapped;
    }

    auto canonical_pair = [](unsigned int a, unsigned int b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
    std::unordered_map<uint64_t, unsigned int>                                  new_stable_id_to_virtual_id;
    std::map<std::pair<unsigned int, unsigned int>, std::vector<unsigned int>> new_pair_to_ids;
    unsigned int next_virtual_id = unsigned(new_num_filaments + 1);
    for (const auto& mf : new_mixed) {
        if (!mf.enabled) continue;
        if (mf.stable_id != 0) new_stable_id_to_virtual_id.emplace(mf.stable_id, next_virtual_id);
        new_pair_to_ids[canonical_pair(mf.component_a, mf.component_b)].push_back(next_virtual_id++);
    }

    std::map<std::pair<unsigned int, unsigned int>, size_t> used_per_pair;
    unsigned int old_virtual_id = unsigned(old_num_filaments + 1);
    for (size_t midx = 0; midx < old_mixed.size(); ++midx) {
        const auto& mf = old_mixed[midx];
        if (!mf.enabled) continue;
        if (midx == deleted_mixed_idx) { ++old_virtual_id; continue; }
        const std::string norm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
        unsigned int      a    = mf.component_a;
        unsigned int      b    = mf.component_b;
        if (norm.empty() && (a == deleted_1based || b == deleted_1based)) {
            remap[old_virtual_id] = 0;
        } else {
            bool mapped_by_stable_id = false;
            if (mf.stable_id != 0) {
                auto it_stable = new_stable_id_to_virtual_id.find(mf.stable_id);
                if (it_stable != new_stable_id_to_virtual_id.end()) {
                    remap[old_virtual_id] = it_stable->second;
                    mapped_by_stable_id   = true;
                }
            }
            if (!mapped_by_stable_id) {
                const auto key = canonical_pair(a, b);
                auto       it  = new_pair_to_ids.find(key);
                if (it == new_pair_to_ids.end()) {
                    remap[old_virtual_id] = 0;
                } else {
                    size_t& used = used_per_pair[key];
                    remap[old_virtual_id] = used >= it->second.size() ? 0 : it->second[used++];
                }
            }
        }
        ++old_virtual_id;
    }
    return remap;
}

// {"<old id>": <new id>} for every id whose number changes (0 = removed; Snapmaker then clears that paint to NONE).
static json remap_json(const std::vector<unsigned int>& remap)
{
    json out = json::object();
    for (size_t i = 1; i < remap.size(); ++i)
        if (remap[i] != i) out[std::to_string(i)] = remap[i];
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// MixedFilamentDialog (Add Mix / Edit Mix): collect_result per mode, with the dialog's validation and banners
// ---------------------------------------------------------------------------------------------------------------------

static constexpr int MODE_RATIO = 0, MODE_CYCLE = 1, MODE_MATCH = 2, MODE_GRADIENT = 3;
static constexpr int MIN_RATIO_PERCENT = 10, MAX_RATIO_PERCENT = 90; // MixedGradientSelector.hpp

struct DialogOutcome {
    MixedFilament r;
    std::string   warning; // the orange advisory banner, if any (OK stays enabled)
};

static int mode_from_name(const std::string& m)
{
    if (m == "ratio") return MODE_RATIO;
    if (m == "cycle") return MODE_CYCLE;
    if (m == "match") return MODE_MATCH;
    if (m == "gradient") return MODE_GRADIENT;
    throw BadRequest("\"mode\" must be ratio, cycle, match or gradient");
}

static std::string fmt(const char* f, int a, int b = 0)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

// get_ratio_warning_msg (MixedFilamentDialog.cpp 2262-2351): `ratios` indexed by 0-based physical slot.
static std::string ratio_warning(const std::vector<double>& ratios)
{
    static constexpr double HIGH_RATIO_THRESHOLD = 0.667;
    double total = 0.0;
    for (double r : ratios) total += r;
    if (total <= 0.0) return {};
    double max_ratio = 0.0;
    int    max_idx   = -1;
    for (int i = 0; i < int(ratios.size()); ++i) {
        const double ratio = ratios[i] / total;
        if (ratio > max_ratio) { max_ratio = ratio; max_idx = i; }
    }
    if (max_idx >= 0 && max_ratio > HIGH_RATIO_THRESHOLD)
        return fmt("Filament %d ratio is too high. Mix may be affected.", max_idx + 1);
    return {};
}

// update_compatibility_warning's error branch: `fids` 0-based.
static void check_compatibility(const std::vector<unsigned int>& fids, const std::vector<std::string>& types)
{
    if (is_filament_compatible(fids, types)) return;
    const auto pair = find_incompatible_filament_pair(fids, types);
    if (pair.first != 0)
        throw BadRequest(fmt("Filament %d and Filament %d cannot be mixed. Please select filaments of the same type.", int(pair.first), int(pair.second)));
    throw BadRequest("Different filament types cannot be mixed. Please correct the settings.");
}

static std::vector<unsigned int> read_filaments(const json& req, size_t num_physical, size_t min_count, size_t max_count)
{
    if (!req.contains("filaments") || !req["filaments"].is_array()) throw BadRequest("this mode needs \"filaments\": [1-based slot ids]");
    std::vector<unsigned int> ids;
    for (const json& v : req["filaments"]) {
        if (!v.is_number_integer() || v.get<long long>() < 1 || v.get<long long>() > (long long) num_physical)
            throw BadRequest("filaments must be physical slot ids 1.." + std::to_string(num_physical));
        const unsigned int id = v.get<unsigned int>();
        if (std::find(ids.begin(), ids.end(), id) != ids.end()) throw BadRequest("each filament can be picked once"); // combos exclude each other
        ids.push_back(id);
    }
    if (ids.size() < min_count || ids.size() > max_count)
        throw BadRequest("this mode takes " + std::to_string(min_count) + (min_count == max_count ? "" : "-" + std::to_string(max_count)) + " filaments");
    return ids;
}

static std::vector<double> read_weights(const json& req, size_t n)
{
    std::vector<double> w;
    if (!req.contains("weights")) return w;
    if (!req["weights"].is_array() || req["weights"].size() != n) throw BadRequest("\"weights\" must have one number per filament");
    for (const json& v : req["weights"]) {
        if (!v.is_number() || v.get<double>() < 0) throw BadRequest("weights must be non-negative numbers");
        w.push_back(v.get<double>());
    }
    return w;
}

// validate_cycle_pattern (3155-3227): returns the normalized pattern or throws the dialog's error text.
static std::string validate_cycle_pattern(const std::string& raw, size_t num_physical)
{
    bool        has_invalid_chars = false;
    std::string filtered;
    for (char c : raw) {
        if (c == ',' || c == '[' || c == ']' || (c >= '0' && c <= '9')) filtered.push_back(c);
        else has_invalid_chars = true;
    }
    const bool        has_leading_trailing_comma = !raw.empty() && (raw.front() == ',' || raw.back() == ',');
    const std::string normalized                 = MixedFilamentManager::normalize_manual_pattern(filtered);
    const bool        is_malformed               = !filtered.empty() && normalized.empty();
    if (has_leading_trailing_comma) throw BadRequest("Leading or trailing commas are not allowed.");
    if (has_invalid_chars) throw BadRequest("Invalid characters found. Only digits, square brackets ([ and ]), and commas (,) are allowed.");
    if (is_malformed) throw BadRequest("Unrecognized pattern format. Please check the pattern syntax.");
    if (num_physical >= 2 && !normalized.empty()) {
        // parse_cycle_pattern (MixedColorMatchHelpers.cpp 1361-1385)
        for (const auto& group : MixedFilamentManager::split_pattern_groups(normalized))
            for (const auto& token : MixedFilamentManager::split_pattern_group_to_tokens(group, num_physical)) {
                char*         end = nullptr;
                unsigned long id  = std::strtoul(token.c_str(), &end, 10);
                if (!end || *end != '\0') throw BadRequest("Unrecognized pattern format. Please check the pattern syntax.");
                if (id < 1 || id > num_physical) throw BadRequest(fmt("Filament %d not recognized. Please re-enter.", int(id)));
            }
    }
    return normalized.empty() ? "12" : normalized;
}

// The row MixedFilamentDialog::collect_result (3246-3414) returns for `mode`, starting from `base` (m_result: the edited
// row, or the Add dialog's fresh row a=1, b=2, 50%), with the controls set from the request.
static DialogOutcome dialog_collect(const MixedFilament& base, const json& req, const std::vector<std::string>& colours,
                                   const std::vector<std::string>& types)
{
    const size_t  n_phys = colours.size();
    const int     mode   = mode_from_name(req["mode"].get<std::string>());
    DialogOutcome out;
    MixedFilament& r = out.r;
    r = base;
    // The Ratio card's MixedGradientSelector: constructed from m_result.mix_b_percent, clamped to 10..90.
    int val = std::clamp(base.mix_b_percent, MIN_RATIO_PERCENT, MAX_RATIO_PERCENT);
    r.ui_mode = mode;
    r.gradient_enabled = false;
    std::vector<unsigned int> fids; // 0-based, for the compatibility check
    std::vector<double>       ratios(n_phys, 0.0);

    switch (mode) {
    case MODE_RATIO: {
        const auto ids = read_filaments(req, n_phys, 2, 3);
        r.component_a = ids[0];
        r.component_b = ids[1];
        if (req.contains("mix_b_percent")) val = std::clamp(read_int(req, "mix_b_percent", val), MIN_RATIO_PERCENT, MAX_RATIO_PERCENT);
        r.mix_b_percent = val;
        r.gradient_component_weights.clear();
        if (ids.size() == 3) {
            // Tri picker: weights clamped to 10..90% (4 passes of clamp + renormalize, as a drag does). Without
            // "weights": the Edit dialog's restored weights, else 1/3 each.
            double wx = 1.0 / 3.0, wy = 1.0 / 3.0, wz = 1.0 / 3.0;
            const auto w = read_weights(req, 3);
            if (!w.empty()) {
                const double sum = w[0] + w[1] + w[2];
                if (sum <= 0) throw BadRequest("weights must not all be zero");
                wx = w[0] / sum; wy = w[1] / sum; wz = w[2] / sum;
                for (int i = 0; i < 4; ++i) {
                    wx = std::clamp(wx, 0.10, 0.90); wy = std::clamp(wy, 0.10, 0.90); wz = std::clamp(wz, 0.10, 0.90);
                    const double s2 = wx + wy + wz;
                    if (s2 > 0) { wx /= s2; wy /= s2; wz /= s2; }
                }
            } else if (base.ui_mode == MODE_RATIO && !base.gradient_component_weights.empty()) {
                std::vector<int> vals;
                const char*      p = base.gradient_component_weights.c_str();
                while (*p) {
                    char* end;
                    int   v = (int) std::strtol(p, &end, 10);
                    if (end == p) break;
                    vals.push_back(v);
                    p = end;
                    if (*p == '/') ++p;
                }
                if (vals.size() >= 3) {
                    int total = 0;
                    for (int v : vals) total += v;
                    if (total > 0) {
                        wx = std::clamp(vals[0] / (double) total, 0.10, 0.90);
                        wy = std::clamp(vals[1] / (double) total, 0.10, 0.90);
                        wz = std::clamp(vals[2] / (double) total, 0.10, 0.90);
                        const double s2 = wx + wy + wz;
                        if (s2 > 0) { wx /= s2; wy /= s2; wz /= s2; }
                    }
                }
            }
            r.distribution_mode = int(MixedFilament::LayerCycle);
            r.manual_pattern.clear();
            r.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
            const int r0 = (int) (wx * 100 + 0.5);
            const int r1 = (int) (wy * 100 + 0.5);
            const int r2 = 100 - r0 - r1;
            r.gradient_component_weights = std::to_string(r0) + "/" + std::to_string(r1) + "/" + std::to_string(r2);
            ratios[ids[0] - 1] = wx; ratios[ids[1] - 1] = wy; ratios[ids[2] - 1] = wz;
        } else {
            r.distribution_mode = int(MixedFilament::Simple);
            r.gradient_component_ids.clear();
            r.manual_pattern.clear();
            const int pct_b   = std::clamp(val, 0, 100);
            int       ratio_a = 1, ratio_b = 0;
            if (pct_b >= 100) {
                ratio_a = 0; ratio_b = 1;
            } else if (pct_b > 0) {
                const int  pct_a      = 100 - pct_b;
                const bool b_is_major = pct_b >= pct_a;
                const int  major_pct  = b_is_major ? pct_b : pct_a;
                const int  minor_pct  = b_is_major ? pct_a : pct_b;
                const int  g          = std::gcd(major_pct, minor_pct);
                ratio_a = b_is_major ? (minor_pct / g) : (major_pct / g);
                ratio_b = b_is_major ? (major_pct / g) : (minor_pct / g);
            }
            r.ratio_a = std::max(0, ratio_a);
            r.ratio_b = std::max(0, ratio_b);
            ratios[ids[0] - 1] = (100.0 - val) / 100.0;
            ratios[ids[1] - 1] = val / 100.0;
        }
        for (unsigned int id : ids) fids.push_back(id - 1);
        break;
    }
    case MODE_CYCLE: {
        std::string raw = MixedFilamentManager::normalize_manual_pattern(base.ui_mode == MODE_CYCLE || base.ui_mode < 0 ? base.manual_pattern : std::string());
        if (raw.empty()) raw = "12";
        if (req.contains("pattern")) {
            if (!req["pattern"].is_string()) throw BadRequest("\"pattern\" must be a string");
            raw = req["pattern"].get<std::string>();
        }
        r.mix_b_percent     = val;
        r.distribution_mode = int(MixedFilament::Simple);
        r.manual_pattern    = validate_cycle_pattern(raw, n_phys);
        r.gradient_component_ids.clear();
        r.gradient_component_weights.clear();
        r.component_a = 1;
        r.component_b = 2;
        for (const auto& group : MixedFilamentManager::split_pattern_groups(r.manual_pattern))
            for (const auto& token : MixedFilamentManager::split_pattern_group_to_tokens(group, n_phys)) {
                const unsigned long id = std::strtoul(token.c_str(), nullptr, 10);
                if (id >= 1 && id <= n_phys && std::find(fids.begin(), fids.end(), unsigned(id - 1)) == fids.end()) fids.push_back(unsigned(id - 1));
            }
        break;
    }
    case MODE_MATCH: {
        // The Match card: tri picker for 3 filaments, gradient bar (range min..100-min) for 2.
        const auto ids     = read_filaments(req, n_phys, 2, 3);
        const int  min_pct = std::clamp(read_int(req, "min_percent", 15), 0, 50);
        r.mix_b_percent     = val;
        r.distribution_mode = int(MixedFilament::Simple);
        r.manual_pattern.clear();
        r.ratio_a = 1;
        r.ratio_b = 1;
        r.component_a = ids[0];
        r.component_b = ids[1];
        if (ids.size() == 3) {
            const auto w = read_weights(req, 3);
            if (w.empty()) throw BadRequest("match with 3 filaments needs \"weights\"");
            const double total = w[0] + w[1] + w[2];
            if (total <= 0) throw BadRequest("weights must not all be zero");
            const double wx = w[0] / total, wy = w[1] / total, wz = w[2] / total;
            const int w0 = (int) (wx * 100 + 0.5);
            const int w1 = (int) (wy * 100 + 0.5);
            const int w2 = std::max(0, 100 - w0 - w1);
            std::string               weights_str;
            std::vector<unsigned int> match_ids;
            if (w0 > 0) { match_ids.push_back(ids[0]); weights_str += std::to_string(w0); }
            if (w1 > 0) { match_ids.push_back(ids[1]); if (!weights_str.empty()) weights_str += "/"; weights_str += std::to_string(w1); }
            if (w2 > 0) { match_ids.push_back(ids[2]); if (!weights_str.empty()) weights_str += "/"; weights_str += std::to_string(w2); }
            r.gradient_component_ids     = MixedFilamentManager::encode_gradient_component_ids(match_ids);
            r.gradient_component_weights = weights_str;
            if (match_ids.size() >= 3) {
                r.distribution_mode = int(MixedFilament::LayerCycle);
                r.mix_b_percent     = 50;
            } else if (match_ids.size() == 2) {
                r.distribution_mode = int(MixedFilament::Simple);
                int total_w = 0;
                if (w0 > 0) total_w += w0;
                if (w1 > 0) total_w += w1;
                if (w2 > 0) total_w += w2;
                r.mix_b_percent = (total_w > 0) ? (100 * (w1 > 0 ? w1 : w2) / total_w) : 50;
            }
            ratios[ids[0] - 1] = wx; ratios[ids[1] - 1] = wy; ratios[ids[2] - 1] = wz;
        } else {
            const int mval = std::clamp(read_int(req, "mix_b_percent", 50), min_pct, 100 - min_pct);
            r.mix_b_percent              = mval;
            r.gradient_component_ids     = MixedFilamentManager::encode_gradient_component_ids({r.component_a, r.component_b});
            r.gradient_component_weights.clear();
            ratios[ids[0] - 1] = (100.0 - mval) / 100.0;
            ratios[ids[1] - 1] = mval / 100.0;
        }
        for (unsigned int id : ids) fids.push_back(id - 1);
        break;
    }
    case MODE_GRADIENT: {
        const auto ids = read_filaments(req, n_phys, 2, 2);
        int direction  = (base.gradient_start >= base.gradient_end) ? 0 : 1;
        if (req.contains("direction")) direction = read_int(req, "direction", direction) != 0 ? 1 : 0;
        r.component_a       = ids[0];
        r.component_b       = ids[1];
        r.distribution_mode = int(MixedFilament::LayerCycle);
        r.gradient_component_ids.clear();
        r.gradient_component_weights.clear();
        r.manual_pattern.clear();
        r.gradient_enabled = true;
        if (direction == 0) {
            r.gradient_start = MixedFilament::k_default_gradient_dominant;
            r.gradient_end   = MixedFilament::k_default_gradient_minority;
        } else {
            r.gradient_start = MixedFilament::k_default_gradient_minority;
            r.gradient_end   = MixedFilament::k_default_gradient_dominant;
        }
        r.mix_b_percent = 50;
        r.ratio_a       = 1;
        r.ratio_b       = 1;
        if (r.local_z_max_sublayers < 2) r.local_z_max_sublayers = 2;
        ratios[ids[0] - 1] = 0.5;
        ratios[ids[1] - 1] = 0.5;
        for (unsigned int id : ids) fids.push_back(id - 1);
        break;
    }
    }
    r.custom = true;

    // update_compatibility_warning (2173-2237): an incompatible mix blocks OK; the rest are advisories.
    check_compatibility(fids, types);
    if (mode != MODE_CYCLE) out.warning = ratio_warning(ratios);
    if (out.warning.empty() && mode == MODE_CYCLE && fids.size() == 1)
        out.warning = "Same filament colors cannot produce new colors. Please select different colors for mixing.";
    else if (out.warning.empty() && mode == MODE_CYCLE && fids.size() > 4)
        out.warning = "Excessive filaments in the mix may affect the result. Please use with caution.";
    return out;
}

// Fields no dialog control writes, accepted as explicit overrides on add/update (see the report).
static void apply_row_overrides(MixedFilament& mf, const json& req)
{
    auto fnum = [&](const char* key, float& out) {
        if (!req.contains(key)) return;
        if (!req[key].is_number()) throw BadRequest(std::string("\"") + key + "\" must be a number");
        out = req[key].get<float>();
    };
    if (req.contains("local_z_max_sublayers")) mf.local_z_max_sublayers = std::max(0, read_int(req, "local_z_max_sublayers", 0));
    fnum("component_a_surface_offset", mf.component_a_surface_offset);
    fnum("component_b_surface_offset", mf.component_b_surface_offset);
    if (req.contains("gradient_enabled")) {
        if (!req["gradient_enabled"].is_boolean()) throw BadRequest("\"gradient_enabled\" must be a boolean");
        mf.gradient_enabled = req["gradient_enabled"].get<bool>();
    }
    fnum("gradient_start", mf.gradient_start);
    fnum("gradient_end", mf.gradient_end);
}

// MixedFilamentDialog::collect_result in RATIO mode, starting from `base` (the row being edited, or a fresh row), with
// the request's explicit fields applied on top. Used when the request names no "mode" (the flat-field form).
static MixedFilament dialog_result(const MixedFilament& base, const json& req, size_t num_physical)
{
    MixedFilament r = base;
    r.component_a   = unsigned(read_int(req, "a", int(base.component_a)));
    r.component_b   = unsigned(read_int(req, "b", int(base.component_b)));
    if (r.component_a < 1 || r.component_a > num_physical || r.component_b < 1 || r.component_b > num_physical)
        throw BadRequest("a and b must be physical filament ids (1.." + std::to_string(num_physical) + ")");
    r.mix_b_percent    = std::clamp(read_int(req, "mix_b_percent", base.mix_b_percent), 0, 100);
    r.ui_mode          = 0; // MODE_RATIO
    r.gradient_enabled = false;
    std::string gids   = base.gradient_component_ids;
    if (req.contains("gradient_ids")) {
        if (!req["gradient_ids"].is_string()) throw BadRequest("\"gradient_ids\" must be a string such as \"124\"");
        gids = req["gradient_ids"].get<std::string>();
    } else if (req.contains("a") || req.contains("b") || req.contains("mix_b_percent")) {
        gids.clear();
    }
    const std::vector<unsigned int> ids = MixedFilamentManager::decode_gradient_component_ids(gids, num_physical);
    if (ids.size() >= 3) {
        r.distribution_mode = int(MixedFilament::LayerCycle);
        r.manual_pattern.clear();
        r.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
        if (req.contains("gradient_weights")) {
            if (!req["gradient_weights"].is_string()) throw BadRequest("\"gradient_weights\" must be a string such as \"50/25/25\"");
            r.gradient_component_weights = req["gradient_weights"].get<std::string>();
        }
    } else {
        r.gradient_component_weights.clear();
        r.distribution_mode = int(MixedFilament::Simple);
        r.gradient_component_ids.clear();
        r.manual_pattern.clear();
        const int pct_b   = std::clamp(r.mix_b_percent, 0, 100);
        int       ratio_a = 1, ratio_b = 0;
        if (pct_b >= 100) {
            ratio_a = 0; ratio_b = 1;
        } else if (pct_b > 0) {
            const int  pct_a      = 100 - pct_b;
            const bool b_is_major = pct_b >= pct_a;
            const int  major_pct  = b_is_major ? pct_b : pct_a;
            const int  minor_pct  = b_is_major ? pct_a : pct_b;
            const int  g          = std::gcd(major_pct, minor_pct);
            ratio_a = b_is_major ? (minor_pct / g) : (major_pct / g);
            ratio_b = b_is_major ? (major_pct / g) : (minor_pct / g);
        }
        r.ratio_a = std::max(0, ratio_a);
        r.ratio_b = std::max(0, ratio_b);
    }
    if (req.contains("distribution_mode")) r.distribution_mode = std::clamp(read_int(req, "distribution_mode", r.distribution_mode), 0, 2);
    if (req.contains("manual_pattern")) {
        if (!req["manual_pattern"].is_string()) throw BadRequest("\"manual_pattern\" must be a string");
        r.manual_pattern = MixedFilamentManager::normalize_manual_pattern(req["manual_pattern"].get<std::string>());
    }
    if (req.contains("ui_mode")) r.ui_mode = read_int(req, "ui_mode", r.ui_mode);
    return r;
}

static json display_response(MixedFilamentManager& mgr, const std::vector<std::string>& colors, const PanelSettings& settings)
{
    json out;
    json rows          = panel_rows(mgr, colors, settings);
    out["definitions"] = mgr.serialize_custom_entries();
    out["rows"]        = std::move(rows);
    return out;
}

// Plater's copy of the dialog result into the row ("+" handler 6585-6620, Edit handlers 6840-6915).
static void copy_dialog_result(MixedFilament& dst, const MixedFilament& r, bool edit)
{
    if (edit) {
        dst.component_a   = r.component_a;
        dst.component_b   = r.component_b;
        dst.mix_b_percent = r.mix_b_percent;
    }
    dst.distribution_mode          = r.distribution_mode;
    dst.manual_pattern             = r.manual_pattern;
    dst.gradient_component_ids     = r.gradient_component_ids;
    dst.gradient_component_weights = r.gradient_component_weights;
    dst.ratio_a                    = r.ratio_a;
    dst.ratio_b                    = r.ratio_b;
    dst.local_z_max_sublayers      = r.local_z_max_sublayers;
    dst.gradient_enabled           = r.gradient_enabled;
    dst.gradient_start             = r.gradient_start;
    dst.gradient_end               = r.gradient_end;
    dst.ui_mode                    = r.ui_mode;
    dst.custom                     = true;
}

// The row the dialog produces for this request: a named "mode" runs the dialog emulation, otherwise the flat fields.
static MixedFilament request_row(const MixedFilament& base, const json& req, const std::vector<std::string>& colours,
                                 const std::vector<std::string>& types, std::string& warning)
{
    MixedFilament r;
    if (req.contains("mode")) {
        if (!req["mode"].is_string()) throw BadRequest("\"mode\" must be a string");
        DialogOutcome o = dialog_collect(base, req, colours, types);
        r       = o.r;
        warning = o.warning;
    } else {
        r = dialog_result(base, req, colours.size());
    }
    return r;
}

// Sidebar::init_color_mix_panel "+" handler.
static json op_add(const json& req)
{
    const auto          colors   = read_physical(req);
    const auto          types    = read_types(req);
    const PanelSettings settings = read_settings(req);
    read_compatibility(req);
    MixedFilamentManager mgr;
    load_manager(mgr, read_definitions(req), colors);
    if (!req.contains("mode"))
        for (const char* k : {"a", "b"})
            if (!req.contains(k)) throw BadRequest(std::string("add needs \"") + k + "\" (or a \"mode\")");

    MixedFilament fresh; // MixedFilamentDialog's "Add Mix" m_result: a=1, b=2, 50%
    fresh.component_a   = 1;
    fresh.component_b   = 2;
    fresh.mix_b_percent = 50;
    std::string         warning;
    const MixedFilament r = request_row(fresh, req, colors, types, warning);

    long long added_id = 0;
    if (mgr.total_filaments(colors.size()) < MAXIMUM_FILAMENT_NUMBER) {
        mgr.add_custom_filament(r.component_a, r.component_b, r.mix_b_percent, colors);
        auto& mfs = mgr.mixed_filaments();
        if (!mfs.empty()) {
            copy_dialog_result(mfs.back(), r, false);
            apply_row_overrides(mfs.back(), req);
            size_t visible = 0;
            for (const MixedFilament& mf : mfs) if (!mf.deleted) ++visible;
            added_id = (long long) (colors.size() + visible);
        }
    }
    if (added_id == 0) throw std::runtime_error("The filament list is full (" + std::to_string(MAXIMUM_FILAMENT_NUMBER) + " filaments).");
    const std::string    defs = mgr.serialize_custom_entries();
    MixedFilamentManager saved;
    load_manager(saved, defs, colors);
    json out = display_response(saved, colors, settings);
    out["added_id"] = added_id;
    if (!warning.empty()) out["warning"] = warning;
    return out;
}

// Sidebar::init_color_mix_panel "-" handler (no id: last custom row) and the row menu's Delete (with id).
static json op_remove(const json& req)
{
    const auto          colors   = read_physical(req);
    const PanelSettings settings = read_settings(req);
    MixedFilamentManager mgr;
    load_manager(mgr, read_definitions(req), colors);
    auto&                            mfs       = mgr.mixed_filaments();
    const std::vector<MixedFilament> old_mixed = mfs;
    int                              i         = -1;
    if (req.contains("id") && !req["id"].is_null()) {
        if (!req["id"].is_number_integer()) throw BadRequest("\"id\" must be an integer");
        i = panel_index_from_id(mgr, colors.size(), req["id"].get<long long>());
        if (i < 0) throw BadRequest("No colour-mixing row has id " + std::to_string(req["id"].get<long long>()));
        mfs[size_t(i)].deleted = true;
        mfs[size_t(i)].enabled = false;
    } else {
        for (int k = static_cast<int>(mfs.size()) - 1; k >= 0; --k) {
            if (mfs[k].custom && !mfs[k].deleted) {
                mfs[k].deleted = true;
                // The "-" handler leaves `enabled` set; the row is disabled when the project string is reloaded.
                mfs[k].enabled = false;
                i = k;
                break;
            }
        }
    }
    // Delete menu: update_mixed_filament_id_remap(old_mixed, N, N, i) (Plater.cpp 7020).
    const std::vector<unsigned int> remap =
        build_filament_id_remap(old_mixed, mfs, colors.size(), colors.size(), i >= 0 ? size_t(i) : size_t(-1));
    const std::string    defs = mgr.serialize_custom_entries();
    MixedFilamentManager saved;
    load_manager(saved, defs, colors);
    json out    = display_response(saved, colors, settings);
    out["remap"] = remap_json(remap);
    return out;
}

// The row Edit handler (MixedFilamentDialog on an existing row). Editing never changes which rows are enabled, so no id
// is renumbered.
static json op_update(const json& req)
{
    const auto          colors   = read_physical(req);
    const auto          types    = read_types(req);
    const PanelSettings settings = read_settings(req);
    read_compatibility(req);
    MixedFilamentManager mgr;
    load_manager(mgr, read_definitions(req), colors);
    if (!req.contains("id") || !req["id"].is_number_integer()) throw BadRequest("update needs an integer \"id\"");
    const int i = panel_index_from_id(mgr, colors.size(), req["id"].get<long long>());
    if (i < 0) throw BadRequest("No colour-mixing row has id " + std::to_string(req["id"].get<long long>()));
    auto&               mfs2 = mgr.mixed_filaments();
    std::string         warning;
    const MixedFilament r = request_row(mfs2[size_t(i)], req, colors, types, warning);
    copy_dialog_result(mfs2[size_t(i)], r, true);
    apply_row_overrides(mfs2[size_t(i)], req);
    const std::string    defs = mgr.serialize_custom_entries();
    MixedFilamentManager saved;
    load_manager(saved, defs, colors);
    json out = display_response(saved, colors, settings);
    out["remap"] = json::object();
    if (!warning.empty()) out["warning"] = warning;
    return out;
}

static json op_display(const json& req)
{
    const auto           colors = read_physical(req);
    MixedFilamentManager mgr;
    load_manager(mgr, read_definitions(req), colors);
    return display_response(mgr, colors, read_settings(req));
}

// Sidebar::cleanup_unused_filaments_after_batch_match (Plater.cpp 8672-8990), mixed rows only: Nozzle's physical slots
// are toolheads and are never deleted, so every physical slot is kept for the cascade check, and the physical slots
// Snapmaker would have deleted are only reported.
static json op_cleanup(const json& req)
{
    const auto          colors   = read_physical(req);
    const PanelSettings settings = read_settings(req);
    MixedFilamentManager mgr;
    load_manager(mgr, read_definitions(req), colors);
    const size_t num_physical = colors.size();
    if (!req.contains("used_ids") || !req["used_ids"].is_array()) throw BadRequest("cleanup needs \"used_ids\"");
    std::vector<unsigned int> used_physical, kept_mixed;
    for (const json& v : req["used_ids"]) {
        if (!v.is_number_integer() || v.get<long long>() < 1) throw BadRequest("used_ids must be positive integers");
        const unsigned int id = v.get<unsigned int>();
        (id <= num_physical ? used_physical : kept_mixed).push_back(id);
    }
    auto& mfs = mgr.mixed_filaments();

    std::vector<unsigned int> all_physical;
    for (unsigned int k = 1; k <= num_physical; ++k) all_physical.push_back(k);
    const RedundantFilamentSet red = compute_redundant_filaments(num_physical, all_physical, kept_mixed, mfs);

    // Physical slots Snapmaker would delete: those no used id needs, counting the components of the mixed rows that
    // survive (otherwise Snapmaker's cascade would delete a row the project still uses).
    std::vector<unsigned int> needed_physical = used_physical;
    {
        const std::unordered_set<unsigned int> doomed(red.redundant_mixed.begin(), red.redundant_mixed.end());
        unsigned int vid = static_cast<unsigned int>(num_physical) + 1;
        for (const MixedFilament& mf : mfs) {
            if (!mf.enabled || mf.deleted) continue;
            if (!doomed.count(vid)) {
                const std::string norm = MixedFilamentManager::normalize_manual_pattern(mf.manual_pattern);
                if (!norm.empty()) {
                    for (const auto& g : MixedFilamentManager::split_pattern_groups(norm))
                        for (const auto& t : MixedFilamentManager::split_pattern_group_to_tokens(g, 0))
                            needed_physical.push_back(MixedFilamentManager::physical_filament_from_token(t, mf, num_physical));
                } else {
                    needed_physical.push_back(mf.component_a);
                    needed_physical.push_back(mf.component_b);
                    for (unsigned int gid : MixedFilamentManager::decode_gradient_component_ids(mf.gradient_component_ids, 0))
                        needed_physical.push_back(gid);
                }
            }
            ++vid;
        }
    }
    const RedundantFilamentSet phys = compute_redundant_filaments(num_physical, needed_physical, {}, {});
    std::vector<unsigned int>  unused_physical(phys.redundant_physical.rbegin(), phys.redundant_physical.rend());

    // Mark the redundant rows (by stable id) and build the T2 -> T3 painting remap, as the source does.
    std::vector<uint64_t> redundant_sids;
    for (unsigned int redundant_id : red.redundant_mixed) {
        const int idx = mgr.mixed_index_from_filament_id(redundant_id, num_physical);
        if (idx >= 0 && mfs[size_t(idx)].stable_id != 0) redundant_sids.push_back(mfs[size_t(idx)].stable_id);
    }
    std::vector<unsigned int> remap;
    if (!redundant_sids.empty()) {
        const std::unordered_set<uint64_t> to_delete(redundant_sids.begin(), redundant_sids.end());
        std::vector<unsigned int>          deleted_t2_vids;
        unsigned int                       vid = static_cast<unsigned int>(num_physical) + 1;
        for (const MixedFilament& mf : mfs) {
            if (!mf.enabled || mf.deleted) continue;
            if (mf.stable_id != 0 && to_delete.count(mf.stable_id) > 0) deleted_t2_vids.push_back(vid);
            ++vid;
        }
        remap = MixedFilamentManager::build_mixed_deletion_painting_remap(num_physical, num_physical + mgr.enabled_count(), deleted_t2_vids);
        for (auto& mf : mfs)
            if (mf.stable_id != 0 && to_delete.count(mf.stable_id) > 0) { mf.deleted = true; mf.enabled = false; }
    }
    const std::string    defs = mgr.serialize_custom_entries();
    MixedFilamentManager saved;
    load_manager(saved, defs, colors);
    json out = display_response(saved, colors, settings);
    out["deleted_ids"]     = red.redundant_mixed;
    out["remap"]           = remap_json(remap);
    out["unused_physical"] = unused_physical;
    return out;
}

// The Match card's tri/gradient state after a recipe arrives (MixedFilamentDialog.cpp 1224-1275: weights expanded over
// the physical slots, sorted by weight, the top three kept), as the dialog_collect "match" request that reproduces it.
static json match_dialog_request(const MixedColorMatchRecipeResult& recipe, size_t num_physical, int min_pct)
{
    const auto                       weights = expand_color_match_recipe_weights(recipe, num_physical);
    std::vector<std::pair<int, int>> sorted;
    for (int i = 0; i < (int) weights.size(); ++i)
        if (weights[i] > 0) sorted.push_back({i, weights[i]});
    std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
    json d;
    d["mode"]        = "match";
    d["min_percent"] = min_pct;
    if (sorted.size() >= 3) {
        sorted.resize(3);
        d["filaments"] = {sorted[0].first + 1, sorted[1].first + 1, sorted[2].first + 1};
        d["weights"]   = {sorted[0].second, sorted[1].second, sorted[2].second};
    } else if (sorted.size() == 2) {
        const int total = sorted[0].second + sorted[1].second;
        d["filaments"]     = {sorted[0].first + 1, sorted[1].first + 1};
        d["mix_b_percent"] = (total > 0) ? (sorted[1].second * 100 / total) : 50;
    }
    return d;
}

static json row_fields(const MixedFilament& r)
{
    json j;
    j["a"]                = r.component_a;
    j["b"]                = r.component_b;
    j["mix_b_percent"]    = r.mix_b_percent;
    j["gradient_ids"]     = r.gradient_component_ids;
    j["gradient_weights"] = r.gradient_component_weights;
    j["manual_pattern"]   = r.manual_pattern;
    j["distribution_mode"] = r.distribution_mode;
    j["gradient_enabled"] = r.gradient_enabled;
    j["gradient_start"]   = r.gradient_start;
    j["gradient_end"]     = r.gradient_end;
    j["local_z_max_sublayers"] = r.local_z_max_sublayers;
    j["ui_mode"]          = r.ui_mode;
    return j;
}

// The recipe search behind the dialog's Match mode (MixedColorMatchPanel::launch_recipe_match and
// sync_recipe_preview; the same call as MixedFilamentColorMatchDialog in Plater.cpp ~1925), without adding a row.
static json op_match_one(const json& req)
{
    const auto          colors   = read_physical(req);
    const auto          types    = read_types(req);
    const PanelSettings settings = read_settings(req);
    read_compatibility(req);
    if (!req.contains("target")) throw BadRequest("match_one needs a \"target\" colour");
    FsColour target;
    try_parse_color_match_hex(require_colour(req["target"], "target"), target);
    const int min_pct = read_int(req, "min_percent", 15);  // m_match_min_pct / m_min_component_percent default
    const int max_pct = read_int(req, "max_percent", 100); // build_best_color_match_recipe's default
    if (min_pct < 0 || min_pct > 50) throw BadRequest("min_percent must be in [0, 50]");
    if (max_pct < 50 || max_pct > 100) throw BadRequest("max_percent must be in [50, 100]");

    // Optional subset of slots (not a Snapmaker control: the dialog always searches every physical filament).
    std::vector<unsigned int> slots;
    if (req.contains("slots")) {
        if (!req["slots"].is_array()) throw BadRequest("\"slots\" must be an array");
        for (const json& v : req["slots"]) {
            if (!v.is_number_integer() || v.get<long long>() < 1 || v.get<long long>() > (long long) colors.size())
                throw BadRequest("slots must be physical slot ids");
            slots.push_back(v.get<unsigned int>());
        }
        if (slots.size() < 2) throw BadRequest("slots needs at least 2 slots");
    } else {
        for (unsigned int k = 1; k <= colors.size(); ++k) slots.push_back(k);
    }
    std::vector<std::string> palette, palette_types;
    for (unsigned int k : slots) { palette.push_back(colors[k - 1]); palette_types.push_back(types[k - 1]); }

    MixedColorMatchRecipeResult recipe = build_best_color_match_recipe(palette, target, min_pct, max_pct, true, palette_types);
    if (!recipe.valid) throw std::runtime_error("Unable to create a color match from the current physical filament colors.");
    auto remap_id = [&](unsigned int& id) { if (id >= 1 && id <= slots.size()) id = slots[id - 1]; };
    remap_id(recipe.component_a);
    remap_id(recipe.component_b);
    if (!recipe.gradient_component_ids.empty()) {
        auto ids = MixedFilamentManager::decode_gradient_component_ids(recipe.gradient_component_ids);
        for (auto& id : ids) remap_id(id);
        recipe.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
    }
    const FsColour search_preview = recipe.preview_color;
    // sync_recipe_preview: the preview becomes the row's display colour; ΔE is re-measured against it.
    recipe.preview_color = compute_color_match_recipe_display_color(recipe, print_display_context(colors, settings));
    recipe.delta_e       = color_delta_e00(target, recipe.preview_color);

    std::vector<unsigned int> ids;
    std::vector<int>          weights;
    color_match_recipe_components(recipe, ids, weights);
    json out;
    out["target"]            = target.hex();
    out["components"]        = ids;
    out["weights"]           = weights;
    out["summary"]           = summarize_color_match_recipe(recipe);
    out["a"]                 = recipe.component_a;
    out["b"]                 = recipe.component_b;
    out["mix_b_percent"]     = recipe.mix_b_percent;
    out["gradient_ids"]      = recipe.gradient_component_ids;
    out["gradient_weights"]  = recipe.gradient_component_weights;
    out["distribution_mode"] = recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
    out["preview"]           = recipe.preview_color.hex();
    out["search_preview"]    = search_preview.hex();
    out["delta_e"]           = std::round(recipe.delta_e * 100.0) / 100.0;
    out["quality"]           = recipe.delta_e < kDeltaEGoodMax ? "Good" : (recipe.delta_e < kDeltaEFairMax ? "Fair" : "Poor");
    // What OK in the Match mode stores: pass "dialog" to add/update.
    json          dlg = match_dialog_request(recipe, colors.size(), min_pct);
    MixedFilament fresh;
    fresh.component_a = 1; fresh.component_b = 2; fresh.mix_b_percent = 50;
    DialogOutcome o   = dialog_collect(fresh, dlg, colors, types);
    out["dialog"]     = dlg;
    out["row"]        = row_fields(o.r);
    if (!o.warning.empty()) out["warning"] = o.warning;
    return out;
}

// The dialog's "Mixing Recommendations" swatches (MixedFilamentDialog::build_swatch_grid 2607-2886) per mode; "match"
// uses build_color_match_presets. Each entry carries the dialog request a click + OK produces.
static json op_presets(const json& req)
{
    const auto colors = read_physical(req);
    const auto types  = read_types(req);
    read_compatibility(req);
    const std::string mode    = req.value("mode", std::string("match"));
    const int         min_pct = std::clamp(read_int(req, "min_percent", 15), 0, 50);
    int               n       = (int) colors.size();
    std::vector<FsColour> palette;
    for (const auto& c : colors) palette.push_back(parse_mixed_color(c));

    struct Candidate {
        FsColour    color;
        std::string tooltip;
        int         rows[3] = {0, 0, 0};
        int         n_rows  = 2;
        int         b_pct   = 50;
        std::vector<int> w;  // triple weights (percent)
        std::vector<unsigned int> ids; // summary ids
        std::vector<int> wts;          // summary weights
    };
    std::vector<Candidate> candidates;
    char                   buf[128];

    if (mode == "match") {
        for (const auto& preset : build_color_match_presets(colors, min_pct, types)) {
            if (!preset.valid) continue;
            Candidate c;
            c.color   = preset.preview_color;
            c.tooltip = summarize_color_match_recipe(preset);
            color_match_recipe_components(preset, c.ids, c.wts);
            auto decoded = MixedFilamentManager::decode_gradient_component_ids(preset.gradient_component_ids);
            if (decoded.size() >= 2) {
                c.rows[0] = (int) decoded[0] - 1; c.rows[1] = (int) decoded[1] - 1;
                c.n_rows = 2; c.b_pct = preset.mix_b_percent;
                if (decoded.size() >= 3) {
                    c.rows[2] = (int) decoded[2] - 1; c.n_rows = 3;
                    c.w = decode_color_match_gradient_weights(preset.gradient_component_weights, 3);
                }
            } else {
                c.rows[0] = (int) preset.component_a - 1; c.rows[1] = (int) preset.component_b - 1;
                c.n_rows = 2; c.b_pct = preset.mix_b_percent;
            }
            candidates.push_back(c);
        }
    } else if (mode == "ratio" || mode == "ratio3") {
        if (mode == "ratio3") {
            n = std::min(n, 6);
            auto add_triple = [&](int i, int j, int k, const std::vector<int>& input_weights) {
                auto recipe = build_multi_color_match_candidate(palette, {unsigned(i + 1), unsigned(j + 1), unsigned(k + 1)}, input_weights, 0);
                if (!recipe.valid) return;
                Candidate c;
                c.color  = recipe.preview_color;
                c.n_rows = 3;
                c.rows[0] = i; c.rows[1] = j; c.rows[2] = k;
                c.w      = input_weights;
                std::snprintf(buf, sizeof buf, "F%d(%d%%)+F%d(%d%%)+F%d(%d%%)", i + 1, input_weights[0], j + 1, input_weights[1], k + 1, input_weights[2]);
                c.tooltip = buf;
                c.ids = {unsigned(i + 1), unsigned(j + 1), unsigned(k + 1)};
                c.wts = input_weights;
                candidates.push_back(c);
            };
            const std::vector<int> eq = normalize_color_match_weights({1, 1, 1}, 3);
            for (int i = 0; i < n; ++i)
                for (int j = i + 1; j < n; ++j)
                    for (int k = j + 1; k < n; ++k) {
                        add_triple(i, j, k, eq);
                        for (int dom = 0; dom < 3; ++dom) {
                            std::vector<int> dw = {25, 25, 25};
                            dw[dom] = 50;
                            add_triple(i, j, k, dw);
                        }
                    }
        } else {
            for (int i = 0; i < n; ++i)
                for (int j = i + 1; j < n; ++j) {
                    auto recipe = build_pair_color_match_candidate(palette, i + 1, j + 1, 50, 0);
                    if (!recipe.valid) continue;
                    Candidate c;
                    c.color = recipe.preview_color;
                    std::snprintf(buf, sizeof buf, "F%d(50%%) + F%d(50%%)", i + 1, j + 1);
                    c.tooltip = buf;
                    c.rows[0] = i; c.rows[1] = j;
                    c.ids = {unsigned(i + 1), unsigned(j + 1)};
                    c.wts = {50, 50};
                    candidates.push_back(c);
                }
        }
    } else if (mode == "gradient") {
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                const FsColour blended = parse_mixed_color(MixedFilamentManager::blend_color(colors[i], colors[j], 50, 50));
                for (int dir = 0; dir < 2; ++dir) {
                    Candidate c;
                    c.color = blended;
                    const int from = dir == 0 ? i : j, to = dir == 0 ? j : i;
                    std::snprintf(buf, sizeof buf, "F%d \xE2\x86\x92 F%d", from + 1, to + 1);
                    c.tooltip = buf;
                    c.rows[0] = from; c.rows[1] = to;
                    c.ids = {unsigned(from + 1), unsigned(to + 1)};
                    c.wts = {50, 50};
                    candidates.push_back(c);
                }
            }
    } else {
        throw BadRequest("presets \"mode\" must be match, ratio, ratio3 or gradient (Cycle mode shows no swatches)");
    }

    json presets = json::array();
    int  badge_idx = 0;
    for (const auto& cand : candidates) {
        std::vector<unsigned int> fids;
        for (int r = 0; r < cand.n_rows; ++r) fids.push_back(unsigned(cand.rows[r]));
        if (!is_filament_compatible(fids, types)) continue;
        int min_w = 100;
        if (cand.n_rows == 2) min_w = std::min(100 - cand.b_pct, cand.b_pct);
        else if (cand.n_rows == 3 && cand.w.size() == 3) min_w = static_cast<int>(std::min({cand.w[0] / 100.0, cand.w[1] / 100.0, cand.w[2] / 100.0}) * 100.0 + 0.5);
        else if (cand.n_rows == 3) min_w = static_cast<int>(1.0 / 3.0 * 100.0 + 0.5);
        json p;
        p["index"]      = ++badge_idx;
        p["components"] = cand.ids;
        p["weights"]    = cand.wts;
        p["preview"]    = cand.color.hex();
        p["tooltip"]    = cand.tooltip;
        // Match mode hides swatches whose smallest component is below Min Mix Ratio (rebuild_swatch_sizer).
        p["visible"]    = mode != "match" || min_w >= min_pct;
        json d;
        d["mode"] = mode == "ratio3" ? "ratio" : mode;
        json f    = json::array();
        for (int r = 0; r < cand.n_rows; ++r) f.push_back(cand.rows[r] + 1);
        d["filaments"] = f;
        if (mode == "match") {
            d["min_percent"] = min_pct;
            if (cand.n_rows == 3) {
                if (cand.w.size() == 3) d["weights"] = cand.w;
                else d["weights"] = {1, 1, 1}; // the tri state keeps its previous weights (default 1/3 each)
            } else {
                d["mix_b_percent"] = cand.b_pct;
            }
        } else if (mode == "ratio3") {
            d["weights"] = cand.w;
        } else if (mode == "ratio") {
            d["mix_b_percent"] = cand.b_pct;
        } else {
            d["direction"] = 0;
        }
        p["dialog"] = d;
        presets.push_back(std::move(p));
    }
    json out;
    out["mode"]    = mode;
    out["presets"] = std::move(presets);
    return out;
}

// FilamentColorLibrary reads filaments_colours.json from <data_dir>/system/Snapmaker/filament/ or
// <resources_dir>/profiles/Snapmaker/filament/. The request names the file directly, so it is staged into a private
// resources tree for the library to find.
struct StagedLibrary {
    std::filesystem::path root;
    ~StagedLibrary() { std::error_code ec; if (!root.empty()) std::filesystem::remove_all(root, ec); }
};

static bool load_colour_library(const std::string& path, StagedLibrary& staged)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw BadRequest("Cannot read colour_library " + path);
    std::string tmpl = (std::filesystem::temp_directory_path() / "nozzle-fs-XXXXXX").string();
    if (mkdtemp(tmpl.data()) == nullptr) throw std::runtime_error("Cannot create a temporary directory");
    staged.root = tmpl;
    const auto dir = staged.root / "profiles" / "Snapmaker" / "filament";
    std::filesystem::create_directories(dir);
    std::filesystem::copy_file(path, dir / "filaments_colours.json", std::filesystem::copy_options::overwrite_existing);
    set_resources_dir(staged.root.string());
    set_data_dir((staged.root / "no-data-dir").string());
    return FilamentColorLibrary::Instance().EnsureLoaded();
}

static const char* quality(double de) { return de < kDeltaEGoodMax ? "Good" : (de < kDeltaEFairMax ? "Fair" : "Poor"); }

static json op_match(const json& req)
{
    const auto          m_physical_colors = read_physical(req);
    const PanelSettings settings          = read_settings(req);
    const std::string   definitions       = read_definitions(req);
    const std::string   mode              = req.value("mode", std::string("auto"));
    if (mode != "auto" && mode != "manual") throw BadRequest("\"mode\" must be \"auto\" or \"manual\"");
    const MatchingMethod method = mode == "auto" ? RECOMMENDED : MANUAL;

    // Targets, de-duplicated by normalized hex as load_palette_colors does (ids accumulate per colour).
    if (!req.contains("targets") || !req["targets"].is_array()) throw BadRequest("\"targets\" must be an array");
    std::vector<std::pair<std::string, std::vector<unsigned int>>> hex_to_eids;
    for (const json& t : req["targets"]) {
        if (!t.is_object() || !t.contains("color")) throw BadRequest("each target needs a \"color\"");
        const std::string hex_norm = normalize_color_match_hex(require_colour(t["color"], "target colour"));
        std::vector<unsigned int> ids;
        if (t.contains("ids")) {
            if (!t["ids"].is_array()) throw BadRequest("target \"ids\" must be an array");
            for (const json& id : t["ids"]) {
                if (!id.is_number_integer() || id.get<long long>() < 1) throw BadRequest("target ids must be positive integers");
                ids.push_back(id.get<unsigned int>());
            }
        }
        auto it = std::find_if(hex_to_eids.begin(), hex_to_eids.end(), [&](const auto& p) { return p.first == hex_norm; });
        if (it != hex_to_eids.end()) it->second.insert(it->second.end(), ids.begin(), ids.end());
        else hex_to_eids.push_back({hex_norm, ids});
    }
    std::vector<ModelColorEntry> model_colors;
    for (const auto& [hex, eids] : hex_to_eids) {
        FsColour c;
        try_parse_color_match_hex(hex, c);
        model_colors.push_back({static_cast<unsigned int>(model_colors.size() + 1), c, hex, eids});
    }
    while (model_colors.size() > kMaxColors) model_colors.pop_back();
    if (model_colors.empty()) throw BadRequest("No model colours to match");

    // Manual combos: the chosen physical slots (1-based in the request); default = the dialog's default, the first
    // min(4, N) slots.
    std::vector<int> manual_selections;
    if (method == MANUAL) {
        if (req.contains("manual_slots")) {
            if (!req["manual_slots"].is_array()) throw BadRequest("\"manual_slots\" must be an array");
            for (const json& s : req["manual_slots"]) {
                if (!s.is_number_integer()) throw BadRequest("manual_slots must be integers");
                manual_selections.push_back(s.get<int>() - 1);
            }
            if (manual_selections.size() < 2 || manual_selections.size() > 4) throw BadRequest("manual_slots takes 2 to 4 slots");
        } else {
            const size_t n = m_physical_colors.size();
            const int    count = n > 4 ? 4 : (n >= 2 ? static_cast<int>(n) : 2);
            for (int i = 0; i < count; ++i) manual_selections.push_back(i);
        }
    }

    // Recommended palette + the dialog's default dropdown selections.
    StagedLibrary                         staged;
    bool                                  library_loaded = false;
    bool                                  palette_fallback = false;
    std::vector<FullSpectrumPaletteEntry> recommended_palette;
    std::vector<int>                      recommended_selections;
    if (method == RECOMMENDED) {
        if (req.contains("colour_library") && req["colour_library"].is_string())
            library_loaded = load_colour_library(req["colour_library"].get<std::string>(), staged);
        recommended_palette = load_recommended_palette(library_loaded, palette_fallback);
        const auto defaults = DefaultFullSpectrumSelections(recommended_palette, default_full_spectrum_family_name());
        for (int i = 0; i < kFullSpectrumSlotCount; ++i)
            recommended_selections.push_back(i < static_cast<int>(defaults.size()) ? defaults[i] : -1);
    }

    const int min_override = req.contains("min_percent") ? read_int(req, "min_percent", 0) : -1;
    const int max_override = req.contains("max_percent") ? read_int(req, "max_percent", 0) : -1;
    if (min_override != -1 && (min_override < 0 || min_override > 50)) throw BadRequest("min_percent must be in [0, 50]");
    if (max_override != -1 && (max_override < 50 || max_override > 100)) throw BadRequest("max_percent must be in [50, 100]");

    // The project's mixed list at dialog time.
    MixedFilamentManager mgr;
    load_manager(mgr, definitions, m_physical_colors);
    const size_t existing_mixed_count = mgr.enabled_count();

    BatchMatchResult result = run_batch_match(method, model_colors, m_physical_colors, manual_selections, recommended_palette,
                                              recommended_selections, existing_mixed_count, min_override, max_override);
    if (!result.success) throw std::runtime_error(result.error_message.empty() ? "Colour matching failed" : result.error_message);

    // ---- Plater.cpp apply path ----
    std::unordered_map<unsigned int, uint64_t> dialog_vid_to_sid;
    const size_t dialog_num_physical = m_physical_colors.size();
    {
        unsigned int vid = static_cast<unsigned int>(dialog_num_physical) + 1;
        for (const MixedFilament& mf : mgr.mixed_filaments()) {
            if (!mf.enabled || mf.deleted) continue;
            dialog_vid_to_sid[vid++] = mf.stable_id;
        }
    }

    std::vector<std::string>  colors_vec;
    std::vector<unsigned int> match_remap;
    if (result.is_recommended_mode && result.recommended_physical_colors.size() >= 4) {
        const auto&  cm            = result.recommended_physical_colors;
        const size_t current_count = m_physical_colors.size();
        const size_t target_count  = std::max<size_t>(4, current_count);
        colors_vec                 = m_physical_colors;
        colors_vec.resize(target_count);
        for (size_t i = 0; i < 4 && i < cm.size(); ++i)
            colors_vec[i] = cm[i];
        // set_num_filaments + the "Restore custom entries" reload, against the new palette.
        const std::vector<MixedFilament> old_mixed_snapshot = mgr.mixed_filaments();
        const std::string saved = mgr.serialize_custom_entries();
        MixedFilamentManager fresh;
        load_manager(fresh, saved, colors_vec);
        mgr = std::move(fresh);
        // update_mixed_filament_id_remap(old_mixed_snapshot, current_count, target_count) (Plater.cpp ~2745).
        if (current_count != target_count || old_mixed_snapshot != mgr.mixed_filaments())
            match_remap = build_filament_id_remap(old_mixed_snapshot, mgr.mixed_filaments(), current_count, target_count, size_t(-1));
    } else {
        colors_vec = m_physical_colors;
    }

    BatchMatchResult model_result = result;
    {
        const size_t cur_num_physical = colors_vec.size();
        bool         any_in_place     = false;
        for (auto& mapping : model_result.mappings) {
            if (mapping.is_pure_recipe || mapping.in_place_edited) continue;
            const MixedColorMatchRecipeResult& r = mapping.recipe;
            if (!r.valid) continue;
            if (r.component_a < 1 || r.component_a > cur_num_physical || r.component_b < 1 || r.component_b > cur_num_physical ||
                r.component_a == r.component_b)
                continue;
            uint64_t sid = 0;
            for (unsigned int src : mapping.source_extruder_ids) {
                auto it = dialog_vid_to_sid.find(src);
                if (it != dialog_vid_to_sid.end() && it->second != 0) { sid = it->second; break; }
            }
            if (sid == 0) continue;
            MixedFilament* target     = nullptr;
            unsigned int   target_vid = 0;
            unsigned int   vid        = static_cast<unsigned int>(cur_num_physical) + 1;
            for (MixedFilament& mf : mgr.mixed_filaments()) {
                if (!mf.enabled || mf.deleted) continue;
                if (mf.stable_id == sid) { target = &mf; target_vid = vid; break; }
                ++vid;
            }
            if (target == nullptr || !target->custom) continue;
            target->component_a                = r.component_a;
            target->component_b                = r.component_b;
            target->mix_b_percent              = r.mix_b_percent;
            target->gradient_component_ids     = r.gradient_component_ids;
            target->gradient_component_weights = r.gradient_component_weights;
            target->distribution_mode = r.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
            target->gradient_enabled  = !r.gradient_component_ids.empty();
            target->manual_pattern.clear();
            target->ratio_a            = 1;
            target->ratio_b            = 1;
            target->ui_mode            = 2;
            mapping.in_place_edited    = true;
            mapping.target_filament_id = target_vid;
            any_in_place               = true;
        }
        if (any_in_place) mgr.refresh_display_colors(colors_vec);
    }

    // Translate mixed-slot source ids from the dialog epoch to the current epoch.
    std::vector<std::vector<unsigned int>> source_ids_before;
    for (auto& mapping : model_result.mappings) source_ids_before.push_back(mapping.source_extruder_ids);
    {
        const unsigned int cur_num_physical = static_cast<unsigned int>(colors_vec.size());
        for (auto& mapping : model_result.mappings) {
            std::vector<unsigned int> translated;
            for (unsigned int src : mapping.source_extruder_ids) {
                if (src <= static_cast<unsigned int>(dialog_num_physical)) { translated.push_back(src); continue; }
                auto it = dialog_vid_to_sid.find(src);
                if (it == dialog_vid_to_sid.end() || it->second == 0) continue;
                unsigned int cur_vid = 0, vid = cur_num_physical + 1;
                for (const MixedFilament& mf : mgr.mixed_filaments()) {
                    if (!mf.enabled || mf.deleted) continue;
                    if (mf.stable_id == it->second) { cur_vid = vid; break; }
                    ++vid;
                }
                if (cur_vid != 0) translated.push_back(cur_vid);
            }
            mapping.source_extruder_ids = std::move(translated);
        }
    }

    std::vector<MixedFilamentBatchEntry> batch_entries;
    for (const auto& mapping : model_result.mappings) {
        if (mapping.is_pure_recipe || mapping.in_place_edited) continue;
        MixedFilamentBatchEntry entry;
        entry.component_a                = mapping.recipe.component_a;
        entry.component_b                = mapping.recipe.component_b;
        entry.mix_b_percent              = mapping.recipe.mix_b_percent;
        entry.manual_pattern             = mapping.recipe.manual_pattern;
        entry.gradient_component_ids     = mapping.recipe.gradient_component_ids;
        entry.gradient_component_weights = mapping.recipe.gradient_component_weights;
        entry.distribution_mode = mapping.recipe.gradient_component_ids.empty() ? int(MixedFilament::Simple) : int(MixedFilament::LayerCycle);
        entry.display_color     = mapping.matched_color.hex();
        batch_entries.push_back(std::move(entry));
    }
    std::vector<unsigned int> assigned_ids;
    mgr.add_batch_custom_filaments(batch_entries, colors_vec, &assigned_ids);
    {
        size_t k = 0;
        for (auto& mapping : model_result.mappings) {
            if (mapping.is_pure_recipe || mapping.in_place_edited) continue;
            const unsigned int vid = (k < assigned_ids.size()) ? assigned_ids[k] : 0u;
            ++k;
            if (vid != 0u) {
                mapping.target_filament_id = vid;
            } else {
                mapping.target_filament_id = 0;
                mapping.source_extruder_ids.clear();
            }
        }
    }

    // ---- response ----
    const MixedFilamentDisplayContext ctx = panel_display_context(colors_vec, settings);
    mgr.set_display_context(ctx);
    json out;
    out["mode"] = mode;
    json palette = json::array();
    if (method == RECOMMENDED) {
        out["palette_source"] = palette_fallback ? "fallback" : "library";
        for (size_t i = 0; i < result.recommended_physical_colors.size(); ++i) {
            json p;
            p["slot"]   = i + 1;
            p["color"]  = result.recommended_physical_colors[i];
            p["family"] = i < result.recommended_physical_family_names.size() ? result.recommended_physical_family_names[i] : std::string();
            palette.push_back(std::move(p));
        }
    } else {
        for (unsigned int id : result.selected_physical_ids) {
            json p;
            p["slot"]  = id;
            p["color"] = m_physical_colors[id - 1];
            palette.push_back(std::move(p));
        }
    }
    out["palette"]        = std::move(palette);
    out["physical_after"] = colors_vec;

    json results = json::array();
    for (size_t mi = 0; mi < model_result.mappings.size(); ++mi) {
        const ColorMappingEntry& m = model_result.mappings[mi];
        json                     r;
        const ModelColorEntry&   src = model_colors[m.model_color_index - 1];
        r["target"] = src.hex_value;
        if (m.merged_model_indices.size() > 1) {
            json merged = json::array();
            for (unsigned int idx : m.merged_model_indices) merged.push_back(model_colors[idx - 1].hex_value);
            r["merged_targets"] = std::move(merged);
        }
        r["source_ids"] = source_ids_before[mi];
        if (m.source_extruder_ids != source_ids_before[mi]) r["source_ids_after"] = m.source_extruder_ids;
        r["pure"] = m.is_pure_recipe;
        if (m.is_pure_recipe) {
            r["slot"] = m.target_filament_id;
        } else {
            r["id"] = m.target_filament_id; // 0 when the filament list was full and the row was dropped
            std::vector<unsigned int> ids;
            std::vector<int>          weights;
            color_match_recipe_components(m.recipe, ids, weights);
            r["components"] = ids;
            r["weights"]    = weights;
            r["summary"]    = summarize_color_match_recipe(m.recipe);
            if (m.in_place_edited) r["in_place"] = true;
            if (const MixedFilament* mf = mgr.mixed_filament_from_id(m.target_filament_id, colors_vec.size()))
                r["display"] = mf->display_color;
        }
        r["preview"] = m.matched_color.hex();
        r["delta_e"] = std::round(m.delta_e * 100.0) / 100.0;
        r["quality"] = quality(m.delta_e);
        results.push_back(std::move(r));
    }
    out["results"]     = std::move(results);
    out["avg_delta_e"] = std::round(result.avg_delta_e * 100.0) / 100.0;

    // Snapmaker then runs Sidebar::cleanup_unused_filaments_after_batch_match, which deletes the physical and mixed
    // filaments the model no longer uses. That needs the model; this reports what compute_redundant_filaments would
    // remove given the ids the caller says are still used ("keep_ids", post-remap), and leaves the list untouched.
    {
        const size_t              num_physical = colors_vec.size();
        std::vector<unsigned int> kept_physical, kept_mixed;
        for (unsigned int id : result.selected_physical_ids) if (id >= 1 && id <= num_physical) kept_physical.push_back(id);
        for (const auto& m : model_result.mappings) if (!m.is_pure_recipe && m.target_filament_id > num_physical) kept_mixed.push_back(m.target_filament_id);
        if (req.contains("keep_ids") && req["keep_ids"].is_array())
            for (const json& id : req["keep_ids"])
                if (id.is_number_integer() && id.get<long long>() > (long long) num_physical) kept_mixed.push_back(id.get<unsigned int>());
        const RedundantFilamentSet red = compute_redundant_filaments(num_physical, kept_physical, kept_mixed, mgr.mixed_filaments());
        json c;
        c["physical"] = red.redundant_physical;
        c["mixed"]    = red.redundant_mixed;
        out["redundant"] = std::move(c);
    }

    out["remap"]       = remap_json(match_remap);
    out["definitions"] = mgr.serialize_custom_entries();
    out["rows"]        = panel_rows(mgr, colors_vec, settings);
    return out;
}

int run_full_spectrum(const std::string& request_text, std::string& response)
{
    try {
        json req;
        try {
            req = json::parse(request_text);
        } catch (const std::exception& e) {
            throw BadRequest(std::string("The request is not valid JSON: ") + e.what());
        }
        if (!req.is_object() || !req.contains("op") || !req["op"].is_string()) throw BadRequest("The request needs an \"op\"");
        const std::string op = req["op"].get<std::string>();
        json              out;
        if (op == "display") out = op_display(req);
        else if (op == "add") out = op_add(req);
        else if (op == "remove") out = op_remove(req);
        else if (op == "update") out = op_update(req);
        else if (op == "match") out = op_match(req);
        else if (op == "match_one") out = op_match_one(req);
        else if (op == "presets") out = op_presets(req);
        else if (op == "cleanup") out = op_cleanup(req);
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

} // namespace nozzle_fs
