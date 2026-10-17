// Plate tools mode of nozzle-engine: `nozzle-engine --plate <request.json>`, the plate operations Orca runs in libslic3r
// (arrange, auto-orient, cut), for Nozzle It All Desktop. Response JSON on stdout; exit 0 ok, 1 failed, 2 bad request.
#pragma once
#include <string>

namespace nozzle_plate {
int run_plate(const std::string& request_text, std::string& response);
}
