// PrusaSlicer 2.9.6 colour mixing (virtual extruders) for nozzle-engine: `nozzle-engine --color-mix <request.json>`.
// Copyright (c) Prusa Research s.r.o. and the PrusaSlicer contributors; licensed under the GNU Affero General Public
// License v3.0 or later, as PrusaSlicer and this file are.
//
// The mixing engine itself is libslic3r's port of PrusaSlicer 2.9.6 (engine/snapmaker/nozzle-engine.patch:
// src/libslic3r/Feature/FullSpectrum/VirtualExtruder.{hpp,cpp} and deps_src/prusa_fdm_mixer), called unchanged here:
//   deserialize_virtual_extruders_from_json, normalize_virtual_extruders, filter_virtual_extruders_for_physical_count,
//   VirtualExtruder::effective_color / build_sequence, build_canonical_cycle, build_gradient_bands (the band loop of
//   resolve_gradient_with_ranges), serialize_virtual_extruders_to_json, remap_full_spectrum_on_import,
//   prusa_fdm_mixer::mix.
// Ported here from PrusaSlicer 2.9.6 src/slic3r/GUI/FullSpectrumDialog.cpp (GUI code; the wx parts dropped):
//   next_free_virtual_id (1759-1768) and on_add_blend_clicked (1535-1546) for "next_id";
//   the preset_already_used check of rebuild_preset_grid (2154-2199) for the presets' "added" flag;
//   the physical colour fallback of the dialog constructor (121-130: an empty colour is "#808080").
// Ported here from PrusaSlicer 3.0.0-alpha12 (commit 30ef591) src/slic3r-biz-algorithms/src/Slic3r/Biz/Algorithms/
// VirtualExtruderPresets.cpp (all of it: build_blend_presets, preset_color and their helpers) and
// VirtualExtruder.cpp balanced_ratios_percent (498-509): PrusaSlicer 2.9.6 builds its preset palette inside
// FullSpectrumDialog (build_two_color_presets, build_three_color_presets, rebuild_preset_grid); 3.0 moved the same
// palette into this GUI-free file and adds the balanced 33/33/34 triple. The only changes are for this engine's C++17:
// std::ranges::stable_sort / any_of become std::stable_sort / std::any_of, std::numbers::pi becomes a constant.
#include "color_mix.hpp"

#include <libslic3r/libslic3r.h>
#include <libslic3r/Model.hpp>
#include <libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp>

#include <prusa_fdm_mixer.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace nozzle_cm {

using namespace Slic3r;
using namespace Slic3r::FullSpectrum;
using json = nlohmann::ordered_json;

