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
}
