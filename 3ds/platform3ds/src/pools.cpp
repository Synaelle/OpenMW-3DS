#include <platform3ds/pools.hpp>

#include <3ds.h>

#include <malloc.h>
#include <sys/lock.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// See pools.hpp. Linked with -Wl,--wrap=malloc,free,realloc,calloc,malloc_usable_size.

extern "C"
{
    void* __real_malloc(size_t size);
    void __real_free(void* ptr);
    void* __real_realloc(void* ptr, size_t size);
    size_t __real_malloc_usable_size(void* ptr);
    void __malloc_lock(struct _reent*);
    void __malloc_unlock(struct _reent*);
    extern size_t p3ds_linear_gpu_reserve; // platform.cpp: linear memory left to the GPU at the first region
    extern void* __malloc_av_[]; // newlib's bins (dlmalloc 2.6.4: 128 bins of fd/bk pairs, top in [2])
    size_t p3ds_heap_arena();
}

namespace p3ds::pools
{
    namespace
    {
        // ---- TLSF (two-level segregated fit) -----------------------------------------------------------
        // A block is an 8-byte header {previous physical block, payload size | flags} and its payload. A free
        // block keeps its free-list links in the payload. Payloads are 8-aligned, sizes multiples of 8. Each
        // region ends with a zero-size used sentinel, so merging never leaves the region.
        constexpr size_t kAlign = 8;
        constexpr unsigned kSlLog2 = 5;
        constexpr unsigned kSlCount = 1u << kSlLog2;
        constexpr unsigned kFlShift = kSlLog2 + 3; // sizes below 256 bytes: first level 0, 8-byte steps
        constexpr unsigned kFlCount = 32 - kFlShift + 1;
        constexpr size_t kSmall = size_t(1) << kFlShift;
        constexpr size_t kHeader = 8;
        constexpr size_t kMinPayload = 8;
        constexpr size_t kFreeBit = 1;
        constexpr size_t kPrevFreeBit = 2;
        constexpr int kMaxRegions = 48;

        struct Block
        {
            Block* prevPhys;
            size_t sizeFlags;
            // payload: when free, the free-list links
            Block* nextFree;
            Block* prevFree;

            size_t size() const { return sizeFlags & ~(kFreeBit | kPrevFreeBit); }
            bool isFree() const { return sizeFlags & kFreeBit; }
            bool prevIsFree() const { return sizeFlags & kPrevFreeBit; }
            void* payload() { return reinterpret_cast<char*>(this) + kHeader; }
            Block* next() { return reinterpret_cast<Block*>(reinterpret_cast<char*>(this) + kHeader + size()); }
        };

        inline Block* blockOf(void* p)
        {
            return reinterpret_cast<Block*>(static_cast<char*>(p) - kHeader);
        }

        inline unsigned fls(size_t v) // index of the highest set bit, v != 0
        {
            return 31u - unsigned(__builtin_clz(uint32_t(v)));
        }

        inline void mapping(size_t size, unsigned& fl, unsigned& sl)
        {
            if (size < kSmall)
            {
                fl = 0;
                sl = unsigned(size / (kSmall / kSlCount));
            }
            else
            {
                const unsigned f = fls(size);
                sl = unsigned(size >> (f - kSlLog2)) ^ kSlCount;
                fl = f - (kFlShift - 1);
            }
        }

        struct Region
        {
            char* begin;
            char* end;
        };

        class Tlsf
        {
        public:
            // Without the lock: regions are only ever added, so a pointer outside [low, high) is not ours.
            bool mayContain(const void* p) const
            {
                const char* c = static_cast<const char*>(p);
                return c >= __atomic_load_n(&mLow, __ATOMIC_RELAXED) && c < __atomic_load_n(&mHigh, __ATOMIC_RELAXED);
            }

