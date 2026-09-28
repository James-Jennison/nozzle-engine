// Adapted from the owner's own orcaslicer-android-engine project (local, /mnt/faststorage/orcaslicer-android-engine),
// which cross-compiles upstream OrcaSlicer's libslic3r for Android. Licensed AGPL-3.0-or-later
// (same as OrcaSlicer itself, and the same family already governing this app since the Helix port).
// See THIRD_PARTY_NOTICES.md and docs/WORK_ORDER.md's WO-13 entry.
// JNI bridge between the Android app and libslic3r. Java-side counterpart:
// org.orcaslicer.engine.NativeEngine (not yet written -- see docs/PLAN.md
// phase 6). The actual slicing pipeline lives in slic3r_engine.cpp, shared
// with the standalone on-device CLI test tool (cli_test.cpp).
//
// Print::process()/export_gcode() are explicitly documented (Print.hpp) as
// safe to call from a background thread, with no wxWidgets/GUI dependency
// in the slicing path itself.
//
// Cancellation and progress are polled, not called back: nativeCancelSlice() flips the running
// Print's own cancel flag (the slice call then throws CancellationException), and
// nativeSliceProgress() reads the engine's latest status percent.
#include <jni.h>
#include <string>
#include <vector>
#include <new>
#include <stdexcept>

#include "slic3r_engine.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/PrintConfig.hpp"

namespace {

std::string jstring_to_string(JNIEnv* env, jstring s) {
    if (s == nullptr) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    if (chars == nullptr) return {};
    std::string result(chars);
    env->ReleaseStringUTFChars(s, chars);
    return result;
}

void throw_java_exception(JNIEnv* env, const std::string& message) {
    jclass exClass = env->FindClass("java/lang/RuntimeException");
    if (exClass != nullptr) {
        env->ThrowNew(exClass, message.c_str());
    }
}

void throw_java_exception(JNIEnv* env, const std::exception& ex) {
    if (dynamic_cast<const engine::SliceCancelled*>(&ex) != nullptr) {
        jclass cancelled = env->FindClass("java/util/concurrent/CancellationException");
        if (cancelled != nullptr) env->ThrowNew(cancelled, ex.what());
        return;
    }
    if (dynamic_cast<const std::bad_alloc*>(&ex) != nullptr) {
        throw_java_exception(env, std::string("Not enough memory to slice this model. Try fewer or smaller objects, a larger layer height, or close other apps."));
        return;
    }
    throw_java_exception(env, std::string(ex.what()));
}

jfloatArray to_jfloat_array(JNIEnv* env, const std::vector<float>& buffer) {
    jfloatArray result = env->NewFloatArray(static_cast<jsize>(buffer.size()));
    if (result != nullptr) {
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(buffer.size()), buffer.data());
    }
    return result;
}

std::vector<std::string> to_string_vector(JNIEnv* env, jobjectArray array) {
    std::vector<std::string> result;
    if (array != nullptr) {
        jsize count = env->GetArrayLength(array);
        for (jsize i = 0; i < count; ++i) {
            auto element = static_cast<jstring>(env->GetObjectArrayElement(array, i));
            result.push_back(jstring_to_string(env, element));
            env->DeleteLocalRef(element);
        }
    }
    return result;
}

std::vector<double> to_double_vector(JNIEnv* env, jdoubleArray array) {
    std::vector<double> result;
    if (array != nullptr) {
        jsize count = env->GetArrayLength(array);
        result.resize(count);
        env->GetDoubleArrayRegion(array, 0, count, result.data());
    }
    return result;
}

std::vector<int> to_int_vector(JNIEnv* env, jintArray array) {
    std::vector<int> result;
    if (array != nullptr) {
        jsize count = env->GetArrayLength(array);
        result.resize(count);
        env->GetIntArrayRegion(array, 0, count, result.data());
    }
    return result;
}

} // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeGetVersion(JNIEnv* env, jclass) {
    return env->NewStringUTF(SLIC3R_VERSION);
}

