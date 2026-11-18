#include "memory.h"
#include <cstdlib>
namespace stockcut {
void* std_aligned_alloc(usize alignment, usize size) {
    // Đảm bảo kích thước chia hết cho alignment theo chuẩn C11/POSIX
    size = (size + alignment - 1) & ~(alignment - 1);
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    return _aligned_malloc(size, alignment);
#elif defined(__EMSCRIPTEN__) || defined(__linux__) || defined(__APPLE__)
    void* mem = nullptr;
    if (posix_memalign(&mem, alignment, size) == 0) return mem;
    return nullptr;
#else
    return std::aligned_alloc(alignment, size);
#endif
}
void std_aligned_free(void* ptr) {
    if (!ptr) return;
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    _aligned_free(ptr);
#else
    std::free(ptr);
#endif
}
void* aligned_large_pages_alloc(usize size) {
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const usize pageSize = si.dwPageSize ? static_cast<usize>(si.dwPageSize) : 4096;
    size = (size + pageSize - 1) & ~(pageSize - 1);
    void* mem = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (mem) return mem;
#endif
    return std_aligned_alloc(4096, size);
}
void aligned_large_pages_free(void* mem) {
    if (!mem) return;
#if defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (VirtualFree(mem, 0, MEM_RELEASE)) return;
#endif
    std_aligned_free(mem);
}
} // namespace stockcut
