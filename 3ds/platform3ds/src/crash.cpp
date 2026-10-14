#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/textfb.hpp>

#include <3ds.h>

#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <cstring>
#include <exception>
#include <new>
#include <typeinfo>

#include <cxxabi.h>
#include <unwind.h>

extern "C"
{
    // platform.cpp two-segment sbrk diagnostics
    extern volatile uint32_t p3ds_sbrk_failures;
    extern volatile int32_t p3ds_sbrk_last_fail_incr;
    extern volatile uint32_t p3ds_sbrk_last_fail_linear_free;
    extern char __start__[];
    extern char __exidx_start[]; // end of .text (start of exception index in rodata)
}

namespace p3ds::crash
{
    namespace
    {
        constexpr size_t kHandlerStackSize = 32 * 1024;
        constexpr size_t kReportSize = 24 * 1024;
        constexpr size_t kEmergencyReserve = 1024 * 1024;
        constexpr int kMaxFpFrames = 32;
        constexpr int kMaxScanHits = 48;
        constexpr int kMaxScanWords = 8192;

        alignas(8) u8 gHandlerStack[kHandlerStackSize];
        char gReport[kReportSize];
        size_t gReportLen = 0;
        char gPath[96] = "/openmw/crash.log";
        volatile bool gWaitForInput = true;
        int gInCrash = 0; // accessed with __atomic builtins
        const char* const* gContext = nullptr;
        void* gEmergency = nullptr;

        // ---- allocation-free report builder ----
        void put(const char* s)
        {
            while (*s != '\0' && gReportLen < kReportSize - 1)
                gReport[gReportLen++] = *s++;
        }
        void putc_(char c)
        {
            if (gReportLen < kReportSize - 1)
                gReport[gReportLen++] = c;
        }
        void putHex(u32 v)
        {
            static const char digits[] = "0123456789ABCDEF";
            put("0x");
            for (int i = 7; i >= 0; --i)
                putc_(digits[(v >> (i * 4)) & 0xF]);
        }
        void putDec(u32 v)
        {
            char tmp[12];
            int n = 0;
            do
            {
                tmp[n++] = char('0' + v % 10);
                v /= 10;
            } while (v != 0 && n < 11);
            while (n > 0)
                putc_(tmp[--n]);
        }
        void putKB(u32 bytes)
        {
            putDec(bytes / 1024);
            put(" KB");
        }

        bool isCodeAddr(u32 a)
        {
            return a >= u32(uintptr_t(__start__)) && a < u32(uintptr_t(__exidx_start));
        }

        // Bounds of the mapped block containing addr (so stack reads can't fault).
        bool regionOf(u32 addr, u32& lo, u32& hi)
        {
            MemInfo mi;
            PageInfo pi;
            if (R_FAILED(svcQueryMemory(&mi, &pi, addr)) || mi.perm == 0)
                return false;
            lo = mi.base_addr;
            hi = mi.base_addr + mi.size;
            return true;
        }