// Temporary diagnostic, not part of the copied original - see WO-13's investigation notes
// (docs/WORK_ORDER.md). nativeSliceFile throws "Some EditGcodeDialog defs were not specified
// properly" (GCode.cpp's ORCA_CHECK_GCODE_PLACEHOLDERS block) only through this JNI path, never
// through the standalone CLI test tool with an identical config. That check reads
// custom_gcode_specific_config_def, a global const object (PrintConfig.cpp) whose constructor
// only calls ConfigDef::add() on itself - no cross-translation-unit dependency - so if .has()
// returns false for a key its own constructor unconditionally adds, that's direct evidence the
// global's constructor never ran (or a duplicate, unconstructed copy is being read) when loaded
// into this .so, as opposed to statically linked into an executable.
extern "C" JNIEXPORT jstring JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeDiagnoseConfigDef(JNIEnv* env, jclass) {
    std::string result = "custom_gcode_specific_config_def.has(\"layer_num\")=";
    result += Slic3r::custom_gcode_specific_config_def.has("layer_num") ? "true" : "false";
    const auto& placeholders = Slic3r::custom_gcode_specific_placeholders();
    result += "; s_CustomGcodeSpecificPlaceholders.size()=" + std::to_string(placeholders.size());
    result += "; has(\"machine_end_gcode\")=";
    result += (placeholders.find("machine_end_gcode") != placeholders.end()) ? "true" : "false";
    return env->NewStringUTF(result.c_str());
}

// Slices inputModelPath (STL/3MF/OBJ) to outputGcodePath. profilePaths is an
// optional array of printer/filament/process profile JSON paths (OrcaSlicer's
// resources/profiles format via ConfigBase::load()), applied in array order
// on top of DynamicPrintConfig::full_print_config()'s factory defaults --
// same layering PresetBundle uses in the GUI, just without preset-selection
// UI. Pass an empty array to slice with stock defaults only.
//
// Throws java.lang.RuntimeException on any failure (missing file, invalid
// config, unsliceable geometry, ...) rather than returning an error code,
// since libslic3r itself reports failures as C++ exceptions
// (Slic3r::SlicingError et al.) that this bridge translates one-to-one.
// Local addition, not in the copied original (see this file's attribution header): the
// original nativeSliceFile never forwarded config_overrides to engine::slice_file, only
// profilePaths. Loading a bare override through a standalone "profile" JSON file (via
// ConfigBase::load()/apply(), the profilePaths path) does not behave the same as a direct
// config_overrides call - it tripped GCode.cpp's placeholder-resolution check
// (ORCA_CHECK_GCODE_PLACEHOLDERS, hardcoded on in GCode.hpp) in a way the CLI test tool's own
// direct config_overrides call (cli_test.cpp) never did. Exposing the already-proven-working
// path through JNI, rather than switching NativeEngineSmokeTest to the profile-file path,
// fixes that.
extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceFile(
    JNIEnv* env, jclass,
    jstring jInputModelPath, jstring jOutputGcodePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues,
    jdouble offsetXMm, jdouble offsetYMm, jdouble rotationZDeg, jdouble scale) {
    try {
        const std::string input_path = jstring_to_string(env, jInputModelPath);
        const std::string output_path = jstring_to_string(env, jOutputGcodePath);

        std::vector<std::string> profile_paths;
        if (jProfilePaths != nullptr) {
            jsize profile_count = env->GetArrayLength(jProfilePaths);
            for (jsize i = 0; i < profile_count; ++i) {
                auto jPath = static_cast<jstring>(env->GetObjectArrayElement(jProfilePaths, i));
                profile_paths.push_back(jstring_to_string(env, jPath));
                env->DeleteLocalRef(jPath);
            }
        }

        std::vector<std::pair<std::string, std::string>> config_overrides;
        if (jOverrideKeys != nullptr) {
            jsize override_count = env->GetArrayLength(jOverrideKeys);
            for (jsize i = 0; i < override_count; ++i) {
                auto jKey = static_cast<jstring>(env->GetObjectArrayElement(jOverrideKeys, i));
                auto jValue = static_cast<jstring>(env->GetObjectArrayElement(jOverrideValues, i));
                config_overrides.emplace_back(jstring_to_string(env, jKey), jstring_to_string(env, jValue));
                env->DeleteLocalRef(jKey);
                env->DeleteLocalRef(jValue);
            }
        }

        engine::ModelTransform transform;
        transform.offset_x_mm = offsetXMm;
        transform.offset_y_mm = offsetYMm;
        transform.rotation_z_deg = rotationZDeg;
        transform.scale = scale;

        engine::slice_file(input_path, output_path, profile_paths, config_overrides, transform);
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
    } catch (...) {
        throw_java_exception(env, "Unknown native error during slicing");
    }
}