            bool contains(const void* p) const
            {
                const char* c = static_cast<const char*>(p);
                if (c < mLow || c >= mHigh)
                    return false;
                for (int i = 0; i < mRegionCount; ++i)
                    if (c >= mRegions[i].begin && c < mRegions[i].end)
                        return true;
                return false;
            }

            bool addRegion(void* mem, size_t bytes)
            {
                if (mRegionCount == kMaxRegions)
                    return false;
                uintptr_t b = (uintptr_t(mem) + kAlign - 1) & ~uintptr_t(kAlign - 1);
                uintptr_t e = (uintptr_t(mem) + bytes) & ~uintptr_t(kAlign - 1);
                if (e <= b || e - b < 4 * kHeader + kMinPayload)
                    return false;
                Block* first = reinterpret_cast<Block*>(b);
                first->prevPhys = nullptr;
                first->sizeFlags = (e - b - 2 * kHeader) | kFreeBit;
                Block* sentinel = first->next();
                sentinel->prevPhys = first;
                sentinel->sizeFlags = 0 | kPrevFreeBit;
                insertFree(first);
                mRegions[mRegionCount++] = { reinterpret_cast<char*>(b), reinterpret_cast<char*>(e) };
                if (mLow == nullptr || reinterpret_cast<char*>(b) < mLow)
                    mLow = reinterpret_cast<char*>(b);
                if (reinterpret_cast<char*>(e) > mHigh)
                    mHigh = reinterpret_cast<char*>(e);
                mTotal += e - b;
                mUsed += 2 * kHeader; // the first header and the sentinel
                return true;
            }

            void* alloc(size_t size)
            {
                size = roundSize(size);
                if (size == 0)
                    return nullptr;
                Block* b = findFree(size);
                if (b == nullptr)
                {
                    ++mFailures;
                    return nullptr;
                }
                removeFree(b);
                splitAndUse(b, size);
                return b->payload();
            }

            void free(void* p)
            {
                Block* b = blockOf(p);
                mUsed -= b->size() + kHeader;
                b->sizeFlags |= kFreeBit;
                if (b->prevIsFree())
                {
                    Block* prev = b->prevPhys;
                    removeFree(prev);
                    prev->sizeFlags = (prev->size() + kHeader + b->size()) | kFreeBit
                        | (prev->sizeFlags & kPrevFreeBit);
                    b = prev;
                }
                Block* next = b->next();
                if (next->isFree())
                {
                    removeFree(next);
                    b->sizeFlags = (b->size() + kHeader + next->size()) | kFreeBit | (b->sizeFlags & kPrevFreeBit);
                    next = b->next();
                }
                next->prevPhys = b;
                next->sizeFlags |= kPrevFreeBit;
                insertFree(b);
            }

            // Grows or shrinks in place when it can. nullptr when it would have to move (the caller moves it).
            void* resizeInPlace(void* p, size_t size)
            {
                size = roundSize(size);
                if (size == 0)
                    return nullptr;
                Block* b = blockOf(p);
                if (b->size() >= size)
                    return p;
                Block* next = b->next();
                if (!next->isFree() || b->size() + kHeader + next->size() < size)
                    return nullptr;
                removeFree(next);
                mUsed += next->size() + kHeader;
                b->sizeFlags = (b->size() + kHeader + next->size()) | (b->sizeFlags & kPrevFreeBit);
                Block* after = b->next();
                after->prevPhys = b;
                after->sizeFlags &= ~kPrevFreeBit;
                // Give the excess back.
                mUsed -= b->size() + kHeader;
                splitAndUse(b, size);
                return p;
            }

            static size_t usableSize(void* p) { return blockOf(p)->size(); }

            size_t largestFree() const
            {
                if (mFlBitmap == 0)
                    return 0;
                const unsigned fl = fls(mFlBitmap);
                const unsigned sl = fls(mSlBitmap[fl]);
                size_t best = 0;
                for (Block* b = mBlocks[fl][sl]; b != nullptr; b = b->nextFree)
                    if (b->size() > best)
                        best = b->size();
                return best;
            }

