// Mixed filaments for nozzle-engine, on Orca 2.5.0's own model: `nozzle-engine --mixed-filament <request.json>`.
//
// A mixed filament is a filament slot after the physical ones that is not loaded into any extruder: the slicer resolves it
// at slice time into the physical filaments listed in its components, in the proportions of its weights, by splitting
// each layer into sub-layers or alternating whole layers; a gradient moves the proportions up the object. This file only
// edits and describes those slots; slicing reads the per-slot options it produces (op "slice_overrides").
// AGPL-3.0, like the libslic3r code it calls (FilamentMixer.hpp, ColorDecomposeRecipe.hpp).
#pragma once

#include <string>

namespace nozzle_mf {

// Runs one request (JSON text). Writes the JSON response to `response` and returns the process exit code:
// 0 success, 1 the operation failed (for example no recipe found), 2 bad request. On 1 and 2 the response is {"error": "..."}.
int run_mixed_filament(const std::string& request_text, std::string& response);

} // namespace nozzle_mf
