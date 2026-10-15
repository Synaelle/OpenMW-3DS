#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/textfb.hpp>

#include <3ds.h>

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <reent.h>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>

// Replaces libctru's weak __system_allocateHeaps (it lives here because this object is always linked).
// libctru's default split gives the app heap everything above 32 MB of linear memory, which in the
// New 3DS 178 MB mode is ~145 MB: more than the 128 MB heap virtual window (0x08000000-0x10000000),
// so svcControlMemory fails and the app panics before main(). We also want more linear (GPU) memory
// than libctru's 32 MB cap for textures and vertex buffers in the big modes.
extern "C"
{
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    extern u32 __ctru_heap;
    extern u32 __ctru_linear_heap;
    extern u32 __ctru_heap_size;
    extern u32 __ctru_linear_heap_size;

    // System tick when the process ran its first instruction of ours (libctru calls this before anything else).
    u64 p3ds_firstCodeTick = 0;

    void __system_allocateHeaps(void)
    {
        p3ds_firstCodeTick = svcGetSystemTick();
        constexpr u32 kMB = 1024 * 1024;
        constexpr u32 kHeapWindow = OS_HEAP_AREA_END - OS_HEAP_AREA_BEGIN - kMB; // keep a guard MB
        constexpr u32 kLinearMin = 24 * kMB;
        constexpr u32 kLinearMax = 48 * kMB;

        Handle reslimit = 0;
        if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE)))
            svcBreak(USERBREAK_PANIC);
        s64 maxCommit = 0, currentCommit = 0;
        ResourceLimitType type = RESLIMIT_COMMIT;
        svcGetResourceLimitLimitValues(&maxCommit, reslimit, &type, 1);
        svcGetResourceLimitCurrentValues(&currentCommit, reslimit, &type, 1);
        svcCloseHandle(reslimit);
        const u32 remaining = u32(maxCommit - currentCommit) & ~0xFFFu;

        // Linear: a third of what is left, within [24, 48] MB. Heap: the rest, capped by its window.
        u32 linear = (remaining / 3) & ~0xFFFu;
        if (linear < kLinearMin)
            linear = kLinearMin;
        if (linear > kLinearMax)
            linear = kLinearMax;
        if (linear > remaining / 2)
            linear = (remaining / 2) & ~0xFFFu;
        u32 heap = remaining - linear;
        if (heap > kHeapWindow)
        {
            heap = kHeapWindow;
            linear = remaining - heap;
        }
        __ctru_heap_size = heap;
        __ctru_linear_heap_size = linear;

        if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size, MEMOP_ALLOC,
                MemPerm(MEMPERM_READ | MEMPERM_WRITE))))
            svcBreak(USERBREAK_PANIC);
        if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size, MEMOP_ALLOC_LINEAR,
                MemPerm(MEMPERM_READ | MEMPERM_WRITE))))
            svcBreak(USERBREAK_PANIC);

        mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);

        fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
        fake_heap_end = fake_heap_start + __ctru_heap_size;
    }
}

// libctru carves the main thread's stack out of the start of the app heap (initSystem in
// stack_adjust.s: [heap start, heap start + __stacksize__)), 32 KB by default. Bullet's GJK/EPA solver
// alone needs ~30 KB in double precision and OSG/NIF traversals recurse deeply; desktop OpenMW
// assumes 1-8 MB. Below the heap start nothing is mapped, so an overflow faults (on hardware).
extern "C" u32 __stacksize__ = 1024 * 1024;

// pthread_create / std::thread end up in libctru's __syscall_thread_create, which creates the thread on
// core 0 at priority 0x3F (the lowest) with a 32 KB stack when none is given. On core 0 such a thread
// only runs while the main thread waits (no time slicing), so OpenMW's async loading and work queue
// crawled on hardware. Put them on core 2 instead: on the New 3DS it runs at full speed and nothing
// else uses it (measured in M0). 256 KB stacks: they load NIFs and traverse scene graphs.
// Linked with -Wl,--wrap=__syscall_thread_create.
extern "C"
{
    int __real___syscall_thread_create(void** thread, void* (*func)(void*), void* arg, void* stack_addr, size_t stack_size);

    int __wrap___syscall_thread_create(void** thread, void* (*func)(void*), void* arg, void* stack_addr, size_t stack_size)
    {
        if (stack_size == 0)
            stack_size = 256 * 1024;
        if (stack_addr == nullptr)
        {
            // Same as libctru's implementation (the pthread handle is the libctru Thread), other core.
            if (Thread t = threadCreate(reinterpret_cast<ThreadFunc>(func), arg, stack_size, 0x30, 2, false))
            {
                *thread = t;
                P3DS_LOGI("thread: std::thread on core 2, prio 0x30, %u KB stack", unsigned(stack_size / 1024));
                return 0;
            }
            P3DS_LOGW("thread: core 2 unavailable, std::thread falls back to core 0 at prio 0x3F");
        }
        return __real___syscall_thread_create(thread, func, arg, stack_addr, stack_size);
    }
}