            PoolStats stats() const
            {
                PoolStats s;
                s.total = uint32_t(mTotal);
                s.used = uint32_t(mUsed);
                s.largestFree = uint32_t(largestFree());
                s.regions = uint32_t(mRegionCount);
                s.failures = mFailures;
                return s;
            }

            size_t total() const { return mTotal; }

        private:
            static size_t roundSize(size_t size)
            {
                if (size > (size_t(1) << 30))
                    return 0;
                size = (size + kAlign - 1) & ~(kAlign - 1);
                return size < kMinPayload ? kMinPayload : size;
            }

            void splitAndUse(Block* b, size_t size)
            {
                const size_t have = b->size();
                if (have >= size + kHeader + kMinPayload)
                {
                    Block* rest = reinterpret_cast<Block*>(static_cast<char*>(b->payload()) + size);
                    rest->prevPhys = b;
                    rest->sizeFlags = (have - size - kHeader) | kFreeBit;
                    b->sizeFlags = size | (b->sizeFlags & kPrevFreeBit);
                    Block* next = rest->next();
                    next->prevPhys = rest;
                    next->sizeFlags |= kPrevFreeBit;
                    insertFree(rest);
                }
                else
                {
                    b->sizeFlags &= ~kFreeBit;
                    b->next()->sizeFlags &= ~kPrevFreeBit;
                }
                mUsed += b->size() + kHeader;
            }

            Block* findFree(size_t size)
            {
                // Round up to the next list so that any block found fits.
                if (size >= kSmall)
                    size += (size_t(1) << (fls(size) - kSlLog2)) - 1;
                unsigned fl, sl;
                mapping(size, fl, sl);
                if (fl >= kFlCount)
                    return nullptr;
                uint32_t slMap = mSlBitmap[fl] & (~0u << sl);
                if (slMap == 0)
                {
                    const uint32_t flMap = fl + 1 < 32 ? (mFlBitmap & (~0u << (fl + 1))) : 0;
                    if (flMap == 0)
                        return nullptr;
                    fl = unsigned(__builtin_ctz(flMap));
                    slMap = mSlBitmap[fl];
                }
                sl = unsigned(__builtin_ctz(slMap));
                return mBlocks[fl][sl];
            }

            void insertFree(Block* b)
            {
                unsigned fl, sl;
                mapping(b->size(), fl, sl);
                Block* head = mBlocks[fl][sl];
                b->nextFree = head;
                b->prevFree = nullptr;
                if (head != nullptr)
                    head->prevFree = b;
                mBlocks[fl][sl] = b;
                mFlBitmap |= 1u << fl;
                mSlBitmap[fl] |= 1u << sl;
            }

            void removeFree(Block* b)
            {
                unsigned fl, sl;
                mapping(b->size(), fl, sl);
                if (b->prevFree != nullptr)
                    b->prevFree->nextFree = b->nextFree;
                else
                    mBlocks[fl][sl] = b->nextFree;
                if (b->nextFree != nullptr)
                    b->nextFree->prevFree = b->prevFree;
                if (mBlocks[fl][sl] == nullptr)
                {
                    mSlBitmap[fl] &= ~(1u << sl);
                    if (mSlBitmap[fl] == 0)
                        mFlBitmap &= ~(1u << fl);
                }
            }

            uint32_t mFlBitmap = 0;
            uint32_t mSlBitmap[kFlCount] = {};
            Block* mBlocks[kFlCount][kSlCount] = {};
            Region mRegions[kMaxRegions] = {};
            int mRegionCount = 0;
            char* mLow = nullptr;
            char* mHigh = nullptr;
            size_t mTotal = 0;
            size_t mUsed = 0;
            uint32_t mFailures = 0;
        };

