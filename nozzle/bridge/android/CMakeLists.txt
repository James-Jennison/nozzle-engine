# Copied from orcaslicer-android-engine's jni/CMakeLists.txt (see the
# attribution headers on the .cpp/.hpp files in this directory, and
# THIRD_PARTY_NOTICES.md). Added as a subdirectory of the real OrcaSlicer
# CMake project (see ../CMakeLists.txt's ANDROID_JNI_BRIDGE_DIR), after
# add_subdirectory(src), so the `libslic3r` target already exists here with
# its full, correct transitive dependency graph. Linking against that CMake
# target (not a hand-ordered list of .a paths) is what makes this tractable
# - libslic3r alone pulls in ~80 static libraries.
cmake_minimum_required(VERSION 3.13)
project(slic3r_android_jni)

find_library(ANDROID_LOG_LIB log)

# Mixed filaments (OrcaSlicer 2.5.0's): the same source file the desktop CLI builds (nozzle/bridge/native/CMakeLists.txt),
# shared so Android's mixing behaviour cannot drift from desktop's; slic3r_jni.cpp calls nozzle_mf::run_mixed_filament directly.
set(NOZZLE_NATIVE_BRIDGE "${CMAKE_CURRENT_LIST_DIR}/../native")

add_library(slic3r_engine STATIC
    slic3r_engine.cpp
    thumbnail_render.cpp
    nanosvg_impl.cpp
    option_states.cpp
    ${NOZZLE_NATIVE_BRIDGE}/mixed_filament.cpp
    ${NOZZLE_NATIVE_BRIDGE}/semantic_paint_op.cpp
    ${NOZZLE_NATIVE_BRIDGE}/paint_ops.cpp
)
target_include_directories(slic3r_engine PUBLIC ${NOZZLE_NATIVE_BRIDGE})
target_link_libraries(slic3r_engine PUBLIC
    libslic3r
    cereal::cereal
    nanosvg
)

add_library(slic3rengine SHARED
    slic3r_jni.cpp
)
target_link_libraries(slic3rengine PRIVATE
    slic3r_engine
    ${ANDROID_LOG_LIB}
)

# Strip unused sections; libslic3r.a alone is >1GB unstripped (debug info +
# heavily templated CGAL/OCCT/Eigen code). -Wl,--gc-sections works with
# -ffunction-sections/-fdata-sections, both already on by default for the
# NDK toolchain.
target_link_options(slic3rengine PRIVATE -Wl,--gc-sections)

if (CMAKE_BUILD_TYPE STREQUAL "Release" OR NOT CMAKE_BUILD_TYPE)
    target_link_options(slic3rengine PRIVATE -Wl,--strip-all)
endif ()