        // pc == 0: no faulting pc (software fatal error); the walk starts at the caller.
        void appendBacktrace(u32 sp, u32 fp, u32 pc, u32 lr)
        {
            put("\nBacktrace (frame pointer walk):\n");
            int frame = 0;
            if (pc != 0)
            {
                put("  #0 pc ");
                putHex(pc);
                put("\n  #1 lr ");
                putHex(lr);
                putc_('\n');
                frame = 2;
            }

            u32 lo = 0, hi = 0;
            const bool haveStack = regionOf(sp, lo, hi);
            if (haveStack)
            {
                // GCC ARM-mode frame: fp -> saved lr, [fp-4] -> caller's fp.
                for (int i = 0; i < kMaxFpFrames; ++i)
                {
                    if (fp < lo + 4 || fp + 4 > hi || (fp & 3) != 0)
                        break;
                    const u32 savedLr = *reinterpret_cast<const u32*>(fp);
                    const u32 prevFp = *reinterpret_cast<const u32*>(fp - 4);
                    if (!isCodeAddr(savedLr & ~1u))
                        break;
                    if (frame == 2 && savedLr == lr) // leaf already reported via lr
                    {
                        fp = prevFp;
                        continue;
                    }
                    put("  #");
                    putDec(u32(frame++));
                    put(" ");
                    putHex(savedLr);
                    putc_('\n');
                    if (prevFp <= fp)
                        break;
                    fp = prevFp;
                }

                put("\nStack scan (code addresses found on the stack, may include stale values):\n ");
                int hits = 0;
                const u32* p = reinterpret_cast<const u32*>(sp & ~3u);
                const u32* end = reinterpret_cast<const u32*>(hi);
                for (int i = 0; p < end && i < kMaxScanWords && hits < kMaxScanHits; ++i, ++p)
                {
                    if (isCodeAddr(*p & ~1u))
                    {
                        putc_(' ');
                        putHex(*p);
                        if (++hits % 6 == 0)
                            put("\n ");
                    }
                }
                putc_('\n');
            }
            else
                put("  (stack pointer not in a mapped region)\n");
        }

        void appendMemAndLog()
        {
            const mem::Stats s = mem::query();
            put("\nMemory:\n  region ");
            putKB(s.regionUsed);
            put(" / ");
            putKB(s.regionTotal);
            put("\n  image ");
            putKB(s.imageSize);
            put("\n  heap used ");
            putKB(s.heapUsed);
            put(" (arena ");
            putKB(s.heapArena);
            put(") / ");
            putKB(s.heapTotal);
            put("\n  linear free ");
            putKB(s.linearFree);
            put(" / ");
            putKB(s.linearTotal);
            put("\n  vram free ");
            putKB(s.vramFree);
            put("\n  sbrk failures ");
            putDec(p3ds_sbrk_failures);
            put(", last incr ");
            putDec(u32(p3ds_sbrk_last_fail_incr));
            put(", linear free then ");
            putKB(p3ds_sbrk_last_fail_linear_free);
            putc_('\n');

            if (gContext != nullptr && *gContext != nullptr)
            {
                put("\nContext: ");
                put(*gContext);
                putc_('\n');
            }

            put("\nLast log lines:\n");
            log::forEachLineUnsafe(64,
                [](const log::Line& line, void*) {
                    put("  ");
                    putc_(log::levelChar(line.level));
                    putc_(' ');
                    put(line.text);
                    putc_('\n');
                },
                nullptr);
        }

