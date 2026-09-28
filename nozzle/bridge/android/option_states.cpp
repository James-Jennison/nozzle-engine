// Port of ConfigManipulation::toggle_print_fff_options (src/slic3r/GUI/ConfigManipulation.cpp, this engine's base:
// Snapmaker Orca cbf7bbb) to a function of the configuration alone. The rules are kept in the GUI's order, with the
// GUI's comments, so a later upstream change to that function can be carried over line by line. Differences:
//   - toggle_field/toggle_line record into `states` instead of changing widgets (a later rule overrides an earlier one
//     for the same key, as it does in the GUI);
//   - values the GUI forces through apply()/set_key_value are reported in `forced`, and the rules that follow read
//     the forced value, as they do in the GUI;
//   - what the GUI reads from the edited printer preset (G-code flavour, single-extruder multi-material, purge in the
//     prime tower) is read from the full configuration, and whether the printer is a Bambu Lab one comes from the caller.
#include "option_states.hpp"

#include <algorithm>

namespace engine {
using namespace Slic3r;

OptionStates print_option_states(const DynamicPrintConfig& input, const OptionContext& context) {
    OptionStates out;
    DynamicPrintConfig cfg = input;
    DynamicPrintConfig* config = &cfg;
    const bool is_BBL_Printer = context.is_bbl_printer;
    const bool is_global_config = context.is_global_config;
    const size_t flow_variant_index = context.flow_variant_index;

    auto toggle_field = [&](const std::string& key, bool on) { out.states[key].enabled = on; };
    auto toggle_line = [&](const std::string& key, bool on) { out.states[key].visible = on; };
    auto force = [&](const std::string& key, ConfigOption* value) {
        config->set_key_value(key, value);
        const std::string text = config->option(key)->serialize();
        const auto it = std::find_if(out.forced.begin(), out.forced.end(), [&](const auto& f) { return f.first == key; });
        if (it != out.forced.end()) it->second = text; else out.forced.emplace_back(key, text);
    };

    auto gcflavor = config->option<ConfigOptionEnum<GCodeFlavor>>("gcode_flavor")->value;
    const bool bSEMM = config->opt_bool("single_extruder_multi_material");

    auto process_flow_float = [config, flow_variant_index](const char *key, double fallback) {
        const auto *option = config->option<ConfigOptionFloats>(key);
        if (option == nullptr || option->values.empty())
            return fallback;
        return option->get_at(std::min(flow_variant_index, option->values.size() - 1));
    };

    bool have_volumetric_extrusion_rate_slope = process_flow_float("max_volumetric_extrusion_rate_slope", 0) > 0;
    float have_volumetric_extrusion_rate_slope_segment_length = process_flow_float("max_volumetric_extrusion_rate_slope_segment_length", 0);
    toggle_field("enable_arc_fitting", !have_volumetric_extrusion_rate_slope);
    toggle_line("max_volumetric_extrusion_rate_slope_segment_length", have_volumetric_extrusion_rate_slope);
    toggle_line("extrusion_rate_smoothing_external_perimeter_only", have_volumetric_extrusion_rate_slope);
    if(have_volumetric_extrusion_rate_slope) force("enable_arc_fitting", new ConfigOptionBool(false));
    if(have_volumetric_extrusion_rate_slope_segment_length < 0.5) {
        force("max_volumetric_extrusion_rate_slope_segment_length", new ConfigOptionFloats { 1. });
    }

    bool have_perimeters = config->opt_int("wall_loops") > 0;
    for (auto el : { "extra_perimeters_on_overhangs", "ensure_vertical_shell_thickness", "detect_thin_wall", "detect_overhang_wall",
        "seam_position", "staggered_inner_seams", "wall_sequence", "outer_wall_line_width",
        "inner_wall_speed", "outer_wall_speed", "small_perimeter_speed", "small_perimeter_threshold" })
        toggle_field(el, have_perimeters);

    bool have_infill = config->option<ConfigOptionPercent>("sparse_infill_density")->value > 0;
    // sparse_infill_filament uses the same logic as in Print::extruders()
    for (auto el : { "sparse_infill_pattern", "infill_combination",
        "minimum_sparse_infill_area", "infill_anchor_max","infill_shift_step","sparse_infill_rotate_template","symmetric_infill_y_axis"})
        toggle_line(el, have_infill);

    bool have_combined_infill = config->opt_bool("infill_combination") && have_infill;
    toggle_line("infill_combination_max_layer_height", have_combined_infill);

    // Infill patterns that support multiline infill.
    InfillPattern pattern = config->opt_enum<InfillPattern>("sparse_infill_pattern");
    bool          have_multiline_infill_pattern = pattern == ipGyroid || pattern == ipGrid || pattern == ipRectilinear || pattern == ipTpmsD || pattern == ipTpmsFK || pattern == ipCrossHatch || pattern == ipHoneycomb || pattern == ipLateralLattice || pattern == ipLateralHoneycomb ||
                                                  pattern == ipCubic || pattern == ipStars || pattern == ipAlignedRectilinear || pattern == ipLightning || pattern == ip3DHoneycomb || pattern == ipAdaptiveCubic || pattern == ipSupportCubic;
    toggle_line("fill_multiline", have_multiline_infill_pattern);

    // If the infill pattern does not support multiline infill, set fill_multiline to 1.
    if (!have_multiline_infill_pattern) {
        force("fill_multiline", new ConfigOptionInt(1));
    }

    // Hide infill anchor max if sparse_infill_pattern is not line or if sparse_infill_pattern is line but infill_anchor_max is 0.
    bool infill_anchor = config->opt_enum<InfillPattern>("sparse_infill_pattern") != ipLine;
    toggle_field("infill_anchor_max",infill_anchor);

    // Only allow configuration of open anchors if the anchoring is enabled.
    bool has_infill_anchors = have_infill && config->option<ConfigOptionFloatOrPercent>("infill_anchor_max")->value > 0 && infill_anchor;
    toggle_field("infill_anchor", has_infill_anchors);

    //cross zag
    bool is_cross_zag = config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value == InfillPattern::ipCrossZag;
    bool is_locked_zig = config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value == InfillPattern::ipLockedZag;

    toggle_line("infill_shift_step", is_cross_zag || is_locked_zig);

    for (auto el : { "skeleton_infill_density", "skin_infill_density", "infill_lock_depth", "skin_infill_depth","skin_infill_line_width", "skeleton_infill_line_width" })
        toggle_line(el, is_locked_zig);

    bool is_zig_zag = config->option<ConfigOptionEnum<InfillPattern>>("sparse_infill_pattern")->value == InfillPattern::ipZigZag;

    toggle_line("symmetric_infill_y_axis", is_zig_zag || is_cross_zag || is_locked_zig);

    bool has_spiral_vase         = config->opt_bool("spiral_mode");
    toggle_line("spiral_mode_smooth", has_spiral_vase);
    toggle_line("spiral_mode_max_xy_smoothing", has_spiral_vase && config->opt_bool("spiral_mode_smooth"));
    toggle_line("spiral_starting_flow_ratio", has_spiral_vase);
    toggle_line("spiral_finishing_flow_ratio", has_spiral_vase);
    bool has_top_shell    = config->opt_int("top_shell_layers") > 0 || (has_spiral_vase && config->opt_int("bottom_shell_layers") > 1);
    bool has_bottom_shell = config->opt_int("bottom_shell_layers") > 0;
    bool has_solid_infill = has_top_shell || has_bottom_shell;
    toggle_field("top_surface_pattern", has_top_shell);
    toggle_field("bottom_surface_pattern", has_bottom_shell);
    toggle_field("top_surface_density", has_top_shell);
    toggle_field("bottom_surface_density", has_bottom_shell);

    for (auto el : { "infill_direction", "sparse_infill_line_width", "fill_multiline","gap_fill_target","filter_out_gap_fill","infill_wall_overlap",
        "sparse_infill_speed", "bridge_speed", "internal_bridge_speed", "bridge_angle", "internal_bridge_angle",
        "solid_infill_direction", "solid_infill_rotate_template", "internal_solid_infill_pattern", "solid_infill_filament",
        })
        toggle_field(el, have_infill || has_solid_infill);

    toggle_field("top_shell_thickness", ! has_spiral_vase && has_top_shell);
    toggle_field("bottom_shell_thickness", ! has_spiral_vase && has_bottom_shell);

    toggle_field("wall_direction", !has_spiral_vase);

    // Gap fill is newly allowed in between perimeter lines even for empty infill (see GH #1476).
    toggle_field("gap_infill_speed", have_perimeters);

    for (auto el : { "top_surface_line_width", "top_surface_speed" })
        toggle_field(el, has_top_shell);

    bool have_default_acceleration = process_flow_float("default_acceleration", 0) > 0;

    for (auto el : {"outer_wall_acceleration", "inner_wall_acceleration", "initial_layer_acceleration",
        "top_surface_acceleration", "travel_acceleration", "bridge_acceleration", "sparse_infill_acceleration", "internal_solid_infill_acceleration"})
        toggle_field(el, have_default_acceleration);

    bool have_default_jerk = process_flow_float("default_jerk", 0) > 0;

    for (auto el : { "outer_wall_jerk", "inner_wall_jerk", "initial_layer_jerk", "top_surface_jerk", "travel_jerk", "infill_jerk"})
        toggle_field(el, have_default_jerk);

    toggle_line("default_junction_deviation", gcflavor == gcfMarlinFirmware);

    bool have_skirt = config->opt_int("skirt_loops") > 0;
    toggle_field("skirt_height", have_skirt && config->opt_enum<DraftShield>("draft_shield") != dsEnabled);
    toggle_line("single_loop_draft_shield", have_skirt); // ORCA: Display one wall if skirt enabled
    for (auto el : {"skirt_type", "min_skirt_length", "skirt_distance", "skirt_start_angle", "skirt_speed", "draft_shield"})
        toggle_field(el, have_skirt);

    bool have_brim = (config->opt_enum<BrimType>("brim_type") != btNoBrim);
    toggle_field("brim_object_gap", have_brim);
    bool have_brim_width = (config->opt_enum<BrimType>("brim_type") != btNoBrim) && config->opt_enum<BrimType>("brim_type") != btAutoBrim &&
                           config->opt_enum<BrimType>("brim_type") != btPainted;
    toggle_field("brim_width", have_brim_width);
    // wall_filament uses the same logic as in Print::extruders()
    toggle_field("wall_filament", have_perimeters || have_brim);

    bool have_brim_ear = (config->opt_enum<BrimType>("brim_type") == btEar);
    const auto brim_width = config->opt_float("brim_width");
    // disable brim_ears_max_angle and brim_ears_detection_length if brim_width is 0
    toggle_field("brim_ears_max_angle", brim_width > 0.0f);
    toggle_field("brim_ears_detection_length", brim_width > 0.0f);
    // hide brim_ears_max_angle and brim_ears_detection_length if brim_ear is not selected
    toggle_line("brim_ears_max_angle", have_brim_ear);
    toggle_line("brim_ears_detection_length", have_brim_ear);

    // Hide Elephant foot compensation layers if elefant_foot_compensation is not enabled
    toggle_line("elefant_foot_compensation_layers", config->opt_float("elefant_foot_compensation") > 0);

    bool have_raft = config->opt_int("raft_layers") > 0;
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
        "support_object_xy_distance", "support_object_first_layer_gap"/*, "independent_support_layer_height"*/})
        toggle_field(el, have_support_material);
    toggle_field("support_threshold_angle", have_support_material && is_auto(support_type));
    toggle_field("support_threshold_overlap", config->opt_int("support_threshold_angle") == 0 && have_support_material && is_auto(support_type));
    //toggle_field("support_closing_radius", have_support_material && support_style == smsSnug);

    bool support_is_tree = config->opt_bool("enable_support") && is_tree(support_type);
    bool support_is_normal_tree = support_is_tree && support_style != smsTreeOrganic &&
    // Orca: use organic as default
    support_style != smsDefault;
    bool support_is_organic = support_is_tree && !support_is_normal_tree;
    // settings shared by normal and organic trees
    for (auto el : {"tree_support_branch_angle", "tree_support_branch_distance", "tree_support_branch_diameter" })
        toggle_line(el, support_is_normal_tree);
    // settings specific to normal trees
    for (auto el : {"tree_support_auto_brim", "tree_support_brim_width", "tree_support_adaptive_layer_height"})
        toggle_line(el, support_is_normal_tree);
    // settings specific to organic trees
    for (auto el : {"tree_support_branch_angle_organic", "tree_support_branch_distance_organic", "tree_support_branch_diameter_organic", "tree_support_angle_slow", "tree_support_tip_diameter", "tree_support_top_rate", "tree_support_branch_diameter_angle"})
        toggle_line(el, support_is_organic);

    toggle_field("tree_support_brim_width", support_is_tree && !config->opt_bool("tree_support_auto_brim"));
    // non-organic tree support use max_bridge_length instead of bridge_no_support
    toggle_line("max_bridge_length", support_is_normal_tree);
    toggle_line("bridge_no_support", !support_is_normal_tree);
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

    bool have_skirt_height = have_skirt &&
    (config->opt_int("skirt_height") > 1 || config->opt_enum<DraftShield>("draft_shield") != dsEnabled);
    toggle_line("support_speed", have_support_material || have_skirt_height);
    toggle_line("support_interface_speed", have_support_material && have_support_interface);

    toggle_field("inner_wall_line_width", have_perimeters || have_skirt || have_brim);
    toggle_field("support_filament", have_support_material || have_skirt);

    toggle_line("raft_contact_distance", have_raft && !have_support_soluble);

    // Orca: Raft, grid, snug and organic supports use these two parameters to control the size & density of the "brim"/flange
    for (auto el : { "raft_first_layer_expansion", "raft_first_layer_density"})
        toggle_field(el, have_support_material && !(support_is_normal_tree && !have_raft));

    bool has_ironing = (config->opt_enum<IroningType>("ironing_type") != IroningType::NoIroning);
    for (auto el : { "ironing_pattern", "ironing_flow", "ironing_spacing", "ironing_angle", "ironing_inset"})
        toggle_line(el, has_ironing);

    toggle_line("ironing_speed", has_ironing || has_support_ironing);

    bool have_sequential_printing = (config->opt_enum<PrintSequence>("print_sequence") == PrintSequence::ByObject);
    toggle_field("print_order", !have_sequential_printing);

    toggle_field("single_extruder_multi_material", !is_BBL_Printer);

    toggle_field("ooze_prevention", !bSEMM);
    bool have_ooze_prevention = config->opt_bool("ooze_prevention");
    toggle_line("standby_temperature_delta", have_ooze_prevention);
    toggle_line("preheat_time", have_ooze_prevention);
    int preheat_steps = config->opt_int("preheat_steps");
    toggle_line("preheat_steps", have_ooze_prevention && (preheat_steps > 0));

    bool have_prime_tower = config->opt_bool("enable_prime_tower");
    for (auto el : { "prime_tower_width", "prime_tower_brim_width"})
        toggle_line(el, have_prime_tower);

    for (auto el : {"wall_filament", "sparse_infill_filament", "solid_infill_filament", "wipe_tower_filament"})
        toggle_line(el, !bSEMM);

    bool purge_in_primetower = config->opt_bool("purge_in_prime_tower");

    for (auto el : {"wipe_tower_cone_angle",
                    "wipe_tower_extra_spacing", "wipe_tower_max_purge_speed",
                    "wipe_tower_wall_type",
                    "wipe_tower_extra_rib_length","wipe_tower_rib_width","wipe_tower_fillet_wall",
                    "wipe_tower_bridging", "wipe_tower_extra_flow",
                    "wipe_tower_no_sparse_layers"})
      toggle_line(el, have_prime_tower && !is_BBL_Printer);

    const bool local_z_dithering_enabled =
        config->has("dithering_local_z_mode") && config->option("dithering_local_z_mode") != nullptr &&
        config->opt_bool("dithering_local_z_mode");
    toggle_line("dithering_local_z_whole_objects", local_z_dithering_enabled);
    toggle_line("dithering_local_z_infill", local_z_dithering_enabled);
    toggle_line("dithering_local_z_direct_multicolor", local_z_dithering_enabled);

    WipeTowerWallType wipe_tower_wall_type = config->opt_enum<WipeTowerWallType>("wipe_tower_wall_type");
    toggle_line("wipe_tower_cone_angle", have_prime_tower && !is_BBL_Printer && wipe_tower_wall_type == WipeTowerWallType::wtwCone);
    toggle_line("wipe_tower_extra_rib_length", have_prime_tower && !is_BBL_Printer && wipe_tower_wall_type == WipeTowerWallType::wtwRib);
    toggle_line("wipe_tower_rib_width", have_prime_tower && !is_BBL_Printer && wipe_tower_wall_type == WipeTowerWallType::wtwRib);
    toggle_line("wipe_tower_fillet_wall", have_prime_tower && !is_BBL_Printer && wipe_tower_wall_type == WipeTowerWallType::wtwRib);

    toggle_field("prime_tower_width", have_prime_tower && wipe_tower_wall_type != WipeTowerWallType::wtwRib);

    toggle_line("single_extruder_multi_material_priming", !bSEMM && have_prime_tower && !is_BBL_Printer);

    toggle_line("prime_volume",have_prime_tower && (!purge_in_primetower || !bSEMM));

    for (auto el : {"flush_into_infill", "flush_into_support", "flush_into_objects"})
        toggle_field(el, have_prime_tower);

    bool have_avoid_crossing_perimeters = config->opt_bool("reduce_crossing_wall");
    toggle_line("max_travel_detour_distance", have_avoid_crossing_perimeters);

    bool has_overhang_speed = config->opt_bool("enable_overhang_speed", 0);
    for (auto el : {"overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed"})
        toggle_line(el, has_overhang_speed);

    toggle_line("slowdown_for_curled_perimeters", has_overhang_speed);

    toggle_line("flush_into_objects", !is_global_config);

    toggle_line("support_interface_not_for_body",config->opt_int("support_interface_filament")&&!config->opt_int("support_filament"));

    NoiseType fuzzy_skin_noise_type = config->opt_enum<NoiseType>("fuzzy_skin_noise_type");
    toggle_line("fuzzy_skin_scale", fuzzy_skin_noise_type != NoiseType::Classic);
    toggle_line("fuzzy_skin_octaves", fuzzy_skin_noise_type != NoiseType::Classic && fuzzy_skin_noise_type != NoiseType::Voronoi);
    toggle_line("fuzzy_skin_persistence", fuzzy_skin_noise_type == NoiseType::Perlin || fuzzy_skin_noise_type == NoiseType::Billow);

    bool have_arachne = config->opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Arachne;
    for (auto el : { "wall_transition_length", "wall_transition_filter_deviation", "wall_transition_angle",
        "min_feature_size", "min_length_factor", "min_bead_width", "wall_distribution_count", "initial_layer_min_bead_width"})
        toggle_line(el, have_arachne);
    toggle_field("detect_thin_wall", !have_arachne);

    // Orca
    auto is_role_based_wipe_speed = config->opt_bool("role_based_wipe_speed");
    toggle_field("wipe_speed",!is_role_based_wipe_speed);

    for (auto el : {"accel_to_decel_enable", "accel_to_decel_factor"})
        toggle_line(el, gcflavor == gcfKlipper);
    if(gcflavor == gcfKlipper)
        toggle_field("accel_to_decel_factor", config->opt_bool("accel_to_decel_enable", 0));

    bool have_make_overhang_printable = config->opt_bool("make_overhang_printable");
    toggle_line("make_overhang_printable_angle", have_make_overhang_printable);
    toggle_line("make_overhang_printable_hole_size", have_make_overhang_printable);

    toggle_line("min_width_top_surface", config->opt_bool("only_one_wall_top") || ((config->opt_float("min_length_factor") > 0.5f) && have_arachne)); // 0.5 is default value

    for (auto el : { "hole_to_polyhole_threshold", "hole_to_polyhole_twisted" })
        toggle_line(el, config->opt_bool("hole_to_polyhole"));

    bool has_detect_overhang_wall = config->opt_bool("detect_overhang_wall");
    bool has_overhang_reverse     = config->opt_bool("overhang_reverse");
    bool force_wall_direction     = config->opt_enum<WallDirection>("wall_direction") != WallDirection::Auto;
    bool allow_overhang_reverse   = !has_spiral_vase && !force_wall_direction;
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


    toggle_field("seam_slope_type", !has_spiral_vase);
    bool has_seam_slope = !has_spiral_vase && config->opt_enum<SeamScarfType>("seam_slope_type") != SeamScarfType::None;
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

    //Orca: disable infill_direction/solid_infill_direction if sparse_infill_rotate_template/solid_infill_rotate_template is not empty value
    toggle_field("infill_direction", config->opt_string("sparse_infill_rotate_template") == "");
    toggle_field("solid_infill_direction", config->opt_string("solid_infill_rotate_template") == "");


    toggle_line("infill_overhang_angle", config->opt_enum<InfillPattern>("sparse_infill_pattern") == InfillPattern::ipLateralHoneycomb);

    return out;
}

} // namespace engine
