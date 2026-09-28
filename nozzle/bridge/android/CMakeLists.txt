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

add_library(slic3r_engine STATIC
    slic3r_engine.cpp
    thumbnail_render.cpp
    nanosvg_impl.cpp
    option_states.cpp
)
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

# Standalone on-device CLI smoke-test tool (docs/WORK_ORDER.md's WO-13
# entry) - not packaged into the APK.
add_executable(slic3r_cli_test cli_test.cpp)
target_link_libraries(slic3r_cli_test PRIVATE slic3r_engine)
target_link_options(slic3r_cli_test PRIVATE -Wl,--gc-sections)
