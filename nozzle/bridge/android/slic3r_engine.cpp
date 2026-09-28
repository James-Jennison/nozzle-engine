// Adapted from the owner's own orcaslicer-android-engine project (local, /mnt/faststorage/orcaslicer-android-engine),
// which cross-compiles upstream OrcaSlicer's libslic3r for Android. Licensed AGPL-3.0-or-later
// (same as OrcaSlicer itself, and the same family already governing this app since the Helix port).
// See THIRD_PARTY_NOTICES.md and docs/WORK_ORDER.md's WO-13 entry.
// The load -> configure -> process -> export sequence mirrors OrcaSlicer's
// own tests/fff_print/test_helpers.cpp (init_print / init_and_process_print
// / gcode()), the canonical reference for driving libslic3r headlessly.
#include "slic3r_engine.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ExtruderVariants.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleMeshSlicer.hpp"
#include "libslic3r/Arrange.hpp"
#include "libslic3r/ModelArrange.hpp"
#include "libslic3r/Orient.hpp"
// PrusaSlicer 2.9.6 virtual extruders exist only in the desktop engine (Snapmaker Orca + Nozzle's changes); the
// same bridge still builds against upstream OrcaSlicer (Android, Web), where they are refused.
#if __has_include("libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp")
#include "libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp"
#endif
#include <sstream>

#include "thumbnail_render.hpp"

#include <boost/filesystem.hpp>

