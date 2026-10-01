// `nozzle-engine --option-states <request.json>`: which print settings apply to a configuration (see
// ../android/option_states.hpp). Request:
//   {"profiles":[machine.json, filament.json, process.json], "overrides":{key:value}, "bblPrinter":false,
//    "global":true, "flowVariant":0, "scope":"all|process|printer|filament", "printerName":"...", "printerModelId":"..."}
// Response: {"states":{key or key#extruder:{"enabled":bool,"visible":bool}}, "forced":{key:value},
//            "conflicts":[{"id","message","choices":[{"label","changes":{key:value}}]}]}
#pragma once
#include <string>

namespace nozzle_contract {
int run_option_states(const std::string& request, std::string& response);
// `nozzle-engine --config-checks <request.json>`: the same request (plus "plate":bool, "filamentCount":n); response
// {"notices":[{"id","message","changes":{key:value}}], "conflicts":[...], "states":{}, "forced":{}} (see print_config_checks).
int run_config_checks(const std::string& request, std::string& response);
// `nozzle-engine --compatible-presets <request.json>`: which print or filament presets a printer offers, by Orca's own
// is_compatible_with_printer (compatible_printers, else compatible_printers_condition evaluated against the printer).
// Request: {"printer":machine.json, "printerName":"...", "type":"print|filament", "presets":[preset.json, ...]}, every
// file flattened (inherits resolved). Response: {"compatible":[bool per preset, in request order]}.
int run_compatible_presets(const std::string& request, std::string& response);
}