// Phase 6 (Consumer Slicer Plan §16): the real Bambu-compatible .gcode.3mf bundle - same
// parameter shape as nativeSliceFile, minus the transform (the caller only ever slices one
// already-placed model for this path today - see slic3r_engine.hpp for why the bundle itself
// doesn't need per-object placement metadata for a single-object print).
extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceBambuBundle(
    JNIEnv* env, jclass,
    jstring jInputModelPath, jstring jOutputBundlePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues,
    jdouble offsetXMm, jdouble offsetYMm, jdouble rotationZDeg, jdouble scale) {
    try {
        const std::string input_path = jstring_to_string(env, jInputModelPath);
        const std::string output_path = jstring_to_string(env, jOutputBundlePath);

        std::vector<std::string> profile_paths;
        if (jProfilePaths != nullptr) {
            jsize profile_count = env->GetArrayLength(jProfilePaths);
            for (jsize i = 0; i < profile_count; ++i) {
                auto jPath = static_cast<jstring>(env->GetObjectArrayElement(jProfilePaths, i));
                profile_paths.push_back(jstring_to_string(env, jPath));
                env->DeleteLocalRef(jPath);
            }
        }

        std::vector<std::pair<std::string, std::string>> config_overrides;
        if (jOverrideKeys != nullptr) {
            jsize override_count = env->GetArrayLength(jOverrideKeys);
            for (jsize i = 0; i < override_count; ++i) {
                auto jKey = static_cast<jstring>(env->GetObjectArrayElement(jOverrideKeys, i));
                auto jValue = static_cast<jstring>(env->GetObjectArrayElement(jOverrideValues, i));
                config_overrides.emplace_back(jstring_to_string(env, jKey), jstring_to_string(env, jValue));
                env->DeleteLocalRef(jKey);
                env->DeleteLocalRef(jValue);
            }
        }

        engine::ModelTransform transform;
        transform.offset_x_mm = offsetXMm;
        transform.offset_y_mm = offsetYMm;
        transform.rotation_z_deg = rotationZDeg;
        transform.scale = scale;

        engine::slice_bambu_bundle(input_path, output_path, profile_paths, config_overrides, transform);
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
    } catch (...) {
        throw_java_exception(env, "Unknown native error during Bambu bundle export");
    }
}

// Phase 1 (WO-16 follow-up): slices a real multi-object build plate into one G-code file. Model
// paths and the five per-object arrays (four transform + tool-slot index) are parallel arrays
// (index i is one object) - the simplest JNI shape for a variable-length list of (path,
// transform, tool_index) triples, matching this bridge's existing convention of plain arrays
// over a custom marshalled object type. jToolSlotIndices (Phase 8 follow-up, §11, WO-25) may be
// null - every existing caller before this parameter existed - treated the same as an
// all-zeros array (every object keeps the printer's default extruder).
static std::vector<engine::ObjectExtras> extras_from(JNIEnv* env, jobjectArray jPaint, jobjectArray jVolumes, size_t count) {
    std::vector<std::string> paint = jPaint != nullptr ? to_string_vector(env, jPaint) : std::vector<std::string>{};
    std::vector<std::string> volumes = jVolumes != nullptr ? to_string_vector(env, jVolumes) : std::vector<std::string>{};
    std::vector<engine::ObjectExtras> extras(count);
    for (size_t i = 0; i < count; ++i) {
        if (i < paint.size()) extras[i].paint_strokes = paint[i];
        if (i < volumes.size()) extras[i].volume_specs = volumes[i];
    }
    return extras;
}