namespace engine {

namespace {

// Cancellation + progress: one slice runs at a time (the Kotlin coordinator serializes them), so a
// single registered Print is enough. PrintBase::cancel() only flips an atomic, so calling it from the
// JNI cancel thread while process()/export_gcode() run on the slicing thread is safe; the engine then
// unwinds itself via CanceledException at its next throw_if_canceled() checkpoint.
std::mutex g_active_mutex;
Slic3r::Print* g_active_print = nullptr;
std::atomic<bool> g_cancel_requested{false};
std::atomic<int> g_progress_percent{0};

struct ActiveSlice {
    explicit ActiveSlice(Slic3r::Print& print) {
        g_progress_percent = 0;
        print.set_status_callback([](const Slic3r::PrintBase::SlicingStatus& status) {
            if (status.percent >= 0) g_progress_percent = status.percent;
        });
        std::lock_guard<std::mutex> lock(g_active_mutex);
        g_active_print = &print;
        if (g_cancel_requested.load()) print.cancel();
    }
    ~ActiveSlice() {
        std::lock_guard<std::mutex> lock(g_active_mutex);
        g_active_print = nullptr;
    }
};

// Runs process()+export under the cancel registration; converts the engine's own CanceledException
// into SliceCancelled and removes any partial output.
template <typename Fn>
void run_cancellable(Slic3r::Print& print, const std::string& partial_output, Fn&& fn) {
    ActiveSlice active(print);
    bool cancelled = false;
    try {
        fn();
        cancelled = print.canceled();
    } catch (const Slic3r::CanceledException&) {
        cancelled = true;
    }
    if (cancelled) {
        boost::system::error_code ec;
        boost::filesystem::remove(partial_output, ec);
        throw SliceCancelled();
    }
    g_progress_percent = 100;
}


// Upstream OrcaSlicer's profiles write 0 in the per-feature filament settings to mean "the object's own filament".
// Snapmaker Orca's engine (and the settings it defines, wall_filament / sparse_infill_filament / solid_infill_filament,
// default 1) takes 0 as a filament index, which leaves the walls without a nozzle and crashes Arachne. A 0 is treated
// as unset, so the engine's own default applies. (Support filaments keep 0: there it is the engine's own "current".)
// A Bambu Lab printer profile gets Bambu's G-code layout (labels, wipe tower, tool order), as the slicer's own CLI decides
// it: printer_model starting "Bambu Lab" (Snapmaker Orca's Snapmaker_Orca.cpp, upstream OrcaSlicer's CLI likewise).
// Snapmaker Orca leaves the flag uninitialised headless (the engine patch makes it false), so it is set here; on the
// upstream engine (Android, Web) the flag keeps its default until those platforms move to this engine too.
void mark_bambu_printer(Slic3r::Print& print, const Slic3r::DynamicPrintConfig& config) {
#if __has_include("libslic3r/Feature/FullSpectrum/VirtualExtruder.hpp")
    const Slic3r::ConfigOptionString* model = config.option<Slic3r::ConfigOptionString>("printer_model");
    print.is_BBL_printer() = model != nullptr && model->value.compare(0, 9, "Bambu Lab") == 0;
#else
    (void) print; (void) config;
#endif
}

// Gives every filament of a multi-filament print its own copy of a Bambu filament profile's extruder-variant columns
// (Slic3r::expand_filament_variant_columns); the keys the request overrides are the per-filament ones.
void expand_filament_variants(Slic3r::DynamicPrintConfig& config, size_t profile_columns,
                              const std::vector<std::pair<std::string, std::string>>& config_overrides) {
    std::set<std::string> per_filament_keys;
    for (const auto& override_entry : config_overrides) per_filament_keys.insert(override_entry.first);
    Slic3r::expand_filament_variant_columns(config, profile_columns, per_filament_keys);
}

void drop_unset_feature_filaments(Slic3r::DynamicPrintConfig& profile) {
    for (const char* key : {"wall_filament", "sparse_infill_filament", "solid_infill_filament"})
        if (const Slic3r::ConfigOption* opt = profile.option(key); opt != nullptr && opt->getInt() == 0)
            profile.erase(key);
}

// Shared by slice_file() and load_mesh_preview(): load a model file and place it exactly where
// slicing will actually place it - real coordinates, not the mesh's own local origin. See
// slice_file()'s own history for why centering matters (a real off-bed print abort on real
// hardware, not a hypothetical).
Slic3r::Model load_and_place_model(const std::string& input_model_path, Slic3r::DynamicPrintConfig& config,
                                    const ModelTransform& transform = {}) {
    using namespace Slic3r;
    // Phase 0 (WO-16) real bug fix: the default LoadStrategy (AddDefaultInstances only) does NOT
    // include LoadModel, a *separate* bit (Format/bbs_3mf.hpp's LoadStrategy enum). STL loading
    // (Format/STL.cpp) never consults this flag at all, so it always worked; every .3mf file -
    // a real OrcaSlicer-native multi-object calibration file, a genuine single-object fixture,
    // and a hand-crafted minimal spec-valid one - failed identically ("supplied file couldn't be
    // read because it's empty") because _BBS_3MF_Importer::_handle_start_item short-circuits via
    // `!m_load_model || _create_object_instance(...)`: without LoadModel, m_load_model is false,
    // so every <build><item> is silently accepted (no error) but never actually materialized into
    // a real ModelObject - confirmed via temporary __android_log_print diagnostics added to the
    // vendored bbs_3mf.cpp (reverted once root-caused, same discipline as WO-13's own
    // GCode.cpp investigation) that traced this exact code path, both by itself and by first
    // proving the XML parse itself (every <object>/<mesh>/<vertex>/<triangle>/<build>/<item>
    // element) was already 100% correct. Desktop OrcaSlicer's own GUI file-open path passes this
    // flag explicitly; this headless engine's bridge simply never did.
    Model model = Model::read_from_file(input_model_path, &config,
        nullptr, LoadStrategy::AddDefaultInstances | LoadStrategy::LoadModel);
    if (model.objects.empty()) {
        throw std::runtime_error("No printable objects found in " + input_model_path);
    }
    for (ModelObject* object : model.objects) {
        if (object->instances.empty()) {
            object->add_instance();
        }
    }

    // Model::read_from_file() places each instance at the mesh's own local origin, not the
    // printer's actual bed coordinates. For a part whose local origin isn't already at its
    // footprint centroid (most STL exports aren't), that can put much of the object outside
    // printable_area - e.g. a part centered on (0,0) locally, sliced against a bed spanning
    // (0,0)-(256,256), ends up half off-bed. OrcaSlicer's own GUI always centers on import
    // (Plater::load_files() -> model.center_instances_around_point(bed center)); this headless
    // path skipped that step entirely. Caught live: a real print on the CC1 aborted right after
    // the purge line with Klipper's own "Move out of range" error, because roughly half of
    // squatchee_spin_mount's toolpath fell into negative Y.
    Points bed_shape = Slic3r::get_bed_shape(config);
    if (!bed_shape.empty()) {
        BoundingBox bed_bbox(bed_shape);
        model.center_instances_around_point(unscale(bed_bbox.center()));
    }

    for (ModelObject* object : model.objects) {
        object->ensure_on_bed();
    }

    // WO-15 part E: apply the user's real placement, on top of the default bed-centered one
    // above - not instead of it, since the default centering is still what a bare offset of
    // (0,0) should mean. Scale and rotation are applied before the extra XY offset (matches the
    // live GL preview's own model-matrix order in ModelViewer.kt: scale -> rotateZ -> translate)
    // so a rotated/scaled model's on-screen position and its sliced position agree exactly.
    // ensure_on_bed() runs again afterward because scaling or rotating can change which point of
    // the mesh is lowest, and only ensure_on_bed() (not this function) knows how to find that.
    if (transform.scale != 1.0 || transform.rotation_z_deg != 0.0 ||
        transform.offset_x_mm != 0.0 || transform.offset_y_mm != 0.0) {
        for (ModelObject* object : model.objects) {
            for (ModelInstance* instance : object->instances) {
                if (transform.scale != 1.0) {
                    instance->set_scaling_factor(instance->get_scaling_factor() * transform.scale);
                }
                if (transform.rotation_z_deg != 0.0) {
                    instance->set_rotation(Z, instance->get_rotation(Z) + Geometry::deg2rad(transform.rotation_z_deg));
                }
                instance->set_offset(X, instance->get_offset(X) + transform.offset_x_mm);
                instance->set_offset(Y, instance->get_offset(Y) + transform.offset_y_mm);
            }
            object->invalidate_bounding_box();
            object->ensure_on_bed();
        }
    }
    return model;
}

// Phase 9d: paint strokes and modifier/blocker volumes. Both arrive in the object's mesh frame - the "default
// placement" frame load_mesh_preview() reports (bed-centred instance, no user transform) - so they stay valid
// however the user moves, rotates or scales the object afterwards.
struct PaintStrokeRec { int kind; Slic3r::Vec3d origin; Slic3r::Vec3d dir; double radius; };

std::vector<std::string> split_text(const std::string& text, char sep) {
    std::vector<std::string> out;
    std::string item;
    std::istringstream stream(text);
    while (std::getline(stream, item, sep)) out.push_back(item);
    return out;
}

bool parse_vec3(const std::string& text, Slic3r::Vec3d& out) {
    std::vector<std::string> p = split_text(text, ',');
    if (p.size() != 3) return false;
    try { out = Slic3r::Vec3d(std::stod(p[0]), std::stod(p[1]), std::stod(p[2])); } catch (...) { return false; }
    return true;
}

std::vector<PaintStrokeRec> parse_strokes(const std::string& text) {
    std::vector<PaintStrokeRec> out;
    for (const std::string& rec : split_text(text, ';')) {
        std::vector<std::string> p = split_text(rec, ',');
        if (p.size() != 8) continue;
        try {
            PaintStrokeRec s;
            s.kind = std::stoi(p[0]);
            s.origin = Slic3r::Vec3d(std::stod(p[1]), std::stod(p[2]), std::stod(p[3]));
            s.dir = Slic3r::Vec3d(std::stod(p[4]), std::stod(p[5]), std::stod(p[6]));
            s.radius = std::stod(p[7]);
            if (((s.kind >= 0 && s.kind <= 3) || (s.kind >= 11 && s.kind <= 26)) && s.radius > 0.0 && s.dir.norm() > 1e-9) out.push_back(s);
        } catch (...) {}
    }
    return out;
}

void apply_paint_strokes(Slic3r::ModelObject* object, const std::vector<PaintStrokeRec>& strokes) {
    using namespace Slic3r;
    if (strokes.empty() || object->volumes.empty()) return;
    ModelVolume* volume = object->volumes.front();
    AABBMesh aabb(volume->mesh());
    TriangleSelector support_selector(volume->mesh());
    TriangleSelector seam_selector(volume->mesh());
    TriangleSelector material_selector(volume->mesh());
    Transform3d inv = volume->get_matrix().inverse(); // object frame -> the volume's own mesh frame
    bool support_painted = false, seam_painted = false, material_painted = false;
    for (const PaintStrokeRec& s : strokes) {
        Vec3d local_origin = inv * s.origin;
        Vec3d local_dir = (inv.linear() * s.dir).normalized();
        AABBMesh::hit_result hit = aabb.query_ray_hit(local_origin, local_dir);
        if (!hit.is_hit()) continue;
        const bool material = s.kind >= 11;
        const bool seam = !material && s.kind >= 2;
        const bool enforcer = (s.kind % 2) == 0;
        TriangleSelector::ClippingPlane clip;
        std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::SinglePointCursor::cursor_factory(
            hit.position().cast<float>(), local_origin.cast<float>(), static_cast<float>(s.radius),
            TriangleSelector::CursorType::SPHERE, Transform3d::Identity(), clip);
        // Multi-material regions: libslic3r stores the 1-based filament number as the facet state (1 = ENFORCER, 2 = BLOCKER, 3...).
        const EnforcerBlockerType state = material ? static_cast<EnforcerBlockerType>(s.kind - 10)
                                                   : (enforcer ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::BLOCKER);
        (material ? material_selector : seam ? seam_selector : support_selector).select_patch(hit.face(), std::move(cursor), state, Transform3d::Identity(), /*triangle_splitting=*/true);
        (material ? material_painted : seam ? seam_painted : support_painted) = true;
    }
    if (support_painted) volume->supported_facets.set(support_selector);
    if (seam_painted) volume->seam_facets.set(seam_selector);
    if (material_painted) volume->mmu_segmentation_facets.set(material_selector);
}

void apply_volume_specs(Slic3r::ModelObject* object, const std::string& text) {
    using namespace Slic3r;
    for (const std::string& rec : split_text(text, ';')) {
        std::vector<std::string> p = split_text(rec, ':');
        if (p.size() < 4) continue;
        Vec3d center, size;
        if (!parse_vec3(p[2], center) || !parse_vec3(p[3], size)) continue;
        if (size.minCoeff() <= 0.0) continue;
        ModelVolumeType type;
        if (p[0] == "modifier") type = ModelVolumeType::PARAMETER_MODIFIER;
        else if (p[0] == "blocker") type = ModelVolumeType::SUPPORT_BLOCKER;
        else if (p[0] == "enforcer") type = ModelVolumeType::SUPPORT_ENFORCER;
        else continue;
        TriangleMesh mesh;
        if (p[1] == "box") mesh = make_cube(size.x(), size.y(), size.z());
        else if (p[1] == "cylinder") { mesh = make_cylinder(0.5, 1.0); mesh.scale(Vec3f(size.x(), size.y(), size.z())); }
        else if (p[1] == "sphere") { mesh = make_sphere(0.5); mesh.scale(Vec3f(size.x(), size.y(), size.z())); }
        else continue;
        ModelVolume* volume = object->add_volume(std::move(mesh), type);
        volume->set_offset(center); // already in the object's frame
        if (type == ModelVolumeType::PARAMETER_MODIFIER && p.size() == 5 && !p[4].empty()) {
            ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Disable);
            for (const std::string& kv : split_text(p[4], '|')) {
                size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                volume->config.set_deserialize(kv.substr(0, eq), kv.substr(eq + 1), substitutions);
            }
        }
    }
}

// Applies one object's extras. Strokes and volumes arrive in the object's own frame - MeshGeometry coordinates
// minus MeshGeometry.origin - which does not depend on the bed the model is later centred on (the preview loads
// with a default bed, the real slice with the printer's own).
void apply_object_extras(Slic3r::ModelObject* object, const ObjectExtras& extras) {
    if (object == nullptr) return;
    apply_paint_strokes(object, parse_strokes(extras.paint_strokes));
    apply_volume_specs(object, extras.volume_specs);
    // Per-object settings: the options Orca's object list offers for an object (region and object options).
    static const Slic3r::PrintObjectConfig object_options;
    static const Slic3r::PrintRegionConfig region_options;
    for (const auto& [key, value] : extras.settings) {
        if (object_options.option(key) == nullptr && region_options.option(key) == nullptr)
            throw std::runtime_error("\"" + key + "\" can't be set for one object.");
        Slic3r::ConfigSubstitutionContext ctx(Slic3r::ForwardCompatibilitySubstitutionRule::Disable);
        try { object->config.set_deserialize(key, value, ctx); }
        catch (const std::exception& e) { throw std::runtime_error("Bad value for \"" + key + "\" on " + object->name + ": " + e.what()); }
    }
}


// PrintObject::m_id (the "id:" in GCode.cpp's "; printing object <name> id:<id>" labels) is only ever
// assigned by the upstream GUI; nothing in the headless engine initializes it, so it holds whatever the
// freshly allocated PrintObject's heap memory happened to contain - identical stale values for two
// objects once earlier slices in the same process have churned the heap. Give every object a real unique id.
void assign_print_object_ids(Slic3r::Print& print) {
    size_t next_id = 1;
    for (Slic3r::PrintObject* object : print.objects_mutable()) object->set_id(next_id++);
}

// The actual process/export tail, shared by slice_file() (fresh load from disk) and
// slice_paint_session() (an already-loaded, possibly support-painted in-memory model) - both end
// the same way, just start from a different Model.
void slice_model(Slic3r::Model& model, Slic3r::DynamicPrintConfig& config, const std::string& output_gcode_path) {
    using namespace Slic3r;

    // Captured now (world/bed coordinates, after centering) and before print.apply()/process(),
    // which are free to mutate `model` - see thumbnail_render.hpp. Real printer screens (COSMOS,
    // Klipperscreen, Mainsail/Fluidd file browsers) read this straight out of the .gcode file's
    // own embedded "; THUMBNAIL_BLOCK" comments; without a thumbnail_cb here, print.export_gcode()
    // silently skips writing them even though the default "thumbnails" config value already asks
    // for 48x48 and 300x300 PNGs.
    ThumbnailsGeneratorCallback thumbnail_cb = make_thumbnail_callback(model.mesh());

    Print print;
    for (ModelObject* object : model.objects) {
        print.auto_assign_extruders(object);
    }
    mark_bambu_printer(print, config);
    print.apply(model, config);
    assign_print_object_ids(print);

    StringObjectException validation_error = print.validate();
    if (!validation_error.string.empty()) {
        throw std::runtime_error("Validation failed: " + validation_error.string);
    }

    // A real result object, never nullptr: Snapmaker Orca's Print::export_gcode() writes result->conflict_result
    // unconditionally. The G-code file written is the same either way.
    GCodeProcessorResult gcode_result;
    run_cancellable(print, output_gcode_path, [&] {
        print.process();
        print.export_gcode(output_gcode_path, &gcode_result, thumbnail_cb);
    });
}

// The real Bambu-compatible .gcode.3mf bundle tail, shared by slice_bambu_bundle() (a fresh
// single-object load) and slice_multi_object_bambu_bundle() (a combined multi-object plate) -
// both end the same way, just start from a different already-loaded/placed Model, mirroring
// slice_model()'s own single-vs-multi-object sharing above. See slice_bambu_bundle()'s own
// header comment (this file, below) for why each PlateData/StoreParams field is set the way it
// is - that reasoning is unchanged here, just factored out so it isn't duplicated per caller.
// Prints an object with filament (tool) `tool_index`, 1-based; 0 leaves the default. The per-feature filament keys are
// what region_config_from_model_volume() reads (see slice_multi_object()'s comment on why "extruder" alone is inert).
// Snapmaker Orca names these wall_filament / sparse_infill_filament / solid_infill_filament; upstream Orca uses the six
// *_filament_id keys. Whichever this engine defines is set, so the bridge builds on both.
void assign_object_filament(Slic3r::ModelObject* object, int tool_index) {
    if (tool_index == 0)
        return;
    for (const char* key : {"outer_wall_filament_id", "inner_wall_filament_id", "sparse_infill_filament_id",
                             "internal_solid_filament_id", "top_surface_filament_id", "bottom_surface_filament_id",
                             "wall_filament", "sparse_infill_filament", "solid_infill_filament"}) {
        if (Slic3r::print_config_def.get(key) != nullptr) object->config.set(key, tool_index);
    }
}

void bundle_model(Slic3r::Model& model, Slic3r::DynamicPrintConfig& config, const std::string& output_bundle_path) {
    using namespace Slic3r;

    ThumbnailsGeneratorCallback thumbnail_cb = make_thumbnail_callback(model.mesh());
    // The last field (use_plate_box, upstream Orca only) is left at its default of true, so this compiles on both engine bases.
    ThumbnailsParams thumb_params{Vec2ds{Vec2d(512, 512)}, true, false, false, true, 0};
    ThumbnailsList thumbnails = thumbnail_cb(thumb_params);

    Print print;
    for (ModelObject* object : model.objects) {
        print.auto_assign_extruders(object);
    }
    mark_bambu_printer(print, config);
    print.apply(model, config);
    assign_print_object_ids(print);

    StringObjectException validation_error = print.validate();
    if (!validation_error.string.empty()) {
        throw std::runtime_error("Validation failed: " + validation_error.string);
    }

    std::string temp_gcode_path = output_bundle_path + ".gcode.tmp";
    GCodeProcessorResult gcode_result;
    run_cancellable(print, temp_gcode_path, [&] {
        print.process();
        print.export_gcode(temp_gcode_path, &gcode_result, thumbnail_cb);
    });

    PlateData plate_data;
    plate_data.plate_index = 0;
    plate_data.is_sliced_valid = true;
    plate_data.gcode_file = temp_gcode_path;
    plate_data.printer_model_id = config.opt_string("printer_model");
    plate_data.config = config;
    if (!thumbnails.empty()) {
        plate_data.plate_thumbnail.load_from(thumbnails.front());
    }
    plate_data.parse_filament_info(&gcode_result);
    // Each used filament's type, colour and catalogue id, as upstream's Plater fills them before writing slice_info
    // (Plater::export_3mf); the printer shows them and maps AMS trays by them. The id is the preset's filament_ids entry
    // when the request sets one (upstream converts Orca ids to Bambu's with its network plugin, which this has not).
    {
        const auto* types  = config.option<ConfigOptionStrings>("filament_type");
        const auto* colors = config.option<ConfigOptionStrings>("filament_colour");
        const auto* ids    = config.option<ConfigOptionStrings>("filament_ids");
        for (FilamentInfo& info : plate_data.slice_filaments_info) {
            info.type        = types && ! types->values.empty() ? types->get_at(info.id) : std::string();
            info.color       = colors && ! colors->values.empty() ? colors->get_at(info.id) : std::string("#FFFFFF");
            info.filament_id = ids && info.id < int(ids->values.size()) ? ids->values[info.id] : std::string();
        }
    }

    PlateDataPtrs plate_data_list = {&plate_data};
    std::vector<Preset*> project_presets; // deliberately empty - see slice_bambu_bundle()'s own header comment
    std::vector<ThumbnailData*> thumbnail_data_ptrs = {&plate_data.plate_thumbnail};

    StoreParams store_params;
    store_params.path = output_bundle_path.c_str(); // const char* on Snapmaker Orca, std::string upstream
    store_params.model = &model;
    store_params.plate_data_list = plate_data_list;
    store_params.project_presets = project_presets;
    store_params.config = &config;
    store_params.thumbnail_data = thumbnail_data_ptrs;
    store_params.strategy = SaveStrategy::Silence | SaveStrategy::WithGcode | SaveStrategy::SkipModel | SaveStrategy::SkipAuxiliary;
    store_params.export_plate_idx = 0;

    bool ok = store_bbs_3mf(store_params);
    boost::system::error_code ec;
    boost::filesystem::remove(temp_gcode_path, ec); // best-effort cleanup, not load-bearing for correctness
    model.remove_backup_path_if_exist(); // best-effort cleanup of the scratch dir set_temporary_dir() pointed at
    if (!ok) {
        throw std::runtime_error("Failed to write the .gcode.3mf bundle.");
    }
}

} // namespace

