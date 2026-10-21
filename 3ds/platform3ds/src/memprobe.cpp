#include <platform3ds/crash.hpp>
#include <platform3ds/loadscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>

#include <3ds.h>
#include <malloc.h>

#include <algorithm>
#include <cstdio>

// Defined in crash.cpp (operator new/delete replacement).
extern thread_local int64_t p3ds_thread_net_bytes;

namespace p3ds::memprobe
{
    namespace
    {
        struct Entry
        {
            uint32_t key = 0;
            int64_t bytes = 0;
            uint32_t count = 0;
            uint64_t ticks = 0;
        };
        constexpr int kMaxEntries = 96;
        Entry gEntries[kMaxEntries];
        int gUsed = 0;

        // FourCC (little endian, as ESM::NAME::toInt) or a plain number.
        void keyName(uint32_t key, char out[12])
        {
            bool printable = true;
            for (int i = 0; i < 4; ++i)
            {
                const char c = char((key >> (8 * i)) & 0xFF);
                printable = printable && c >= 0x20 && c < 0x7F;
                out[i] = c;
            }
            out[4] = '\0';
            if (!printable)
                snprintf(out, 12, "0x%08lX", (unsigned long)key);
        }
    }

    uint32_t heapUsed()
    {
        // Per-thread net C++ allocations (operator new/delete, see crash.cpp): unlike mallinfo, not
        // polluted by other threads (e.g. the loading screen drawing while the ESM loads).
        return uint32_t(p3ds_thread_net_bytes);
    }

    uint64_t ticks()
    {
        return svcGetSystemTick();
    }
}

#ifdef OPENMW3DS_DEBUG
namespace p3ds::perf
{
    namespace
    {
        struct PhaseSum
        {
            const char* label;
            uint64_t ticks;
            uint64_t frameTicks = 0; // in the current frame (endFrame)
        };
        constexpr int kMaxPhases = 32;
        PhaseSum gPhases[kMaxPhases];
        int gPhaseCount = 0;
        LightLock gPhaseLock;
        bool gPhaseLockInit = [] {
            LightLock_Init(&gPhaseLock);
            return true;
        }();
    }

    void add(const char* label, uint64_t ticks)
    {
        // Perf batch 5: the lock only to add a label (the per-object phases, "lights" and "skinning", call this
        // thousands of times a frame: ~2% of the main thread in Zeste). Entries are only appended, a new one
        // published by the count; the sums are added atomically.
        // Each label's slot, remembered by its address (labels are string literals): no search for a known one.
        static int sSlotOf[64];
        const unsigned hash = (reinterpret_cast<uintptr_t>(label) >> 2) % 64;
        const int count = __atomic_load_n(&gPhaseCount, __ATOMIC_ACQUIRE);
        int found = sSlotOf[hash];
        if (found >= count || gPhases[found].label != label)
        {
            found = -1;
            for (int i = 0; i < count && found < 0; ++i)
                if (gPhases[i].label == label)
                    found = i;
            if (found >= 0)
                sSlotOf[hash] = found;
        }
        if (found >= 0)
        {
            __atomic_fetch_add(&gPhases[found].ticks, ticks, __ATOMIC_RELAXED);
            __atomic_fetch_add(&gPhases[found].frameTicks, ticks, __ATOMIC_RELAXED);
            return;
        }
        LightLock_Lock(&gPhaseLock);
        int i = 0;
        while (i < gPhaseCount && gPhases[i].label != label)
            ++i;
        if (i < gPhaseCount)
        {
            __atomic_fetch_add(&gPhases[i].ticks, ticks, __ATOMIC_RELAXED);
            __atomic_fetch_add(&gPhases[i].frameTicks, ticks, __ATOMIC_RELAXED);
        }
        else if (gPhaseCount < kMaxPhases)
        {
            gPhases[gPhaseCount] = PhaseSum{ label, ticks, ticks };
            __atomic_store_n(&gPhaseCount, gPhaseCount + 1, __ATOMIC_RELEASE);
        }
        LightLock_Unlock(&gPhaseLock);
    }