        void writeReportFile()
        {
            Handle file = 0;
            const Result rc = FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""),
                fsMakePath(PATH_ASCII, gPath), FS_OPEN_CREATE | FS_OPEN_WRITE, 0);
            if (R_FAILED(rc))
                return;
            FSFILE_SetSize(file, 0);
            u32 written = 0;
            FSFILE_Write(file, &written, 0, gReport, u32(gReportLen), FS_WRITE_FLUSH);
            FSFILE_Close(file);
        }

        void showScreenAndExit(const char* headline)
        {
            using namespace textfb;
            clear(color::darkRed);
            drawText(1, 1, "OpenMW 3DS stopped", color::white, color::darkRed);
            drawText(1, 3, headline, color::yellow, color::darkRed, kCols - 2);
            drawText(1, 5, "Report written to", color::white, color::darkRed);
            drawText(1, 6, "sdmc:/openmw/crash.log", color::cyan, color::darkRed);
            drawText(1, 8, gWaitForInput ? "Press START to exit." : "Exiting...", color::white, color::darkRed);
            present();

            if (gWaitForInput)
            {
                for (;;)
                {
                    hidScanInput();
                    if (hidKeysDown() & KEY_START)
                        break;
                    svcSleepThread(50 * 1000 * 1000);
                }
            }
            else
                svcSleepThread(2000LL * 1000 * 1000);
            svcExitProcess();
        }

        const char* exceptionName(ERRF_ExceptionType t)
        {
            switch (t)
            {
                case ERRF_EXCEPTION_PREFETCH_ABORT:
                    return "prefetch abort";
                case ERRF_EXCEPTION_DATA_ABORT:
                    return "data abort";
                case ERRF_EXCEPTION_UNDEFINED:
                    return "undefined instruction";
                case ERRF_EXCEPTION_VFP:
                    return "VFP exception";
            }
            return "unknown exception";
        }

        void beginReport(const char* kind)
        {
            gReportLen = 0;
            put("=== OpenMW 3DS crash report ===\nBuild: ");
            put(p3ds_build_id);
            putc_('\n');
            put("Kind: ");
            put(kind);
            put("\nUptime: ");
            const u64 t = svcGetSystemTick();
            putDec(u32(t / SYSCLOCK_ARM11));
            put(" s");
            put("\nThread id: ");
            u32 tid = 0;
            svcGetThreadId(&tid, CUR_THREAD_HANDLE);
            putDec(tid);
            put("\nImage: ");
            putHex(u32(uintptr_t(__start__)));
            put(" - ");
            putHex(u32(uintptr_t(__exidx_start)));
            put(" (text)\n");
        }

        // Runs on gHandlerStack. Must not return.
        [[noreturn]] void exceptionHandler(ERRF_ExceptionInfo* info, CpuRegisters* regs)
        {
            if (__atomic_fetch_add(&gInCrash, 1, __ATOMIC_SEQ_CST) != 0)
                svcExitProcess(); // crashed while reporting a crash

            beginReport(exceptionName(info->type));
            put("Fault address (FAR): ");
            putHex(info->far);
            put("\nFault status (FSR): ");
            putHex(info->fsr);
            put("\n\nRegisters:\n");
            for (int i = 0; i < 13; ++i)
            {
                put(i < 10 ? "  r" : "  r");
                putDec(u32(i));
                put(i < 10 ? "  " : " ");
                putHex(regs->r[i]);
                if (i % 4 == 3)
                    putc_('\n');
            }
            put("\n  sp  ");
            putHex(regs->sp);
            put("  lr  ");
            putHex(regs->lr);
            put("  pc  ");
            putHex(regs->pc);
            put("  cpsr ");
            putHex(regs->cpsr);
            putc_('\n');

            appendBacktrace(regs->sp, regs->r[11], regs->pc, regs->lr);
            appendMemAndLog();
            writeReportFile();

            char headline[40] = "";
            strncpy(headline, exceptionName(info->type), sizeof(headline) - 1);
            showScreenAndExit(headline);
            __builtin_unreachable();
        }

        void terminateHandler()
        {
            const char* what = "std::terminate";
            static char buf[160];
            if (std::exception_ptr ep = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(ep);
                }
                catch (const std::exception& e)
                {
                    strncpy(buf, "uncaught exception: ", sizeof(buf) - 1);
                    strncat(buf, e.what(), sizeof(buf) - strlen(buf) - 1);
                    what = buf;
                }
                catch (...)
                {
                    what = "uncaught non-std exception";
                }
            }
            fatal(what);
        }

        void newHandler()
        {
            if (gEmergency != nullptr)
            {
                free(gEmergency);
                gEmergency = nullptr;
                P3DS_LOGE("OOM: released the %u KB emergency reserve", unsigned(kEmergencyReserve / 1024));
                if (memprobe::callerProfileActive())
                    memprobe::callerProfileStop("at out-of-memory");
                return;
            }
            fatal("out of memory (operator new)");
        }
    }

    void installForCurrentThread()
    {
        threadOnException(exceptionHandler, gHandlerStack + kHandlerStackSize, WRITE_DATA_TO_HANDLER_STACK);
    }

    void install(const char* crashLogPath)
    {
        // FS:USER wants an archive-relative path; strip "sdmc:".
        const char* p = crashLogPath;
        if (strncmp(p, "sdmc:", 5) == 0)
            p += 5;
        strncpy(gPath, p, sizeof(gPath) - 1);

        installForCurrentThread();
        std::set_terminate(terminateHandler);
        gEmergency = malloc(kEmergencyReserve);
        std::set_new_handler(newHandler);
    }

    void setWaitForInput(bool wait)
    {
        gWaitForInput = wait;
    }

    void setContextString(const char* const* ctx)
    {
        gContext = ctx;
    }

    [[noreturn]] __attribute__((noinline)) void fatal(const char* reason)
    {
        if (__atomic_fetch_add(&gInCrash, 1, __ATOMIC_SEQ_CST) != 0)
            svcExitProcess();

        u32 sp, fp;
        asm volatile("mov %0, sp" : "=r"(sp));
        asm volatile("mov %0, fp" : "=r"(fp));
        const u32 lr = u32(uintptr_t(__builtin_return_address(0)));

        beginReport("fatal error");
        put("Reason: ");
        put(reason);
        putc_('\n');
        (void)lr;
        appendBacktrace(sp, fp, 0, 0);
        appendMemAndLog();
        writeReportFile();
        showScreenAndExit(reason);
        __builtin_unreachable();
    }
}