CutResult cut_mesh_soup(const std::vector<float>& soup, float z) {
    using namespace Slic3r;
    if (soup.empty() || soup.size() % 9 != 0) throw std::runtime_error("Unexpected mesh data for cutting.");
    indexed_triangle_set mesh;
    const size_t triangles = soup.size() / 9;
    mesh.vertices.reserve(triangles * 3);
    mesh.indices.reserve(triangles);
    for (size_t t = 0; t < triangles; ++t) {
        for (size_t k = 0; k < 3; ++k) mesh.vertices.emplace_back(soup[t * 9 + k * 3], soup[t * 9 + k * 3 + 1], soup[t * 9 + k * 3 + 2]);
        mesh.indices.emplace_back(int(t * 3), int(t * 3 + 1), int(t * 3 + 2));
    }
    its_merge_vertices(mesh);
    indexed_triangle_set upper, lower;
    cut_mesh(mesh, z, &upper, &lower, true);
    auto flatten = [](const indexed_triangle_set& its) {
        std::vector<float> out;
        out.reserve(its.indices.size() * 9);
        for (const auto& f : its.indices)
            for (int k = 0; k < 3; ++k) { const auto& v = its.vertices[f(k)]; out.push_back(v.x()); out.push_back(v.y()); out.push_back(v.z()); }
        return out;
    };
    return CutResult{flatten(upper), flatten(lower)};
}

