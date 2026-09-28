# Nozzle It All browser engine: libslic3r + the shared slicing pipeline Android uses
# (nozzle/bridge/android/slic3r_engine.cpp), exposed to a Web Worker through a small C API (wasm_engine.cpp).
# Hooked into the patched OrcaSlicer CMake project through its ANDROID_JNI_BRIDGE_DIR hook
# (engine/android-headless-engine.patch), exactly like the Android bridge, so libslic3r's full link graph is reused.
cmake_minimum_required(VERSION 3.13)
project(nozzle_wasm_engine)

set(NOZZLE_SHARED_BRIDGE "${CMAKE_CURRENT_LIST_DIR}/../android")

add_library(slic3r_engine STATIC
    ${NOZZLE_SHARED_BRIDGE}/slic3r_engine.cpp
    ${NOZZLE_SHARED_BRIDGE}/thumbnail_render.cpp
    ${NOZZLE_SHARED_BRIDGE}/nanosvg_impl.cpp
    ${NOZZLE_SHARED_BRIDGE}/option_states.cpp
)
target_include_directories(slic3r_engine PUBLIC ${NOZZLE_SHARED_BRIDGE})
target_link_libraries(slic3r_engine PUBLIC libslic3r cereal::cereal nanosvg)

add_executable(nozzle-engine wasm_engine.cpp wasm_standins.cpp)
target_link_libraries(nozzle-engine PRIVATE slic3r_engine)
set_target_properties(nozzle-engine PROPERTIES SUFFIX ".js")
target_link_options(nozzle-engine PRIVATE
    -pthread -fwasm-exceptions
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createNozzleEngine -sENVIRONMENT=worker,node
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=4GB
    -sPTHREAD_POOL_SIZE=4 -sPTHREAD_POOL_SIZE_STRICT=0
    -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=4MB
    -sFORCE_FILESYSTEM=1 -sEXIT_RUNTIME=0
    -sEXPORTED_FUNCTIONS=_nz_version,_nz_slice_start,_nz_slice_status,_nz_slice_result,_nz_cancel,_nz_free,_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=FS,ccall,cwrap,UTF8ToString,stringToNewUTF8
    -O2)
