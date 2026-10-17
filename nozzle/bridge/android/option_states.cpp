// Port of ConfigManipulation::toggle_print_fff_options (src/slic3r/GUI/ConfigManipulation.cpp, this engine's base:
// Orca 2.5.0) to a function of the configuration alone. The rules are kept in the GUI's order, with the
// GUI's comments, so a later upstream change to that function can be carried over line by line. Differences:
//   - toggle_field/toggle_line record into `states` instead of changing widgets (a later rule overrides an earlier one
//     for the same key, as it does in the GUI);
//   - values the GUI forces through apply()/set_key_value are reported in `forced`, and the rules that follow read
//     the forced value, as they do in the GUI;
//   - what the GUI reads from the edited printer preset (G-code flavour, single-extruder multi-material, purge in the
//     prime tower) is read from the full configuration, and whether the printer is a Bambu Lab one comes from the caller.
#include "option_states.hpp"

#include "libslic3r/BeltBrim.hpp"

// The GUI marks translatable strings with L(); the engine reports the untranslated text.
#ifndef L
#define L(s) s
#endif

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <set>

namespace engine {
using namespace Slic3r;

OptionStates print_option_states(const DynamicPrintConfig& input, const OptionContext& context) {
    OptionStates out;
    DynamicPrintConfig cfg = input;
    DynamicPrintConfig* config = &cfg;
    const bool is_BBL_Printer = context.is_bbl_printer;
    const bool is_global_config = context.is_global_config;
    const int variant_index = int(context.flow_variant_index);

    // The GUI toggles a field for one flow variant of an option; the contract reports one state per option key.
    auto toggle_field = [&](const std::string& key, bool on, int = -1) { out.states[key].enabled = on; };
    auto toggle_line = [&](const std::string& key, bool on, int = -1) { out.states[key].visible = on; };
    auto set_option_label = [&](const std::string&, const char*) {};
    auto force = [&](const std::string& key, ConfigOption* value) {
        config->set_key_value(key, value);
        const std::string text = config->option(key)->serialize();
        const auto it = std::find_if(out.forced.begin(), out.forced.end(), [&](const auto& f) { return f.first == key; });
        if (it != out.forced.end()) it->second = text; else out.forced.emplace_back(key, text);
    };

    const GCodeFlavor gcflavor = config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;

    // Orca: use booleans to avoid repeated comparisons with enum values
    const bool gcf_is_marlin_firmware = gcflavor == GCodeFlavor::gcfMarlinFirmware;
    const bool gcf_is_klipper = gcflavor == GCodeFlavor::gcfKlipper;

    // Belt printer: detect early since it affects multiple toggle decisions below.
    // `is_belt_tilted` is the stricter test that mirrors PrintObject::has_belt_brim():
    // only a tilted belt gets the belt-plane brim, while a belt printer with no
    // rotation is geometrically a flat bed and keeps the ordinary plate brim.
    bool is_belt_printer = false;
    bool is_belt_tilted  = false;
    {
        const auto &printer_cfg = (*config);
        const auto *belt_opt = printer_cfg.option<ConfigOptionBool>("belt_printer");
        if (belt_opt)
            is_belt_printer = belt_opt->value;
        const auto *axis  = printer_cfg.option<ConfigOptionEnum<BeltRotationAxis>>("belt_slice_rotation");
        const auto *angle = printer_cfg.option<ConfigOptionFloat>("belt_slice_rotation_angle");
        if (is_belt_printer && axis != nullptr && angle != nullptr) {
            // Same window as Print::has_tilted_belt(); shared constants so the GUI and
            // the backend cannot drift apart.
            const double tilt = std::abs(angle->value);
            is_belt_tilted = (axis->value == BeltRotationAxis::X || axis->value == BeltRotationAxis::Y)
                          && tilt >= BELT_BRIM_MIN_TILT_DEG && tilt <= BELT_BRIM_MAX_TILT_DEG;
        }
    }

    bool have_volumetric_extrusion_rate_slope = config->option<ConfigOptionFloat>("max_volumetric_extrusion_rate_slope")->value > 0;
    float have_volumetric_extrusion_rate_slope_segment_length = config->option<ConfigOptionFloat>("max_volumetric_extrusion_rate_slope_segment_length")->value;
    toggle_field("enable_arc_fitting", !have_volumetric_extrusion_rate_slope && !is_belt_printer);
    toggle_line("max_volumetric_extrusion_rate_slope_segment_length", have_volumetric_extrusion_rate_slope);
    toggle_line("extrusion_rate_smoothing_external_perimeter_only", have_volumetric_extrusion_rate_slope);
    if(have_volumetric_extrusion_rate_slope) force("enable_arc_fitting", new ConfigOptionBool(false));
    if(have_volumetric_extrusion_rate_slope_segment_length < 0.5) {
        force("max_volumetric_extrusion_rate_slope_segment_length", new ConfigOptionFloat(1));
    }

    bool have_perimeters = config->opt_int("wall_loops") > 0;
    for (auto el : { "extra_perimeters_on_overhangs", "ensure_vertical_shell_thickness", "detect_thin_wall", "detect_overhang_wall",
        "seam_position", "staggered_inner_seams", "wall_sequence", "outer_wall_line_width" })
        toggle_field(el, have_perimeters);
    for (auto el : { "inner_wall_speed", "outer_wall_speed", "small_perimeter_speed", "small_perimeter_threshold" })
        toggle_field(el, have_perimeters, variant_index);

    bool have_infill = config->option<ConfigOptionPercent>("sparse_infill_density")->value > 0;
    // sparse_infill_filament_id uses the same logic as in Print::extruders()
    for (auto el : { "sparse_infill_pattern", "infill_combination", "fill_multiline","infill_direction",
                     "infill_complete_top",
        "minimum_sparse_infill_area", "sparse_infill_filament_id","infill_shift_step","sparse_infill_rotate_template","symmetric_infill_y_axis"})
        toggle_line(el, have_infill);

    InfillPattern pattern = config->opt_enum<InfillPattern>("sparse_infill_pattern");

    // Orca: the concentric patterns follow the surface outline instead of crossing it, so there is
    // nothing for an infill anchor to attach to. Hide the anchor settings for them.
    bool have_infill_anchor = have_infill && pattern != ipConcentric && pattern != ipSpiralInset;
    toggle_line("infill_anchor", have_infill_anchor);
    toggle_line("infill_anchor_max", have_infill_anchor);

    bool have_combined_infill = config->opt_bool("infill_combination") && have_infill;
    toggle_line("infill_combination_max_layer_height", have_combined_infill);

    // Infill patterns that support multiline infill.
    bool          have_multiline_infill_pattern = pattern == ipGyroid || pattern == ipGrid || pattern == ipRectilinear || pattern == ipTpmsD || pattern == ipTpmsFK || pattern == ipCrossHatch || pattern == ipHoneycomb || pattern == ipLateralLattice || pattern == ipLateralHoneycomb || pattern == ipConcentric ||
                                                  pattern == ipCubic || pattern == ipStars || pattern == ipAlignedRectilinear || pattern == ipLightning || pattern == ip3DHoneycomb || pattern == ipAdaptiveCubic || pattern == ipSupportCubic|| pattern == ipTriangles || pattern == ipQuarterCubic|| pattern == ipArchimedeanChords || pattern == ipHilbertCurve || pattern == ipOctagramSpiral;

    // The sparse infill density is the surface density of the adaptive TPMS infill; at 100% the infill is solid.
    bool have_tpms_infill = have_infill && config->option<ConfigOptionPercent>("sparse_infill_density")->value < 100 &&
                            is_tpms_adaptive_pattern(pattern);
    toggle_line("tpms_adaptive", have_tpms_infill);
    bool have_tpms_adaptive = have_tpms_infill && config->opt_enum<TpmsAdaptiveMode>("tpms_adaptive") != TpmsAdaptiveMode::Disabled;
    toggle_line("tpms_interior_density", have_tpms_adaptive);
    toggle_line("tpms_adaptive_gradient", have_tpms_adaptive);

    // gyroid_optimized only applies when the sparse infill pattern is gyroid without adaptive density;
    // hide the whole line otherwise.
    toggle_line("gyroid_optimized", have_infill && pattern == ipGyroid && !have_tpms_adaptive);

    // If there is infill, enable/disable fill_multiline according to whether the pattern supports multiline infill.
    if (have_infill) {
        toggle_field("fill_multiline", have_multiline_infill_pattern);
        // If the infill pattern does not support multiline fill_multiline is changed to 1.
        // Necessary when the pattern contains params.multiline (for example, triangles because they belong to the rectilinear class)
        if (!have_multiline_infill_pattern) {
            force("fill_multiline", new ConfigOptionInt(1));
        }
        // Hide infill anchor max if sparse_infill_pattern is not line or if sparse_infill_pattern is line but infill_anchor_max is 0.
        bool infill_anchor = config->opt_enum<InfillPattern>("sparse_infill_pattern") != ipLine;
        toggle_field("infill_anchor_max", infill_anchor);

        // Only allow configuration of open anchors if the anchoring is enabled.
        bool has_infill_anchors = infill_anchor && config->option<ConfigOptionFloatOrPercent>("infill_anchor_max")->value > 0;
        toggle_field("infill_anchor", has_infill_anchors);
    }

    //cross zag
    bool is_cross_zag = config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value == InfillPattern::ipCrossZag;
    bool is_locked_zig = config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value == InfillPattern::ipLockedZag;

    toggle_line("infill_shift_step", is_cross_zag || is_locked_zig);
    
    for (auto el : { "skeleton_infill_density", "skin_infill_density", "infill_lock_depth", "skin_infill_depth","skin_infill_line_width", "skeleton_infill_line_width" })
        toggle_line(el, is_locked_zig);

    bool is_zig_zag = config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value == InfillPattern::ipZigZag;

    toggle_line("symmetric_infill_y_axis", is_zig_zag || is_cross_zag || is_locked_zig);

    bool is_3Dhoneycomb = config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value == InfillPattern::ip3DHoneycomb;
    toggle_line("infill_complete_top", have_infill && is_3Dhoneycomb);

    bool has_spiral_vase         = config->opt_bool("spiral_mode");
    toggle_line("spiral_mode_smooth", has_spiral_vase);
    toggle_line("spiral_mode_max_xy_smoothing", has_spiral_vase && config->opt_bool("spiral_mode_smooth"));
    toggle_line("spiral_starting_flow_ratio", has_spiral_vase);
    toggle_line("spiral_finishing_flow_ratio", has_spiral_vase);
    bool has_top_shell_layers = config->opt_int("top_shell_layers") > 0 || (has_spiral_vase && config->opt_int("bottom_shell_layers") > 1);
    bool has_top_shell    = has_top_shell_layers && config->option<ConfigOptionPercent>("top_surface_density")->value > 0;
    bool has_bottom_shell = config->opt_int("bottom_shell_layers") > 0;
    bool has_solid_infill = has_top_shell_layers || has_bottom_shell;
    toggle_line("sparse_infill_smooth_factor", have_infill && is_smoothable_infill_pattern(pattern, config->opt_int("fill_multiline")));
    toggle_field("top_surface_pattern", has_top_shell);
    toggle_field("bottom_surface_pattern", has_bottom_shell);
    toggle_field("top_surface_density", has_top_shell_layers);
    toggle_field("bottom_surface_density", has_bottom_shell);
    toggle_field("top_layer_direction", has_top_shell);
    toggle_field("bottom_layer_direction", has_bottom_shell);

    toggle_line("top_surface_expansion", has_top_shell);
    toggle_line("top_surface_expansion_margin", has_top_shell);
    bool has_top_surface_expansion = config->opt_float("top_surface_expansion") > 0;
    toggle_field("top_surface_expansion_margin", has_top_surface_expansion);
    toggle_line("top_surface_expansion_direction", has_top_shell);
    toggle_field("top_surface_expansion_direction", has_top_surface_expansion);

    // Orca: Archimedean Chords and Octagram Spiral are the centered surface patterns that the
    // pattern-centering feature acts on.
    auto is_centered_pattern = [](InfillPattern p) {
        return p == InfillPattern::ipArchimedeanChords || p == InfillPattern::ipOctagramSpiral;
    };
    bool is_top_centered    = is_centered_pattern(config->option<ConfigOptionEnum<InfillPattern>>("top_surface_pattern")->value);
    bool is_bottom_centered = is_centered_pattern(config->option<ConfigOptionEnum<InfillPattern>>("bottom_surface_pattern")->value);
    bool has_centered_surface = (has_top_shell && is_top_centered) || (has_bottom_shell && is_bottom_centered);

    // Orca: center of surface pattern
    toggle_line("center_of_surface_pattern", has_centered_surface);

    // Orca: separate infills
    toggle_line("separated_infills", is_separable_infill_pattern(pattern));

    // Fill order is only meaningful for the center-based surface fill patterns; hide it otherwise.
    auto is_centered_fill = [](InfillPattern p) { return p == ipConcentric || p == ipSpiralInset || p == ipArchimedeanChords || p == ipOctagramSpiral; };
    toggle_line("top_surface_fill_order", has_top_shell && is_centered_fill(config->opt_enum<InfillPattern>("top_surface_pattern")));
    toggle_line("bottom_surface_fill_order", has_bottom_shell && is_centered_fill(config->opt_enum<InfillPattern>("bottom_surface_pattern")));

    for (auto el : { "infill_direction", "sparse_infill_line_width", "gap_fill_target","filter_out_gap_fill","infill_wall_overlap",
        "bridge_angle", "internal_bridge_angle", "relative_bridge_angle",
        "solid_infill_direction", "solid_infill_rotate_template", "internal_solid_infill_pattern", "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id",
        })
        toggle_field(el, have_infill || has_solid_infill);
    for (auto el : { "sparse_infill_speed", "bridge_speed", "internal_bridge_speed"})
        toggle_field(el, have_infill || has_solid_infill, variant_index);

    toggle_field("top_shell_thickness", ! has_spiral_vase && has_top_shell_layers);
    toggle_field("bottom_shell_thickness", ! has_spiral_vase && has_bottom_shell);

    // Gap fill is newly allowed in between perimeter lines even for empty infill (see GH #1476).
    toggle_field("gap_infill_speed", have_perimeters, variant_index);
    
    toggle_field("top_surface_line_width", has_top_shell);
    toggle_field("top_surface_speed", has_top_shell, variant_index);

    bool have_default_acceleration = config->opt_float_nullable("default_acceleration", variant_index) > 0;

    for (auto el : {"outer_wall_acceleration", "inner_wall_acceleration", "initial_layer_acceleration", "initial_layer_travel_acceleration",
        "top_surface_acceleration", "travel_acceleration", "bridge_acceleration", "sparse_infill_acceleration", "internal_solid_infill_acceleration"})
        toggle_field(el, have_default_acceleration, variant_index);

    bool machine_supports_junction_deviation = false;
    if (gcf_is_marlin_firmware) {
        if (const auto *machine_jd = (*config).option<ConfigOptionFloats>("machine_max_junction_deviation")) {
            machine_supports_junction_deviation = !machine_jd->values.empty() && machine_jd->values.front() > 0.0;
        }
    }
    toggle_line("default_junction_deviation", gcf_is_marlin_firmware, variant_index);
    if (machine_supports_junction_deviation) {
        toggle_field("default_junction_deviation", true, variant_index);
        toggle_field("default_jerk", false, variant_index);
        for (auto el : { "outer_wall_jerk", "inner_wall_jerk", "initial_layer_jerk", "initial_layer_travel_jerk", "top_surface_jerk", "travel_jerk", "infill_jerk"})
            toggle_line(el, false, variant_index);
    } else {
        toggle_field("default_junction_deviation", false, variant_index);
        toggle_field("default_jerk", true, variant_index);
        bool have_default_jerk = config->has("default_jerk") && config->opt_float_nullable("default_jerk", variant_index) > 0;
        for (auto el : { "outer_wall_jerk", "inner_wall_jerk", "initial_layer_jerk", "initial_layer_travel_jerk", "top_surface_jerk", "travel_jerk", "infill_jerk"}) {
            toggle_line(el, true, variant_index);
            toggle_field(el, have_default_jerk, variant_index);
        }
    }

    // Belt printer: disable skirt, brim, raft, and draft shield controls.
    bool have_skirt = config->opt_int("skirt_loops") > 0 && !is_belt_printer;
    toggle_field("skirt_height", have_skirt && config->opt_enum<DraftShield>("draft_shield") != dsEnabled);
    toggle_line("single_loop_draft_shield", have_skirt); // ORCA: Display one wall if skirt enabled
    for (auto el : {"skirt_type", "min_skirt_length", "skirt_distance", "skirt_start_angle", "skirt_speed", "draft_shield"})
        toggle_field(el, have_skirt);
    if (is_belt_printer) {
        toggle_field("skirt_loops", false);
        toggle_field("skirt_height", false);
    }

    // Belt printers now get a brim too, laid onto the tilted belt by BeltBrim.cpp,
    // so brim type / width / object gap all apply.  A belt printer with no tilt is
    // geometrically a flat bed and uses the ordinary plate brim, hence the separate
    // is_belt_tilted test.
    bool have_brim = config->opt_enum<BrimType>("brim_type") != btNoBrim;
    toggle_field("brim_object_gap", have_brim);
    // Both are first-layer-only concepts that the belt path cannot honour.
    toggle_field("brim_use_efc_outline", have_brim && !is_belt_tilted);
    toggle_field("combine_brims", have_brim && !is_belt_tilted);
    bool have_brim_width = have_brim && config->opt_enum<BrimType>("brim_type") != btAutoBrim &&
                           config->opt_enum<BrimType>("brim_type") != btPainted;
    // On a tilted belt Auto / Mouse ear / Painted all collapse to outer-only at the
    // configured width, so the width field has to stay live for them too.
    toggle_field("brim_width", have_brim_width || (have_brim && is_belt_tilted));
    toggle_field("brim_flow_ratio", have_brim);
    // Paired toggle_line + toggle_field: cb_toggle_line is null in the per-object
    // override panel, so the row cannot be hidden there and greying out is the
    // fallback.  Both extras are belt-only: one extends the brim ahead along the belt,
    // the other widens it across the belt.
    for (auto el : { "leading_brim_length", "extra_brim_width" }) {
        toggle_line(el, is_belt_tilted);
        toggle_field(el, is_belt_tilted && have_brim);
    }
    // Wall filament selectors use the same logic as in Print::extruders().
    toggle_field("outer_wall_filament_id", have_perimeters || have_brim);
    toggle_field("inner_wall_filament_id", have_perimeters || have_brim);

    const BrimType brim_type = config->opt_enum<BrimType>("brim_type");
    const bool have_auto_brim_ear = brim_type == btEar && !is_belt_tilted;
    const bool have_painted_brim_ear = brim_type == btPainted && !is_belt_tilted;
    set_option_label("brim_width", have_auto_brim_ear ? L("Brim ear radius") : L("Brim width"));
    const auto brim_width = config->opt_float("brim_width");
    // Automatic brim ear settings require a non-zero brim width.
    toggle_field("brim_ears_max_angle", brim_width > 0.0f);
    toggle_field("brim_ears_detection_length", brim_width > 0.0f);
    // Painted ears carry their own radius and do not depend on brim_width.
    toggle_field("brim_ears_outer_only", have_painted_brim_ear || brim_width > 0.0f);
    toggle_line("brim_ears_max_angle", have_auto_brim_ear);
    toggle_line("brim_ears_detection_length", have_auto_brim_ear);
    toggle_line("brim_ears_outer_only", have_auto_brim_ear || have_painted_brim_ear);

    // Hide Elephant foot compensation layers if elefant_foot_compensation is not enabled
    toggle_line("elefant_foot_compensation_layers", config->opt_float("elefant_foot_compensation") > 0 || config->option<ConfigOptionPercent>("elefant_foot_layers_density")->get_abs_value(1.0f) < 1.0f);

    bool have_raft = config->opt_int("raft_layers") > 0 && !is_belt_printer;
    if (is_belt_printer)
        toggle_field("raft_layers", false);
    bool have_support_material = config->opt_bool("enable_support") || have_raft;

    SupportType support_type = config->opt_enum<SupportType>("support_type");
    bool have_support_interface = config->opt_int("support_interface_top_layers") > 0 || config->opt_int("support_interface_bottom_layers") > 0;
    bool have_support_soluble = have_support_material && config->opt_float("support_top_z_distance") == 0;
    auto support_style = config->opt_enum<SupportMaterialStyle>("support_style");
    for (auto el : { "support_style", "support_base_pattern",
        "support_base_pattern_spacing", "support_expansion", "support_angle",
        "support_interface_pattern", "support_interface_top_layers", "support_interface_bottom_layers",
        "bridge_no_support", "max_bridge_length", "support_top_z_distance", "support_bottom_z_distance",
        "support_type", "support_on_build_plate_only", "support_critical_regions_only", "support_interface_not_for_body",
        "support_object_xy_distance", "support_object_first_layer_gap", "independent_support_layer_height"})
        toggle_field(el, have_support_material);
    toggle_field("support_threshold_angle", have_support_material && is_auto(support_type));
    toggle_field("support_threshold_overlap", config->opt_int("support_threshold_angle") == 0 && have_support_material && is_auto(support_type));
    //toggle_field("support_closing_radius", have_support_material && support_style == smsSnug);

    bool support_is_tree = config->opt_bool("enable_support") && is_tree(support_type);
    bool support_is_organic = support_is_tree && (support_style == smsTreeOrganic || support_style == smsDefault);
    bool support_is_normal_tree = support_is_tree && !support_is_organic;

    // hide settings that are not used by tree supports
    toggle_line("support_threshold_overlap", !support_is_tree); // ORCA: tree supports do not use Threshold Overlap
    // settings specific to normal trees
    for (auto el : {"tree_support_branch_angle", "tree_support_branch_distance", "tree_support_branch_diameter", "tree_support_auto_brim", "tree_support_brim_width"})
        toggle_line(el, support_is_normal_tree);
    // settings specific to organic trees
    for (auto el : {"tree_support_branch_angle_organic", "tree_support_branch_distance_organic", "tree_support_branch_diameter_organic", "tree_support_angle_slow", "tree_support_tip_diameter", "tree_support_top_rate", "tree_support_branch_diameter_angle"})
        toggle_line(el, support_is_organic);
    // ORCA: Independent support layer height is not compatible with organic tree supports,
    // as they rely on the support layers being the same as the object layers to determine where to place branches.
    toggle_line("independent_support_layer_height", have_support_material && !support_is_organic);

    toggle_field("tree_support_brim_width", support_is_tree && !config->opt_bool("tree_support_auto_brim"));
    // tree support use max_bridge_length instead of bridge_no_support
    toggle_line("max_bridge_length", support_is_tree);
    toggle_line("bridge_no_support", !support_is_tree);
    toggle_line("support_critical_regions_only", is_auto(support_type) && support_is_tree);

    for (auto el : { "support_interface_filament",
        "support_interface_loop_pattern", "support_bottom_interface_spacing" })
        toggle_field(el, have_support_material && have_support_interface);

    bool can_ironing_support = have_raft || (have_support_material && config->opt_int("support_interface_top_layers") > 0);
    toggle_field("support_ironing", can_ironing_support);
    bool has_support_ironing = can_ironing_support && config->opt_bool("support_ironing");
    for (auto el : {"support_ironing_pattern", "support_ironing_flow", "support_ironing_spacing" })
        toggle_line(el, has_support_ironing);
    // Orca: Force solid support interface when using support ironing
    toggle_field("support_interface_spacing", have_support_material && have_support_interface && !has_support_ironing);

//    see issue #10915
//    bool have_skirt_height = have_skirt &&
//    (config->opt_int("skirt_height") > 1 || config->opt_enum<DraftShield>("draft_shield") != dsEnabled);
//    toggle_line("support_speed", have_support_material || have_skirt_height);
//    toggle_line("support_interface_speed", have_support_material && have_support_interface);

    // Orca:
    for (auto el : {"small_support_perimeter_speed", "small_support_perimeter_threshold"})
        toggle_field(el, config->opt_bool("enable_support"));

    // BBS
    //toggle_field("support_material_synchronize_layers", have_support_soluble);

    toggle_field("inner_wall_line_width", have_perimeters || have_skirt || have_brim);
    toggle_field("support_filament", have_support_material || have_skirt);

    toggle_line("raft_contact_distance", have_raft && !have_support_soluble);
    // Orca:Hide raft expansion when raft is not enabled
    toggle_line("raft_expansion", have_raft);
    // Orca: Slim, Strong and Hybrid tree supports size their raft by first layer expansion only.
    toggle_field("raft_expansion", !(is_tree(support_type) && (support_style == smsTreeSlim || support_style == smsTreeStrong || support_style == smsTreeHybrid)));

    // Orca: First-layer density is available for supports broadly.
    toggle_field("raft_first_layer_density", have_support_material);
    // Orca: For regular tree (Slim/Strong) without raft, hide first-layer expansion.
    // Keep it enabled for non-tree supports, organic tree, hybrid tree, and any raft case.
    toggle_field("raft_first_layer_expansion",
                 have_support_material && ((!support_is_normal_tree || support_style == smsTreeHybrid) || have_raft));

    bool has_ironing = (config->opt_enum<IroningType>("ironing_type") != IroningType::NoIroning);
    for (auto el : { "ironing_pattern", "ironing_flow", "ironing_spacing", "ironing_angle", "ironing_inset", "ironing_angle_fixed" })
        toggle_line(el, has_ironing);
    bool has_rectilinear_ironing = (config->opt_enum<InfillPattern>("ironing_pattern") == InfillPattern::ipRectilinear);
    for (auto el : {"ironing_angle", "ironing_angle_fixed"})
        toggle_field(el, has_ironing && has_rectilinear_ironing);
    
    toggle_line("ironing_speed", has_ironing || has_support_ironing);

    bool has_zaa = config->opt_bool("zaa_enabled");
    for (auto el : {"zaa_minimize_perimeter_height", "zaa_min_z", "zaa_dont_alternate_fill_direction", "ironing_expansion"})
        toggle_line(el, has_zaa);

    bool have_sequential_printing = (config->opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject);
    // for (auto el : { "extruder_clearance_radius", "extruder_clearance_height_to_rod", "extruder_clearance_height_to_lid" })
    //     toggle_field(el, have_sequential_printing);
    toggle_field("print_order", !have_sequential_printing);

    toggle_field("single_extruder_multi_material", !is_BBL_Printer);

    auto bSEMM = (*config).opt_bool("single_extruder_multi_material");
    const bool supports_wipe_tower_2 = !is_BBL_Printer && (*config).opt_enum<WipeTowerType>("wipe_tower_type") == WipeTowerType::Type2;

    toggle_field("ooze_prevention", !bSEMM);
    bool have_ooze_prevention = config->opt_bool("ooze_prevention");
    toggle_line("standby_temperature_delta", have_ooze_prevention);
    toggle_line("preheat_time", have_ooze_prevention);
    int preheat_steps = config->opt_int("preheat_steps");
    toggle_line("preheat_steps", have_ooze_prevention && (preheat_steps > 0));

    bool have_prime_tower = config->opt_bool("enable_prime_tower");
    // ORCA-Belt: belt printers replace the classic wipe tower with the
    // auto-generated belt purge prism, enabled by the belt-only printer option
    // enable_belt_purge_tower. Its width (a process option) is only relevant
    // then. (is_belt_printer is computed at the top of this function.)
    const bool have_belt_purge_tower = is_belt_printer
        && (*config).has("enable_belt_purge_tower")
        && (*config).opt_bool("enable_belt_purge_tower");
    toggle_line("belt_purge_tower_width", have_belt_purge_tower);
    for (auto el : {"prime_tower_width", "prime_tower_brim_width", "prime_tower_skip_points", "wipe_tower_wall_type", "prime_tower_infill_gap","prime_tower_enable_framework", "enable_tower_interface_features"})
        toggle_line(el, have_prime_tower && !is_belt_printer);

    toggle_line("enable_tower_interface_cooldown_during_tower",
                have_prime_tower && !is_belt_printer && config->opt_bool("enable_tower_interface_features"));

    bool purge_in_primetower = (*config).opt_bool("purge_in_prime_tower");

    for (auto el : {"wipe_tower_rotation_angle", "wipe_tower_cone_angle",
                    "wipe_tower_extra_spacing", "wipe_tower_max_purge_speed",
                    "wipe_tower_bridging", "wipe_tower_extra_flow"})
            toggle_line(el, have_prime_tower && supports_wipe_tower_2 && !is_belt_printer);

    // Orca: both tower generators skip sparse layers, so this is not a wipe tower 2 exclusive.
    toggle_line("wipe_tower_no_sparse_layers", have_prime_tower && !is_belt_printer);
    // Dropping the sparse layers outright leaves nothing to combine, so the two are exclusive.
    toggle_line("wipe_tower_sparse_layers_combination", have_prime_tower && !is_belt_printer && !config->opt_bool("wipe_tower_no_sparse_layers"));

    WipeTowerWallType wipe_tower_wall_type = config->opt_enum<WipeTowerWallType>("wipe_tower_wall_type");
    bool have_rib_wall = (wipe_tower_wall_type == WipeTowerWallType::wtwRib)&&have_prime_tower&&!is_belt_printer;
    toggle_line("wipe_tower_cone_angle", have_prime_tower && supports_wipe_tower_2 && !is_belt_printer && wipe_tower_wall_type == WipeTowerWallType::wtwCone);
    toggle_line("wipe_tower_extra_rib_length", have_rib_wall);
    toggle_line("wipe_tower_rib_width", have_rib_wall);
    toggle_line("wipe_tower_fillet_wall", have_rib_wall);
    toggle_field("prime_tower_width", have_prime_tower && !have_rib_wall && !is_belt_printer);

    toggle_line("single_extruder_multi_material_priming", !bSEMM && have_prime_tower && supports_wipe_tower_2 && !is_belt_printer);

    bool use_cyclic_ordering = config->opt_enum<ToolChangeOrderingType>("toolchange_ordering") == ToolChangeOrderingType::Cyclic;
    toggle_line("toolchange_cyclic_order", use_cyclic_ordering);
    toggle_line("toolchange_cyclic_first_layer", use_cyclic_ordering);

    toggle_line("prime_volume",have_prime_tower && (!purge_in_primetower || !bSEMM));

    for (auto el : {"flush_into_infill", "flush_into_support", "flush_into_objects"})
        toggle_field(el, have_prime_tower);

    bool have_avoid_crossing_perimeters = config->opt_bool("reduce_crossing_wall");
    toggle_line("max_travel_detour_distance", have_avoid_crossing_perimeters);

    bool has_set_other_flow_ratios = config->opt_bool("set_other_flow_ratios");
    for (auto el : {"first_layer_flow_ratio", "outer_wall_flow_ratio", "inner_wall_flow_ratio", "overhang_flow_ratio", "sparse_infill_flow_ratio", "internal_solid_infill_flow_ratio", "gap_fill_flow_ratio", "support_flow_ratio", "support_interface_flow_ratio"})
        toggle_line(el, has_set_other_flow_ratios);

    bool has_overhang_speed = config->opt_bool_nullable("enable_overhang_speed", variant_index);
    for (auto el : {"overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed"})
        toggle_line(el, has_overhang_speed, variant_index);

    toggle_line("slowdown_for_curled_perimeters", has_overhang_speed, variant_index);

    toggle_line("flush_into_objects", !is_global_config);

    toggle_line("support_interface_not_for_body",config->opt_int("support_interface_filament")&&!config->opt_int("support_filament"));

    // Get the current fuzzy skin state
    bool has_fuzzy_skin = config->opt_enum<FuzzySkinType>("fuzzy_skin") != FuzzySkinType::Disabled_fuzzy;
    
    // Show fuzzy skin options when fuzzy skin is not disabled
    for (auto el : {"fuzzy_skin_mode", "fuzzy_skin_noise_type", "fuzzy_skin_point_distance", "fuzzy_skin_thickness", "fuzzy_skin_first_layer"})
        toggle_line(el, has_fuzzy_skin);
    
    // Show noise type specific options with the same logic
    NoiseType fuzzy_skin_noise_type = config->opt_enum<NoiseType>("fuzzy_skin_noise_type");
    const bool is_ripple = fuzzy_skin_noise_type == NoiseType::Ripple;
    toggle_line("fuzzy_skin_scale", fuzzy_skin_noise_type != NoiseType::Classic && has_fuzzy_skin && !is_ripple);
    toggle_line("fuzzy_skin_octaves", fuzzy_skin_noise_type != NoiseType::Classic && fuzzy_skin_noise_type != NoiseType::Voronoi && has_fuzzy_skin && !is_ripple);
    toggle_line("fuzzy_skin_persistence", (fuzzy_skin_noise_type == NoiseType::Perlin || fuzzy_skin_noise_type == NoiseType::Billow) && has_fuzzy_skin && !is_ripple);
    toggle_line("fuzzy_skin_ripples_per_layer", is_ripple && has_fuzzy_skin);
    toggle_line("fuzzy_skin_ripple_offset", is_ripple && has_fuzzy_skin);
    toggle_line("fuzzy_skin_layers_between_ripple_offset", is_ripple && has_fuzzy_skin);

    bool have_arachne = config->opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Arachne;
    for (auto el : {"wall_transition_length", "wall_transition_filter_deviation", "wall_transition_angle", "min_feature_size", "min_length_factor",
        "min_bead_width", "wall_distribution_count", "initial_layer_min_bead_width", "wall_maximum_resolution", "wall_maximum_deviation"})
        toggle_line(el, have_arachne);
    toggle_field("detect_thin_wall", !have_arachne);

    // Orca
    auto is_role_based_wipe_speed = config->opt_bool("role_based_wipe_speed");
    toggle_field("wipe_speed",!is_role_based_wipe_speed);

    const bool have_wipe_inward = config->opt_bool("wipe_inward");
    toggle_line("wipe_inward_distance", have_wipe_inward);

    for (auto el : {"accel_to_decel_enable", "accel_to_decel_factor"})
        toggle_line(el, gcf_is_klipper);
    if(gcf_is_klipper)
        toggle_field("accel_to_decel_factor", config->opt_bool("accel_to_decel_enable"));

    bool have_make_overhang_printable = config->opt_bool("make_overhang_printable");
    toggle_line("make_overhang_printable_angle", have_make_overhang_printable);
    toggle_line("make_overhang_printable_hole_size", have_make_overhang_printable);

    // Orca: the one-wall options act on top/bottom surfaces, which exist only with a shell. An unfilled surface
    // (0% surface density) is still a surface, so these are gated on the layer counts alone.
    toggle_line("only_one_wall_first_layer", has_bottom_shell);
    toggle_line("only_one_wall_top", has_top_shell_layers);
    toggle_line("min_width_top_surface", (has_top_shell_layers && config->opt_bool("only_one_wall_top")) || ((config->opt_float("min_length_factor") > 0.5f) && have_arachne)); // 0.5 is default value

    for (auto el : { "hole_to_polyhole_threshold", "hole_to_polyhole_twisted", "hole_to_polyhole_max_edges" })
        toggle_line(el, config->opt_bool("hole_to_polyhole"));

    bool has_detect_overhang_wall = config->opt_bool("detect_overhang_wall");
    bool has_overhang_reverse     = config->opt_bool("overhang_reverse");
    bool allow_overhang_reverse   = !has_spiral_vase;
    toggle_line("unsupported_wall_last", has_detect_overhang_wall);
    toggle_line("overhang_reverse", allow_overhang_reverse);
    toggle_line("overhang_reverse_internal_only", allow_overhang_reverse && has_overhang_reverse);
    bool has_overhang_reverse_internal_only = config->opt_bool("overhang_reverse_internal_only");
    if (has_overhang_reverse_internal_only){
        force("overhang_reverse_threshold", new ConfigOptionFloatOrPercent(0,true));
    }
    toggle_line("overhang_reverse_threshold", has_detect_overhang_wall && allow_overhang_reverse && has_overhang_reverse && !has_overhang_reverse_internal_only);
    toggle_line("timelapse_type", is_BBL_Printer);


    bool have_small_area_infill_flow_compensation = config->opt_bool("small_area_infill_flow_compensation");
    toggle_line("small_area_infill_flow_compensation_model", have_small_area_infill_flow_compensation);


    // Belt printers: the scarf would start one layer back along the belt, inside the
    // previous layer (GCode::extrude_loop skips it there too).
    toggle_field("seam_slope_type", !has_spiral_vase && !is_belt_printer);
    bool has_seam_slope = !has_spiral_vase && !is_belt_printer && config->opt_enum<SeamScarfType>("seam_slope_type") != SeamScarfType::None;
    toggle_line("seam_slope_conditional", has_seam_slope);
    toggle_line("seam_slope_start_height", has_seam_slope);
    toggle_line("seam_slope_entire_loop", has_seam_slope);
    toggle_line("seam_slope_min_length", has_seam_slope);
    toggle_line("seam_slope_steps", has_seam_slope);
    toggle_line("seam_slope_inner_walls", has_seam_slope);
    toggle_line("scarf_joint_speed", has_seam_slope);
    toggle_line("scarf_joint_flow_ratio", has_seam_slope);
    toggle_field("seam_slope_min_length", !config->opt_bool("seam_slope_entire_loop"));
    toggle_line("scarf_angle_threshold", has_seam_slope && config->opt_bool("seam_slope_conditional"));
    toggle_line("scarf_overhang_threshold", has_seam_slope && config->opt_bool("seam_slope_conditional"));

    bool use_beam_interlocking = config->opt_bool("interlocking_beam");
    toggle_line("mmu_segmented_region_interlocking_depth", !use_beam_interlocking);
    toggle_line("interlocking_beam_width", use_beam_interlocking);
    toggle_line("interlocking_orientation", use_beam_interlocking);
    toggle_line("interlocking_beam_layer_count", use_beam_interlocking);
    toggle_line("interlocking_depth", use_beam_interlocking);
    toggle_line("interlocking_boundary_avoidance", use_beam_interlocking);

    bool lattice_options = config->opt_enum<InfillPattern>("sparse_infill_pattern") == InfillPattern::ipLateralLattice;
    for (auto el : { "lateral_lattice_angle_1", "lateral_lattice_angle_2"})
        toggle_line(el, lattice_options);

    bool lightning_options = config->opt_enum<InfillPattern>("sparse_infill_pattern") == InfillPattern::ipLightning;
    for (auto el : { "lightning_overhang_angle", "lightning_prune_angle", "lightning_straightening_angle" })
        toggle_line(el, lightning_options);
        
    // Adaptative Cubic and support cubic infill patterns do not support infill rotation.
    bool FillAdaptive = (pattern == InfillPattern::ipAdaptiveCubic || pattern == InfillPattern::ipSupportCubic);

    //Orca: disable infill_direction/solid_infill_direction if sparse_infill_rotate_template/solid_infill_rotate_template is not empty value and adaptive cubic/support cubic infill pattern is not selected
    toggle_field("sparse_infill_rotate_template", !FillAdaptive);
    toggle_field("infill_direction", config->opt_string("sparse_infill_rotate_template") == "" && !FillAdaptive);
    toggle_field("solid_infill_direction", config->opt_string("solid_infill_rotate_template") == "");
    
    toggle_line("infill_overhang_angle", config->opt_enum<InfillPattern>("sparse_infill_pattern") == InfillPattern::ipLateralHoneycomb);

    // Wrapping detection is a Bambu Lab device capability (the GUI looks the printer model up in its device tables); the
    // headless engine has no such tables, so the option is offered only to Bambu printers.
    toggle_line("enable_wrapping_detection", is_BBL_Printer);

    return out;
}

namespace {
// The printer and filament rules read some settings that a full print configuration may lack (printer-only flags);
// a missing or empty option reads as its default, as an unset option does in the GUI.
bool get_bool(const DynamicPrintConfig& c, const char* key, int idx = -1) {
    if (const ConfigOption* o = c.option(key)) {
        if (auto* v = dynamic_cast<const ConfigOptionBools*>(o)) return !v->values.empty() && v->get_at(size_t(std::max(idx, 0)));
        if (auto* b = dynamic_cast<const ConfigOptionBool*>(o)) return b->value;
    }
    return false;
}
double get_float(const DynamicPrintConfig& c, const char* key, int idx = -1) {
    if (const ConfigOption* o = c.option(key)) {
        if (auto* v = dynamic_cast<const ConfigOptionFloats*>(o)) return v->values.empty() ? 0. : v->get_at(size_t(std::max(idx, 0)));
        if (auto* f = dynamic_cast<const ConfigOptionFloat*>(o)) return f->value;
    }
    return 0.;
}
int get_int(const DynamicPrintConfig& c, const char* key, int idx = -1) {
    if (const ConfigOption* o = c.option(key)) {
        if (auto* v = dynamic_cast<const ConfigOptionInts*>(o)) return v->values.empty() ? 0 : v->get_at(size_t(std::max(idx, 0)));
        if (auto* i = dynamic_cast<const ConfigOptionInt*>(o)) return i->value;
        if (auto* e = dynamic_cast<const ConfigOptionEnumsGeneric*>(o)) return e->values.empty() ? 0 : e->get_at(size_t(std::max(idx, 0)));
    }
    return 0;
}
} // namespace
// ---- Printer settings: TabPrinter::toggle_options (src/slic3r/GUI/Tab.cpp), every page ------------------------------
OptionStates printer_option_states(const DynamicPrintConfig& input, const OptionContext& context) {
    OptionStates out;
    DynamicPrintConfig cfg = input;
    DynamicPrintConfig* m_config = &cfg;
    const bool is_BBL_printer = context.is_bbl_printer;
    auto key_at = [](const std::string& key, int i) { return i < 0 ? key : key + "#" + std::to_string(i); };
    auto toggle_option = [&](const std::string& key, bool on, int i = -1) { out.states[key_at(key, i)].enabled = on; };
    auto toggle_line = [&](const std::string& key, bool on) { out.states[key].visible = on; };
    auto force = [&](const std::string& key, ConfigOption* value) {
        m_config->set_key_value(key, value);
        out.forced.emplace_back(key, m_config->option(key)->serialize());
    };

    bool have_multiple_extruders = true;

    // Basic information
    // SoftFever: hide BBL specific settings
    for (auto el : {"scan_first_layer", "bbl_calib_mark_logo", "bbl_use_printhost"})
        toggle_line(el, is_BBL_printer);
    // SoftFever: hide non-BBL settings
    for (auto el : {"use_firmware_retraction", "use_relative_e_distances", "support_multi_bed_types", "pellet_modded_printer", "bed_mesh_max", "bed_mesh_min", "bed_mesh_probe_distance", "adaptive_bed_mesh_margin", "thumbnails"})
        toggle_line(el, !is_BBL_printer);

    // Multimaterial
    // SoftFever: hide specific settings for BBL printer
    for (auto el : {"enable_filament_ramming", "cooling_tube_retraction", "cooling_tube_length", "parking_pos_retraction",
                    "extra_loading_move", "high_current_on_filament_swap"})
        toggle_option(el, !is_BBL_printer);
    auto bSEMM = get_bool(*m_config, "single_extruder_multi_material");
    if (!bSEMM && get_bool(*m_config, "manual_filament_change"))
        force("manual_filament_change", new ConfigOptionBool(false));
    toggle_option("extruders_count", !bSEMM);
    toggle_option("manual_filament_change", bSEMM);
    toggle_option("purge_in_prime_tower", bSEMM && !is_BBL_printer);

    // Extruder pages
    const int extruders = int(m_config->option<ConfigOptionFloats>("nozzle_diameter")->values.size());
    bool wipe_with_firmware_retraction = false;
    for (int i = 0; i < extruders; ++i) {
        bool have_retract_length = get_float(*m_config, "retraction_length", int(i)) > 0;
        // when using firmware retraction, firmware decides retraction length
        bool use_firmware_retraction = get_bool(*m_config, "use_firmware_retraction");
        toggle_option("retract_length", !use_firmware_retraction, i);
        // user can customize travel length if we have retraction length or we"re using firmware retraction
        toggle_option("retraction_minimum_travel", have_retract_length || use_firmware_retraction, i);
        // user can customize other retraction options if retraction is enabled
        bool retraction = have_retract_length || use_firmware_retraction;
        for (auto el : {"z_hop", "retract_when_changing_layer"})
            toggle_option(el, retraction, i);
        // retract lift above / below + enforce only applies if using retract lift
        for (auto el : {"retract_lift_above", "retract_lift_below", "retract_lift_enforce"})
            toggle_option(el, retraction && (get_float(*m_config, "z_hop", int(i)) > 0), i);
        // some options only apply when not using firmware retraction
        for (auto el : {"retraction_speed", "deretraction_speed", "retract_before_wipe", "retract_length", "retract_restart_extra", "wipe", "wipe_distance"})
            toggle_option(el, retraction && !use_firmware_retraction, i);
        bool wipe = retraction && get_bool(*m_config, "wipe", int(i));
        toggle_option("retract_before_wipe", wipe, i);
        if (use_firmware_retraction && wipe) wipe_with_firmware_retraction = true;
        toggle_option("wipe_distance", wipe, i);
        toggle_option("retract_length_toolchange", have_multiple_extruders, i);
        bool toolchange_retraction = get_float(*m_config, "retract_length_toolchange", int(i)) > 0;
        toggle_option("retract_restart_extra_toolchange", have_multiple_extruders && toolchange_retraction, i);
        toggle_option("long_retractions_when_cut", !use_firmware_retraction && get_int(*m_config, "enable_long_retraction_when_cut"), i);
        toggle_line(key_at("retraction_distances_when_cut", i), get_bool(*m_config, "long_retractions_when_cut", int(i)));
        toggle_option("travel_slope", get_int(*m_config, "z_hop_types", i) != ZHopType::zhtNormal, i);
    }
    if (wipe_with_firmware_retraction) {
        // Orca asks with a dialog; the caller asks in its own UI.
        OptionConflict c;
        c.id = "wipe_with_firmware_retraction";
        c.message = "The Wipe option is not available when using the Firmware Retraction mode.";
        OptionConflict::Choice no_wipe { "Disable Wipe", {} };
        std::string wipes;
        for (int i = 0; i < extruders; ++i) wipes += (i ? ",0" : "0");
        no_wipe.changes.emplace_back("wipe", wipes);
        c.choices.push_back(no_wipe);
        c.choices.push_back({ "Disable Firmware Retraction", {{"use_firmware_retraction", "0"}} });
        out.conflicts.push_back(c);
    }

    // Motion ability
    auto gcf = m_config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    bool silent_mode = get_bool(*m_config, "silent_mode");
    int  max_field   = silent_mode ? 2 : 1;
    for (int i = 0; i < max_field; ++i)
        toggle_option("machine_max_acceleration_travel", gcf != gcfMarlinLegacy && gcf != gcfKlipper, i);
    toggle_line("machine_max_acceleration_travel", gcf != gcfMarlinLegacy && gcf != gcfKlipper);
    for (int i = 0; i < max_field; ++i)
        toggle_option("machine_max_junction_deviation", gcf == gcfMarlinFirmware, i);
    toggle_line("machine_max_junction_deviation", gcf == gcfMarlinFirmware);
    bool resonance_avoidance = get_bool(*m_config, "resonance_avoidance");
    toggle_option("min_resonance_avoidance_speed", resonance_avoidance);
    toggle_option("max_resonance_avoidance_speed", resonance_avoidance);
    return out;
}

namespace {
// Preset::get_default_bed_type without the preset bundle: the printer's default_bed_type, else the vendor model id.
BedType default_bed_type(const DynamicPrintConfig& cfg, const std::string& model_id) {
    if (cfg.has("default_bed_type") && !cfg.opt_string("default_bed_type").empty()) {
        const int value = std::atoi(cfg.opt_string("default_bed_type").c_str());
        if (value != 0) return BedType(value);
    }
    if (model_id == "BL-P001" || model_id == "BL-P002" || model_id == "C13") return BedType::btPC;
    if (model_id == "C11") return BedType::btPEI;
    if (model_id == "SM_U1") return BedType::btPTE;
    return BedType::btPEI;
}
bool contains_nocase(std::string haystack, std::string needle) {
    for (auto& c : haystack) c = char(std::tolower((unsigned char) c));
    for (auto& c : needle) c = char(std::tolower((unsigned char) c));
    return haystack.find(needle) != std::string::npos;
}
} // namespace

// ---- Filament settings: TabFilament::toggle_options (src/slic3r/GUI/Tab.cpp), every page -----------------------------
OptionStates filament_option_states(const DynamicPrintConfig& input, const OptionContext& context) {
    OptionStates out;
    const DynamicPrintConfig& cfg = input;   // the printer's settings are part of the full configuration
    const DynamicPrintConfig* m_config = &input;
    const bool is_BBL_printer = context.is_bbl_printer;
    auto toggle_option = [&](const std::string& key, bool on) { out.states[key].enabled = on; };
    auto toggle_line = [&](const std::string& key, bool on) { out.states[key].visible = on; };

    // Cooling
    bool has_enable_overhang_bridge_fan = get_bool(*m_config, "enable_overhang_bridge_fan", int(0));
    for (auto el : {"overhang_fan_speed", "overhang_fan_threshold", "internal_bridge_fan_speed"}) // ORCA: Add support for separate internal bridge fan speed control
        toggle_option(el, has_enable_overhang_bridge_fan);
    toggle_option("additional_cooling_fan_speed", get_bool(cfg, "auxiliary_fan"));
    // Orca: toggle dont slow down for external perimeters if
    bool has_slow_down_for_layer_cooling = get_bool(*m_config, "slow_down_for_layer_cooling", int(0));
    toggle_option("dont_slow_down_outer_wall", has_slow_down_for_layer_cooling);

    // Filament
    const size_t flow_index = context.flow_variant_index;
    {
        const auto* pa_opt = m_config->option<ConfigOptionBools>("enable_pressure_advance");
        bool pa = pa_opt != nullptr && !pa_opt->values.empty() && pa_opt->get_at(std::min(flow_index, pa_opt->values.size() - 1));
        toggle_option("pressure_advance", pa);
    }
    // BBS: bed temperature rows per plate type
    auto support_multi_bed_types = is_BBL_printer || get_bool(cfg, "support_multi_bed_types");
    bool is_snapmaker_u1 = contains_nocase(context.printer_name, "Snapmaker U1");
    if (auto printer_model_opt = cfg.option<ConfigOptionString>("printer_model")) {
        const std::string& printer_model = printer_model_opt->value;
        is_snapmaker_u1 = is_snapmaker_u1 || (contains_nocase(printer_model, "Snapmaker") && contains_nocase(printer_model, "U1"));
    }
    if (is_snapmaker_u1 && !support_multi_bed_types) {
        // U1 default show 3 plates; Cool Steel Plate only appears with support_multi_bed_types
        for (auto el : {"supertack_plate_temp_initial_layer", "supertack_plate_temp", "cool_plate_temp_initial_layer", "cool_plate_temp",
                        "textured_cool_plate_temp_initial_layer", "textured_cool_plate_temp", "eng_plate_temp_initial_layer", "eng_plate_temp"})
            toggle_line(el, false);
        for (auto el : {"hot_plate_temp_initial_layer", "hot_plate_temp", "textured_plate_temp_initial_layer", "textured_plate_temp",
                        "graphic_effect_plate_temp_initial_layer", "graphic_effect_plate_temp"})
            toggle_line(el, true);
    } else if (support_multi_bed_types) {
        for (auto el : {"supertack_plate_temp_initial_layer", "cool_plate_temp", "cool_plate_temp_initial_layer",
                        "textured_cool_plate_temp_initial_layer", "textured_cool_plate_temp", "eng_plate_temp_initial_layer", "eng_plate_temp",
                        "hot_plate_temp_initial_layer", "hot_plate_temp", "textured_plate_temp_initial_layer", "textured_plate_temp"})
            toggle_line(el, true);
        toggle_line("graphic_effect_plate_temp_initial_layer", is_snapmaker_u1);
        toggle_line("graphic_effect_plate_temp", is_snapmaker_u1);
    } else {
        BedType curr_bed_type = default_bed_type(cfg, context.printer_model_id);
        toggle_line("supertack_plate_temp_initial_layer", curr_bed_type == btSuperTack);
        toggle_line("supertack_plate_temp", curr_bed_type == btSuperTack);
        toggle_line("cool_plate_temp_initial_layer", curr_bed_type == btPC);
        toggle_line("cool_plate_temp", curr_bed_type == btPC);
        toggle_line("textured_cool_plate_temp_initial_layer", curr_bed_type == btPCT);
        toggle_line("textured_cool_plate_temp", curr_bed_type == btPCT);
        toggle_line("eng_plate_temp_initial_layer", curr_bed_type == btEP);
        toggle_line("eng_plate_temp", curr_bed_type == btEP);
        toggle_line("hot_plate_temp_initial_layer", curr_bed_type == btPEI);
        toggle_line("hot_plate_temp", curr_bed_type == btPEI);
        toggle_line("textured_plate_temp_initial_layer", curr_bed_type == btPTE);
        toggle_line("textured_plate_temp", curr_bed_type == btPTE);
    }
    bool is_pellet_printer = get_bool(cfg, "pellet_modded_printer");
    toggle_line("pellet_flow_coefficient", is_pellet_printer);
    toggle_line("filament_diameter", !is_pellet_printer);
    bool support_chamber_temp_control = get_bool(cfg, "support_chamber_temp_control");
    toggle_line("chamber_temperatures", support_chamber_temp_control);

    // Multimaterial
    // Orca: hide specific settings for BBL printers
    for (auto el : {"filament_minimal_purge_on_wipe_tower", "filament_loading_speed_start", "filament_loading_speed",
                    "filament_unloading_speed_start", "filament_unloading_speed", "filament_toolchange_delay", "filament_cooling_moves",
                    "filament_cooling_initial_speed", "filament_cooling_final_speed"})
        toggle_option(el, !is_BBL_printer);
    {
        const auto* ramming = m_config->option<ConfigOptionBools>("filament_multitool_ramming");
        bool multitool_ramming = ramming != nullptr && !ramming->values.empty() && ramming->get_at(std::min(flow_index, ramming->values.size() - 1));
        toggle_option("filament_multitool_ramming_volume", multitool_ramming);
        toggle_option("filament_multitool_ramming_flow", multitool_ramming);
    }
    return out;
}

// ---- Checks: ConfigManipulation::update_print_fff_config (src/slic3r/GUI/ConfigManipulation.cpp) ----------------------
// Kept in the GUI's order. Each OK-only dialog becomes a notice (the reset is applied to the working copy so later
// checks see it, as in the GUI); each Yes/No dialog becomes a conflict whose first choice is the GUI's "Yes". Later
// checks see the configuration as if the first choice were taken.
OptionStates print_config_checks(const DynamicPrintConfig& input, const OptionContext& context) {
    OptionStates out;
    DynamicPrintConfig cfg = input;
    DynamicPrintConfig* config = &cfg;
    auto ser = [&](const std::string& key) { return config->option(key)->serialize(); };
    auto notice = [&](const std::string& id, const std::string& message, std::vector<std::pair<std::string, ConfigOption*>> sets) {
        OptionNotice n { id, message, {} };
        for (auto& [key, value] : sets) { config->set_key_value(key, value); n.changes.emplace_back(key, ser(key)); }
        out.notices.push_back(n);
    };
    auto conflict = [&](const std::string& id, const std::string& message,
                        std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> choices) {
        OptionConflict c { id, message, {} };
        for (auto& [label, changes] : choices) c.choices.push_back({ label, changes });
        out.conflicts.push_back(c);
        for (auto& [key, value] : choices.front().second) config->set_deserialize_strict(key, value);
    };

    // layer_height shouldn't be equal to zero
    auto layer_height = config->opt_float("layer_height");
    if (layer_height < EPSILON)
        notice("layer_height_too_small", "Too small layer height. Reset to 0.2.", {{"layer_height", new ConfigOptionFloat(0.2)}});
    layer_height = config->opt_float("layer_height");
    //BBS: limite the max layer_herght
    auto max_lh = get_float(*config, "max_layer_height", 0);
    if (max_lh > 0.2 && layer_height > max_lh + EPSILON) {
        char msg[96]; std::snprintf(msg, sizeof msg, "Too large layer height. Reset to %0.3f.", max_lh);
        notice("layer_height_too_large", msg, {{"layer_height", new ConfigOptionFloat(max_lh)}});
    }
    if (config->opt_float("ironing_spacing") < 0.05)
        notice("ironing_spacing_too_small", "Too small ironing spacing. Reset to 0.1.", {{"ironing_spacing", new ConfigOptionFloat(0.1)}});
    if (config->opt_float("support_ironing_spacing") < 0.05)
        notice("support_ironing_spacing_too_small", "Too small ironing spacing. Reset to 0.1.", {{"support_ironing_spacing", new ConfigOptionFloat(0.1)}});
    if (config->option<ConfigOptionFloat>("initial_layer_print_height")->value < EPSILON)
        notice("initial_layer_height_zero", "Zero initial layer height is invalid. The first layer height will be reset to 0.2.",
               {{"initial_layer_print_height", new ConfigOptionFloat(0.2)}});
    const char* small_tuning = "This setting is only used for model size tunning with small value in some cases. For example, when model "
                               "size has small error and hard to be assembled. For large size tuning, please use model scale function. "
                               "The value will be reset to 0.";
    if (std::abs(config->option<ConfigOptionFloat>("xy_hole_compensation")->value) > 2)
        notice("xy_hole_compensation_too_large", small_tuning, {{"xy_hole_compensation", new ConfigOptionFloat(0)}});
    if (std::abs(config->option<ConfigOptionFloat>("xy_contour_compensation")->value) > 2)
        notice("xy_contour_compensation_too_large", small_tuning, {{"xy_contour_compensation", new ConfigOptionFloat(0)}});
    if (config->option<ConfigOptionFloat>("elefant_foot_compensation")->value > 1)
        notice("elephant_foot_too_large", "Too large elephant foot compensation is unreasonable. If really have serious elephant foot "
               "effect, please check other settings. For example, whether bed temperature is too high. The value will be reset to 0.",
               {{"elefant_foot_compensation", new ConfigOptionFloat(0)}});

    if (!context.is_plate_config &&
        config->opt_bool("spiral_mode") &&
        ! (config->opt_int("wall_loops") == 1 &&
           config->opt_int("top_shell_layers") == 0 &&
           config->option<ConfigOptionPercent>("sparse_infill_density")->value == 0 &&
           ! config->opt_bool("enable_support") &&
           config->opt_int("enforce_support_layers") == 0 &&
           ! config->opt_bool("detect_thin_wall") &&
           ! config->opt_bool("overhang_reverse") &&
            config->opt_enum<TimelapseType>("timelapse_type") == TimelapseType::tlTraditional))
        conflict("spiral_mode_settings",
                 "Spiral mode only works when wall loops is 1, support is disabled, top shell layers is 0, sparse infill density is 0 "
                 "and timelapse type is traditional.",
                 { { "Change these settings", { {"wall_loops", "1"}, {"top_shell_layers", "0"}, {"sparse_infill_density", "0%"},
                     {"enable_support", "0"}, {"enforce_support_layers", "0"}, {"detect_thin_wall", "0"}, {"overhang_reverse", "0"},
                     {"wall_direction", "auto"}, {"timelapse_type", "0"} } },
                   { "Turn off spiral mode", { {"spiral_mode", "0"} } } });

    if (config->opt_bool("alternate_extra_wall") &&
        (config->opt_enum<EnsureVerticalShellThickness>("ensure_vertical_shell_thickness") == evstAll)) {
        const char* msg = "Alternate extra wall does't work well when ensure vertical shell thickness is set to All.";
        if (context.is_global_config)
            conflict("alternate_extra_wall_vertical_shell", msg,
                     { { "Change ensure vertical shell thickness to Moderate", { {"ensure_vertical_shell_thickness", "ensure_moderate"}, {"alternate_extra_wall", "1"} } },
                       { "Don't use alternate extra wall", { {"ensure_vertical_shell_thickness", "ensure_all"}, {"alternate_extra_wall", "0"} } } });
        else
            notice("alternate_extra_wall_vertical_shell", msg,
                   {{"ensure_vertical_shell_thickness", new ConfigOptionEnum<EnsureVerticalShellThickness>(evstModerate)},
                    {"alternate_extra_wall", new ConfigOptionBool(true)}});
    }

    // Orca enables detect_overhang_wall once per session when support is switched on in the global settings.
    if (context.is_global_config && config->opt_bool("enable_support") && !config->opt_bool("detect_overhang_wall"))
        notice("support_needs_overhang_detection", "Support uses overhang wall detection; it has been turned on.",
               {{"detect_overhang_wall", new ConfigOptionBool(true)}});

    if (config->opt_bool("enable_support")) {
        auto   support_type = config->opt_enum<SupportType>("support_type");
        auto   support_style = config->opt_enum<SupportMaterialStyle>("support_style");
        std::set<int> enum_set_normal = { smsDefault, smsGrid, smsSnug };
        std::set<int> enum_set_tree   = { smsDefault, smsTreeSlim, smsTreeStrong, smsTreeHybrid, smsTreeOrganic };
        auto &           set             = is_tree(support_type) ? enum_set_tree : enum_set_normal;
        if (set.find(support_style) == set.end())
            notice("support_style_mismatch", "This support style doesn't apply to the chosen support type; it was reset to Default.",
                   {{"support_style", new ConfigOptionEnum<SupportMaterialStyle>(smsDefault)}});
    }

    // BBS
    for (const char* key : { "support_filament", "support_interface_filament" }) {
        auto* opt = dynamic_cast<const ConfigOptionInt*>(config->option(key));
        if (opt != nullptr && opt->getInt() > context.filament_count)
            notice(std::string(key) + "_out_of_range", "The chosen filament doesn't exist in this project; it was reset to Default.",
                   {{key, new ConfigOptionInt(0)}});
    }

    if (config->opt_enum<SeamScarfType>("seam_slope_type") != SeamScarfType::None &&
        config->get_abs_value("seam_slope_start_height") >= layer_height)
        notice("seam_slope_start_height_too_large", "seam_slope_start_height need to be smaller than layer_height. Reset to 0.",
               {{"seam_slope_start_height", new ConfigOptionFloatOrPercent(0, false)}});

    float skin_depth = config->opt_float("skin_infill_depth");
    if (config->opt_float("infill_lock_depth") > skin_depth)
        notice("infill_lock_depth_too_large", "Lock depth should smaller than skin depth. Reset to 50% of skin depth.",
               {{"infill_lock_depth", new ConfigOptionFloat(skin_depth / 2)}});

    bool have_arachne = config->opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Arachne;
    if (config->opt_enum<FuzzySkinMode>("fuzzy_skin_mode") != FuzzySkinMode::Displacement && !have_arachne)
        conflict("fuzzy_skin_needs_arachne",
                 "Both [Extrusion] and [Combined] modes of Fuzzy Skin require the Arachne Wall Generator to be enabled.",
                 { { "Enable Arachne Wall Generator", { {"wall_generator", "arachne"} } },
                   { "Use Displacement fuzzy skin mode", { {"fuzzy_skin_mode", "displacement"} } } });
    return out;
}

OptionStates all_option_states(const DynamicPrintConfig& config, const OptionContext& context) {
    OptionStates out = printer_option_states(config, context);
    // Printer rules may force values (manual_filament_change) that the others then read.
    DynamicPrintConfig applied = config;
    for (const auto& [key, value] : out.forced) applied.set_deserialize_strict(key, value);
    for (OptionStates part : { filament_option_states(applied, context), print_option_states(applied, context) }) {
        for (auto& [key, state] : part.states) out.states[key] = state;
        for (auto& f : part.forced) out.forced.push_back(f);
        for (auto& c : part.conflicts) out.conflicts.push_back(c);
    }
    return out;
}

} // namespace engine