namespace {
indexed_triangle_set soup_to_its(const std::vector<float>& soup) {
    using namespace Slic3r;
    if (soup.empty() || soup.size() % 9 != 0) throw std::runtime_error("Unexpected mesh data.");
    indexed_triangle_set mesh;
    const size_t triangles = soup.size() / 9;
    mesh.vertices.reserve(triangles * 3);
    mesh.indices.reserve(triangles);
    for (size_t t = 0; t < triangles; ++t) {
        for (size_t k = 0; k < 3; ++k) mesh.vertices.emplace_back(soup[t * 9 + k * 3], soup[t * 9 + k * 3 + 1], soup[t * 9 + k * 3 + 2]);
        mesh.indices.emplace_back(int(t * 3), int(t * 3 + 1), int(t * 3 + 2));
    }
    its_merge_vertices(mesh);
    return mesh;
}
} // namespace

std::vector<double> orient_mesh_soup(const std::vector<float>& soup) {
    using namespace Slic3r;
    Model model;
    ModelObject* object = model.add_object();
    object->add_volume(TriangleMesh(soup_to_its(soup)));
    ModelInstance* instance = object->add_instance();
    const Matrix3d before = instance->get_matrix().linear();
    orientation::orient(instance);
    const Matrix3d r = instance->get_matrix().linear() * before.inverse();
    return {r(0, 0), r(0, 1), r(0, 2), r(1, 0), r(1, 1), r(1, 2), r(2, 0), r(2, 1), r(2, 2)};
}

std::vector<ArrangeResult> arrange_models(const std::vector<ArrangeItem>& items, const std::vector<std::string>& profile_paths,
                                          const std::vector<std::pair<std::string, std::string>>& config_overrides, const ArrangeOptions& options) {
    using namespace Slic3r;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    for (const auto& profile_path : profile_paths) {
        DynamicPrintConfig profile_config;
        profile_config.load(profile_path, ForwardCompatibilitySubstitutionRule::Enable);
        drop_unset_feature_filaments(profile_config);
        config.apply(profile_config);
    }
    const size_t filament_columns = Slic3r::filament_variant_columns(config);
    for (const auto& [key, value] : config_overrides) config.set_deserialize_strict(key, value);
    expand_filament_variants(config, filament_columns, config_overrides);

    Model model;
    for (const ArrangeItem& item : items) {
        Model loaded = load_and_place_model(item.model_path, config, item.transform);
        if (loaded.objects.size() != 1) throw std::runtime_error("Arrange takes one object per model file.");
        model.add_object(*loaded.objects.front());
    }
    arrangement::ArrangePolygons polys;
    std::vector<Vec3d> offsets;
    std::vector<double> rotations;
    for (size_t i = 0; i < model.objects.size(); ++i) {
        ModelInstance* instance = model.objects[i]->instances.front();
        arrangement::ArrangePolygon ap = get_instance_arrange_poly(instance, config);
        ap.itemid = int(i);
        polys.push_back(std::move(ap));
        offsets.push_back(instance->get_offset());
        rotations.push_back(instance->get_rotation(Z));
    }

    // init_arrange_params (ArrangeJob.cpp) without the GUI: the arrange settings are Orca's defaults unless given.
    auto number = [&](const char* key, double fallback) { const ConfigOption* o = config.option(key); return o != nullptr ? o->getFloat() : fallback; };
    arrangement::ArrangeParams params;
    params.clearance_height_to_rod = float(number("extruder_clearance_height_to_rod", 0));
    params.clearance_height_to_lid = float(number("extruder_clearance_height_to_lid", 0));
    params.clearance_radius = float(number("extruder_clearance_radius", 0));
    params.printable_height = float(number("printable_height", 256));
    params.nozzle_height = float(number("nozzle_height", 0));
    if (const auto* best = config.option<ConfigOptionPoint>("best_object_pos")) params.align_center = best->value;
    params.allow_rotations = options.allow_rotations;
    params.min_obj_distance = scaled(options.distance_mm);
    params.align_to_y_axis = options.align_to_y_axis;
    if (const auto* seq = config.option<ConfigOptionEnum<PrintSequence>>("print_sequence")) params.is_seq_print = seq->value == PrintSequence::ByObject;
    if (params.is_seq_print) { params.bed_shrink_x = BED_SHRINK_SEQ_PRINT; params.bed_shrink_y = BED_SHRINK_SEQ_PRINT; }
    params.progressind = [](unsigned, std::string) {};

    update_arrange_params(params, &config, polys);
    update_selected_items_inflation(polys, &config, params);
    update_selected_items_axis_align(polys, &config, params);
    const Points bedpts = get_shrink_bedpts(&config, params);
    arrangement::arrange(polys, {}, bedpts, params);

    std::vector<ArrangeResult> out(polys.size());
    for (const auto& ap : polys) {
        const size_t i = size_t(ap.itemid);
        out[i].dx_mm = unscale<double>(ap.translation.x()) - offsets[i].x();
        out[i].dy_mm = unscale<double>(ap.translation.y()) - offsets[i].y();
        out[i].rotation_deg = (ap.rotation - rotations[i]) * 180.0 / M_PI;
        out[i].plate = ap.bed_idx;
    }
    return out;
}