// ---- hang watchdog ----
namespace p3ds::crash
{
    namespace
    {
        const char* volatile gStep = "startup";
        volatile u64 gLastKick = 0;
        volatile u64 gTimeoutTicks = 0;
        volatile bool gWatchdogRun = false;
        Thread gWatchdog = nullptr;

        [[noreturn]] void reportHang()
        {
            if (__atomic_fetch_add(&gInCrash, 1, __ATOMIC_SEQ_CST) != 0)
                svcExitProcess();
            const u32 stalledSec = u32((svcGetSystemTick() - gLastKick) / SYSCLOCK_ARM11);
            beginReport("hang (watchdog)");
            put("Stuck in step: ");
            put(gStep);
            put("\nNo progress for ");
            putDec(stalledSec);
            put(" s\n");
            appendMemAndLog();
            writeReportFile();

            static char headline[48];
            strcpy(headline, "hang: ");
            strncat(headline, gStep, sizeof(headline) - strlen(headline) - 1);
            showScreenAndExit(headline);
            __builtin_unreachable();
        }

        void watchdogMain(void*)
        {
            installForCurrentThread();
            int tick = 0;
            uint32_t peakHeap = 0;
            while (gWatchdogRun)
            {
                svcSleepThread(500LL * 1000 * 1000);
                // Memory profile every 5 s (the log is thread-safe).
                const mem::Stats s = mem::query();
                if (s.heapUsed > peakHeap)
                    peakHeap = s.heapUsed;
                if (++tick % 10 == 0)
                {
                    P3DS_LOGI("mem: heap %.1f MB used (arena %.1f / %.1f, peak %.1f), linear free %.1f MB [%s]",
                        mem::toMB(s.heapUsed), mem::toMB(s.heapArena), mem::toMB(s.heapTotal), mem::toMB(peakHeap),
                        mem::toMB(s.linearFree), gStep);
                    memprobe::logThreads();
                    memprobe::logReporters();
                    unsigned stackPeak = 0, stackSize = 0;
                    if (mainStackUsage(stackPeak, stackSize))
                        P3DS_LOGI("mem/stack: main thread peak %u KB of %u KB", stackPeak / 1024, stackSize / 1024);
                    if (p3ds_sbrk_failures != 0)
                        P3DS_LOGW("sbrk: %lu failures, last incr %ld, linear free then %lu",
                            (unsigned long)p3ds_sbrk_failures, (long)p3ds_sbrk_last_fail_incr,
                            (unsigned long)p3ds_sbrk_last_fail_linear_free);
                }
                // Every 0.5 s: the emulator ignores the faulting accesses below the heap, so an overflow
                // would otherwise just corrupt silently there.
                if (!mainStackIntact())
                    fatal("main thread stack overflow");
                const u64 timeout = gTimeoutTicks;
                if (timeout != 0 && svcGetSystemTick() - gLastKick > timeout)
                    reportHang();
            }
        }
    }

