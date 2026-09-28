// PrusaSlicer 2.9.6 colour mixing (virtual extruders) mode of nozzle-engine: `nozzle-engine --color-mix <request.json>`.
// Ported from PrusaSlicer 2.9.6 (src/libslic3r/Feature/FullSpectrum/VirtualExtruder.cpp, src/slic3r/GUI/FullSpectrumDialog.cpp)
// and PrusaSlicer 3.0.0-alpha12 (src/slic3r-biz-algorithms/src/Slic3r/Biz/Algorithms/VirtualExtruderPresets.cpp);
// Copyright (c) Prusa Research s.r.o. and the PrusaSlicer contributors, AGPL-3.0-or-later, as is this file.
// See color_mix.cpp for the list of ported functions.
#pragma once

#include <string>

namespace nozzle_cm {

// Runs one request (JSON text). Writes the JSON response to `response` and returns the process exit code:
// 0 success, 1 the operation failed, 2 bad request. On 1 and 2 the response is {"error": "..."}.
int run_color_mix(const std::string& request_text, std::string& response);

// Whether `text` is a virtual extruders file the engine can read (Metadata/Prusa_Slicer_full_spectrum.json format:
// a JSON object with a supported "version" and a "virtual_extruders" array). PrusaSlicer's reader silently ignores a
// file that is not; a slice request names the file explicitly, so this lets it be refused instead.
bool check_virtual_extruders_file(const std::string& text, std::string& why);

} // namespace nozzle_cm
