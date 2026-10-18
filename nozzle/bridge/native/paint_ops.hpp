// Paint tools for nozzle-engine: `nozzle-engine --paint <request.json>`, run by libslic3r's own TriangleSelector so a stroke
// splits and stores triangles exactly as Orca's support / seam / colour / fuzzy-skin painting does.
//   {"op":"stroke","model":stl,"paint":[[triangle,"HEX"],...],"strokes":[{"origin":[x,y,z],"dir":[x,y,z],"radius":mm,"state":n}],
//    "mode":"brush"|"fill"|"smart","angle":deg}
//       -> {"paint":[[triangle,"HEX"],...],"hits":n}
//      "state" is Orca's facet state: 1 = enforcer (colour 1), 2 = blocker (colour 2), 3.. = colour N; 0 erases. "origin"/"dir" are a
//      ray in the model's own coordinates; the first facet it hits is painted: a sphere of "radius" around the hit for a brush, Orca's
//      bucket fill ("fill": the connected same-state surface, up to sharp edges over "angle" degrees) or its smart fill ("smart"). "paint" is the kind's existing paint (the triangle strings
//      a 3MF stores: paint_color, paint_supports, paint_seam or paint_fuzzy_skin).
//   {"op":"facets","model":stl,"paint":[[triangle,"HEX"],...]}
//       -> {"states":[{"state":n,"triangles":[x0,y0,z0,x1,y1,z1,x2,y2,z2,...]}]}   (the painted pieces, for drawing)
// Models are binary STL files in the mesh's own coordinates; triangle i is triangle i of the file.
// Exit codes: 0 success, 1 the operation failed, 2 bad request. AGPL-3.0, like the libslic3r code it calls.
#pragma once

#include <string>

namespace nozzle_paint {

int run_paint(const std::string& request_text, std::string& response);

} // namespace nozzle_paint