    void kick(const char* step)
    {
        if (step != nullptr)
            gStep = step;
        gLastKick = svcGetSystemTick();
    }

    void setWatchdogTimeout(int seconds)
    {
        gLastKick = svcGetSystemTick();
        gTimeoutTicks = seconds > 0 ? u64(seconds) * SYSCLOCK_ARM11 : 0;
    }

    bool startWatchdog(int seconds, int core)
    {
        if (gWatchdog != nullptr)
            return true;
        setWatchdogTimeout(seconds);
        gWatchdogRun = true;
        // Highest application priority: it sleeps almost always, and must preempt a spinning thread
        // when it does wake up (the 3DS scheduler never time-slices equal priorities).
        gWatchdog = threadCreate(watchdogMain, nullptr, 8 * 1024, 0x18, core, true);
        if (gWatchdog == nullptr && core != 0)
            gWatchdog = threadCreate(watchdogMain, nullptr, 8 * 1024, 0x18, 0, true);
        P3DS_LOGI("watchdog: %s (timeout %d s, core %d)", gWatchdog ? "running" : "FAILED to start", seconds, core);
        return gWatchdog != nullptr;
    }
}

// ---- operator new instrumentation ----
// Replaces the global operator new so out-of-memory reports say what was asked for, and large
// allocations (often the real culprits on a 3DS) show up in the log. operator new[] and the nothrow
// forms in libstdc++ call this one.
namespace
{
    constexpr std::size_t kLargeAllocLog = 4u * 1024u * 1024u;
}

// Net bytes allocated minus freed through operator new/delete by the current thread. Used by
// p3ds::memprobe for exact, concurrency-free attribution (malloc_usable_size includes chunk rounding).
thread_local int64_t p3ds_thread_net_bytes = 0;

// Registry of per-thread counters so the watchdog can log every thread's net C++ allocations.
// Note: memory freed by a different thread than the one that allocated it shows up as a negative
// balance on the freeing thread; the sum is still exact.
namespace
{
    // Global slots (not pointers into thread-local storage, which is freed when a thread exits).
    struct ThreadSlot
    {
        u32 tid;
        int64_t bytes; // updated only by the owning thread
    };
    constexpr int kMaxThreadSlots = 32;
    ThreadSlot gThreadSlots[kMaxThreadSlots];
    int gThreadSlotCount = 0; // grows only (__atomic)
    thread_local bool tRegistered = false;
    thread_local int tSlot = -1;

    void registerThreadCounter()
    {
        tRegistered = true;
        const int i = __atomic_fetch_add(&gThreadSlotCount, 1, __ATOMIC_SEQ_CST);
        if (i >= kMaxThreadSlots)
            return;
        u32 tid = 0;
        svcGetThreadId(&tid, CUR_THREAD_HANDLE);
        gThreadSlots[i].tid = tid;
        gThreadSlots[i].bytes = p3ds_thread_net_bytes;
        tSlot = i;
    }

    inline void accountThread(int64_t delta)
    {
        p3ds_thread_net_bytes += delta;
        if (tSlot >= 0)
            gThreadSlots[tSlot].bytes += delta;
    }
}

namespace p3ds::memprobe
{
    // Logs each thread's net operator-new balance (called by the watchdog).
    void logThreads()
    {
        const int n = __atomic_load_n(&gThreadSlotCount, __ATOMIC_ACQUIRE);
        char line[256];
        int len = snprintf(line, sizeof(line), "mem/threads (net new MB):");
        for (int i = 0; i < n && i < kMaxThreadSlots; ++i)
        {
            if (gThreadSlots[i].tid == 0 || len >= int(sizeof(line)) - 24)
                continue;
            len += snprintf(line + len, sizeof(line) - len, " t%lu=%.1f", (unsigned long)gThreadSlots[i].tid,
                double(gThreadSlots[i].bytes) / 1048576.0);
        }
        P3DS_LOGI("%s", line);
    }
}

