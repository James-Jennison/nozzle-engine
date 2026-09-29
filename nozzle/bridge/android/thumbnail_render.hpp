// A minimal, headless software rasterizer standing in for OrcaSlicer's own thumbnail
// generation, which normally comes from rendering the GUI's live OpenGL scene - not available
// here (SLIC3R_GUI=OFF, no GL context on a headless slice). This renders a plain isometric,
// flat-shaded view of the model's own meshes instead, each part in the colour of the filament it
// prints with (colour-painted areas in their painted filament's colour). Simpler than the real
// preview OrcaSlicer ships, but real: it's the actual sliced geometry, not a placeholder icon, so
// the printer's own screen shows something a person picking a job off a list can recognize.
#pragma once

#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"

namespace engine {

// Captures `model`'s printable parts by value, already in world/bed coordinates and coloured from
// `config`'s filament_colour (the caller's Model is about to be consumed by print.apply()/
// print.process(), which may not leave it in a usable state afterward), and renders them fresh for
// each of ThumbnailsParams.sizes when the returned callback is invoked. Call it after each object's
// filament ("extruder") is assigned.
Slic3r::ThumbnailsGeneratorCallback make_thumbnail_callback(const Slic3r::Model& model, const Slic3r::DynamicPrintConfig& config);

} // namespace engine