void request_cancel() {
    g_cancel_requested = true;
    std::lock_guard<std::mutex> lock(g_active_mutex);
    if (g_active_print != nullptr) g_active_print->cancel();
}

void reset_cancel() { g_cancel_requested = false; g_progress_percent = 0; }

int slice_progress() { return g_progress_percent.load(); }

void slice_file(const std::string& input_model_path,
                 const std::string& output_gcode_path,
                 const std::vector<std::string>& profile_paths,
                 const std::vector<std::pair<std::string, std::string>>& config_overrides,
                 const ModelTransform& transform) {
    using namespace Slic3r;

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    for (const std::string& profile_path : profile_paths) {
        DynamicPrintConfig profile_config;
        profile_config.load(profile_path, ForwardCompatibilitySubstitutionRule::Enable);
        drop_unset_feature_filaments(profile_config);
        config.apply(profile_config);
    }

    const size_t filament_columns = Slic3r::filament_variant_columns(config);
    for (const auto& [key, value] : config_overrides) {
        config.set_deserialize_strict(key, value);
    }
    expand_filament_variants(config, filament_columns, config_overrides);

    Model model = load_and_place_model(input_model_path, config, transform);
    slice_model(model, config, output_gcode_path);
}

// Phase 6 (Consumer Slicer Plan §16): the real Bambu-compatible .gcode.3mf bundle - see
// slic3r_engine.hpp's own header comment for why this builds PlateData directly rather than
// going through the GUI-only PartPlateList::store_to_3mf_structure. Every field set below was
// checked against the real writer (libslic3r/Format/bbs_3mf.cpp) that actually consumes it:
// - gcode_file/is_sliced_valid: _add_gcode_file_to_archive() reads gcode_file as a real
//   filesystem path (must exist on disk), copies it into "Metadata/plate_N.gcode" inside the
//   zip, and rewrites plate_data->gcode_file to that in-archive path itself - the caller doesn't
//   need to place the file there manually.
// - gcode_file_md5: computed and written by the exporter itself (a real MD5 over the same file),
//   not something this function needs to precompute.
// - slice_filaments_info: filled by PlateData::parse_filament_info(GCodeProcessorResult*), a
//   real libslic3r method - requires a real (non-null) GCodeProcessorResult from
//   Print::export_gcode(), unlike every other slice path in this file which passes nullptr there
//   since nothing else needs it.
// - objects_and_instances: left empty deliberately - the <instance> block that reads it in
//   _add_slice_info_config_file_to_archive() is itself gated on `!m_skip_model`, and this
//   function's own SaveStrategy sets SkipModel (matching Bambu's real "send to printer" flags,
//   Plater.cpp's own PLATE_TO_PRINTER strategy) - so nothing ever reads this field for this
//   strategy combination.
// - printer_model_id: the config's own real "printer_model" value (e.g. "Bambu Lab A1"), not
//   invented - already present in every bundled Bambu machine.json.
// - project_presets stays empty in the returned StoreParams (set by the caller below): the
//   archive writer only embeds project presets when `project_presets.size() > 0`
//   (_BBS_3MF_Exporter::_save_model_to_file), so an empty vector is a real, intentional no-op,
//   not a missing-data workaround.
void slice_bambu_bundle(const std::string& input_model_path,
                         const std::string& output_bundle_path,
                         const std::vector<std::string>& profile_paths,
                         const std::vector<std::pair<std::string, std::string>>& config_overrides,
                         const ModelTransform& transform) {
    using namespace Slic3r;

    // Real bug fix, root-caused via a temporary logcat diagnostic (a boost::log sink forwarding
    // to __android_log_print, since this build otherwise never routes BOOST_LOG_TRIVIAL anywhere
    // observable - since removed, its job done): store_bbs_3mf's own
    // _add_project_config_file_to_archive() writes a real temp file under Model::get_backup_path()
    // before zipping it in - and this app never calls Slic3r::set_temporary_dir(), so
    // Utils.cpp's own g_temporary_dir is empty, which makes get_backup_path() fall back to the
    // bare, non-writable, root-relative "/orcaslicer_model/..." (Model.cpp) - confirmed on-device
    // as a real "Read-only file system" failure creating that path's "/3D/Objects" subdir, not
    // guessed. output_bundle_path is always inside the caller's own writable app cache dir (every
    // Kotlin call site passes an appContext.cacheDir-relative path), so its parent directory is a
    // real, guaranteed-writable place to point the engine's temp/backup machinery at.
    set_temporary_dir(boost::filesystem::path(output_bundle_path).parent_path().string());

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    for (const std::string& profile_path : profile_paths) {
        DynamicPrintConfig profile_config;
        profile_config.load(profile_path, ForwardCompatibilitySubstitutionRule::Enable);
        drop_unset_feature_filaments(profile_config);
        config.apply(profile_config);
    }
    const size_t filament_columns = Slic3r::filament_variant_columns(config);
    for (const auto& [key, value] : config_overrides) {
        config.set_deserialize_strict(key, value);
    }
    expand_filament_variants(config, filament_columns, config_overrides);

    Model model = load_and_place_model(input_model_path, config, transform);
    bundle_model(model, config, output_bundle_path);
}