namespace {

struct BadRequest : std::runtime_error { using std::runtime_error::runtime_error; };

// The Metadata/Prusa_Slicer_full_spectrum.json version PrusaSlicer 2.9.6 writes and reads (FULL_SPECTRUM_CONFIG_VERSION).
constexpr int k_full_spectrum_config_version = 1;
// FullSpectrumDialog's fallback for a slot without a colour.
constexpr const char* k_fallback_color = "#808080";

struct Physical {
    std::vector<std::string> colors;
    std::vector<std::string> types;
    std::vector<bool>        enabled;
};

// "physical": [{"color": "#rrggbb", "type": "PLA", "enabled": true}, ...], 0-based slot order (extruder id = index + 1).
Physical parse_physical(const json& req, bool required = true)
{
    Physical p;
    if (!req.contains("physical")) {
        if (required) throw BadRequest("The request needs \"physical\"");
        return p;
    }
    const json& arr = req["physical"];
    if (!arr.is_array()) throw BadRequest("\"physical\" must be an array");
    for (const json& e : arr) {
        if (!e.is_object()) throw BadRequest("Each \"physical\" entry must be an object");
        std::string color = e.contains("color") && e["color"].is_string() ? e["color"].get<std::string>() : std::string();
        if (color.empty()) color = k_fallback_color;
        p.colors.push_back(color);
        p.types.push_back(e.contains("type") && e["type"].is_string() ? e["type"].get<std::string>() : std::string());
        p.enabled.push_back(!e.contains("enabled") || !e["enabled"].is_boolean() || e["enabled"].get<bool>());
    }
    return p;
}

const json& require_array(const json& req, const char* key)
{
    if (!req.contains(key) || !req[key].is_array()) throw BadRequest(std::string("The request needs a \"") + key + "\" array");
    return req[key];
}

unsigned int require_count(const json& req, const char* key)
{
    if (!req.contains(key) || !req[key].is_number_integer() || req[key].get<long long>() < 0 || req[key].get<long long>() > 255)
        throw BadRequest(std::string("The request needs \"") + key + "\" (0..255)");
    return static_cast<unsigned int>(req[key].get<long long>());
}

// Reads virtual extruder entries in the sidecar format with PrusaSlicer's own reader, by wrapping them in a sidecar.
FullSpectrumConfig read_virtual(const json& virtual_entries, const std::vector<std::string>& physical_colors)
{
    json root;
    root["version"] = k_full_spectrum_config_version;
    json phys = json::array();
    for (size_t i = 0; i < physical_colors.size(); ++i) phys.push_back(json{{"id", int(i + 1)}, {"color", physical_colors[i]}});
    root["physical_extruders"] = phys;
    root["virtual_extruders"] = virtual_entries;
    return deserialize_virtual_extruders_from_json(root.dump());
}

json cycle_json(const std::vector<unsigned int>& cycle)
{
    json a = json::array();
    for (unsigned int e : cycle) a.push_back(e);
    return a;
}

// ---------------------------------------------------------------------------------------------------------------------
// normalize
// ---------------------------------------------------------------------------------------------------------------------

// Layers of a uniform-height print (print_z = k * layer_height, k >= 1) inside [z_min, z_max]: the layers
// resolve_gradient_with_ranges() would count for that range.
size_t layers_in_range(double z_min, double z_max, double layer_height)
{
    const long first = std::max(1L, long(std::ceil(z_min / layer_height - 1e-9)));
    const long last  = long(std::floor(z_max / layer_height + 1e-9));
    return last >= first ? size_t(last - first + 1) : 0;
}

json op_normalize(const json& req)
{
    const Physical physical = parse_physical(req);
    const json&    raw      = require_array(req, "virtual");
    const unsigned int num_physical = static_cast<unsigned int>(physical.colors.size());

    const FullSpectrumConfig parsed     = read_virtual(raw, physical.colors);
    const VirtualExtruders   normalized = normalize_virtual_extruders(parsed.virtual_extruders);
    const VirtualExtruders   compatible = filter_virtual_extruders_for_physical_count(num_physical, normalized);

    double layer_height = 0.2;
    if (req.contains("layer_height")) {
        if (!req["layer_height"].is_number() || !(req["layer_height"].get<double>() > 0.)) throw BadRequest("\"layer_height\" must be a positive number");
        layer_height = req["layer_height"].get<double>();
    }
    std::optional<size_t> range_layers_override;
    if (req.contains("range_layers")) {
        if (!req["range_layers"].is_number_integer() || req["range_layers"].get<long long>() < 1) throw BadRequest("\"range_layers\" must be a positive integer");
        range_layers_override = size_t(req["range_layers"].get<long long>());
    }

    // The sidecar entries, exactly as PrusaSlicer writes them, extended with what the engine derives.
    const json sidecar = json::parse(serialize_virtual_extruders_to_json(physical.colors, compatible));
    json out_virtual = json::array();
    for (size_t i = 0; i < compatible.size(); ++i) {
        const VirtualExtruder& ve = compatible[i];
        json entry = sidecar["virtual_extruders"][i];
        entry["effective_color"] = ve.effective_color(physical.colors);
        if (ve.type() == VirtualExtruder::Type::Blend) {
            entry["color_override"] = ve.color.has_value();
            entry["cycle"] = cycle_json(ve.build_sequence());
        } else {
            const VirtualExtruderGradient& g = *ve.gradient;
            std::optional<size_t> range_layers = range_layers_override;
            if (!range_layers && g.z_min && g.z_max) range_layers = layers_in_range(*g.z_min, *g.z_max, layer_height);
            entry["auto_range"] = !(g.z_min && g.z_max);
            if (range_layers) {
                const std::vector<GradientBand> bands = build_gradient_bands(g.stops, *range_layers);
                entry["range_layers"] = *range_layers;
                json jb = json::array();
                for (size_t b = 0; b < bands.size(); ++b) {
                    json band;
                    band["band"] = b;
                    const double t0 = double(b) / double(bands.size()), t1 = double(b + 1) / double(bands.size());
                    band["t_min"] = t0;
                    band["t_max"] = t1;
                    if (g.z_min && g.z_max) {
                        band["z_min"] = *g.z_min + t0 * (*g.z_max - *g.z_min);
                        band["z_max"] = *g.z_min + t1 * (*g.z_max - *g.z_min);
                    }
                    json w = json::array();
                    for (size_t s = 0; s < g.stops.size(); ++s) w.push_back(json{{"extruder", g.stops[s].extruder_id}, {"weight", bands[b].weights[s]}});
                    band["weights"] = w;
                    band["cycle"] = cycle_json(bands[b].cycle);
                    jb.push_back(band);
                }
                entry["bands"] = jb;
            } else {
                entry["bands"] = nullptr;
            }
        }
        out_virtual.push_back(entry);
    }

    // Entries the request listed that did not survive (unreadable, invalid, or needing a missing physical extruder).
    std::set<unsigned int> kept;
    for (const VirtualExtruder& ve : compatible) kept.insert(ve.id);
    json dropped = json::array();
    for (const json& e : raw) {
        const int id = e.is_object() ? e.value("id", -1) : -1;
        if (id > 0 && kept.count(unsigned(id)) == 0) dropped.push_back(id);
    }

    json out;
    out["physical_count"] = num_physical;
    out["virtual"] = out_virtual;
    out["dropped"] = dropped;
    out["sidecar"] = sidecar;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// mix
// ---------------------------------------------------------------------------------------------------------------------

json op_mix(const json& req)
{
    const json& colors = require_array(req, "colors");
    const json& ratios = require_array(req, "ratios");
    if (colors.empty() || colors.size() != ratios.size()) throw BadRequest("\"colors\" and \"ratios\" must be non-empty and the same length");
    std::vector<prusa_fdm_mixer::Part> parts;
    for (size_t i = 0; i < colors.size(); ++i) {
        if (!colors[i].is_string() || !ratios[i].is_number()) throw BadRequest("Each colour must be a string and each ratio a number");
        parts.push_back({colors[i].get<std::string>(), ratios[i].get<double>()});
    }
    std::string mixed;
    try {
        mixed = prusa_fdm_mixer::mix(parts);
    } catch (const std::invalid_argument& e) {
        throw BadRequest(e.what());
    }
    return json{{"color", mixed}};
}

// ---------------------------------------------------------------------------------------------------------------------
// presets: PrusaSlicer 3.0.0-alpha12 VirtualExtruderPresets.cpp (C++17 spelling)
// ---------------------------------------------------------------------------------------------------------------------

namespace VirtualExtruderPresets {

struct PhysicalExtruderSlot
{
    std::string hex_color;
    std::string filament_type;
    bool enabled{true};
};

using PhysicalExtruderSlots = std::vector<PhysicalExtruderSlot>;

struct BlendPreset
{
    std::vector<unsigned int> extruder_ids_1based;
    std::vector<int> ratios_percent;
};

using BlendPresets = std::vector<BlendPreset>;

struct BlendPresetGroups
{
    BlendPresets two_color;
    BlendPresets three_color;
};

// PrusaSlicer 3.0.0-alpha12 VirtualExtruder.cpp balanced_ratios_percent (498-509).
std::vector<int> balanced_ratios_percent(const size_t count)
{
    if (count == 0) {
        return {};
    }

    const int even_percent = static_cast<int>(100 / count);
    std::vector<int> ratios_percent(count, even_percent);
    ratios_percent.back() += 100 - even_percent * static_cast<int>(count);

    return ratios_percent;
}

constexpr const char* FALLBACK_COLOR          = "#808080";
constexpr double PRESET_DEDUPE_DELTA_E        = 5.0;
constexpr std::size_t MAX_THREE_COLOR_PRESETS = 200;
constexpr double HUE_BUCKET_DEGREES           = 20.0;
constexpr double ACHROMATIC_CHROMA            = 5.0;
constexpr double PI                           = 3.14159265358979323846; // std::numbers::pi

bool is_slot_enabled(const PhysicalExtruderSlots& slots, unsigned int extruder_id_1based)
{
    return extruder_id_1based >= 1
        && extruder_id_1based <= slots.size()
        && slots[extruder_id_1based - 1].enabled;
}

bool have_same_filament_type(
    const PhysicalExtruderSlots& slots,
    const std::vector<unsigned int>& extruder_ids_1based
)
{
    const std::string& first_type = slots[extruder_ids_1based.front() - 1].filament_type;
    if (first_type.empty()) {
        return false;
    }

    for (const unsigned int extruder_id_1based : extruder_ids_1based) {
        if (slots[extruder_id_1based - 1].filament_type != first_type) {
            return false;
        }
    }

    return true;
}

BlendPresets build_two_color_presets(const PhysicalExtruderSlots& slots)
{
    const unsigned int slot_count = static_cast<unsigned int>(slots.size());

    BlendPresets presets;
    for (unsigned int first = 1; first <= slot_count; ++first) {
        if (!is_slot_enabled(slots, first)) {
            continue;
        }

        for (unsigned int second = first + 1; second <= slot_count; ++second) {
            if (!is_slot_enabled(slots, second)) {
                continue;
            }

            if (!have_same_filament_type(slots, {first, second})) {
                continue;
            }

            presets.push_back({{first, second}, {50, 50}});
            presets.push_back({{first, second}, {75, 25}});
            presets.push_back({{first, second}, {25, 75}});
        }
    }

    return presets;
}

BlendPresets build_three_color_presets(const PhysicalExtruderSlots& slots)
{
    const unsigned int slot_count = static_cast<unsigned int>(slots.size());

    BlendPresets presets;
    for (unsigned int first = 1; first <= slot_count; ++first) {
        if (!is_slot_enabled(slots, first)) {
            continue;
        }

        for (unsigned int second = first + 1; second <= slot_count; ++second) {
            if (!is_slot_enabled(slots, second)) {
                continue;
            }

            for (unsigned int third = second + 1; third <= slot_count; ++third) {
                if (!is_slot_enabled(slots, third)) {
                    continue;
                }

                if (!have_same_filament_type(slots, {first, second, third})) {
                    continue;
                }

                presets.push_back(
                    {{first, second, third}, balanced_ratios_percent(3)}
                );

                for (std::size_t dominant = 0; dominant < 3; ++dominant) {
                    BlendPreset preset;
                    preset.extruder_ids_1based      = {first, second, third};
                    preset.ratios_percent           = {25, 25, 25};
                    preset.ratios_percent[dominant] = 50;
                    presets.push_back(std::move(preset));
                }

                if (presets.size() >= MAX_THREE_COLOR_PRESETS) {
                    return presets;
                }
            }
        }
    }

    return presets;
}

std::string preset_color(const BlendPreset& preset, const PhysicalExtruderSlots& slots)
{
    std::vector<prusa_fdm_mixer::Part> parts;
    parts.reserve(preset.extruder_ids_1based.size());
    for (std::size_t i = 0; i < preset.extruder_ids_1based.size(); ++i) {
        const unsigned int extruder_id_1based = preset.extruder_ids_1based[i];
        if (extruder_id_1based >= 1 && extruder_id_1based <= slots.size()) {
            parts.push_back(
                {slots[extruder_id_1based - 1].hex_color,
                 static_cast<double>(preset.ratios_percent[i]) / 100.0}
            );
        }
    }

    if (parts.empty()) {
        return FALLBACK_COLOR;
    }

    try {
        return prusa_fdm_mixer::mix(parts);
    } catch (...) {
        return FALLBACK_COLOR;
    }
}

std::optional<prusa_fdm_mixer::LAB>
preset_lab(const BlendPreset& preset, const PhysicalExtruderSlots& slots)
{
    try {
        return prusa_fdm_mixer::rgb_to_lab(
            prusa_fdm_mixer::hex_to_rgb(preset_color(preset, slots))
        );
    } catch (...) {
        return std::nullopt;
    }
}

std::tuple<int, int, double> preset_sort_key(const prusa_fdm_mixer::LAB& lab)
{
    const double chroma = std::sqrt(lab.a * lab.a + lab.b * lab.b);
    if (chroma < ACHROMATIC_CHROMA) {
        return {1, 0, lab.L};
    }

    double hue_degrees = std::atan2(lab.b, lab.a) * 180.0 / PI;
    if (hue_degrees < 0.0) {
        hue_degrees += 360.0;
    }

    return {0, static_cast<int>(hue_degrees / HUE_BUCKET_DEGREES), lab.L};
}

struct AcceptedPreset
{
    BlendPreset preset;
    prusa_fdm_mixer::LAB lab;
};

BlendPresets sorted_presets(std::vector<AcceptedPreset> accepted_presets)
{
    std::vector<std::pair<std::tuple<int, int, double>, BlendPreset>> decorated_presets;
    decorated_presets.reserve(accepted_presets.size());
    for (AcceptedPreset& accepted_preset : accepted_presets) {
        decorated_presets
            .emplace_back(preset_sort_key(accepted_preset.lab), std::move(accepted_preset.preset));
    }

    std::stable_sort(
        decorated_presets.begin(),
        decorated_presets.end(),
        [](const auto& left, const auto& right) { return left.first < right.first; }
    );

    BlendPresets presets;
    presets.reserve(decorated_presets.size());
    for (auto& decorated_preset : decorated_presets) {
        presets.push_back(std::move(decorated_preset.second));
    }

    return presets;
}

BlendPresetGroups build_blend_presets(const PhysicalExtruderSlots& slots)
{
    BlendPresets two_color_presets   = build_two_color_presets(slots);
    BlendPresets three_color_presets = build_three_color_presets(slots);

    // Deduplicate against everything accepted so far, two-color recipes first.
    // When a pair and a triple predict the same color, the simpler recipe is the one worth offering.
    std::vector<AcceptedPreset> accepted_two_color;
    std::vector<AcceptedPreset> accepted_three_color;
    std::vector<prusa_fdm_mixer::LAB> accepted_labs;
    accepted_labs.reserve(two_color_presets.size() + three_color_presets.size());

    const auto accept_distinct_presets =
        [&](BlendPresets& presets, std::vector<AcceptedPreset>& accepted_presets)
    {
        for (BlendPreset& preset : presets) {
            const std::optional<prusa_fdm_mixer::LAB> lab = preset_lab(preset, slots);
            if (!lab.has_value()) {
                continue;
            }

            const bool too_close = std::any_of(
                accepted_labs.begin(),
                accepted_labs.end(),
                [&lab](const prusa_fdm_mixer::LAB& accepted_lab)
                {
                    return prusa_fdm_mixer::delta_e_2000(*lab, accepted_lab)
                        < PRESET_DEDUPE_DELTA_E;
                }
            );
            if (too_close) {
                continue;
            }

            accepted_labs.push_back(*lab);
            accepted_presets.push_back({std::move(preset), *lab});
        }
    };

    accept_distinct_presets(two_color_presets, accepted_two_color);
    accept_distinct_presets(three_color_presets, accepted_three_color);

    return {
        sorted_presets(std::move(accepted_two_color)),
        sorted_presets(std::move(accepted_three_color))
    };
}

} // namespace VirtualExtruderPresets

// PrusaSlicer 2.9.6 FullSpectrumDialog::rebuild_preset_grid's preset_already_used (2154-2199): a blend in the working
// list with the same extruders and ratios (within 0.02) as the preset.
bool preset_already_used(const VirtualExtruderPresets::BlendPreset& preset, const VirtualExtruders& working_list)
{
    for (const VirtualExtruder& ve : working_list) {
        if (ve.type() != VirtualExtruder::Type::Blend) {
            continue;
        }

        if (ve.components.size() != preset.extruder_ids_1based.size()) {
            continue;
        }

        struct IdRatio
        {
            unsigned int id;
            double ratio;
        };
        auto by_id_then_ratio = [](const IdRatio& a, const IdRatio& b)
        { return a.id != b.id ? a.id < b.id : a.ratio < b.ratio; };

        std::vector<IdRatio> ve_sorted, preset_sorted;
        ve_sorted.reserve(ve.components.size());
        preset_sorted.reserve(preset.extruder_ids_1based.size());

        for (const auto& c : ve.components)
            ve_sorted.push_back({c.extruder_id, c.ratio});
        for (size_t i = 0; i < preset.extruder_ids_1based.size(); ++i)
            preset_sorted.push_back(
                {preset.extruder_ids_1based[i], double(preset.ratios_percent[i]) / 100.0}
            );

        std::sort(ve_sorted.begin(), ve_sorted.end(), by_id_then_ratio);
        std::sort(preset_sorted.begin(), preset_sorted.end(), by_id_then_ratio);

        bool match = true;
        for (size_t i = 0; i < ve_sorted.size() && match; ++i) {
            if (ve_sorted[i].id != preset_sorted[i].id)
                match = false;
            else if (std::abs(ve_sorted[i].ratio - preset_sorted[i].ratio) > 0.02)
                match = false;
        }
        if (match) {
            return true;
        }
    }
    return false;
}

json op_presets(const json& req)
{
    const Physical physical = parse_physical(req);
    VirtualExtruderPresets::PhysicalExtruderSlots slots;
    for (size_t i = 0; i < physical.colors.size(); ++i) slots.push_back({physical.colors[i], physical.types[i], physical.enabled[i]});

    // The dialog marks presets already in the working list; the list is optional here.
    VirtualExtruders working_list;
    const bool with_added = req.contains("virtual");
    if (with_added) working_list = read_virtual(require_array(req, "virtual"), physical.colors).virtual_extruders;

    const VirtualExtruderPresets::BlendPresetGroups groups = VirtualExtruderPresets::build_blend_presets(slots);
    auto to_json = [&](const VirtualExtruderPresets::BlendPresets& presets) {
        json arr = json::array();
        for (const VirtualExtruderPresets::BlendPreset& p : presets) {
            json e;
            e["extruders"] = p.extruder_ids_1based;
            e["ratios_percent"] = p.ratios_percent;
            json comps = json::array();
            for (size_t i = 0; i < p.extruder_ids_1based.size(); ++i)
                comps.push_back(json{{"extruder", p.extruder_ids_1based[i]}, {"ratio", double(p.ratios_percent[i]) / 100.0}});
            e["components"] = comps;
            e["color"] = VirtualExtruderPresets::preset_color(p, slots);
            if (with_added) e["added"] = preset_already_used(p, working_list);
            arr.push_back(e);
        }
        return arr;
    };
    return json{{"two_color", to_json(groups.two_color)}, {"three_color", to_json(groups.three_color)}};
}

// ---------------------------------------------------------------------------------------------------------------------
// next_id: PrusaSlicer 2.9.6 FullSpectrumDialog::next_free_virtual_id + on_add_blend_clicked
// ---------------------------------------------------------------------------------------------------------------------

json op_next_id(const json& req)
{
    const unsigned int m_num_physical = require_count(req, "physical_count");
    const json& list = require_array(req, "virtual");

    std::set<unsigned int> used;
    for (const json& e : list) {
        if (!e.is_object() || !e.contains("id") || !e["id"].is_number_integer() || e["id"].get<long long>() < 0)
            throw BadRequest("Each \"virtual\" entry needs a non-negative integer \"id\"");
        used.insert(static_cast<unsigned int>(e["id"].get<long long>()));
    }
    unsigned int candidate = m_num_physical + 1;
    while (used.count(candidate) != 0)
        ++candidate;

    json out;
    out["id"] = candidate;
    // "Add blend" adds nothing on a printer with fewer than two physical extruders; otherwise a 50/50 blend of 1 and 2.
    if (m_num_physical < 2) {
        out["blend"] = nullptr;
    } else {
        out["blend"] = json{{"id", candidate}, {"kind", "fullspectrum"},
                            {"components", json::array({json{{"extruder", 1}, {"ratio", 0.5}}, json{{"extruder", 2}, {"ratio", 0.5}}})}};
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// remap_import: PrusaSlicer 2.9.6 3MF import (Format/3mf.cpp: deserialize + normalize) then remap_full_spectrum_on_import
// ---------------------------------------------------------------------------------------------------------------------

json op_remap_import(const json& req)
{
    const unsigned int target_physical_count = require_count(req, "physical_count");
    if (!req.contains("sidecar") || !req["sidecar"].is_object())
        throw BadRequest("The request needs \"sidecar\" (the file's Prusa_Slicer_full_spectrum.json object)");

    const FullSpectrumConfig fs_config = deserialize_virtual_extruders_from_json(req["sidecar"].dump());
    VirtualExtruders target_virtual_extruders = normalize_virtual_extruders(fs_config.virtual_extruders);
    const std::vector<unsigned int> old_ids = [&] {
        std::vector<unsigned int> ids;
        for (const VirtualExtruder& ve : target_virtual_extruders) ids.push_back(ve.id);
        return ids;
    }();

    // The model's painted states are renumbered by the same call when a model is loaded; here there is only the list.
    Model model;
    remap_full_spectrum_on_import(model, target_virtual_extruders, target_physical_count, fs_config);

    json remap = json::object();
    for (size_t i = 0; i < target_virtual_extruders.size(); ++i)
        if (target_virtual_extruders[i].id != old_ids[i]) remap[std::to_string(old_ids[i])] = target_virtual_extruders[i].id;

    const json sidecar = json::parse(serialize_virtual_extruders_to_json(fs_config.physical_colors, target_virtual_extruders));
    json out;
    out["source_physical_count"] = fs_config.source_physical_count;
    out["physical_count"] = target_physical_count;
    out["virtual"] = sidecar["virtual_extruders"];
    out["remap"] = remap;
    return out;
}

} // namespace

bool check_virtual_extruders_file(const std::string& text, std::string& why)
{
    json root;
    try {
        root = json::parse(text);
    } catch (const std::exception& e) {
        why = std::string("not valid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object()) { why = "not a JSON object"; return false; }
    if (!root.contains("version") || !root["version"].is_number_integer()) { why = "missing \"version\""; return false; }
    const long long version = root["version"].get<long long>();
    if (version < 1 || version > k_full_spectrum_config_version) { why = "unsupported version " + std::to_string(version); return false; }
    if (!root.contains("virtual_extruders") || !root["virtual_extruders"].is_array()) { why = "missing \"virtual_extruders\" array"; return false; }
    return true;
}

int run_color_mix(const std::string& request_text, std::string& response)
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
        if (op == "normalize") out = op_normalize(req);
        else if (op == "mix") out = op_mix(req);
        else if (op == "presets") out = op_presets(req);
        else if (op == "next_id") out = op_next_id(req);
        else if (op == "remap_import") out = op_remap_import(req);
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

} // namespace nozzle_cm
