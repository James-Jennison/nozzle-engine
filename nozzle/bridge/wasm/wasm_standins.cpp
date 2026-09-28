// Library entry points the browser build lacks.
//
// oneTBB's scalable allocator (tbbmalloc) has no WebAssembly port. libslic3r uses tbb::scalable_allocator, which calls
// these C entry points; in the browser engine they go to Emscripten's own thread-safe allocator instead.
#include <cerrno>
#include <cstdlib>
#include <malloc.h>
#include <pthread.h>

extern "C" {
void* scalable_malloc(size_t size) { return std::malloc(size); }
void scalable_free(void* p) { std::free(p); }
void* scalable_calloc(size_t n, size_t size) { return std::calloc(n, size); }
void* scalable_realloc(void* p, size_t size) { return std::realloc(p, size); }
int scalable_posix_memalign(void** out, size_t alignment, size_t size) { return posix_memalign(out, alignment, size); }
void* scalable_aligned_malloc(size_t size, size_t alignment) { void* p = nullptr; return posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size) == 0 ? p : nullptr; }
void scalable_aligned_free(void* p) { std::free(p); }
size_t scalable_msize(void* p) { return p ? malloc_usable_size(p) : 0; }
int scalable_allocation_mode(int, long) { return 0; }
int scalable_allocation_command(int, void*) { return 0; }

// Emscripten's pthreads don't support naming threads; libslic3r names its worker threads only for debugging.
int pthread_setname_np(pthread_t, const char*) { return 0; }
}