// Phase 6 follow-up (WO-23): the real multi-object counterpart to slice_bambu_bundle() above -
// same relationship slice_multi_object() already has to slice_file()/slice_model(). Each
// (path, transform) pair is loaded and placed exactly the way slice_multi_object() already does
// (real per-file bed-centering, then that object's own transform on top), copied into one
// combined Model, then handed to the same bundle_model() tail slice_bambu_bundle() uses - so a
// multi-object Bambu plate gets the identical real .gcode.3mf bundle shape (real embedded
// G-code, real MD5, real thumbnail, real slice_info.config) a single-object one does, not a
// second, parallel bundle-writing path. Does not itself detect overlapping objects, same real,
// deliberate gap slice_multi_object()'s own header comment documents.
void slice_multi_object_bambu_bundle(const std::vector<std::pair<std::string, ModelTransform>>& objects,
                                      const std::string& output_bundle_path,
                                      const std::vector<std::string>& profile_paths,
                                      const std::vector<std::pair<std::string, std::string>>& config_overrides,
                                      const std::vector<ObjectExtras>& extras) {
    std::vector<std::tuple<std::string, ModelTransform, int>> with_tools;
    with_tools.reserve(objects.size());
    for (const auto& [path, transform] : objects)
        with_tools.emplace_back(path, transform, 0);
    slice_multi_object_bambu_bundle(with_tools, output_bundle_path, profile_paths, config_overrides, extras);
}

void slice_multi_object_bambu_bundle(const std::vector<std::tuple<std::string, ModelTransform, int>>& objects,
                                      const std::string& output_bundle_path,
                                      const std::vector<std::string>& profile_paths,
                                      const std::vector<std::pair<std::string, std::string>>& config_overrides,
                                      const std::vector<ObjectExtras>& extras) {
    using namespace Slic3r;

    if (objects.empty()) {
        throw std::runtime_error("No objects to slice.");
    }

    // See slice_bambu_bundle()'s own comment on this call - identical real bug fix, needed here
    // for the same reason.
    set_temporary_dir(boost::filesystem::path(output_bundle_path).parent_path().string());

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    for (const std::string& profile_path : profile_paths) {
        DynamicPrintConfig profile_config;
        profile_config.load(profile_path, ForwardCompatibilitySubstitutionRule::Enable);
        drop_unset_feature_filaments(profile_config);
        config.apply(profile_config);
    }
    const size_t filament_columns = Slic3r::filament_variant_columns(config);
    for (const auto& [key, value] : config_overrides) {
        config.set_deserialize_strict(key, value);
    }
    expand_filament_variants(config, filament_columns, config_overrides);

    Model combined;
    size_t object_index = 0;
    for (const auto& [path, transform, tool_index] : objects) {
        Model loaded = load_and_place_model(path, config, transform);
        const size_t first_object_of_file = combined.objects.size();
        for (ModelObject* object : loaded.objects) {
            assign_object_filament(object, tool_index);
            combined.add_object(*object);
        }
        if (object_index < extras.size() && combined.objects.size() > first_object_of_file)
            apply_object_extras(combined.objects[first_object_of_file], extras[object_index]);
        ++object_index;
    }

    bundle_model(combined, config, output_bundle_path);
}

// Phase 1 (Consumer Slicer Plan §16): a real multi-object build plate, sliced together into one
// G-code file - the actual capability Phase 0's count_model_objects()/MultiObjectModelDeviceTest
// only proved was structurally possible. Each (path, transform) pair is loaded and placed exactly
// the way slice_file()'s single-object path already does - real per-file bed-centering, then that
// object's own real transform applied on top (see load_and_place_model()'s own comment on why
// that order matters) - so a project's saved per-object ModelTransform (its arranged position,
// not just move/rotate/scale) reproduces identically at slice time. The resulting ModelObjects are
// copied (Model::add_object(const ModelObject&), a real libslic3r API, not a workaround) into one
// combined Model and sliced once, the same shared slice_model() tail every other slice path uses -
// so multi-object G-code gets the same real thumbnail/validation/extruder-assignment handling as
// everything else, not a second, parallel code path.
//
// Does not itself detect or prevent overlapping objects - collision detection/auto-arrange is a
// real, separate Phase 1 UI concern (Consumer Slicer Plan §16), not something the slicer engine
// enforces; slicing genuinely overlapping objects produces genuinely overlapping (garbage)
// geometry, same as it would in the real upstream GUI.
void slice_multi_object(const std::vector<std::tuple<std::string, ModelTransform, int>>& objects,
                         const std::string& output_gcode_path,
                         const std::vector<std::string>& profile_paths,
                         const std::vector<std::pair<std::string, std::string>>& config_overrides,
                         const std::vector<ObjectExtras>& extras,
                         const std::string& virtual_extruders_json) {
    using namespace Slic3r;

    if (objects.empty()) {
        throw std::runtime_error("No objects to slice.");
    }

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    for (const std::string& profile_path : profile_paths) {
        DynamicPrintConfig profile_config;
        profile_config.load(profile_path, ForwardCompatibilitySubstitutionRule::Enable);
        drop_unset_feature_filaments(profile_config);
        config.apply(profile_config);
    }
    const size_t filament_columns = Slic3r::filament_variant_columns(config);
    for (const auto& [key, value] : config_overrides) {
        config.set_deserialize_strict(key, value);
    }
    expand_filament_variants(config, filament_columns, config_overrides);

    Model combined;
    size_t object_index = 0;
    for (const auto& [path, transform, tool_index] : objects) {
        Model loaded = load_and_place_model(path, config, transform);
        const size_t first_object_of_file = combined.objects.size();
        for (ModelObject* object : loaded.objects) {
            // Phase 8 follow-up (§11, WO-25): real per-object tool assignment - root-caused via a
            // temporary __android_log_print diagnostic (since removed) after the generic
            // per-object "extruder" config key alone was confirmed to reach ModelObject::config
            // correctly (has()=true, extruder()=the real requested value) but produced zero
            // observable effect on the sliced G-code. Read PrintApply.cpp/PrintObject.cpp
            // directly to find why: region_config_from_model_volume() (PrintObject.cpp), the
            // real function that turns a ModelObject's config into the per-region config the
            // GCode generator actually reads tool selection from, only looks at the concrete
            // per-feature filament-id keys below - "extruder" itself is normally translated into
            // those by DynamicPrintConfig::normalize_fdm(), but that call is commented out in
            // this vendored engine's own PrintApply.cpp (line ~1226) - so "extruder" alone is
            // silently inert here, a real, verified fact about this specific engine build, not
            // assumed from upstream OrcaSlicer documentation. Setting the six real per-feature
            // keys directly (normalize_fdm's own real behavior, reproduced by hand) is what
            // actually reaches GCode's tool ordering. Must be set *before* combined.add_object()
            // below - Model::add_object's own real implementation (Model.cpp) force-sets
            // "extruder" (only) to 1 on its clone when the source lacks a real nonzero value;
            // it doesn't touch these keys, so order matters less for them specifically, but
            // matching slice_bambu_bundle's existing convention keeps this one code shape.
            // Snapmaker Orca names these wall_filament / sparse_infill_filament / solid_infill_filament; upstream Orca
            // uses the six *_filament_id keys. Whichever this engine defines is set, so the bridge builds on both.
            assign_object_filament(object, tool_index);
            combined.add_object(*object);
        }
        // Applied to the copy inside `combined` (Model::add_object clones the object), which is what gets sliced.
        if (object_index < extras.size() && combined.objects.size() > first_object_of_file)
            apply_object_extras(combined.objects[first_object_of_file], extras[object_index]);
        ++object_index;
    }

    // PrusaSlicer 2.9.6 virtual extruders go on the combined Model, as its 3MF reader puts them there
    // (Format/3mf.cpp _extract_full_spectrum_from_archive: deserialize, then normalize). Print::apply() keeps the ones
    // this printer's physical count can print (filter_virtual_extruders_for_physical_count).
    if (!virtual_extruders_json.empty()) {
#ifdef SLIC3R_PRUSA_VIRTUAL_EXTRUDERS
        const FullSpectrum::FullSpectrumConfig fs_config = FullSpectrum::deserialize_virtual_extruders_from_json(virtual_extruders_json);
        combined.virtual_extruders = FullSpectrum::normalize_virtual_extruders(fs_config.virtual_extruders);
#else
        throw std::runtime_error("This slicing engine does not support virtual extruders.");
#endif
    }

    slice_model(combined, config, output_gcode_path);
}