// ---------------------------------------------------------------------------------------------
// pthread keys (thread-specific data). newlib's pthread_key_create/getspecific call these hooks, which libctru
// doesn't provide (weak, null): pthread_key_create failed, and libstdc++ then kept ONE exception state
// (eh_globals.cc) for every thread. Two threads handling exceptions at the same time got each other's exceptions
// and freed them twice (hardware test #25: the sound thread and OpenMW's sound warm-up thread crashed on a
// garbage what()). Backed by __thread slots, which libctru sets up for every thread. Key destructors are not run
// at thread exit (a few bytes per finished thread).
namespace
{
    constexpr uint32_t kTlsKeys = 64;
    std::atomic<bool> gTlsKeyUsed[kTlsKeys];
    __thread const void* tTlsValues[kTlsKeys];
}

extern "C"
{
    int __syscall_tls_create(uint32_t* key, void (* /*destructor*/)(void*))
    {
        for (uint32_t i = 0; i < kTlsKeys; ++i)
        {
            bool expected = false;
            if (gTlsKeyUsed[i].compare_exchange_strong(expected, true))
            {
                *key = i;
                return 0;
            }
        }
        return EAGAIN;
    }

    int __syscall_tls_set(uint32_t key, const void* value)
    {
        if (key >= kTlsKeys)
            return EINVAL;
        tTlsValues[key] = value;
        return 0;
    }

    void* __syscall_tls_get(uint32_t key)
    {
        return key < kTlsKeys ? const_cast<void*>(tTlsValues[key]) : nullptr;
    }

    int __syscall_tls_delete(uint32_t key)
    {
        if (key >= kTlsKeys)
            return EINVAL;
        gTlsKeyUsed[key] = false;
        return 0;
    }
}

namespace
{
    // Startup check of the hooks above: a pthread key can be made, and (debug builds) two threads throwing and
    // catching at the same time each get their own exception back.
    void checkThreadExceptions()
    {
        pthread_key_t key;
        const int rc = pthread_key_create(&key, nullptr);
        if (rc == 0)
            pthread_key_delete(key);
#ifdef OPENMW3DS_DEBUG
        std::atomic<int> mismatches{ 0 };
        auto worker = [&mismatches](const char* tag) {
            for (int i = 0; i < 200; ++i)
            {
                try
                {
                    throw std::runtime_error(tag);
                }
                catch (const std::exception& e)
                {
                    // Hold it while the other thread throws and catches: with one shared exception state, the
                    // other thread's end of catch releases this exception.
                    svcSleepThread(50 * 1000);
                    if (std::strcmp(e.what(), tag) != 0)
                        ++mismatches;
                }
            }
        };
        std::thread other(worker, "other thread");
        worker("main thread");
        other.join();
        P3DS_LOGI("threads: pthread keys %s, exceptions on two threads at once %s (%d mismatches in 400)",
            rc == 0 ? "ok" : "FAILED", mismatches == 0 ? "ok" : "BROKEN", mismatches.load());
#else
        if (rc != 0)
            P3DS_LOGE("threads: pthread keys unavailable (%d): exceptions on two threads at once are unsafe", rc);
#endif
    }
}