    void endFrame(bool report, char* out, unsigned size)
    {
        if (size == 0)
            return;
        out[0] = '\0';
        PhaseSum frame[kMaxPhases];
        int count = 0;
        LightLock_Lock(&gPhaseLock);
        for (int i = 0; i < gPhaseCount; ++i)
        {
            const uint64_t ticks = __atomic_exchange_n(&gPhases[i].frameTicks, 0, __ATOMIC_RELAXED);
            if (report)
            {
                frame[count] = gPhases[i];
                frame[count++].frameTicks = ticks;
            }
        }
        LightLock_Unlock(&gPhaseLock);
        if (!report)
            return;
        std::sort(frame, frame + count, [](const PhaseSum& a, const PhaseSum& b) { return a.frameTicks > b.frameTicks; });
        unsigned len = 0;
        for (int i = 0; i < count && len + 32 < size; ++i)
        {
            const double ms = double(frame[i].frameTicks) * 1000.0 / SYSCLOCK_ARM11;
            if (ms < 2.0)
                break;
            len += unsigned(std::snprintf(out + len, size - len, " %s %.0f", frame[i].label, ms));
        }
    }

    void logAndReset(unsigned frames)
    {
        if (frames == 0)
            return;
        char line[640];
        int len = snprintf(line, sizeof(line), "perf/more:");
        LightLock_Lock(&gPhaseLock);
        for (int i = 0; i < gPhaseCount; ++i)
        {
            const uint64_t ticks = __atomic_exchange_n(&gPhases[i].ticks, 0, __ATOMIC_RELAXED);
            if (len < int(sizeof(line)) - 32)
                len += snprintf(line + len, sizeof(line) - len, " %s %.1f", gPhases[i].label,
                    double(ticks) * 1000.0 / SYSCLOCK_ARM11 / frames);
        }
        LightLock_Unlock(&gPhaseLock);
        // writef stops at 511 characters
        log::write(log::Level::Info, std::string_view(line, std::min<size_t>(size_t(len), sizeof(line) - 1)));
    }
}

namespace p3ds::perf::detail
{
    namespace
    {
        struct DetailSum
        {
            const char* label;
            uint64_t ticks;
            unsigned count;
        };
        constexpr int kMaxDetails = 32;
        thread_local bool tActive = false;
        thread_local bool tLogMisses = false;
        thread_local DetailSum tDetails[kMaxDetails];
        thread_local int tDetailCount = 0;
    }

    void begin()
    {
        tActive = true;
        tDetailCount = 0;
    }

    bool active()
    {
        return tActive;
    }

    void setLogMisses(bool enabled)
    {
        tLogMisses = enabled;
    }

    bool logMisses()
    {
        return tLogMisses;
    }

    void add(const char* label, uint64_t ticks)
    {
        int i = 0;
        while (i < tDetailCount && tDetails[i].label != label)
            ++i;
        if (i < tDetailCount)
        {
            tDetails[i].ticks += ticks;
            ++tDetails[i].count;
        }
        else if (tDetailCount < kMaxDetails)
            tDetails[tDetailCount++] = DetailSum{ label, ticks, 1 };
    }

    void end(char* out, unsigned size)
    {
        tActive = false;
        std::sort(tDetails, tDetails + tDetailCount,
            [](const DetailSum& a, const DetailSum& b) { return a.ticks > b.ticks; });
        unsigned len = 0;
        if (size > 0)
            out[0] = '\0';
        for (int i = 0; i < tDetailCount && len + 1 < size; ++i)
        {
            const double ms = double(tDetails[i].ticks) * 1000.0 / SYSCLOCK_ARM11;
            if (ms < 1.0)
                break;
            const int n = snprintf(out + len, size - len, " %s %.0f/%u", tDetails[i].label, ms, tDetails[i].count);
            if (n < 0)
                break;
            len += unsigned(n);
        }
        tDetailCount = 0;
    }
}
#endif