        Tlsf gLarge;
        Tlsf gLua;
        bool gLuaTried = false;
        constexpr size_t kLuaPoolSize = 8u * 1024u * 1024u; // Lua peaked at 6.4 MB in test #67
        constexpr size_t kGrowStep = 2u * 1024u * 1024u;

        struct Lock
        {
            Lock() { __malloc_lock(_REENT); }
            ~Lock() { __malloc_unlock(_REENT); }
        };

        // Under the lock. The first region: the linear memory beyond the GPU reserve (as the old heap
        // extension took). Later ones: kGrowStep (or the request) while kGpuFloor stays free.
        bool growLarge(size_t request)
        {
            const size_t avail = linearSpaceFree();
            size_t bytes = 0;
            if (gLarge.total() == 0 && avail > p3ds_linear_gpu_reserve + request + 64 * 1024)
                bytes = (avail - p3ds_linear_gpu_reserve) & ~size_t(0xFFFF);
            else
            {
                bytes = (request + 64 * 1024 > kGrowStep ? request + 64 * 1024 : kGrowStep);
                bytes = (bytes + 0xFFFF) & ~size_t(0xFFFF);
                if (avail < kGpuFloor + bytes)
                    return false;
            }
            void* mem = linearMemAlign(bytes, 0x1000);
            if (mem == nullptr)
                return false;
            if (!gLarge.addRegion(mem, bytes))
            {
                linearFree(mem);
                return false;
            }
            return true;
        }

        void* largeAlloc(size_t size)
        {
            Lock lock;
            void* p = gLarge.alloc(size);
            if (p == nullptr && growLarge(size))
                p = gLarge.alloc(size);
            return p;
        }

        size_t usableSize(void* p)
        {
            if (gLarge.contains(p) || gLua.contains(p))
                return Tlsf::usableSize(p);
            return __real_malloc_usable_size(p);
        }

        // Moves a block to fresh memory from malloc (any pool), then frees the old one.
        void* moveTo(void* p, size_t size, size_t oldUsable);
    }

    PoolStats large()
    {
        Lock lock;
        return gLarge.stats();
    }

    PoolStats lua()
    {
        Lock lock;
        return gLua.stats();
    }

    NewlibStats newlib()
    {
        NewlibStats s;
        Lock lock;
        const struct mallinfo mi = mallinfo();
        s.arena = uint32_t(mi.arena);
        s.used = uint32_t(mi.uordblks);
        // dlmalloc 2.6.4 layout (SIZE_SZ 4): bin i's header sits at &av_[2i+2] - 8, so its bk is av_[2i+3].
        // A chunk is {prev_size, size, fd, bk}. The top chunk is av_[2] (bin 0's fd).
        char* top = static_cast<char*>(__malloc_av_[2]);
        if (top != nullptr)
            s.top = uint32_t(*reinterpret_cast<size_t*>(top + 4) & ~size_t(3));
        for (int i = 1; i < 128; ++i)
        {
            char* const bin = reinterpret_cast<char*>(&__malloc_av_[2 * i + 2]) - 8;
            for (char* p = *reinterpret_cast<char**>(bin + 12); p != bin; p = *reinterpret_cast<char**>(p + 12))
            {
                const uint32_t size = uint32_t(*reinterpret_cast<size_t*>(p + 4) & ~size_t(3));
                if (size > s.largestFree)
                    s.largestFree = size;
                s.freeBySize[size < 1024 ? 0 : size < 16384 ? 1 : size < 262144 ? 2 : 3] += size;
            }
        }
        return s;
    }