// ---------------------------------------------------------------------------------------------
// Two-segment sbrk (replaces libsysbase's weak _sbrk_r).
//
// The libctru app heap is capped by its virtual window (~95 MB even in the 178 MB mode), while the
// linear (GPU-visible) heap gets the rest of the memory and is mostly idle until textures and vertex
// buffers exist. When the app heap is exhausted, malloc continues in a block carved from the linear
// heap, leaving kLinearGpuReserve for citro3d. newlib's malloc accepts sbrk jumping to a higher,
// non-contiguous address (it fences off the gap), and never mixes segments afterwards.
extern "C"
{
    size_t p3ds_linear_gpu_reserve = 24u * 1024u * 1024u; // keep this much linear memory for the GPU

    static char* gSbrkCur = nullptr; // current break
    static char* gSbrkEnd = nullptr; // end of the current segment
    static char* gExtStart = nullptr; // linear extension segment (once used)
    static size_t gExtSize = 0;
    // Diagnostics (read by the watchdog; sbrk runs under the malloc lock and must not log).
    volatile uint32_t p3ds_sbrk_failures = 0;
    volatile int32_t p3ds_sbrk_last_fail_incr = 0;
    volatile uint32_t p3ds_sbrk_last_fail_linear_free = 0;

    void* _sbrk_r(struct _reent* r, ptrdiff_t incr)
    {
        if (gSbrkCur == nullptr)
        {
            gSbrkCur = fake_heap_start;
            gSbrkEnd = fake_heap_end;
        }
        if (incr >= 0 ? (gSbrkCur + incr > gSbrkEnd) : (gSbrkCur + incr < (gExtStart ? gExtStart : fake_heap_start)))
        {
            // Out of the app heap: move once to a linear-heap segment.
            if (incr > 0 && gExtStart == nullptr)
            {
                const size_t avail = linearSpaceFree();
                if (avail > p3ds_linear_gpu_reserve + size_t(incr))
                {
                    gExtSize = (avail - p3ds_linear_gpu_reserve) & ~size_t(0xFFFF);
                    gExtStart = static_cast<char*>(linearMemAlign(gExtSize, 0x1000));
                }
                if (gExtStart != nullptr && gExtStart > gSbrkEnd)
                {
                    gSbrkCur = gExtStart;
                    gSbrkEnd = gExtStart + gExtSize;
                    char* prev = gSbrkCur;
                    gSbrkCur += incr;
                    return prev;
                }
                if (gExtStart != nullptr)
                {
                    linearFree(gExtStart); // below the app heap: newlib can't use it
                    gExtStart = nullptr;
                    gExtSize = 0;
                }
            }
            ++p3ds_sbrk_failures;
            p3ds_sbrk_last_fail_incr = int32_t(incr);
            p3ds_sbrk_last_fail_linear_free = uint32_t(linearSpaceFree());
            r->_errno = ENOMEM;
            return reinterpret_cast<void*>(-1);
        }
        char* prev = gSbrkCur;
        gSbrkCur += incr;
        return prev;
    }

    // malloc's arena without mallinfo's walk: what sbrk handed out, not counting the gap to the extension.
    size_t p3ds_heap_arena()
    {
        char* const cur = gSbrkCur;
        if (cur == nullptr)
            return 0;
        if (gExtStart != nullptr && cur >= gExtStart)
            return size_t(fake_heap_end - fake_heap_start) + size_t(cur - gExtStart);
        return size_t(cur - fake_heap_start);
    }

    size_t p3ds_heap_extension_size()
    {
        return gExtSize;
    }

    // Address gap newlib's malloc counts as allocated once it jumped to the extension segment.
    size_t p3ds_heap_extension_gap()
    {
        return gExtStart ? size_t(gExtStart - fake_heap_end) : 0;
    }
}

namespace p3ds
{
    namespace
    {
        PlatformInfo gInfo;

        // Main stack watermark: the unused part is filled with a pattern at init.
        constexpr u32 kStackPattern = 0x5AC3A55Cu;
        const u32* gStackBottom = nullptr;
        u32 gStackSize = 0;

        __attribute__((noinline)) void paintMainStack()
        {
            gStackBottom = reinterpret_cast<const u32*>(__ctru_heap);
            gStackSize = __stacksize__;
            // Paint up to 4 KB below the current frame (everything deeper is unused so far).
            u32* const limit = reinterpret_cast<u32*>(__builtin_frame_address(0)) - 1024;
            for (u32* p = const_cast<u32*>(gStackBottom); p < limit; ++p)
                *p = kStackPattern;
        }
    }

    bool mainStackUsage(unsigned& peakBytes, unsigned& sizeBytes)
    {
        if (gStackBottom == nullptr)
            return false;
        const u32* p = gStackBottom;
        const u32* const end = gStackBottom + gStackSize / 4;
        while (p < end && *p == kStackPattern)
            ++p;
        sizeBytes = gStackSize;
        peakBytes = unsigned(reinterpret_cast<const char*>(end) - reinterpret_cast<const char*>(p));
        return mainStackIntact();
    }

    bool mainStackIntact()
    {
        // Overflowed (or about to): the bottom words are gone.
        return gStackBottom == nullptr || (gStackBottom[0] == kStackPattern && gStackBottom[1] == kStackPattern);
    }