namespace {

// Preview-only simplification by vertex clustering: vertices snap to a g x g x g grid over the bounding box, each cell becomes one vertex
// (the average of its members), and triangles that collapse are dropped. Slicing always uses the original mesh; this only bounds how much
// the on-screen preview has to hold (a 1.4M-triangle model needs ~100 MB as interleaved floats, which is more than the app's heap allows).
indexed_triangle_set cluster_mesh(const indexed_triangle_set& its, const Slic3r::Vec3f& lo, const Slic3r::Vec3f& span, int g) {
    std::unordered_map<uint64_t, std::pair<Slic3r::Vec3d, int>> cells;
    std::vector<uint64_t> key_of(its.vertices.size());
    for (size_t i = 0; i < its.vertices.size(); ++i) {
        const Slic3r::Vec3f& v = its.vertices[i];
        auto cell = [&](int axis) { return static_cast<uint64_t>(std::min(g - 1, std::max(0, static_cast<int>((v(axis) - lo(axis)) / span(axis) * g)))); };
        const uint64_t key = cell(0) | (cell(1) << 16) | (cell(2) << 32);
        key_of[i] = key;
        auto& entry = cells[key];
        entry.first += v.cast<double>(); entry.second += 1;
    }
    indexed_triangle_set out;
    std::unordered_map<uint64_t, int> index_of;
    out.vertices.reserve(cells.size());
    for (const auto& [key, entry] : cells) {
        index_of[key] = static_cast<int>(out.vertices.size());
        out.vertices.push_back((entry.first / entry.second).cast<float>());
    }
    for (const Slic3r::Vec3i32& tri : its.indices) {
        const int a = index_of[key_of[tri(0)]], b = index_of[key_of[tri(1)]], c = index_of[key_of[tri(2)]];
        if (a != b && b != c && a != c) out.indices.emplace_back(a, b, c);
    }
    return out;
}

indexed_triangle_set decimate_for_preview(const indexed_triangle_set& its, size_t max_triangles) {
    if (its.indices.size() <= max_triangles || its.vertices.empty()) return its;
    Slic3r::Vec3f lo = its.vertices.front(), hi = its.vertices.front();
    for (const Slic3r::Vec3f& v : its.vertices) { lo = lo.cwiseMin(v); hi = hi.cwiseMax(v); }
    Slic3r::Vec3f span = (hi - lo).cwiseMax(Slic3r::Vec3f::Constant(1e-6f));
    // Triangle count grows roughly with g^2 for a surface, so a few multiplicative steps land under the budget.
    int g = 512;
    indexed_triangle_set best;
    for (int attempt = 0; attempt < 8; ++attempt) {
        indexed_triangle_set candidate = cluster_mesh(its, lo, span, g);
        const size_t count = candidate.indices.size();
        if (count <= max_triangles && count > 0) { best = std::move(candidate); if (count * 10 >= max_triangles * 6) break; g = std::min(4096, static_cast<int>(g * std::sqrt(static_cast<double>(max_triangles) / count) * 0.98)); if (g >= 4096) break; continue; }
        if (count == 0) { g = std::min(4096, g * 2); continue; }
        g = std::max(8, static_cast<int>(g * std::sqrt(static_cast<double>(max_triangles) / count) * 0.9));
    }
    return best.indices.empty() ? its : std::move(best);
}

constexpr size_t kPreviewTriangleBudget = 250000;

} // namespace

std::vector<float> load_mesh_preview(const std::string& input_model_path) {
    using namespace Slic3r;

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    Model model = load_and_place_model(input_model_path, config);
    TriangleMesh mesh = model.mesh();
    const indexed_triangle_set its = decimate_for_preview(mesh.its, kPreviewTriangleBudget);

    // WO-15 part E: the first object's first instance's own offset - the real pivot
    // load_and_place_model()'s rotate/scale (ModelInstance::set_rotation/set_scaling_factor,
    // both about the instance's local origin, not its bounding-box center) will actually use.
    // Prepended so the Kotlin-side interactive preview can rotate/scale around the identical
    // point the native engine will at slice time, rather than guessing at a bounding-box center
    // that's wrong for any mesh whose local origin isn't its own centroid. v1 scope (matches
    // open_paint_session's own): meaningful for the common single-object/single-instance case
    // this whole viewer targets; a multi-object model still previews correctly, it just has no
    // single well-defined pivot for this purpose.
    Vec3d origin = model.objects.empty() || model.objects.front()->instances.empty()
        ? Vec3d::Zero() : model.objects.front()->instances.front()->get_offset();

    std::vector<float> buffer;
    buffer.reserve(3 + its.indices.size() * 3 * 6);
    buffer.push_back(static_cast<float>(origin.x()));
    buffer.push_back(static_cast<float>(origin.y()));
    buffer.push_back(static_cast<float>(origin.z()));
    for (const Vec3i32& tri : its.indices) {
        const Vec3f& v0 = its.vertices[tri(0)];
        const Vec3f& v1 = its.vertices[tri(1)];
        const Vec3f& v2 = its.vertices[tri(2)];
        Vec3f normal = (v1 - v0).cross(v2 - v0);
        float len = normal.norm();
        if (len > 1e-9f) normal /= len;
        for (const Vec3f& v : {v0, v1, v2}) {
            buffer.push_back(v.x()); buffer.push_back(v.y()); buffer.push_back(v.z());
            buffer.push_back(normal.x()); buffer.push_back(normal.y()); buffer.push_back(normal.z());
        }
    }
    return buffer;
}

int count_model_objects(const std::string& input_model_path) {
    using namespace Slic3r;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    Model model = load_and_place_model(input_model_path, config);
    return static_cast<int>(model.objects.size());
}