    void* luaRealloc(void* ptr, size_t nsize)
    {
        if (nsize == 0)
        {
            std::free(ptr);
            return nullptr;
        }
        {
            Lock lock;
            if (!gLuaTried)
            {
                gLuaTried = true;
                if (void* mem = __real_malloc(kLuaPoolSize))
                    gLua.addRegion(mem, kLuaPoolSize);
            }
            if (ptr == nullptr)
            {
                if (void* p = gLua.alloc(nsize))
                    return p;
            }
            else if (gLua.contains(ptr))
            {
                if (void* p = gLua.resizeInPlace(ptr, nsize))
                    return p;
                if (void* p = gLua.alloc(nsize))
                {
                    const size_t old = Tlsf::usableSize(ptr);
                    std::memcpy(p, ptr, old < nsize ? old : nsize);
                    gLua.free(ptr);
                    return p;
                }
            }
        }
        return std::realloc(ptr, nsize); // the pool is full (or ptr lives elsewhere): malloc's pools
    }
}

using namespace p3ds::pools;

namespace p3ds::pools
{
    namespace
    {
        void* moveTo(void* p, size_t size, size_t oldUsable)
        {
            void* q = std::malloc(size);
            if (q == nullptr)
                return nullptr;
            std::memcpy(q, p, oldUsable < size ? oldUsable : size);
            std::free(p);
            return q;
        }
    }
}

extern "C"
{
    void* __wrap_malloc(size_t size)
    {
        if (size >= kLargeMin)
        {
            if (void* p = largeAlloc(size))
                return p;
        }
        if (void* p = __real_malloc(size))
            return p;
        // newlib's arena is full or too fragmented: any pool with room.
        if (size < kLargeMin)
        {
            if (void* p = largeAlloc(size))
                return p;
        }
        Lock lock;
        return gLua.alloc(size);
    }

    void __wrap_free(void* ptr)
    {
        if (ptr == nullptr)
            return;
        if (gLarge.mayContain(ptr) || gLua.mayContain(ptr))
        {
            Lock lock;
            if (gLarge.contains(ptr))
            {
                gLarge.free(ptr);
                return;
            }
            if (gLua.contains(ptr))
            {
                gLua.free(ptr);
                return;
            }
        }
        __real_free(ptr);
    }

    void* __wrap_realloc(void* ptr, size_t size)
    {
        if (ptr == nullptr)
            return __wrap_malloc(size);
        if (size == 0)
        {
            __wrap_free(ptr);
            return nullptr;
        }
        size_t old = 0;
        if (gLarge.mayContain(ptr) || gLua.mayContain(ptr))
        {
            Lock lock;
            Tlsf* pool = gLarge.contains(ptr) ? &gLarge : gLua.contains(ptr) ? &gLua : nullptr;
            if (pool != nullptr)
            {
                if (void* p = pool->resizeInPlace(ptr, size))
                    return p;
                old = Tlsf::usableSize(ptr);
            }
        }
        if (old != 0)
            return moveTo(ptr, size, old);
        if (size >= kLargeMin && __real_malloc_usable_size(ptr) < kLargeMin)
        {
            // A small block growing large: into the large pool, not newlib's arena.
            if (void* p = largeAlloc(size))
            {
                const size_t was = __real_malloc_usable_size(ptr);
                std::memcpy(p, ptr, was < size ? was : size);
                __real_free(ptr);
                return p;
            }
        }
        if (void* p = __real_realloc(ptr, size))
            return p;
        return moveTo(ptr, size, __real_malloc_usable_size(ptr)); // newlib full: another pool
    }

    void* __wrap_calloc(size_t count, size_t size)
    {
        if (size != 0 && count > size_t(-1) / size)
            return nullptr;
        const size_t bytes = count * size;
        void* p = __wrap_malloc(bytes);
        if (p != nullptr)
            std::memset(p, 0, bytes);
        return p;
    }

    size_t __wrap_malloc_usable_size(void* ptr)
    {
        if (ptr == nullptr)
            return 0;
        // M10: newlib's blocks (most of them) without the pools' lock, as free() does (the Zeste profile of 20/10:
        // this lock was ~2% of the main thread's instructions).
        if (!gLarge.mayContain(ptr) && !gLua.mayContain(ptr))
            return __real_malloc_usable_size(ptr);
        Lock lock;
        return usableSize(ptr);
    }
}
