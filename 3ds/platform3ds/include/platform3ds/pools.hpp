#pragma once

#include <cstddef>
#include <cstdint>

// Memory pools beside newlib's malloc (M9, hardware tests #63 and #67).
//
// One malloc arena for everything fragmented: with 15 MB free in it, a 256 KB request failed, because small
// long-lived objects sat in every hole large blocks had left. malloc/free/realloc/calloc are wrapped at link
// time (platform3ds/CMakeLists.txt) and sorted by size:
// - the large pool takes requests of kLargeMin and up: file buffers, images, vertex arrays, big vectors. Its
//   memory is linear memory the GPU doesn't use (the first region at the first large request, more regions
//   on demand down to a floor kept for the GPU), so the heap also grows into what used to sit idle;
// - the Lua pool takes the Lua state's allocations (its churn every frame), one region taken from the heap at
//   the first Lua allocation;
// - newlib's malloc keeps everything else, in the app heap only.
// Each falls back on the others when full. Both pools are TLSF allocators (two-level segregated fit: a good
// fit in constant time, neighbours merged on free), under newlib's malloc lock.
namespace p3ds::pools
{
    constexpr size_t kLargeMin = 32 * 1024;

    struct PoolStats
    {
        uint32_t total = 0; // bytes in the pool's regions
        uint32_t used = 0; // bytes handed out (with block headers)
        uint32_t largestFree = 0;
        uint32_t regions = 0;
        uint32_t failures = 0; // requests the pool could not serve
    };

    struct NewlibStats
    {
        uint32_t arena = 0; // obtained by sbrk
        uint32_t used = 0; // mallinfo's uordblks
        uint32_t top = 0; // the top chunk (can still grow by sbrk)
        uint32_t largestFree = 0; // in the bins, without the top chunk
        uint32_t freeBySize[4] = {}; // free bytes in chunks < 1 KB, < 16 KB, < 256 KB, >= 256 KB
    };

    PoolStats large();
    PoolStats lua();
    // Walks newlib's bins under the malloc lock (as long as mallinfo): loading screens and crash reports only.
    NewlibStats newlib();

    // Lua state allocator (lua_Alloc semantics without the user data): the Lua pool, falling back on malloc.
    void* luaRealloc(void* ptr, size_t nsize);

    // Linear memory the large pool leaves to the GPU when it grows (it never shrinks below what it took).
    constexpr size_t kGpuFloor = 8u * 1024u * 1024u;
}