// --- Support painting (WO-14 part D) ---------------------------------------------------------
//
// Every transform/argument shape below was traced against the real upstream GUI source
// (slic3r/GUI/Gizmos/GLGizmoPainterBase.cpp, vendored but not compiled into this headless
// engine - SLIC3R_GUI=OFF) rather than guessed, since a wrong transform here would silently
// paint the wrong triangles - a real, safety-relevant correctness risk for a feature whose whole
// point is controlling where supports print. In particular:
//   - AABBMesh/TriangleSelector operate on the volume's own RAW LOCAL mesh, not the world-
//     transformed one - the GUI's own mouse-to-mesh raycast inverse-transforms the ray into
//     local space first (GLGizmoPainterBase.cpp's own `camera_pos = trafo_matrix.inverse() *
//     camera.get_position()` and equivalent for the ray), then intersects locally.
//   - `select_patch()` itself takes the *no-translate* transform (rotation/scale only) - used
//     internally for face-normal-relative math where translation is irrelevant - while the
//     Cursor object passed into it is built with the *full* (translated) transform, since it
//     needs real-world-sized radius comparisons against local-space geometry.
namespace {

using namespace Slic3r;

struct PaintSession {
    // Guards every field below against concurrent JNI calls on the same handle. The Kotlin side
    // is expected to serialize its own paint-stroke dispatch (a drag gesture fires many touch
    // samples quickly), but TriangleSelector/AABBMesh have no thread-safety of their own, and a
    // wrong-triangle race here is a real correctness risk, not just a performance one - this is
    // defense in depth, not a substitute for that Kotlin-side serialization.
    std::mutex mutex;
    Model model;
    DynamicPrintConfig config;
    ModelVolume* volume = nullptr;
    std::unique_ptr<AABBMesh> aabb;
    std::unique_ptr<TriangleSelector> selector;
    Transform3d trafo;               // instance * volume, WITH translation
    Transform3d trafo_not_translate; // instance * volume, WITHOUT translation
};

std::mutex g_paint_sessions_mutex;
std::unordered_map<PaintSessionHandle, std::unique_ptr<PaintSession>> g_paint_sessions;
std::atomic<PaintSessionHandle> g_next_paint_handle{1};

// Only looks the session up (briefly holding the map lock); does NOT lock the session's own
// mutex - every caller below does that itself, immediately, so the map lock is never held while
// doing real work on a session.
PaintSession& find_paint_session(PaintSessionHandle handle) {
    std::lock_guard<std::mutex> lock(g_paint_sessions_mutex);
    auto it = g_paint_sessions.find(handle);
    if (it == g_paint_sessions.end()) {
        throw std::runtime_error("Unknown or already-closed paint session.");
    }
    return *it->second;
}

} // namespace

PaintSessionHandle open_paint_session(const std::string& input_model_path, const ModelTransform& transform) {
    auto session = std::make_unique<PaintSession>();
    session->config = DynamicPrintConfig::full_print_config();
    session->model = load_and_place_model(input_model_path, session->config, transform);

    ModelObject* object = session->model.objects.front();
    if (object->instances.empty() || object->volumes.empty()) {
        throw std::runtime_error("No paintable volume found in " + input_model_path);
    }
    // v1 scope: the first object's first volume - the common single-part case every model used
    // in this app so far actually is. A multi-volume/multi-object model still slices (and still
    // gets a real, correctly-centered 3D preview via load_mesh_preview), it just can't be
    // support-painted yet.
    ModelInstance* instance = object->instances.front();
    ModelVolume* volume = object->volumes.front();
    session->volume = volume;
    session->trafo = instance->get_transformation().get_matrix() * volume->get_matrix();
    session->trafo_not_translate = instance->get_transformation().get_matrix_no_offset() * volume->get_matrix_no_offset();
    session->aabb = std::make_unique<AABBMesh>(volume->mesh());
    session->selector = std::make_unique<TriangleSelector>(volume->mesh());

    PaintSessionHandle handle = g_next_paint_handle++;
    std::lock_guard<std::mutex> lock(g_paint_sessions_mutex);
    g_paint_sessions[handle] = std::move(session);
    return handle;
}

void paint_stroke(PaintSessionHandle handle, double originX, double originY, double originZ,
                   double dirX, double dirY, double dirZ, double radiusMm, bool enforcer) {
    PaintSession& s = find_paint_session(handle);
    std::lock_guard<std::mutex> lock(s.mutex);

    Transform3d inv = s.trafo.inverse();
    Vec3d local_origin = inv * Vec3d(originX, originY, originZ);
    Vec3d local_dir = (inv.linear() * Vec3d(dirX, dirY, dirZ)).normalized();

    AABBMesh::hit_result hit = s.aabb->query_ray_hit(local_origin, local_dir);
    if (!hit.is_hit()) return; // a stray touch missing the mesh entirely - a normal, silent no-op

    Vec3f mesh_hit = hit.position().cast<float>();
    Vec3f camera_pos = local_origin.cast<float>();
    TriangleSelector::ClippingPlane clip; // default-constructed: inactive (offset == FLT_MAX)
    std::unique_ptr<TriangleSelector::Cursor> cursor = TriangleSelector::SinglePointCursor::cursor_factory(
        mesh_hit, camera_pos, static_cast<float>(radiusMm), TriangleSelector::CursorType::SPHERE, s.trafo, clip);
    s.selector->select_patch(hit.face(), std::move(cursor),
        enforcer ? EnforcerBlockerType::ENFORCER : EnforcerBlockerType::BLOCKER,
        s.trafo_not_translate, /*triangle_splitting=*/true);
    // Persisted after every stroke, not just on commit: get_painted_facets() (the live overlay
    // while painting) and slice_paint_session() both read supported_facets, and a mid-drag crash
    // or dismissed dialog should still leave whatever was actually painted, not lose it.
    s.volume->supported_facets.set(*s.selector);
}

std::vector<float> get_painted_facets(PaintSessionHandle handle) {
    PaintSession& s = find_paint_session(handle);
    std::lock_guard<std::mutex> lock(s.mutex);
    indexed_triangle_set enforced = s.volume->supported_facets.get_facets(*s.volume, EnforcerBlockerType::ENFORCER);
    // Position-only (no normals) - this is an unlit highlight overlay, not a second lit mesh, and
    // transformed into the same world-space coordinates load_mesh_preview's vertices already use
    // so it draws in registration with the base model without the Kotlin side needing its own
    // copy of this transform.
    std::vector<float> buffer;
    buffer.reserve(enforced.indices.size() * 3 * 3);
    for (const Vec3i32& tri : enforced.indices) {
        for (int i = 0; i < 3; ++i) {
            Vec3d local = enforced.vertices[tri(i)].cast<double>();
            Vec3d world = s.trafo * local;
            buffer.push_back(static_cast<float>(world.x()));
            buffer.push_back(static_cast<float>(world.y()));
            buffer.push_back(static_cast<float>(world.z()));
        }
    }
    return buffer;
}

void slice_paint_session(PaintSessionHandle handle, const std::string& output_gcode_path,
                          const std::vector<std::string>& profile_paths,
                          const std::vector<std::pair<std::string, std::string>>& config_overrides) {
    PaintSession& s = find_paint_session(handle);
    std::lock_guard<std::mutex> lock(s.mutex);
    for (const std::string& profile_path : profile_paths) {
        DynamicPrintConfig profile_config;
        profile_config.load(profile_path, ForwardCompatibilitySubstitutionRule::Enable);
        drop_unset_feature_filaments(profile_config);
        s.config.apply(profile_config);
    }
    const size_t filament_columns = Slic3r::filament_variant_columns(s.config);
    for (const auto& [key, value] : config_overrides) {
        s.config.set_deserialize_strict(key, value);
    }
    expand_filament_variants(s.config, filament_columns, config_overrides);
    slice_model(s.model, s.config, output_gcode_path);
}

void close_paint_session(PaintSessionHandle handle) {
    std::unique_ptr<PaintSession> session;
    {
        std::lock_guard<std::mutex> lock(g_paint_sessions_mutex);
        auto it = g_paint_sessions.find(handle);
        if (it == g_paint_sessions.end()) return; // already closed, or never opened - a safe no-op
        session = std::move(it->second);
        g_paint_sessions.erase(it);
    }
    // Removed from the map above (so no new operation can find it) - now wait for any operation
    // already in flight on it to finish before letting `session` go out of scope and destroy it.
    std::lock_guard<std::mutex> session_lock(session->mutex);
}

} // namespace engine