// ---- caller profile: bytes per operator-new call site while enabled ----
// Gross and net: every allocation made while profiling is remembered (pointer -> call site) in a
// hash table in linear memory, so frees from any thread are matched back to their call site. The
// table is taken out of the GPU reserve (unused until the renderer exists), so profiling doesn't
// shrink the heap and move the out-of-memory point.
extern "C" size_t p3ds_linear_gpu_reserve; // platform.cpp

namespace
{
    struct CallerEntry
    {
        void* caller;
        uint64_t bytes; // allocated
        int64_t net; // allocated minus freed (of the allocations made while profiling)
        uint32_t count;
    };
    constexpr int kCallerSlots = 2048;
    constexpr u32 kAllThreads = 0xFFFFFFFFu;
    CallerEntry gCallers[kCallerSlots];
    volatile u32 gCallerTid = 0; // 0 = profiling off, kAllThreads = every thread
    // Blocking lock (kernel address arbitration): a waiter sleeps, so a preempted lower-priority holder
    // gets to run. A spinlock deadlocks here: the 3DS scheduler never time-slices.
    LightLock gCallerLock;
    u32 gCallerDropped = 0; // allocations not tracked (a table was full)

    struct LiveEntry
    {
        void* p; // nullptr = empty, kTomb = deleted
        uint32_t size;
        uint32_t slot;
    };
    void* const kTomb = reinterpret_cast<void*>(1);
    constexpr u32 kLiveSlots = 1u << 19; // 512K entries, 6 MB
    constexpr u32 kLiveMax = kLiveSlots / 4 * 3;
    LiveEntry* volatile gLive = nullptr;
    u32 gLiveUsed = 0; // entries ever filled (live + tombstones; no rehash)
    size_t gLiveReserveTaken = 0;

    inline u32 hashPtr(const void* p)
    {
        return (u32(uintptr_t(p)) >> 3) * 2654435761u;
    }

    int callerSlot(void* caller)
    {
        const u32 h = hashPtr(caller);
        for (int probe = 0; probe < kCallerSlots; ++probe)
        {
            const int i = int((h + u32(probe)) % kCallerSlots);
            if (gCallers[i].caller == caller)
                return i;
            if (gCallers[i].caller == nullptr)
            {
                gCallers[i].caller = caller;
                return i;
            }
        }
        return -1;
    }

    // Optional: full backtraces of a few allocations from one call site (sdmc:/openmw/trace.txt holds
    // "<hex caller address> [count]"), read when a profile starts. Uses the EHABI unwinder, which
    // works without frame pointers.
    void* volatile gTraceCaller = nullptr;
    int gTraceLeft = 0;

    struct TraceState
    {
        char line[200];
        int len;
        int depth;
    };

    _Unwind_Reason_Code traceFrame(_Unwind_Context* ctx, void* arg)
    {
        auto* st = static_cast<TraceState*>(arg);
        const uintptr_t pc = _Unwind_GetIP(ctx);
        if (pc == 0 || st->depth >= 24)
            return _URC_END_OF_STACK;
        if (st->len > int(sizeof(st->line)) - 12)
        {
            P3DS_LOGI("%s", st->line);
            st->len = snprintf(st->line, sizeof(st->line), "   ");
        }
        st->len += snprintf(st->line + st->len, sizeof(st->line) - st->len, " 0x%08lX", (unsigned long)pc);
        ++st->depth;
        return _URC_NO_REASON;
    }

    void traceAllocation(std::size_t size)
    {
        TraceState st;
        st.len = snprintf(st.line, sizeof(st.line), "trace alloc %u B from %p:", unsigned(size), gTraceCaller);
        st.depth = 0;
        _Unwind_Backtrace(traceFrame, &st);
        P3DS_LOGI("%s", st.line);
    }