static void nativeSliceMultiObject_impl(
    JNIEnv* env, jobjectArray jPaint, jobjectArray jVolumes,
    jobjectArray jModelPaths, jdoubleArray jOffsetXMm, jdoubleArray jOffsetYMm,
    jdoubleArray jRotationZDeg, jdoubleArray jScale, jintArray jToolSlotIndices,
    jstring jOutputGcodePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues) {
    try {
        std::vector<std::string> model_paths = to_string_vector(env, jModelPaths);
        std::vector<double> offsets_x = to_double_vector(env, jOffsetXMm);
        std::vector<double> offsets_y = to_double_vector(env, jOffsetYMm);
        std::vector<double> rotations_z = to_double_vector(env, jRotationZDeg);
        std::vector<double> scales = to_double_vector(env, jScale);
        std::vector<int> tool_indices = to_int_vector(env, jToolSlotIndices);
        if (tool_indices.empty()) tool_indices.assign(model_paths.size(), 0);

        if (offsets_x.size() != model_paths.size() || offsets_y.size() != model_paths.size() ||
            rotations_z.size() != model_paths.size() || scales.size() != model_paths.size() ||
            tool_indices.size() != model_paths.size()) {
            throw_java_exception(env, "Model paths and transform arrays must be the same length.");
            return;
        }

        std::vector<std::tuple<std::string, engine::ModelTransform, int>> objects;
        objects.reserve(model_paths.size());
        for (size_t i = 0; i < model_paths.size(); ++i) {
            engine::ModelTransform transform;
            transform.offset_x_mm = offsets_x[i];
            transform.offset_y_mm = offsets_y[i];
            transform.rotation_z_deg = rotations_z[i];
            transform.scale = scales[i];
            objects.emplace_back(model_paths[i], transform, tool_indices[i]);
        }

        const std::string output_path = jstring_to_string(env, jOutputGcodePath);
        std::vector<std::string> profile_paths = to_string_vector(env, jProfilePaths);
        std::vector<std::string> keys = to_string_vector(env, jOverrideKeys);
        std::vector<std::string> values = to_string_vector(env, jOverrideValues);
        std::vector<std::pair<std::string, std::string>> config_overrides;
        for (size_t i = 0; i < keys.size() && i < values.size(); ++i) {
            config_overrides.emplace_back(keys[i], values[i]);
        }

        engine::slice_multi_object(objects, output_path, profile_paths, config_overrides, extras_from(env, jPaint, jVolumes, objects.size()));
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
    } catch (...) {
        throw_java_exception(env, "Unknown native error during multi-object slicing");
    }
}

