#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>

#include <3ds.h>

#include <malloc.h>

extern "C"
{
    extern char __start__[];
    extern char __end__[];
}

namespace p3ds::mem
{
    namespace
    {
        bool gOverBudget = false;
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
        s.heapTotal = envGetHeapSize();
        s.linearTotal = envGetLinearHeapSize();
        s.linearFree = linearSpaceFree();
        s.vramFree = vramSpaceFree();

        const struct mallinfo mi = mallinfo();
        s.heapArena = uint32_t(mi.arena);
        s.heapUsed = uint32_t(mi.uordblks);
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
