#include "contract_cli.hpp"
#include "option_states.hpp"

#include <libslic3r/PrintConfig.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>

namespace nozzle_contract {
using json = nlohmann::ordered_json;

int run_option_states(const std::string& request, std::string& response) {
    using namespace Slic3r;
    try {
        const json req = json::parse(request);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        const json profiles = req.value("profiles", json::array());
        const json overrides = req.value("overrides", json::object());
        for (const auto& p : profiles) {
            DynamicPrintConfig profile;
            profile.load(p.get<std::string>(), ForwardCompatibilitySubstitutionRule::Enable);
            config.apply(profile);
        }
        for (const auto& [key, value] : overrides.items())
            config.set_deserialize_strict(key, value.is_string() ? value.get<std::string>() : value.dump());
        engine::OptionContext context;
        context.is_bbl_printer = req.value("bblPrinter", false);
        context.is_global_config = req.value("global", true);
        context.flow_variant_index = req.value("flowVariant", 0);
        context.printer_name = req.value("printerName", std::string());
        context.printer_model_id = req.value("printerModelId", std::string());
        const std::string scope = req.value("scope", std::string("all"));
        const engine::OptionStates result = scope == "process"  ? engine::print_option_states(config, context)
                                          : scope == "printer"  ? engine::printer_option_states(config, context)
                                          : scope == "filament" ? engine::filament_option_states(config, context)
                                                                : engine::all_option_states(config, context);
        json out;
        out["states"] = json::object();
        for (const auto& [key, state] : result.states)
            out["states"][key] = {{"enabled", state.enabled}, {"visible", state.visible}};
        out["forced"] = json::object();
        for (const auto& [key, value] : result.forced) out["forced"][key] = value;
        out["conflicts"] = json::array();
        for (const auto& c : result.conflicts) {
            json choices = json::array();
            for (const auto& ch : c.choices) {
                json changes = json::object();
                for (const auto& [k, v] : ch.changes) changes[k] = v;
                choices.push_back({{"label", ch.label}, {"changes", changes}});
            }
            out["conflicts"].push_back({{"id", c.id}, {"message", c.message}, {"choices", choices}});
        }
        response = out.dump();
        return 0;
    } catch (const std::exception& e) {
        response = json{{"error", e.what()}}.dump();
        return 1;
    }
}
} // namespace nozzle_contract
