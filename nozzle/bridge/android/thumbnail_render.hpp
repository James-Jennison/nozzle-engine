// A minimal, headless software rasterizer standing in for OrcaSlicer's own thumbnail
// generation, which normally comes from rendering the GUI's live OpenGL scene - not available
// here (SLIC3R_GUI=OFF, no GL context on a headless slice). This renders a plain isometric,
// flat-shaded silhouette of the model's own mesh instead. Simpler than the real preview OrcaSlicer
// ships, but real: it's the actual sliced geometry, not a placeholder icon, so the printer's own
// screen shows something a person picking a job off a list can recognize.
#pragma once

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/GCode/ThumbnailData.hpp"

namespace engine {

// Captures `mesh` by value (the caller's Model is about to be consumed by print.apply()/
// print.process(), which may not leave the source mesh in a usable state afterward) and renders
// it fresh for each of ThumbnailsParams.sizes when the returned callback is invoked.
Slic3r::ThumbnailsGeneratorCallback make_thumbnail_callback(Slic3r::TriangleMesh mesh);

} // namespace engine