// Phase 6 follow-up (WO-23): the real multi-object counterpart to nativeSliceBambuBundle above -
// same parallel-array marshalling nativeSliceMultiObject already uses, producing a
// .gcode.3mf bundle (engine::slice_multi_object_bambu_bundle) instead of plain .gcode.
static void nativeSliceMultiObjectBambuBundle_impl(
    JNIEnv* env, jobjectArray jPaint, jobjectArray jVolumes,
    jobjectArray jModelPaths, jdoubleArray jOffsetXMm, jdoubleArray jOffsetYMm,
    jdoubleArray jRotationZDeg, jdoubleArray jScale, jintArray jToolSlotIndices,
    jstring jOutputBundlePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues) {
    try {
        std::vector<std::string> model_paths = to_string_vector(env, jModelPaths);
        // 1-based filament (AMS slot) per object; absent (older callers) = all on the default filament.
        std::vector<int> tool_indices = jToolSlotIndices != nullptr ? to_int_vector(env, jToolSlotIndices) : std::vector<int>(model_paths.size(), 0);
        std::vector<double> offsets_x = to_double_vector(env, jOffsetXMm);
        std::vector<double> offsets_y = to_double_vector(env, jOffsetYMm);
        std::vector<double> rotations_z = to_double_vector(env, jRotationZDeg);
        std::vector<double> scales = to_double_vector(env, jScale);

        if (offsets_x.size() != model_paths.size() || offsets_y.size() != model_paths.size() ||
            rotations_z.size() != model_paths.size() || scales.size() != model_paths.size()) {
            throw_java_exception(env, "Model paths and transform arrays must be the same length.");
            return;
        }

        if (tool_indices.size() != model_paths.size()) {
            throw_java_exception(env, "Model paths and tool slot arrays must be the same length.");
            return;
        }

        std::vector<std::tuple<std::string, engine::ModelTransform, int>> objects;
        objects.reserve(model_paths.size());
        for (size_t i = 0; i < model_paths.size(); ++i) {
            engine::ModelTransform transform;
            transform.offset_x_mm = offsets_x[i];
            transform.offset_y_mm = offsets_y[i];
            transform.rotation_z_deg = rotations_z[i];
            transform.scale = scales[i];
            objects.emplace_back(model_paths[i], transform, tool_indices[i]);
        }

        const std::string output_path = jstring_to_string(env, jOutputBundlePath);
        std::vector<std::string> profile_paths = to_string_vector(env, jProfilePaths);
        std::vector<std::string> keys = to_string_vector(env, jOverrideKeys);
        std::vector<std::string> values = to_string_vector(env, jOverrideValues);
        std::vector<std::pair<std::string, std::string>> config_overrides;
        for (size_t i = 0; i < keys.size() && i < values.size(); ++i) {
            config_overrides.emplace_back(keys[i], values[i]);
        }

        engine::slice_multi_object_bambu_bundle(objects, output_path, profile_paths, config_overrides, extras_from(env, jPaint, jVolumes, objects.size()));
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
    } catch (...) {
        throw_java_exception(env, "Unknown native error during multi-object Bambu bundle export");
    }
}

// Loads inputModelPath (STL/3MF/OBJ) the same real way nativeSliceFile does - not a second,
// weaker parser - and returns a flat interleaved vertex buffer for an in-app 3D preview: 6
// floats per vertex (x,y,z,nx,ny,nz), 3 vertices per triangle. See engine::load_mesh_preview.
extern "C" JNIEXPORT jfloatArray JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeLoadMeshPreview(
    JNIEnv* env, jclass, jstring jInputModelPath) {
    try {
        const std::string input_path = jstring_to_string(env, jInputModelPath);
        std::vector<float> buffer = engine::load_mesh_preview(input_path);
        // One bulk SetFloatArrayRegion call, not per-element JNI calls - a real perf cliff at
        // the vertex counts a detailed model can reach.
        jfloatArray result = env->NewFloatArray(static_cast<jsize>(buffer.size()));
        if (result == nullptr) {
            throw_java_exception(env, "Could not allocate the mesh preview buffer.");
            return nullptr;
        }
        env->SetFloatArrayRegion(result, 0, static_cast<jsize>(buffer.size()), buffer.data());
        return result;
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
        return nullptr;
    } catch (...) {
        throw_java_exception(env, "Unknown native error while loading the mesh preview");
        return nullptr;
    }
}

// Phase 0 (WO-16): see engine::count_model_objects for why this exists ahead of any UI using it.
extern "C" JNIEXPORT jint JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeCountModelObjects(
    JNIEnv* env, jclass, jstring jInputModelPath) {
    try {
        return static_cast<jint>(engine::count_model_objects(jstring_to_string(env, jInputModelPath)));
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
        return 0;
    } catch (...) {
        throw_java_exception(env, "Unknown native error while counting model objects");
        return 0;
    }
}

