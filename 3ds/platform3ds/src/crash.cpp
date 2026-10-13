#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/textfb.hpp>

#include <3ds.h>

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <typeinfo>

#include <cxxabi.h>

extern "C"
{
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
            put("=== OpenMW 3DS crash report ===\n");
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
            while (gWatchdogRun)
            {
                svcSleepThread(500LL * 1000 * 1000);
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

// Linked with -Wl,--wrap=abort (see platform3ds/CMakeLists.txt).
extern "C" [[noreturn]] void __wrap_abort()
{
    p3ds::crash::fatal("abort() called");
}
