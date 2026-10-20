#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/pools.hpp>

#include <3ds.h>

#include <malloc.h>

extern "C"
{
    extern char __start__[];
    extern char __end__[];
    size_t p3ds_heap_arena(); // platform.cpp: newlib's arena
    size_t p3ds_heap_window(); // the app heap newlib can use
}

namespace p3ds::mem
{
    namespace
    {
        bool gOverBudget = false;
        // Last mallinfo() figures (see queryExact): mallinfo walks every free chunk under the malloc lock,
        // ~10+ ms with OpenMW's fragmented heap on hardware, stalling every other thread's malloc.
        uint32_t gHeapUsed = 0;
        uint32_t gHeapArena = 0;
        uint64_t gWalkTick = 0;
    }

    uint32_t arenaAtLastWalk()
    {
        return __atomic_load_n(&gHeapArena, __ATOMIC_RELAXED);
    }

    Stats queryExact()
    {
        const struct mallinfo mi = mallinfo();
        const pools::PoolStats large = pools::large();
        const pools::PoolStats lua = pools::lua();
        // The Lua pool is one newlib block: count what Lua uses of it, not the whole block.
        __atomic_store_n(&gHeapArena, uint32_t(mi.arena) + large.used, __ATOMIC_RELAXED);
        __atomic_store_n(&gHeapUsed, uint32_t(mi.uordblks) - lua.total + lua.used + large.used, __ATOMIC_RELAXED);
        __atomic_store_n(&gWalkTick, svcGetSystemTick(), __ATOMIC_RELAXED);
        return query();
    }

    Stats query()
    {
        Stats s;
        Handle reslimit = 0;
        if (R_SUCCEEDED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
        {
            s64 limit = 0, current = 0;
            ResourceLimitType type = RESLIMIT_COMMIT;
            svcGetResourceLimitLimitValues(&limit, reslimit, &type, 1);
            svcGetResourceLimitCurrentValues(&current, reslimit, &type, 1);
            svcCloseHandle(reslimit);
            s.regionTotal = uint32_t(limit);
            s.regionUsed = uint32_t(current);
        }
        s.imageSize = uint32_t(__end__ - __start__);
        const pools::PoolStats large = pools::large();
        s.heapTotal = uint32_t(p3ds_heap_window()) + large.total;
        s.largeTotal = large.total;
        s.largeUsed = large.used;
        s.linearTotal = envGetLinearHeapSize();
        s.linearFree = linearSpaceFree();
        s.vramFree = vramSpaceFree();

        if (__atomic_load_n(&gHeapArena, __ATOMIC_RELAXED) == 0)
            return queryExact(); // never sampled yet (e.g. no watchdog running)
        s.heapArena = uint32_t(p3ds_heap_arena()) + large.used;
        s.heapUsed = __atomic_load_n(&gHeapUsed, __ATOMIC_RELAXED);
        s.heapUsedAgeMs = uint32_t((svcGetSystemTick() - __atomic_load_n(&gWalkTick, __ATOMIC_RELAXED))
            / (SYSCLOCK_ARM11 / 1000));
        return s;
    }

    uint32_t expectedRegion()
    {
        // .3dsx (Homebrew Launcher) inherits the default mode: 64 MB O3DS / 124 MB N3DS.
        // Our .cia requests the 178 MB New 3DS mode (packaging/app.rsf).
        uint32_t mb = 64;
        if (info().isNew3DS)
            mb = info().launchedAsCia ? 178 : 124;
        return mb * 1024u * 1024u;
    }

    uint32_t overProvision(const Stats& s)
    {
        const uint32_t expected = expectedRegion();
        return s.regionTotal > expected ? s.regionTotal - expected : 0;
    }

    uint32_t heapBudget(const Stats& s)
    {
        const uint32_t excess = overProvision(s);
        return s.heapTotal > excess ? s.heapTotal - excess : 0;
    }

    bool checkBudget(const Stats& s)
    {
        const bool over = s.heapUsed > heapBudget(s);
        if (over != gOverBudget)
        {
            gOverBudget = over;
            if (over)
                P3DS_LOGE("BUDGET: heap use %.1f MB exceeds the hardware budget %.1f MB - this would fail on console",
                    toMB(s.heapUsed), toMB(heapBudget(s)));
            else
                P3DS_LOGI("BUDGET: heap use back under the hardware budget");
        }
        return !over;
    }
}
