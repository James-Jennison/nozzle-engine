# Nozzle It All Desktop engine: libslic3r + the shared slicing pipeline Android and the Web App use
# (app/src/main/cpp/bridge/slic3r_engine.cpp), as a native Linux executable (native_cli.cpp) that Nozzle It All Desktop
# runs once per slice. Hooked into the patched OrcaSlicer CMake project through its ANDROID_JNI_BRIDGE_DIR hook
# (engine/android-headless-engine.patch), exactly like the Android and browser bridges, so libslic3r's full link graph
# is reused.
cmake_minimum_required(VERSION 3.13)
project(nozzle_native_engine)

set(NOZZLE_SHARED_BRIDGE "${CMAKE_CURRENT_LIST_DIR}/../android")

add_library(slic3r_engine STATIC
    ${NOZZLE_SHARED_BRIDGE}/slic3r_engine.cpp
    ${NOZZLE_SHARED_BRIDGE}/thumbnail_render.cpp
)
target_include_directories(slic3r_engine PUBLIC ${NOZZLE_SHARED_BRIDGE})
target_link_libraries(slic3r_engine PUBLIC libslic3r cereal::cereal nanosvg)

# nanosvg's implementation is compiled into the executable itself, not the static bridge library: libslic3r also calls
# nsvg*, and GNU ld only pulls archive members for symbols already undefined when it reaches that archive (Android's
# and Emscripten's lld linkers do not care about archive order).
add_executable(nozzle-engine native_cli.cpp full_spectrum.cpp color_mix.cpp plate_ops.cpp ${NOZZLE_SHARED_BRIDGE}/nanosvg_impl.cpp)
# The prefix's libjpeg is linked whole, first: libslic3r's JPG thumbnails are built against its API (80), and OpenCV's
# bundled libjpeg-turbo (API 62) would otherwise supply jpeg_* to them ("Wrong JPEG library version: library is 62,
# caller expects 80"). CMake keeps raw linker flags in place, where a library path would be de-duplicated and moved.
find_package(JPEG QUIET)
if (JPEG_FOUND AND JPEG_LIBRARIES AND NOT ANDROID AND NOT EMSCRIPTEN)
    list(GET JPEG_LIBRARIES 0 NOZZLE_JPEG_ARCHIVE)
    if (NOZZLE_JPEG_ARCHIVE MATCHES "\\.a$")
        target_link_libraries(nozzle-engine PRIVATE "-Wl,--whole-archive" "${NOZZLE_JPEG_ARCHIVE}" "-Wl,--no-whole-archive")
    endif ()
endif ()
target_link_libraries(nozzle-engine PRIVATE slic3r_engine)
# Keep the C++ runtime inside the binary so it runs on desktops older than the build host.
target_link_options(nozzle-engine PRIVATE -static-libstdc++ -static-libgcc)