// Support painting (WO-14 part D). See engine::open_paint_session/paint_stroke/
// get_painted_facets/slice_paint_session/close_paint_session in slic3r_engine.cpp/hpp for the
// real, upstream-GUI-traced transform reasoning - this file is only the JNI marshalling.
extern "C" JNIEXPORT jlong JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeOpenPaintSession(
    JNIEnv* env, jclass, jstring jInputModelPath,
    jdouble offsetXMm, jdouble offsetYMm, jdouble rotationZDeg, jdouble scale) {
    try {
        engine::ModelTransform transform;
        transform.offset_x_mm = offsetXMm;
        transform.offset_y_mm = offsetYMm;
        transform.rotation_z_deg = rotationZDeg;
        transform.scale = scale;
        return static_cast<jlong>(engine::open_paint_session(jstring_to_string(env, jInputModelPath), transform));
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
        return 0;
    } catch (...) {
        throw_java_exception(env, "Unknown native error while opening the paint session");
        return 0;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativePaintStroke(
    JNIEnv* env, jclass, jlong handle,
    jdouble originX, jdouble originY, jdouble originZ,
    jdouble dirX, jdouble dirY, jdouble dirZ,
    jdouble radiusMm, jboolean enforcer) {
    try {
        engine::paint_stroke(static_cast<engine::PaintSessionHandle>(handle), originX, originY, originZ, dirX, dirY, dirZ, radiusMm, enforcer == JNI_TRUE);
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
    } catch (...) {
        throw_java_exception(env, "Unknown native error during a paint stroke");
    }
}

extern "C" JNIEXPORT jfloatArray JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeGetPaintedFacets(JNIEnv* env, jclass, jlong handle) {
    try {
        return to_jfloat_array(env, engine::get_painted_facets(static_cast<engine::PaintSessionHandle>(handle)));
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
        return nullptr;
    } catch (...) {
        throw_java_exception(env, "Unknown native error while reading painted facets");
        return nullptr;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSlicePaintSession(
    JNIEnv* env, jclass, jlong handle, jstring jOutputGcodePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues) {
    try {
        std::vector<std::pair<std::string, std::string>> config_overrides;
        std::vector<std::string> keys = to_string_vector(env, jOverrideKeys);
        std::vector<std::string> values = to_string_vector(env, jOverrideValues);
        for (size_t i = 0; i < keys.size() && i < values.size(); ++i) {
            config_overrides.emplace_back(keys[i], values[i]);
        }
        engine::slice_paint_session(static_cast<engine::PaintSessionHandle>(handle), jstring_to_string(env, jOutputGcodePath),
            to_string_vector(env, jProfilePaths), config_overrides);
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
    } catch (...) {
        throw_java_exception(env, "Unknown native error while slicing the paint session");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeClosePaintSession(JNIEnv*, jclass, jlong handle) {
    engine::close_paint_session(static_cast<engine::PaintSessionHandle>(handle));
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeCancelSlice(JNIEnv*, jclass) { engine::request_cancel(); }

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeResetCancel(JNIEnv*, jclass) { engine::reset_cancel(); }

extern "C" JNIEXPORT jint JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceProgress(JNIEnv*, jclass) { return engine::slice_progress(); }

// Returns [upper, lower] triangle soups (9 floats per triangle); either may be empty.
extern "C" JNIEXPORT jobjectArray JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeCutMesh(JNIEnv* env, jclass, jfloatArray jSoup, jfloat z) {
    try {
        if (jSoup == nullptr) throw std::runtime_error("No mesh to cut.");
        const jsize n = env->GetArrayLength(jSoup);
        std::vector<float> soup(static_cast<size_t>(n));
        env->GetFloatArrayRegion(jSoup, 0, n, soup.data());
        engine::CutResult result = engine::cut_mesh_soup(soup, z);
        jclass floatArrayClass = env->FindClass("[F");
        jobjectArray out = env->NewObjectArray(2, floatArrayClass, nullptr);
        if (out == nullptr) throw std::runtime_error("Could not allocate the cut result.");
        jfloatArray upper = to_jfloat_array(env, result.upper);
        jfloatArray lower = to_jfloat_array(env, result.lower);
        env->SetObjectArrayElement(out, 0, upper);
        env->SetObjectArrayElement(out, 1, lower);
        return out;
    } catch (const std::exception& ex) {
        throw_java_exception(env, ex);
    } catch (...) {
        throw_java_exception(env, "Unknown native error while cutting the mesh");
    }
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceMultiObject(
    JNIEnv* env, jclass, jobjectArray jModelPaths, jdoubleArray jOffsetXMm, jdoubleArray jOffsetYMm,
    jdoubleArray jRotationZDeg, jdoubleArray jScale, jintArray jToolSlotIndices,
    jstring jOutputGcodePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues) {
    nativeSliceMultiObject_impl(env, nullptr, nullptr, jModelPaths, jOffsetXMm, jOffsetYMm, jRotationZDeg, jScale, jToolSlotIndices,
                                jOutputGcodePath, jProfilePaths, jOverrideKeys, jOverrideValues);
}

// Phase 9d: same as nativeSliceMultiObject plus per-object paint strokes and modifier/blocker volumes.
extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceMultiObjectEx(
    JNIEnv* env, jclass, jobjectArray jModelPaths, jdoubleArray jOffsetXMm, jdoubleArray jOffsetYMm,
    jdoubleArray jRotationZDeg, jdoubleArray jScale, jintArray jToolSlotIndices,
    jstring jOutputGcodePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues,
    jobjectArray jPaintStrokes, jobjectArray jVolumeSpecs) {
    nativeSliceMultiObject_impl(env, jPaintStrokes, jVolumeSpecs, jModelPaths, jOffsetXMm, jOffsetYMm, jRotationZDeg, jScale, jToolSlotIndices,
                                jOutputGcodePath, jProfilePaths, jOverrideKeys, jOverrideValues);
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceMultiObjectBambuBundle(
    JNIEnv* env, jclass, jobjectArray jModelPaths, jdoubleArray jOffsetXMm, jdoubleArray jOffsetYMm,
    jdoubleArray jRotationZDeg, jdoubleArray jScale,
    jstring jOutputBundlePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues) {
    nativeSliceMultiObjectBambuBundle_impl(env, nullptr, nullptr, jModelPaths, jOffsetXMm, jOffsetYMm, jRotationZDeg, jScale, nullptr,
                                           jOutputBundlePath, jProfilePaths, jOverrideKeys, jOverrideValues);
}

extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceMultiObjectBambuBundleEx(
    JNIEnv* env, jclass, jobjectArray jModelPaths, jdoubleArray jOffsetXMm, jdoubleArray jOffsetYMm,
    jdoubleArray jRotationZDeg, jdoubleArray jScale,
    jstring jOutputBundlePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues,
    jobjectArray jPaintStrokes, jobjectArray jVolumeSpecs) {
    nativeSliceMultiObjectBambuBundle_impl(env, jPaintStrokes, jVolumeSpecs, jModelPaths, jOffsetXMm, jOffsetYMm, jRotationZDeg, jScale, nullptr,
                                           jOutputBundlePath, jProfilePaths, jOverrideKeys, jOverrideValues);
}

// Multi-colour Bambu bundles: nativeSliceMultiObjectBambuBundleEx plus a 1-based filament (AMS slot) per object, as
// nativeSliceMultiObjectEx takes. A new name, so an app built against the older signature keeps working.
extern "C" JNIEXPORT void JNICALL
Java_org_orcaslicer_engine_NativeEngine_nativeSliceMultiObjectBambuBundleTools(
    JNIEnv* env, jclass, jobjectArray jModelPaths, jdoubleArray jOffsetXMm, jdoubleArray jOffsetYMm,
    jdoubleArray jRotationZDeg, jdoubleArray jScale, jintArray jToolSlotIndices,
    jstring jOutputBundlePath, jobjectArray jProfilePaths, jobjectArray jOverrideKeys, jobjectArray jOverrideValues,
    jobjectArray jPaintStrokes, jobjectArray jVolumeSpecs) {
    nativeSliceMultiObjectBambuBundle_impl(env, jPaintStrokes, jVolumeSpecs, jModelPaths, jOffsetXMm, jOffsetYMm, jRotationZDeg, jScale, jToolSlotIndices,
                                           jOutputBundlePath, jProfilePaths, jOverrideKeys, jOverrideValues);
}
