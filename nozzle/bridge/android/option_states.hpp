// Which print settings apply to the current configuration: the engine-side form of the rules Orca's settings tabs
// apply in the GUI (src/slic3r/GUI/ConfigManipulation.cpp, ConfigManipulation::toggle_print_fff_options). Nozzle It All
// generates its settings screens from the engine's schema, so the same rules have to come from the engine rather than
// from a GUI it does not use. AGPL-3.0, like the code it is ported from.
#pragma once

#include <libslic3r/PrintConfig.hpp>

#include <map>
#include <string>
#include <vector>

namespace engine {

struct OptionState {
    bool enabled = true; // Orca's toggle_field: shown but greyed out when false
    bool visible = true; // Orca's toggle_line: the row is hidden when false
};

// A setting combination the GUI resolves by asking the user (Orca shows a dialog). Each choice lists the changes it makes.
struct OptionConflict {
    std::string id;       // stable identifier, for example "wipe_with_firmware_retraction"
    std::string message;  // plain-language explanation
    struct Choice { std::string label; std::vector<std::pair<std::string, std::string>> changes; };
    std::vector<Choice> choices;
};

struct OptionStates {
    // Only settings a rule mentions appear here; every other setting is enabled and visible. Per-extruder settings are
    // keyed "key#index" (extruder index from 0), Orca's own naming for a vector option's line.
    std::map<std::string, OptionState> states;
    std::vector<OptionConflict> conflicts;
    // Values Orca's GUI forces while applying the rules (for example arc fitting is switched off when extrusion-rate
    // smoothing is on), as {key, serialized value}. Callers apply them so the project matches what Orca would slice.
    std::vector<std::pair<std::string, std::string>> forced;
};

struct OptionContext {
    bool is_bbl_printer = false;   // the printer comes from Bambu Lab's vendor profiles (Orca's is_BBL_Printer)
    bool is_global_config = true;  // false when the settings belong to one object, part or height range
    size_t flow_variant_index = 0; // which entry of per-flow-variant vectors applies
    std::string printer_name;      // the printer preset's name (Orca checks it for "Snapmaker U1")
    std::string printer_model_id;  // the vendor model id (for example "SM_U1", "C11"), for the default bed type
};

// `config` is a full print configuration (printer + filament + process), as the slicer resolves it.
OptionStates print_option_states(const Slic3r::DynamicPrintConfig& config, const OptionContext& context = {});
// Orca's TabPrinter::toggle_options, for every page (Orca applies each page's rules only while it is shown).
OptionStates printer_option_states(const Slic3r::DynamicPrintConfig& config, const OptionContext& context = {});
// Orca's TabFilament::toggle_options, for every page.
OptionStates filament_option_states(const Slic3r::DynamicPrintConfig& config, const OptionContext& context = {});
// All three, merged (keys do not overlap).
OptionStates all_option_states(const Slic3r::DynamicPrintConfig& config, const OptionContext& context = {});

} // namespace engine
