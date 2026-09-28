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

struct OptionStates {
    // Only settings a rule mentions appear here; every other setting is enabled and visible.
    std::map<std::string, OptionState> states;
    // Values Orca's GUI forces while applying the rules (for example arc fitting is switched off when extrusion-rate
    // smoothing is on), as {key, serialized value}. Callers apply them so the project matches what Orca would slice.
    std::vector<std::pair<std::string, std::string>> forced;
};

struct OptionContext {
    bool is_bbl_printer = false;   // the printer comes from Bambu Lab's vendor profiles (Orca's is_BBL_Printer)
    bool is_global_config = true;  // false when the settings belong to one object, part or height range
    size_t flow_variant_index = 0; // which entry of per-flow-variant vectors applies
};

// `config` is a full print configuration (printer + filament + process), as the slicer resolves it.
OptionStates print_option_states(const Slic3r::DynamicPrintConfig& config, const OptionContext& context = {});

} // namespace engine
