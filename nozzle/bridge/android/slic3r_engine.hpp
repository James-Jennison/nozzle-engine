// Adapted from the owner's own orcaslicer-android-engine project (local, /mnt/faststorage/orcaslicer-android-engine),
// which cross-compiles upstream OrcaSlicer's libslic3r for Android. Licensed AGPL-3.0-or-later
// (same as OrcaSlicer itself, and the same family already governing this app since the Helix port).
// See THIRD_PARTY_NOTICES.md and docs/WORK_ORDER.md's WO-13 entry.
#pragma once
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Shared slicing pipeline used by both the JNI bridge (slic3r_jni.cpp) and
// the standalone on-device CLI test tool (cli_test.cpp), so the two stay in
// lockstep instead of duplicating the load/configure/process/export
// sequence. Throws std::runtime_error (or whatever libslic3r itself throws)
// on failure.
namespace engine {

// A real, user-applied placement on the bed (WO-15 part E: object transform), applied via the
// actual libslic3r ModelInstance transform (set_offset/set_rotation/set_scaling_factor) inside
// load_and_place_model() - the same call every slice already goes through - not a cosmetic
// preview-only value. v1 scope, matching what the touch UI actually offers: XY position on the
// bed plane, rotation about Z only (turntable - arbitrary/place-on-face rotation isn't supported
// yet), and uniform scale. Default-constructed = identity placement (today's existing behavior,
// unchanged for every call site that doesn't pass one).
struct ModelTransform {
    double offset_x_mm = 0.0;
    double offset_y_mm = 0.0;
    double rotation_z_deg = 0.0;
    double scale = 1.0;
};

// config_overrides are applied (via ConfigBase::set_deserialize) after
// profile_paths, so callers can tweak individual keys without needing a
// full profile file -- e.g. for a bare smoke test with no real printer
// profile at hand.
void slice_file(const std::string& input_model_path,
                 const std::string& output_gcode_path,
                 const std::vector<std::string>& profile_paths,
                 const std::vector<std::pair<std::string, std::string>>& config_overrides = {},
                 const ModelTransform& transform = {});

// Phase 6 (Consumer Slicer Plan §16): produces a real Bambu-compatible .gcode.3mf bundle - the
// same file shape store_bbs_3mf() (Format/bbs_3mf.hpp, real libslic3r, already linked into this
// headless engine) writes for the desktop GUI's own "send to printer" action, using the identical
// SaveStrategy flags Plater.cpp uses for that real case (Silence|WithGcode|SkipModel|
// SkipAuxiliary). Deliberately does NOT go through PartPlateList::store_to_3mf_structure
// (slic3r/GUI/PartPlate.cpp) - that's real code too, but it's part of the GUI module and pulls in
// wxWidgets, which this build deliberately never links (SLIC3R_GUI=OFF). Instead this builds the
// same PlateData struct store_bbs_3mf() actually consumes directly - PlateData itself is a plain
// libslic3r struct (Format/bbs_3mf.hpp), not GUI-scoped, confirmed by reading the real source
// before writing this, not assumed. See slic3r_engine.cpp for field-by-field justification.
void slice_bambu_bundle(const std::string& input_model_path,
                         const std::string& output_bundle_path,
                         const std::vector<std::string>& profile_paths,
                         const std::vector<std::pair<std::string, std::string>>& config_overrides = {},
                         const ModelTransform& transform = {});

// Phase 6 follow-up (WO-23): the real multi-object counterpart to slice_bambu_bundle() above -
// same relationship slice_multi_object() (below) already has to slice_file(). See
// slic3r_engine.cpp's own header comment on the bundle_model() tail these two share.
// Phase 9d: per-object paint strokes and modifier/blocker volumes, in the text formats ObjectExtras.kt writes.
// Parallel to the objects vector (empty string = none).
// settings: per-object print settings (key, value), as Orca's object list sets them on ModelObject::config; only keys of
// PrintObjectConfig or PrintRegionConfig are accepted (SettingsFactory::get_options(false)), anything else is an error.
struct ObjectExtras { std::string paint_strokes; std::string volume_specs; std::vector<std::pair<std::string, std::string>> settings; };

void slice_multi_object_bambu_bundle(const std::vector<std::pair<std::string, ModelTransform>>& objects,
                                      const std::string& output_bundle_path,
                                      const std::vector<std::string>& profile_paths,
                                      const std::vector<std::pair<std::string, std::string>>& config_overrides = {},
                                      const std::vector<ObjectExtras>& extras = {});
// The same with a 1-based filament (AMS slot / tool) per object, 0 = the default, as slice_multi_object() takes.
void slice_multi_object_bambu_bundle(const std::vector<std::tuple<std::string, ModelTransform, int>>& objects,
                                      const std::string& output_bundle_path,
                                      const std::vector<std::string>& profile_paths,
                                      const std::vector<std::pair<std::string, std::string>>& config_overrides,
                                      const std::vector<ObjectExtras>& extras);

// Phase 1 (Consumer Slicer Plan §16): slices a real multi-object build plate - each (model path,
// placement, tool_index) triple loaded and placed exactly the way slice_file()'s single-object
// path already does, then merged into one Model and sliced together into one G-code file. See the
// .cpp for why this doesn't itself do collision detection (a separate, real UI concern).
//
// tool_index (Phase 8 follow-up, §11, WO-25/WO-26): a real 1-based OrcaSlicer filament/extruder
// identity (matching the real "*_filament_id" config options' own convention - 1 = the first
// filament slot, 0 = "don't set, inherit the default"). Applied to each object's own
// ModelConfigObject *before* combining, as the six concrete per-feature keys
// (outer_wall_filament_id/inner_wall_filament_id/sparse_infill_filament_id/
// internal_solid_filament_id/top_surface_filament_id/bottom_surface_filament_id), not the more
// general "extruder" key the desktop GUI's own per-object "Set extruder" writes - see the .cpp's
// own comment on why: this vendored engine's DynamicPrintConfig::normalize_fdm(), the real
// function that would normally translate "extruder" into those six keys, is commented out in its
// own PrintApply.cpp, confirmed by reading it directly, not assumed. This alone is not sufficient
// for a real multi-tool slice, only necessary: the caller's config_overrides must also give the
// target profile a real N-filament-slot config (filament_diameter's own array length is what
// libslic3r actually uses to compute how many extruders exist - PrintApply.cpp's own `size_t
// num_extruders = m_config.filament_diameter.size()` - not machine.json's nozzle_diameter/
// extruder_colour, which only bound how many *could* exist), or every filament id here gets
// silently clamped back down to 1 (PrintObject.cpp's own clamp_feature_filament_to_valid).
// Empirically verified end to end against the real Snapmaker U1 bundled profile - see
// ToolAssignmentSlicingDeviceTest's own header comment for the real config_overrides recipe that
// produces genuine, distinct Tx tool-change commands in the sliced G-code.
//
// virtual_extruders_json: PrusaSlicer 2.9.6 virtual extruders (colour-mixing blends and gradients), as the text of a
// Metadata/Prusa_Slicer_full_spectrum.json sidecar. Empty = none. Objects whose tool index is a virtual id, and
// painted areas whose state is one, then print by PrusaSlicer's layer cycle. Only an engine built with PrusaSlicer's
// virtual extruders (the desktop's Snapmaker Orca engine) accepts it; others throw.
void slice_multi_object(const std::vector<std::tuple<std::string, ModelTransform, int>>& objects,
                         const std::string& output_gcode_path,
                         const std::vector<std::string>& profile_paths,
                         const std::vector<std::pair<std::string, std::string>>& config_overrides = {},
                         const std::vector<ObjectExtras>& extras = {},
                         const std::string& virtual_extruders_json = {});

// Loads input_model_path (STL/3MF/OBJ) the same way slice_file() does - real Model::read_from_file,
// bed-centered - but stops short of slicing. Returns 3 floats (the first object's first
// instance's own world-space offset - the real pivot a later transform's rotate/scale will use,
// see the .cpp) followed by a flat interleaved vertex buffer for a real-time GL preview: 6 floats
// per vertex (x,y,z,nx,ny,nz), 3 vertices per triangle, nx/ny/nz the triangle's own flat face
// normal (matches thumbnail_render.cpp's flat-shading choice).
std::vector<float> load_mesh_preview(const std::string& input_model_path);

// Phase 0 (WO-16): loads input_model_path the same real way slice_file()/load_mesh_preview() do
// and returns how many separate ModelObjects it actually contains - proof, not assumption, that
// the native bridge's model load already preserves a real multi-object Model::objects list rather
// than collapsing everything into one mesh (load_mesh_preview does that collapse deliberately, for
// its own single-mesh preview use case - this function doesn't, and exists specifically so a
// multi-object model's true object count is observable from the Kotlin side ahead of any UI
// actually using it, per the Consumer Slicer Plan's Phase 0 acceptance criteria).
int count_model_objects(const std::string& input_model_path);

// Support painting (WO-14 part D): a stateful session over a loaded model's first object/volume
// (v1 scope - the common single-part case every real model used so far actually is), reusing the
// same real, headless-usable libslic3r machinery the upstream GUI relies on for the identical
// feature: AABBMesh for ray-mesh hit testing, TriangleSelector for the actual brush-paint
// algorithm, ModelVolume::supported_facets for the result print.apply() already reads during
// support generation with zero new slicing-side wiring. See slic3r_engine.cpp for the concrete
// per-call reasoning (each traced against the real upstream GUI source, not guessed).
using PaintSessionHandle = int64_t;

// Opens a session (loads + bed-centers the model, same as slice_file()/load_mesh_preview()) and
// returns an opaque handle. Throws if the model has no paintable volume.
// `transform` is applied once, at open time, and frozen for the session's lifetime - painting
// operates on the volume's raw local mesh via a `trafo` captured at open (see slic3r_engine.cpp's
// own header comment on that), so a transform change after a paint session is already open isn't
// reflected; the Kotlin caller keeps transform controls and Paint mode mutually exclusive so this
// never happens in practice (see ModelViewer.kt).
PaintSessionHandle open_paint_session(const std::string& input_model_path, const ModelTransform& transform = {});

// origin/dir are a world-space ray (the same bed-centered coordinate space load_mesh_preview's
// vertices are already in, so the Kotlin GL camera's own unprojection needs no extra transform).
// A ray that misses the mesh entirely is a silent no-op, not an error - a normal outcome for an
// ordinary stray touch during a drag.
void paint_stroke(PaintSessionHandle handle, double originX, double originY, double originZ,
                   double dirX, double dirY, double dirZ, double radiusMm, bool enforcer);

// The currently enforcer-painted triangles, transformed into the same world-space coordinates
// load_mesh_preview uses, as a flat position-only buffer (3 floats/vertex, no normals - this is
// meant for an unlit highlight overlay, not a second lit mesh).
std::vector<float> get_painted_facets(PaintSessionHandle handle);

// Slices using the session's own already-loaded (and possibly painted) in-memory model directly -
// no re-load from disk, no serialization round-trip for the painted state. Does not close the
// session; the caller closes it explicitly once done.
//
// virtual_extruders_json: same PrusaSlicer 2.9.6 virtual extruders slice_multi_object() takes (empty = none). A
// painted area whose enforcer state equals a virtual id then prints by PrusaSlicer's layer cycle - the main reason
// this parameter exists on the paint path at all, since a painted multi-colour model is how a mixed-filament blend
// actually gets assigned to part of an object today (there being no per-triangle "tool_index" outside painting).
void slice_paint_session(PaintSessionHandle handle, const std::string& output_gcode_path,
                          const std::vector<std::string>& profile_paths,
                          const std::vector<std::pair<std::string, std::string>>& config_overrides = {},
                          const std::string& virtual_extruders_json = {});

void close_paint_session(PaintSessionHandle handle);

// Thrown by every slice_* entry point when request_cancel() stopped it; any partial output is removed.
struct SliceCancelled : std::runtime_error {
    SliceCancelled() : std::runtime_error("Slicing cancelled") {}
};

// Asks the slice currently running (if any) to stop. A request made while nothing is running stays
// pending for the next slice until reset_cancel() - callers reset before starting a slice they own.
void request_cancel();
void reset_cancel();
// 0-100, the engine's own status percent for the slice currently (or most recently) running.
int slice_progress();

// Phase 9c: cuts a triangle soup (9 floats per triangle) with the horizontal plane at z (mesh coordinates) and
// caps both halves with libslic3r's own cap triangulation. Either half may come back empty.
struct CutResult { std::vector<float> upper; std::vector<float> lower; };
CutResult cut_mesh_soup(const std::vector<float>& soup, float z);

// The plate's Arrange, as Orca's ArrangeJob does it (src/slic3r/GUI/Jobs/ArrangeJob.cpp init_arrange_params, process):
// each model loaded and placed as for slicing, get_instance_arrange_poly, update_arrange_params,
// update_selected_items_inflation/axis_align, get_shrink_bedpts, arrangement::arrange. [distance_mm] 0 is Orca's "auto"
// spacing. Results are the change in each object's XY offset and Z rotation, and the plate it landed on (0 = this one).
struct ArrangeItem { std::string model_path; ModelTransform transform; };
struct ArrangeOptions { double distance_mm = 0.0; bool allow_rotations = false; bool align_to_y_axis = false; };
struct ArrangeResult { double dx_mm = 0.0; double dy_mm = 0.0; double rotation_deg = 0.0; int plate = 0; };
std::vector<ArrangeResult> arrange_models(const std::vector<ArrangeItem>& items, const std::vector<std::string>& profile_paths,
                                          const std::vector<std::pair<std::string, std::string>>& config_overrides, const ArrangeOptions& options);

// Orca's Auto orient for one object (libslic3r/Orient.cpp orient(ModelInstance*), AutoOrienter): the rotation that puts
// the mesh (a triangle soup, 9 floats per triangle) on its best face, row-major 3x3.
std::vector<double> orient_mesh_soup(const std::vector<float>& soup);

} // namespace engine