    bool fileExists(const char* path)
    {
        struct stat st;
        return stat(path, &st) == 0;
    }

    bool readSmallFile(const char* path, char* buf, unsigned bufSize)
    {
        FILE* f = fopen(path, "rb");
        if (f == nullptr)
            return false;
        const size_t n = fread(buf, 1, bufSize - 1, f);
        buf[n] = '\0';
        fclose(f);
        return true;
    }

    void writeSmallFile(const char* path, const char* text)
    {
        FILE* f = fopen(path, "wb");
        if (f == nullptr)
            return;
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }

    PlatformInfo init(const InitOptions& options)
    {
        const u64 mainTick = svcGetSystemTick();
        paintMainStack();
        mkdir(kBaseDir, 0777);
        // The package ships the folder: creating one costs ~1.5 s on hardware, a stat ~5 ms.
        struct stat logDir;
        if (stat(kLogDir, &logDir) != 0)
            mkdir(kLogDir, 0777);
        remove(kAutorunDonePath);

        textfb::setupBottomScreen();

        log::init(kLogPath);
        crash::install(kCrashPath);

        bool isNew = false;
        APT_CheckNew3DS(&isNew);
        gInfo.isNew3DS = isNew;

        // envIsHomebrew() is false when an emulator boots a .3dsx directly, so identify our own CIA
        // by its title ID instead: 00040000-0<UniqueId>00.
        u64 programId = 0;
        if (R_SUCCEEDED(APT_GetProgramID(&programId)))
            gInfo.programId = programId;
        gInfo.launchedAsCia = options.ciaUniqueId != 0 && (programId >> 32) == 0x00040000ULL
            && ((programId >> 8) & 0xFFFFF) == options.ciaUniqueId;

        if (isNew && options.enableSpeedup)
        {
            osSetSpeedupEnable(true);
            gInfo.speedupEnabled = true;
        }
        if (options.syscoreTimeLimitPercent > 0)
        {
            if (R_SUCCEEDED(APT_SetAppCpuTimeLimit(options.syscoreTimeLimitPercent)))
                gInfo.syscoreLimit = options.syscoreTimeLimitPercent;
        }

        gInfo.autorun = fileExists(kAutorunPath);
        crash::setWaitForInput(!gInfo.autorun);

        const mem::Stats ms = mem::queryExact();
        P3DS_LOGI("build %s", p3ds_build_id);
        // The system tick counts from before the process exists: the time stamps of the first log lines are the
        // launch (HOME Menu tap to our first instruction), then libctru's setup and static initialisation.
        P3DS_LOGI("startup: our first instruction at %.2f s, main() at %.2f s (system tick)",
            double(p3ds_firstCodeTick) / SYSCLOCK_ARM11, double(mainTick) / SYSCLOCK_ARM11);
        P3DS_LOGI("platform3ds: %s, %s (title %016llX), speedup %s, syscore limit %d%%, autorun %s",
            isNew ? "New 3DS" : "Old 3DS", gInfo.launchedAsCia ? "cia" : "3dsx", (unsigned long long)gInfo.programId,
            gInfo.speedupEnabled ? "on" : "off", gInfo.syscoreLimit, gInfo.autorun ? "yes" : "no");
        P3DS_LOGI("memory: region %.1f MB (expected %d MB), image %.2f MB, heap %.1f MB, linear %.1f MB, vram free %.2f MB",
            mem::toMB(ms.regionTotal), int(mem::expectedRegion() >> 20), mem::toMB(ms.imageSize), mem::toMB(ms.heapTotal),
            mem::toMB(ms.linearTotal), mem::toMB(ms.vramFree));
        if (mem::overProvision(ms) > 0)
            P3DS_LOGW("memory region is %.1f MB larger than hardware: enforcing the hardware budget (%.1f MB heap)",
                mem::toMB(mem::overProvision(ms)), mem::toMB(mem::heapBudget(ms)));
        checkThreadExceptions();
        return gInfo;
    }

    void shutdown()
    {
        P3DS_LOGI("platform3ds: shutdown");
        crash::stopWatchdog();
        log::shutdown();
    }

    const PlatformInfo& info()
    {
        return gInfo;
    }

    int preferredWorkerCore()
    {
        if (gInfo.isNew3DS)
            return 2;
        return gInfo.syscoreLimit > 0 ? 1 : 0;
    }
}