    void loadTraceRequest()
    {
        FILE* f = fopen("sdmc:/openmw/trace.txt", "r");
        if (f == nullptr)
            return;
        unsigned long addr = 0;
        int count = 3;
        if (fscanf(f, "%lx %d", &addr, &count) >= 1 && addr != 0)
        {
            gTraceLeft = count;
            gTraceCaller = reinterpret_cast<void*>(addr);
            P3DS_LOGI("caller profile: tracing %d allocations from 0x%08lX", count, addr);
        }
        fclose(f);
    }

    void recordAlloc(void* caller, void* p, std::size_t size)
    {
        const u32 want = gCallerTid;
        if (want == 0)
            return;
        if (caller == gTraceCaller && gTraceLeft > 0)
        {
            --gTraceLeft;
            traceAllocation(size);
        }
        if (want != kAllThreads)
        {
            u32 tid = 0;
            svcGetThreadId(&tid, CUR_THREAD_HANDLE);
            if (tid != want)
                return;
        }
        LightLock_Lock(&gCallerLock);
        const int slot = gCallerTid != 0 ? callerSlot(caller) : -1;
        LiveEntry* live = gLive;
        if (slot >= 0)
        {
            gCallers[slot].bytes += size;
            ++gCallers[slot].count;
            if (live != nullptr && gLiveUsed < kLiveMax)
            {
                for (u32 i = hashPtr(p);; ++i)
                {
                    LiveEntry& e = live[i & (kLiveSlots - 1)];
                    if (e.p == nullptr)
                    {
                        e = LiveEntry{ p, uint32_t(size), uint32_t(slot) };
                        ++gLiveUsed;
                        gCallers[slot].net += int64_t(size);
                        break;
                    }
                }
            }
            else
                ++gCallerDropped;
        }
        else
            ++gCallerDropped;
        LightLock_Unlock(&gCallerLock);
    }

    // Called before the block is freed (so its address can't be reused and recorded meanwhile).
    void recordFree(void* p)
    {
        if (gLive == nullptr)
            return;
        LightLock_Lock(&gCallerLock);
        if (LiveEntry* live = gLive)
        {
            for (u32 i = hashPtr(p);; ++i)
            {
                LiveEntry& e = live[i & (kLiveSlots - 1)];
                if (e.p == nullptr)
                    break; // not allocated while profiling
                if (e.p == p)
                {
                    gCallers[e.slot].net -= int64_t(e.size);
                    e.p = kTomb;
                    break;
                }
            }
        }
        LightLock_Unlock(&gCallerLock);
    }
}

namespace p3ds::memprobe
{
    void callerProfileStart(bool allThreads)
    {
        // Opt-in: costs 6 MB of linear memory and a lock per allocation while active.
        static const bool enabled = [] {
            FILE* f = fopen("sdmc:/openmw/memprofile", "r");
            if (f != nullptr)
                fclose(f);
            if (f != nullptr)
                P3DS_LOGI("caller profiles enabled (sdmc:/openmw/memprofile exists)");
            return f != nullptr;
        }();
        if (!enabled)
            return;
        static bool lockInit = false;
        if (!lockInit)
        {
            LightLock_Init(&gCallerLock);
            lockInit = true;
        }
        gCallerTid = 0;
        loadTraceRequest();
        // Outside the lock: linearAlloc may itself use operator new (LightLock isn't recursive).
        LiveEntry* fresh = nullptr;
        const size_t bytes = kLiveSlots * sizeof(LiveEntry);
        if (gLive == nullptr)
        {
            fresh = static_cast<LiveEntry*>(linearAlloc(bytes));
            if (fresh != nullptr)
                memset(fresh, 0, bytes);
            else
                P3DS_LOGW("caller profile: no linear memory for the net table, gross bytes only");
        }
        LightLock_Lock(&gCallerLock);
        memset(gCallers, 0, sizeof(gCallers));
        gCallerDropped = 0;
        if (fresh != nullptr)
        {
            gLiveReserveTaken = bytes < p3ds_linear_gpu_reserve ? bytes : 0;
            p3ds_linear_gpu_reserve -= gLiveReserveTaken;
            gLiveUsed = 0;
            gLive = fresh;
        }
        u32 tid = 0;
        svcGetThreadId(&tid, CUR_THREAD_HANDLE);
        gCallerTid = allThreads ? kAllThreads : tid;
        LightLock_Unlock(&gCallerLock);
    }