namespace p3ds::memprobe
{

    void add(uint32_t key, int64_t bytes, uint64_t elapsedTicks)
    {
        for (int i = 0; i < gUsed; ++i)
            if (gEntries[i].key == key)
            {
                gEntries[i].bytes += bytes;
                ++gEntries[i].count;
                gEntries[i].ticks += elapsedTicks;
                return;
            }
        if (gUsed < kMaxEntries)
            gEntries[gUsed++] = Entry{ key, bytes, 1, elapsedTicks };
    }

    namespace
    {
        struct ReporterEntry
        {
            const char* name;
            Reporter fn;
            void* ctx;
        };
        ReporterEntry gReporters[8];
        int gReporterCount = 0;
    }

    void addReporter(const char* name, Reporter fn, void* ctx)
    {
        if (gReporterCount < 8)
            gReporters[gReporterCount++] = ReporterEntry{ name, fn, ctx };
    }

    void logReporters()
    {
        for (int i = 0; i < gReporterCount; ++i)
            P3DS_LOGI("mem/%s: %.2f MB", gReporters[i].name, double(gReporters[i].fn(gReporters[i].ctx)) / 1048576.0);
    }

    void stage(const char* label)
    {
        // A startup stage reached is progress for the hang watchdog (between the content files and the first
        // loading screen nothing else kicks it: interface and Lua set-up, ~6 s on hardware, past 60 s in an
        // emulator at unlimited speed).
        crash::kick();
        loadscreen::noteStage(label);
        static int64_t last = 0;
        static uint64_t lastTick = 0;
        const int64_t now = p3ds_thread_net_bytes;
        const uint64_t tick = svcGetSystemTick();
        // The time since the previous stage (M9, test #72's load hangs: where a cell change's seconds go).
        const double ms = lastTick != 0 ? double(tick - lastTick) * 1000.0 / SYSCLOCK_ARM11 : 0.0;
        // The arena, not mallinfo's "used": the heap walk holds the malloc lock (~60+ ms per call on the
        // main thread with a large heap), and stages run during every cell load.
        if (!debugMode())
            ; // detailed profiling: the start menu's Debug choices only (M15)
        else if (threadAccounting())
            P3DS_LOGI("memstage %-28s thread net %6.2f MB (+%5.2f)  arena %6.2f MB  +%5.0f ms", label,
                double(now) / 1048576.0, double(now - last) / 1048576.0, double(mem::query().heapArena) / 1048576.0,
                ms);
        else
            P3DS_LOGI("memstage %-28s arena %6.2f MB  +%5.0f ms", label, double(mem::query().heapArena) / 1048576.0, ms);
        last = now;
        lastTick = tick;
    }

    void report(const char* label)
    {
        if (!debugMode()) // detailed profiling: the start menu's Debug choices only (M15)
        {
            gUsed = 0;
            return;
        }
        std::sort(gEntries, gEntries + gUsed, [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
        int64_t total = 0;
        for (int i = 0; i < gUsed; ++i)
            total += gEntries[i].bytes;
        P3DS_LOGI("memprobe [%s]: %.2f MB total over %d keys", label, double(total) / 1048576.0, gUsed);
        for (int i = 0; i < gUsed; ++i)
        {
            char name[12];
            keyName(gEntries[i].key, name);
            P3DS_LOGI("  %-10s %8.2f MB  %6lu items  %5.0f B/item  %7.0f ms", name,
                double(gEntries[i].bytes) / 1048576.0, (unsigned long)gEntries[i].count,
                gEntries[i].count ? double(gEntries[i].bytes) / gEntries[i].count : 0.0,
                double(gEntries[i].ticks) * 1000.0 / SYSCLOCK_ARM11);
        }
        gUsed = 0;
    }
}
