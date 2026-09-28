// See ExtruderVariants.hpp. Ported from upstream OrcaSlicer src/libslic3r/PrintConfig.cpp
// (get_index_for_extruder, update_values_to_printer_extruders, update_values_to_printer_extruders_for_multiple_filaments,
// support_different_extruders) and PrintApply.cpp (Print::apply). AGPL-3.0.
#include "ExtruderVariants.hpp"

#include <boost/algorithm/string.hpp>

#include <set>
#include <string>
#include <vector>

namespace Slic3r {

namespace {

// Upstream's key sets, verbatim; keys this engine doesn't define are skipped.
const std::set<std::string> print_options_with_variant = {
    "initial_layer_speed", "initial_layer_infill_speed", "outer_wall_speed", "inner_wall_speed",
    "small_perimeter_speed", "small_perimeter_threshold", "sparse_infill_speed", "internal_solid_infill_speed",
    "top_surface_speed", "enable_overhang_speed", "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed",
    "overhang_4_4_speed", "slowdown_for_curled_perimeters", "bridge_speed", "internal_bridge_speed",
    "gap_infill_speed", "support_speed", "support_interface_speed", "travel_speed", "travel_speed_z",
    "initial_layer_travel_speed", "default_acceleration", "bridge_acceleration", "travel_acceleration",
    "initial_layer_travel_acceleration", "initial_layer_acceleration", "outer_wall_acceleration",
    "inner_wall_acceleration", "sparse_infill_acceleration", "internal_solid_infill_acceleration",
    "top_surface_acceleration", "default_jerk", "outer_wall_jerk", "inner_wall_jerk", "infill_jerk",
    "top_surface_jerk", "initial_layer_jerk", "travel_jerk", "initial_layer_travel_jerk",
    "default_junction_deviation", "print_extruder_id", "print_extruder_variant", "top_solid_infill_flow_ratio"
};

const std::set<std::string> filament_options_with_variant = {
    "filament_flow_ratio", "filament_max_volumetric_speed", "filament_ramming_volumetric_speed",
    "filament_pre_cooling_temperature", "filament_ramming_travel_time", "filament_ramming_volumetric_speed_nc",
    "filament_pre_cooling_temperature_nc", "filament_ramming_travel_time_nc", "filament_retract_length_nc",
    "filament_preheat_temperature_delta", "filament_extruder_id", "filament_extruder_variant",
    "filament_retraction_length", "filament_z_hop", "filament_z_hop_types", "filament_retract_lift_above",
    "filament_retract_lift_below", "filament_retract_lift_enforce", "filament_retract_restart_extra",
    "filament_retract_length_toolchange", "filament_retract_restart_extra_toolchange", "filament_retraction_speed",
    "filament_deretraction_speed", "filament_retraction_minimum_travel", "filament_retract_when_changing_layer",
    "filament_wipe", "filament_wipe_distance", "filament_retract_before_wipe", "filament_retract_after_wipe",
    "filament_long_retractions_when_cut", "filament_retraction_distances_when_cut", "long_retractions_when_ec",
    "retraction_distances_when_ec", "nozzle_temperature_initial_layer", "nozzle_temperature",
    "filament_flush_volumetric_speed", "filament_flush_temp", "filament_cooling_before_tower",
    "volumetric_speed_coefficients", "filament_adaptive_volumetric_speed", "filament_ironing_flow",
    "filament_ironing_spacing", "filament_ironing_inset", "filament_ironing_speed", "activate_air_filtration",
    "activate_air_filtration_during_print", "activate_air_filtration_on_completion",
    "during_print_exhaust_fan_speed", "complete_print_exhaust_fan_speed"
};

// Stored per variant.
const std::set<std::string> printer_options_with_variant_1 = {
    "nozzle_volume", "retraction_length", "z_hop", "travel_slope", "retract_lift_above", "retract_lift_below",
    "retract_lift_enforce", "z_hop_types", "retraction_speed", "deretraction_speed", "retraction_minimum_travel",
    "retract_when_changing_layer", "wipe", "wipe_distance", "retract_before_wipe", "retract_after_wipe",
    "retract_length_toolchange", "retract_restart_extra", "retract_restart_extra_toolchange",
    "long_retractions_when_cut", "retraction_distances_when_cut", "nozzle_volume", "nozzle_type",
    "printer_extruder_id", "printer_extruder_variant", "hotend_cooling_rate", "hotend_heating_rate",
    "nozzle_flush_dataset"
};

// Stored as (normal, silent) pairs per variant.
const std::set<std::string> printer_options_with_variant_2 = {
    "machine_max_acceleration_x", "machine_max_acceleration_y", "machine_max_acceleration_z",
    "machine_max_acceleration_e", "machine_max_acceleration_extruding", "machine_max_acceleration_retracting",
    "machine_max_acceleration_travel", "machine_max_speed_x", "machine_max_speed_y", "machine_max_speed_z",
    "machine_max_speed_e", "machine_max_jerk_x", "machine_max_jerk_y", "machine_max_jerk_z", "machine_max_jerk_e",
    "machine_max_junction_deviation"
};

// "Direct Drive Standard": extruder_type + nozzle volume. This engine's nozzle_volume_type is Snapmaker's flow type
// (standard / high_flow); it names the same two volume types upstream calls "Standard" / "High Flow".
std::string extruder_variant_string(const ConfigBase &config, int extruder_index)
{
    const auto *extruder_type = dynamic_cast<const ConfigOptionEnumsGeneric*>(config.option("extruder_type"));
    const auto *volume_type   = dynamic_cast<const ConfigOptionEnumsGeneric*>(config.option("nozzle_volume_type"));
    const int type   = extruder_type && ! extruder_type->values.empty() ? extruder_type->get_at(extruder_index) : int(etDirectDrive);
    const int volume = volume_type && ! volume_type->values.empty() ? volume_type->get_at(extruder_index) : int(fvtStandard);
    return std::string(type == int(etBowden) ? "Bowden" : "Direct Drive") + (volume == int(fvtHighFlow) ? " High Flow" : " Standard");
}

std::vector<std::string> split_variants(const std::string &list)
{
    std::vector<std::string> variants, tokens;
    boost::split(tokens, list, boost::is_any_of(","), boost::token_compress_on);
    for (std::string &token : tokens) {
        boost::trim(token);
        if (! token.empty())
            variants.push_back(token);
    }
    return variants;
}

// Upstream get_index_for_extruder: the column of `variant` for extruder/filament `id` (1-based), or -1.
int index_for_extruder(const ConfigBase &config, int id, const std::string &id_name, const std::string &variant, const std::string &variant_name,
                       unsigned int stride = 1)
{
    const auto *variant_opt = dynamic_cast<const ConfigOptionStrings*>(config.option(variant_name));
    const auto *id_opt      = id_name.empty() ? nullptr : dynamic_cast<const ConfigOptionInts*>(config.option(id_name));
    const auto *list_opt    = dynamic_cast<const ConfigOptionStrings*>(config.option("extruder_variant_list"));
    if (variant_opt == nullptr)
        return -1;
    // The extruder a column belongs to when the id list is shorter than the variant list: walk extruder_variant_list.
    auto generated_extruder_id = [list_opt](int target_index) {
        if (list_opt == nullptr)
            return 0;
        int variant_index = 0;
        for (int extruder_index = 0; extruder_index < int(list_opt->values.size()); ++extruder_index)
            for (size_t i = 0; i < split_variants(list_opt->get_at(extruder_index)).size(); ++i)
                if (variant_index++ == target_index)
                    return extruder_index + 1;
        return 0;
    };
    const int  size              = int(variant_opt->values.size());
    const bool has_complete_ids  = id_opt && int(id_opt->values.size()) >= size;
    for (int index = 0; index < size; ++index)
        if (variant_opt->get_at(index) == variant) {
            if (id_opt == nullptr)
                return index * stride;
            if ((has_complete_ids ? id_opt->get_at(index) : generated_extruder_id(index)) == id)
                return index * stride;
        }
    return -1;
}

// Upstream ensure_process_variant_columns: a process preset with single-column defaults gets its columns from the
// printer's extruder_variant_list.
void ensure_process_variant_columns(DynamicPrintConfig &config)
{
    auto *id_opt      = dynamic_cast<ConfigOptionInts*>(config.option("print_extruder_id"));
    auto *variant_opt = dynamic_cast<ConfigOptionStrings*>(config.option("print_extruder_variant"));
    const auto *list_opt = dynamic_cast<const ConfigOptionStrings*>(config.option("extruder_variant_list"));
    if (! id_opt || ! variant_opt || ! list_opt || id_opt->values.size() != 1 || variant_opt->values.size() != 1)
        return;
    std::vector<int> ids;
    std::vector<std::string> variants;
    for (int i = 0; i < int(list_opt->values.size()); ++i)
        for (std::string &variant : split_variants(list_opt->get_at(i))) {
            ids.push_back(i + 1);
            variants.push_back(std::move(variant));
        }
    if (ids.size() <= 1)
        return;
    id_opt->values      = std::move(ids);
    variant_opt->values = std::move(variants);
}

// Rebuilds each vector option in `keys` from the given source columns (stride values per column). Out-of-range
// columns read the first value (ConfigOptionVector::get_at).
template<class Opt>
void gather(ConfigOption *option, const std::vector<int> &columns, unsigned int stride)
{
    auto *opt = dynamic_cast<Opt*>(option);
    if (opt == nullptr || opt->values.empty())
        return;
    decltype(opt->values) values;
    values.reserve(columns.size() * stride);
    for (int column : columns)
        for (unsigned int i = 0; i < stride; ++i)
            values.push_back(opt->get_at(size_t(std::max(column, 0)) * stride + i));
    opt->values = std::move(values);
}

void gather_keys(DynamicPrintConfig &config, const std::set<std::string> &keys, const std::vector<int> &columns, unsigned int stride,
                 const std::string &skip_key = std::string())
{
    for (const std::string &key : keys) {
        if (key == skip_key)
            continue;
        const ConfigOptionDef *def = config.def()->get(key);
        ConfigOption          *opt = def ? config.option(key) : nullptr;
        if (opt == nullptr)
            continue;
        switch (def->type) {
        case coStrings:          gather<ConfigOptionStrings>(opt, columns, stride); break;
        case coInts:             gather<ConfigOptionInts>(opt, columns, stride); break;
        case coFloats:           gather<ConfigOptionFloats>(opt, columns, stride); break;
        case coPercents:         gather<ConfigOptionPercents>(opt, columns, stride); break;
        case coFloatsOrPercents: gather<ConfigOptionFloatsOrPercents>(opt, columns, stride); break;
        case coBools:            gather<ConfigOptionBools>(opt, columns, stride); break;
        case coEnums:            gather<ConfigOptionEnumsGeneric>(opt, columns, stride); break;
        default: break;
        }
    }
}

// Upstream update_values_to_printer_extruders (all extruders; one volume type per extruder).
void collapse_to_extruders(DynamicPrintConfig &config, int extruder_count, const std::set<std::string> &keys,
                           const std::string &id_name, const std::string &variant_name, unsigned int stride = 1)
{
    if (id_name == "print_extruder_id")
        ensure_process_variant_columns(config);
    std::vector<int> columns;
    for (int e = 0; e < extruder_count; ++e) {
        const int column = index_for_extruder(config, e + 1, id_name, extruder_variant_string(config, e), variant_name, stride);
        columns.push_back(column < 0 ? 0 : column / int(stride));
    }
    gather_keys(config, keys, columns, stride);
}

// Upstream update_values_to_printer_extruders_for_multiple_filaments: each filament keeps the column of the variant
// its extruder (filament_map) has installed.
void collapse_to_filaments(DynamicPrintConfig &config, const std::set<std::string> &keys)
{
    const auto *filament_map = config.option<ConfigOptionInts>("filament_map");
    const auto *variants     = config.option<ConfigOptionStrings>("filament_extruder_variant");
    if (filament_map == nullptr || variants == nullptr)
        return;
    const int filament_count = int(filament_map->values.size());
    // Which filament each variant column belongs to (filament_self_index, 1-based). Upstream's preset bundle writes it
    // when it joins the filament presets; here the filament settings come from one profile plus per-filament overrides,
    // so it is complete only when the profile says so, or when there is a single filament (every column is its own).
    // Otherwise the columns can't be attributed and the filament settings are left as they are.
    auto *ids = config.option<ConfigOptionInts>("filament_self_index", true);
    if (ids->values.size() != variants->values.size()) {
        if (filament_count != 1)
            return;
        ids->values.assign(variants->values.size(), 1);
    }
    std::vector<int> columns(filament_count, 0);
    for (int f = 0; f < filament_count; ++f) {
        const int extruder = std::max(filament_map->values[f], 1) - 1;
        int column = index_for_extruder(config, f + 1, "filament_self_index", extruder_variant_string(config, extruder), "filament_extruder_variant");
        if (column < 0 && ids != nullptr) {
            // The filament has no preset for that variant: its first column (upstream's fallback).
            column = 0;
            for (int i = 0; i < int(ids->values.size()); ++i)
                if (ids->values[i] == f + 1) { column = i; break; }
        }
        columns[f] = std::max(column, 0);
    }
    gather_keys(config, keys, columns, 1, "filament_self_index");
    if (auto *ids_mutable = config.option<ConfigOptionInts>("filament_self_index"); ids_mutable != nullptr && ! ids_mutable->values.empty())
        gather<ConfigOptionInts>(ids_mutable, columns, 1);
}

} // namespace

size_t filament_variant_columns(const ConfigBase &config)
{
    const auto *variants = dynamic_cast<const ConfigOptionStrings*>(config.option("filament_extruder_variant"));
    return variants ? variants->values.size() : 0;
}

namespace {
// Tiles a vector option's k profile columns once per filament, or repeats each of its per-filament values k times.
template<class Opt>
void expand_columns(ConfigOption *option, size_t filaments, size_t columns, bool per_filament)
{
    auto *opt = dynamic_cast<Opt*>(option);
    if (opt == nullptr || opt->values.empty())
        return;
    decltype(opt->values) values;
    values.reserve(filaments * columns);
    for (size_t f = 0; f < filaments; ++f)
        for (size_t c = 0; c < columns; ++c)
            values.push_back(per_filament ? opt->get_at(f) : opt->get_at(c));
    opt->values = std::move(values);
}
} // namespace

void expand_filament_variant_columns(DynamicPrintConfig &config, size_t profile_columns, const std::set<std::string> &per_filament_keys)
{
    size_t filaments = 0;
    for (const char *key : { "filament_diameter", "filament_colour", "filament_type" })
        if (const auto *vec = dynamic_cast<const ConfigOptionVectorBase*>(config.option(key)))
            filaments = std::max(filaments, vec->size());
    if (profile_columns <= 1 || filaments <= 1)
        return;
    std::set<std::string> keys = filament_options_with_variant;
    keys.insert("filament_extruder_variant");
    for (const std::string &key : keys) {
        const ConfigOptionDef *def = config.def()->get(key);
        ConfigOption          *opt = def ? config.option(key) : nullptr;
        if (opt == nullptr)
            continue;
        const bool per_filament = per_filament_keys.count(key) > 0;
        // A value the profile left single (not per variant) applies to every column already.
        if (! per_filament && static_cast<const ConfigOptionVectorBase*>(opt)->size() != profile_columns)
            continue;
        switch (def->type) {
        case coStrings:          expand_columns<ConfigOptionStrings>(opt, filaments, profile_columns, per_filament); break;
        case coInts:             expand_columns<ConfigOptionInts>(opt, filaments, profile_columns, per_filament); break;
        case coFloats:           expand_columns<ConfigOptionFloats>(opt, filaments, profile_columns, per_filament); break;
        case coPercents:         expand_columns<ConfigOptionPercents>(opt, filaments, profile_columns, per_filament); break;
        case coFloatsOrPercents: expand_columns<ConfigOptionFloatsOrPercents>(opt, filaments, profile_columns, per_filament); break;
        case coBools:            expand_columns<ConfigOptionBools>(opt, filaments, profile_columns, per_filament); break;
        case coEnums:            expand_columns<ConfigOptionEnumsGeneric>(opt, filaments, profile_columns, per_filament); break;
        default: break;
        }
    }
    std::vector<int> ids;
    for (size_t f = 0; f < filaments; ++f)
        ids.insert(ids.end(), profile_columns, int(f + 1));
    config.option<ConfigOptionInts>("filament_self_index", true)->values = std::move(ids);
}

bool support_different_extruders(const DynamicPrintConfig &config, int &extruder_count)
{
    std::set<std::string> variants;
    extruder_count = 1;
    if (const auto *nozzle_diameter = config.option<ConfigOptionFloats>("nozzle_diameter")) {
        extruder_count = int(nozzle_diameter->values.size());
        if (const auto *list = config.option<ConfigOptionStrings>("extruder_variant_list"))
            for (int i = 0; i < extruder_count; ++i)
                for (std::string &variant : split_variants(list->get_at(i)))
                    variants.insert(std::move(variant));
    }
    return variants.size() > 1;
}

bool collapse_extruder_variants(DynamicPrintConfig &config)
{
    int extruder_count = 1;
    // Upstream also collapses any multi-extruder printer; without declared variants that only replicates the first
    // value, and this engine's other multi-extruder printers keep their own per-extruder / flow-variant layout.
    if (! support_different_extruders(config, extruder_count))
        return false;
    // One filament_map entry per filament. A map that doesn't name every filament (the request sent none) gets
    // upstream's rule for printers without its grouping engine: filament i on extruder i while there are extruders, the
    // rest on the master extruder. Upstream's own default for Bambu printers is its flush-minimising grouping, which is
    // not ported.
    if (const auto *filament_diameter = config.option<ConfigOptionFloats>("filament_diameter")) {
        auto *filament_map = config.option<ConfigOptionInts>("filament_map", true);
        const size_t filaments = filament_diameter->values.size();
        const auto  *master    = config.option<ConfigOptionInt>("master_extruder_id");
        const int    master_extruder = master && master->value >= 1 && master->value <= extruder_count ? master->value : 1;
        if (filament_map->values.size() != filaments) {
            filament_map->values.resize(filaments);
            for (size_t f = 0; f < filaments; ++f)
                filament_map->values[f] = int(f) < extruder_count ? int(f) + 1 : master_extruder;
        }
        for (int &extruder : filament_map->values)
            if (extruder < 1 || extruder > extruder_count)
                extruder = master_extruder;
    }
    // Variant 2 first: variant 1 halves printer_extruder_id / printer_extruder_variant, which the stride-2 lookup needs.
    collapse_to_extruders(config, extruder_count, printer_options_with_variant_2, "printer_extruder_id", "printer_extruder_variant", 2);
    collapse_to_extruders(config, extruder_count, printer_options_with_variant_1, "printer_extruder_id", "printer_extruder_variant");
    collapse_to_extruders(config, extruder_count, print_options_with_variant, "print_extruder_id", "print_extruder_variant");
    std::set<std::string> filament_keys = filament_options_with_variant;
    filament_keys.insert("filament_self_index");
    collapse_to_filaments(config, filament_keys);
    return true;
}

bool extruder_variants_collapsed(const ConfigBase &config)
{
    const auto *variants = dynamic_cast<const ConfigOptionStrings*>(config.option("printer_extruder_variant"));
    const auto *list     = dynamic_cast<const ConfigOptionStrings*>(config.option("extruder_variant_list"));
    const auto *nozzles  = dynamic_cast<const ConfigOptionFloats*>(config.option("nozzle_diameter"));
    if (variants == nullptr || list == nullptr || nozzles == nullptr || variants->values.size() != nozzles->values.size())
        return false;
    size_t declared = 0;
    for (const std::string &entry : list->values)
        declared += split_variants(entry).size();
    return declared > 1;
}

} // namespace Slic3r