    bool callerProfileActive()
    {
        return gCallerTid != 0;
    }

    void callerProfileStop(const char* label)
    {
        if (gCallerTid == 0 && gLive == nullptr)
            return; // not running (disabled)
        LightLock_Lock(&gCallerLock);
        gCallerTid = 0;
        P3DS_LOGI("caller profile [%s] (net = still allocated; symbolize with addr2line; %lu untracked):", label,
            (unsigned long)gCallerDropped);
        // Top 40 by net bytes (selection sort; the table is small).
        for (int k = 0; k < 40; ++k)
        {
            int best = -1;
            for (int i = 0; i < kCallerSlots; ++i)
                if (gCallers[i].caller != nullptr && (best < 0 || gCallers[i].net > gCallers[best].net))
                    best = i;
            if (best < 0 || gCallers[best].net <= 0)
                break;
            P3DS_LOGI("  caller 0x%08lX  net %7.2f MB  gross %7.2f MB  %7lu allocs",
                (unsigned long)uintptr_t(gCallers[best].caller), double(gCallers[best].net) / 1048576.0,
                double(gCallers[best].bytes) / 1048576.0, (unsigned long)gCallers[best].count);
            gCallers[best].caller = nullptr;
        }
        LiveEntry* live = gLive;
        gLive = nullptr;
        p3ds_linear_gpu_reserve += gLiveReserveTaken;
        gLiveReserveTaken = 0;
        LightLock_Unlock(&gCallerLock);
        if (live != nullptr)
            linearFree(live);
    }
}

void* operator new(std::size_t size)
{
    if (size == 0)
        size = 1;
    if (size >= kLargeAllocLog)
        P3DS_LOGW("large allocation: %u bytes (%.1f MB) from %p", unsigned(size), double(size) / 1048576.0,
            __builtin_return_address(0));
    for (;;)
    {
        if (void* p = std::malloc(size))
        {
            if (!tRegistered)
                registerThreadCounter();
            accountThread(int64_t(malloc_usable_size(p)));
            if (gCallerTid != 0)
                recordAlloc(__builtin_return_address(0), p, size);
            return p;
        }
        P3DS_LOGE("operator new(%u bytes) failed, caller %p", unsigned(size), __builtin_return_address(0));
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr)
            throw std::bad_alloc();
        handler();
    }
}

// nothrow forms must just return nullptr: callers like std::stable_sort (get_temporary_buffer) handle
// failure by degrading gracefully. libstdc++'s default nothrow new calls operator new above, whose
// new_handler would turn a recoverable failure into a fatal crash.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    if (size == 0)
        size = 1;
    void* p = std::malloc(size);
    if (p == nullptr)
    {
        P3DS_LOGW("operator new(%u bytes, nothrow) failed (caller copes), caller %p", unsigned(size),
            __builtin_return_address(0));
        return nullptr;
    }
    if (!tRegistered)
        registerThreadCounter();
    accountThread(int64_t(malloc_usable_size(p)));
    if (gCallerTid != 0)
        recordAlloc(__builtin_return_address(0), p, size);
    return p;
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return operator new(size, tag);
}

void operator delete(void* p, const std::nothrow_t&) noexcept
{
    operator delete(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept
{
    operator delete(p);
}

void operator delete(void* p) noexcept
{
    if (p != nullptr)
    {
        accountThread(-int64_t(malloc_usable_size(p)));
        if (gLive != nullptr)
            recordFree(p);
    }
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    operator delete(p);
}


// Linked with -Wl,--wrap=abort (see platform3ds/CMakeLists.txt).
extern "C" [[noreturn]] void __wrap_abort()
{
    p3ds::crash::fatal("abort() called");
}
