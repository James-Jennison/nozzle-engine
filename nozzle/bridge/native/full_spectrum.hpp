// Full Spectrum (colour mixing) mode of nozzle-engine: `nozzle-engine --full-spectrum <request.json>`.
// A port of Snapmaker Orca's colour-mixing GUI logic (commit cbf7bbb0b3, AGPL-3.0, as is this file) onto libslic3r's own
// MixedFilamentManager; see full_spectrum.cpp for the list of ported functions.
#pragma once

#include <string>

namespace nozzle_fs {

// Runs one request (JSON text). Writes the JSON response to `response` and returns the process exit code:
// 0 success, 1 the operation failed (e.g. no recipe found), 2 bad request. On 1 and 2 the response is {"error": "..."}.
int run_full_spectrum(const std::string& request_text, std::string& response);

} // namespace nozzle_fs
