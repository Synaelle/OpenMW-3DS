#include <platform3ds/log.hpp>
#include <platform3ds/timing.hpp>

#include <3ds.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace p3ds::log
{
    namespace
    {
        constexpr size_t kPendingSize = 32 * 1024;
        constexpr s64 kFlushIntervalNs = 1000LL * 1000 * 1000; // 1 s

        LightLock gLock;
        bool gLockReady = false;

        // Ring of recent lines (static: usable from the crash handler with no allocation).
        Line gRing[kRingLines];
        uint32_t gRingNext = 0; // total lines ever written; index = gRingNext % kRingLines
        volatile uint32_t gGeneration = 0;

        // Double-buffered pending file output.
        char gPending[2][kPendingSize];
        size_t gPendingLen = 0;
        int gPendingIdx = 0;
        uint32_t gDropped = 0;

        FILE* gFile = nullptr;
        Level gFileLevel = Level::Debug;
        volatile bool gSync = false;

        Thread gThread = nullptr;
        LightEvent gWake;
        volatile bool gRunning = false;
        LightLock gFileLock; // serializes actual file writes (flush thread vs. explicit flush)

        void ensureLock()
        {
            if (!gLockReady)
            {
                LightLock_Init(&gLock);
                LightLock_Init(&gFileLock);
                gLockReady = true;
            }
        }

        // Swaps the pending buffer under gLock and writes the old one to the file.
        void drainToFile()
        {
            LightLock_Lock(&gFileLock);
            LightLock_Lock(&gLock);
            const int idx = gPendingIdx;
            const size_t len = gPendingLen;
            const uint32_t dropped = gDropped;
            gPendingIdx ^= 1;
            gPendingLen = 0;
            gDropped = 0;
            LightLock_Unlock(&gLock);

            if (gFile != nullptr)
            {
                if (len > 0)
                    fwrite(gPending[idx], 1, len, gFile);
                if (dropped > 0)
                    fprintf(gFile, "[log] %lu bytes dropped (pending buffer full)\n", (unsigned long)dropped);
                if (len > 0 || dropped > 0)
                    fflush(gFile);
            }
            LightLock_Unlock(&gFileLock);
        }

        void flushThread(void*)
        {
            while (gRunning)
            {
                LightEvent_WaitTimeout(&gWake, kFlushIntervalNs);
                drainToFile();
            }
        }
    }

    char levelChar(Level level)
    {
        switch (level)
        {
            case Level::Error:
                return 'E';
            case Level::Warning:
                return 'W';
            case Level::Info:
                return 'I';
            case Level::Verbose:
                return 'V';
            case Level::Debug:
                return 'D';
        }
        return '?';
    }

    bool init(const char* path, Level fileLevel)
    {
        ensureLock();
        if (gFile != nullptr)
            return true;
        gFileLevel = fileLevel;
        gFile = fopen(path, "w");
        if (gFile == nullptr)
            return false;
        // We do our own batching; keep stdio's buffer modest.
        setvbuf(gFile, nullptr, _IOFBF, 16 * 1024);

        LightEvent_Init(&gWake, RESET_ONESHOT);
        gRunning = true;
        // The 3DS scheduler is strictly priority-based with no time-slicing, so a lower-priority
        // flusher never runs while the main thread is busy (logs then pile up in RAM and are lost on a
        // hang). It sleeps almost always, so give it a higher priority than main (0x30), on core 1
        // (time-limited syscore) when available so it doesn't preempt the game on core 0.
        s32 mainPrio = 0x30;
        svcGetThreadPriority(&mainPrio, CUR_THREAD_HANDLE);
        gThread = threadCreate(flushThread, nullptr, 8 * 1024, mainPrio - 2, 1, false);
        if (gThread == nullptr)
            gThread = threadCreate(flushThread, nullptr, 8 * 1024, mainPrio - 2, 0, false);
        return true;
    }

    void shutdown()
    {
        if (gThread != nullptr)
        {
            gRunning = false;
            LightEvent_Signal(&gWake);
            threadJoin(gThread, U64_MAX);
            threadFree(gThread);
            gThread = nullptr;
        }
        drainToFile();
        if (gFile != nullptr)
        {
            fclose(gFile);
            gFile = nullptr;
        }
    }

    void setLevel(Level fileLevel)
    {
        gFileLevel = fileLevel;
    }

    void setSyncMode(bool sync)
    {
        gSync = sync;
        if (sync)
            drainToFile();
    }

    void write(Level level, std::string_view msg)
    {
        ensureLock();

        // Format: "[   12.345] E message"
        char prefix[24];
        const double t = secondsSinceBoot();
        const int prefixLen = snprintf(prefix, sizeof(prefix), "[%9.3f] %c ", t, levelChar(level));

        // Strip one trailing newline; we add our own.
        if (!msg.empty() && msg.back() == '\n')
            msg.remove_suffix(1);

        LightLock_Lock(&gLock);

        Line& line = gRing[gRingNext % kRingLines];
        line.level = level;
        const size_t n = msg.size() < kLineLen - 1 ? msg.size() : kLineLen - 1;
        memcpy(line.text, msg.data(), n);
        line.text[n] = '\0';
        ++gRingNext;

        bool wantFlush = false;
        if (gFile != nullptr && level <= gFileLevel)
        {
            const size_t need = size_t(prefixLen) + msg.size() + 1;
            if (gPendingLen + need <= kPendingSize)
            {
                char* dst = gPending[gPendingIdx] + gPendingLen;
                memcpy(dst, prefix, prefixLen);
                memcpy(dst + prefixLen, msg.data(), msg.size());
                dst[prefixLen + msg.size()] = '\n';
                gPendingLen += need;
            }
            else
                gDropped += need;
            wantFlush = level == Level::Error || gPendingLen > kPendingSize * 3 / 4;
        }
        gGeneration = gGeneration + 1;
        LightLock_Unlock(&gLock);

        if (gSync)
            drainToFile();
        else if (wantFlush && gThread != nullptr)
            LightEvent_Signal(&gWake);
    }

    void writef(Level level, const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        if (n < 0)
            return;
        if (size_t(n) >= sizeof(buf))
            n = sizeof(buf) - 1;
        write(level, std::string_view(buf, size_t(n)));
    }

    void flush()
    {
        drainToFile();
    }

    uint32_t generation()
    {
        return gGeneration;
    }

    size_t copyLastLines(Line* out, size_t maxLines)
    {
        ensureLock();
        LightLock_Lock(&gLock);
        const uint32_t total = gRingNext;
        size_t count = total < kRingLines ? total : kRingLines;
        if (count > maxLines)
            count = maxLines;
        for (size_t i = 0; i < count; ++i)
            out[i] = gRing[(total - count + i) % kRingLines];
        LightLock_Unlock(&gLock);
        return count;
    }

    void forEachLineUnsafe(size_t maxLines, void (*fn)(const Line&, void*), void* ctx)
    {
        const uint32_t total = gRingNext;
        size_t count = total < kRingLines ? total : kRingLines;
        if (count > maxLines)
            count = maxLines;
        for (size_t i = 0; i < count; ++i)
            fn(gRing[(total - count + i) % kRingLines], ctx);
    }
}
