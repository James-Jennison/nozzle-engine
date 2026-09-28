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
        const engine::OptionStates result = engine::print_option_states(config, context);
        json out;
        out["states"] = json::object();
        for (const auto& [key, state] : result.states)
            out["states"][key] = {{"enabled", state.enabled}, {"visible", state.visible}};
        out["forced"] = json::object();
        for (const auto& [key, value] : result.forced) out["forced"][key] = value;
        response = out.dump();
        return 0;
    } catch (const std::exception& e) {
        response = json{{"error", e.what()}}.dump();
        return 1;
    }
}
} // namespace nozzle_contract
