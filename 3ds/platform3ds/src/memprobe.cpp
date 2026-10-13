#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>

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

    void add(uint32_t key, int64_t bytes)
    {
        for (int i = 0; i < gUsed; ++i)
            if (gEntries[i].key == key)
            {
                gEntries[i].bytes += bytes;
                ++gEntries[i].count;
                return;
            }
        if (gUsed < kMaxEntries)
            gEntries[gUsed++] = Entry{ key, bytes, 1 };
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
        static int64_t last = 0;
        const int64_t now = p3ds_thread_net_bytes;
        P3DS_LOGI("memstage %-28s thread net %6.2f MB (+%5.2f)  heap %6.2f MB", label, double(now) / 1048576.0,
            double(now - last) / 1048576.0, double(mallinfo().uordblks) / 1048576.0);
        last = now;
    }

    void report(const char* label)
    {
        std::sort(gEntries, gEntries + gUsed, [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
        int64_t total = 0;
        for (int i = 0; i < gUsed; ++i)
            total += gEntries[i].bytes;
        P3DS_LOGI("memprobe [%s]: %.2f MB total over %d keys", label, double(total) / 1048576.0, gUsed);
        for (int i = 0; i < gUsed; ++i)
        {
            char name[12];
            keyName(gEntries[i].key, name);
            P3DS_LOGI("  %-10s %8.2f MB  %6lu items  %5.0f B/item", name, double(gEntries[i].bytes) / 1048576.0,
                (unsigned long)gEntries[i].count,
                gEntries[i].count ? double(gEntries[i].bytes) / gEntries[i].count : 0.0);
        }
        gUsed = 0;
    }
}
