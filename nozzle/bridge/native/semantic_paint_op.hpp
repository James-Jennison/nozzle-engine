// AI auto-paint for nozzle-engine: `nozzle-engine --semantic-paint <request.json>`.
//
// libslic3r's SemanticPaint (src/libslic3r/SemanticPaint.{hpp,cpp}) splits a single-colour mesh into surface segments, renders
// them as numbered views, and turns a model's "segment -> filament" answer into per-face paint. The engine makes no network
// or process calls: the caller (Nozzle It All Desktop or Android) sends the prompt and views to whatever provider the user
// chose, which keeps the feature provider-neutral. All ops are deterministic, so `assign` re-derives the segmentation.
//   {"op":"segment","model":stl,"out_dir":dir,"filaments":[{"index":1,"type":"PLA","color":"#RRGGBB","name":"..."}],
//    "object_name":"...","hint":"...","size":384}
//       -> {"segments":[{"id","faces","area_fraction","centroid":[x,y,z],"normal":[x,y,z]}],"views":[{"name","path"}],
//           "prompt":"...","command_prompt":"..."}        (view PNGs are written to out_dir)
//   {"op":"openai_request","model_name":"...","prompt":"...","png_paths":[files]}  -> {"body":"<chat completions JSON>"}
//   {"op":"reply_text","kind":"command"|"openai","text":"<provider output>"}      -> {"text":"<the answer>"}
//   {"op":"assign","model":stl,"reply":"<answer text>","filament_count":n}
//       -> {"segment_to_filament":[n per segment, 0 = unpainted],"runs":[[faces,filament],...],"faces":N}
// Models are binary STL files in the mesh's own coordinates; face i of the response is triangle i of the file.
// Exit codes: 0 success, 1 the operation failed (for example the answer could not be read), 2 bad request.
// AGPL-3.0, like the libslic3r code it calls.
#pragma once

#include <string>

namespace nozzle_sp {

int run_semantic_paint(const std::string& request_text, std::string& response);

} // namespace nozzle_sp
