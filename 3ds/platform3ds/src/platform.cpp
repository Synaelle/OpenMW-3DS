#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/timing.hpp>
#include <platform3ds/textfb.hpp>

#include <3ds.h>

#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <malloc.h>
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
// alone needs ~30 KB in double precision and OSG/NIF traversals recurse deeply. Desktop OpenMW
// assumes 1-8 MB. Below the heap start nothing is mapped, so an overflow faults (on hardware).
extern "C" u32 __stacksize__ = 1024 * 1024;

// pthread_create / std::thread end up in libctru's __syscall_thread_create, which creates the thread on
// core 0 at priority 0x3F (the lowest) with a 32 KB stack when none is given. On core 0 such a thread
// only runs while the main thread waits (no time slicing), so OpenMW's async loading and work queue
// crawled on hardware. Put them on core 2 instead: on the New 3DS it runs at full speed and nothing
// else uses it (measured in M0). 256 KB stacks: they load NIFs and traverse scene graphs.
// Linked with -Wl,--wrap=__syscall_thread_create.
namespace
{
    // M20: every thread gets the crash handler (a crash on a worker thread wrote no report): installed by the
    // thread itself before the function it runs.
    struct ThreadStart
    {
        void* (*mFunc)(void*);
        void* mArg;
    };

    void threadEntry(void* p)
    {
        const ThreadStart start = *static_cast<ThreadStart*>(p);
        delete static_cast<ThreadStart*>(p);
        p3ds::crash::installForCurrentThread();
        start.mFunc(start.mArg);
    }
}

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
            auto* start = new ThreadStart{ func, arg };
            if (Thread t = threadCreate(threadEntry, start, stack_size, 0x30, 2, false))
            {
                *thread = t;
                P3DS_LOGI("thread: std::thread on core 2, prio 0x30, %u KB stack", unsigned(stack_size / 1024));
                return 0;
            }
            delete start;
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
    // Startup check of the hooks above: a pthread key can be made.
    void checkThreadExceptions()
    {
        pthread_key_t key;
        const int rc = pthread_key_create(&key, nullptr);
        if (rc == 0)
            pthread_key_delete(key);
        // The stress run of two threads throwing at once (test #25's shared exception state) went in M20: the TLS
        // hooks above fixed it, and it cost ~10 ms and 400 exceptions per start.
        if (rc == 0)
            P3DS_LOGI("threads: pthread keys ok");
        else
            P3DS_LOGE("threads: pthread keys unavailable (%d): exceptions on two threads at once are unsafe", rc);
    }
}

// ---------------------------------------------------------------------------------------------
// One lock for the linear heap (-Wl,--wrap, CMakeLists.txt).
//
// libctru's linear allocator (linearAlloc & co.: GPU and DSP memory) has no lock, and several threads use it: the
// renderer and the texture pack (main thread, OpenMW's work queue), sound buffers, the caller profile, and sbrk's
// heap extension below (from whichever thread mallocs). Two at once can hand out overlapping blocks: sound data,
// GPU commands, textures and vertices then overwrite each other (hardware test #26: textures and both screens
// corrupted after an exterior load). Every call goes through these wrappers under newlib's malloc lock, which is
// recursive: the allocator allocates its bookkeeping with operator new, and sbrk calls it with that lock held, so a
// separate lock could deadlock.
extern "C"
{
    void* __real_linearAlloc(size_t size);
    void* __real_linearMemAlign(size_t size, size_t alignment);
    void* __real_linearRealloc(void* mem, size_t size);
    size_t __real_linearGetSize(void* mem);
    void __real_linearFree(void* mem);
    u32 __real_linearSpaceFree(void);

    void* __wrap_linearAlloc(size_t size)
    {
        __malloc_lock(_REENT);
        void* mem = __real_linearAlloc(size);
        __malloc_unlock(_REENT);
        return mem;
    }

    void* __wrap_linearMemAlign(size_t size, size_t alignment)
    {
        __malloc_lock(_REENT);
        void* mem = __real_linearMemAlign(size, alignment);
        __malloc_unlock(_REENT);
        return mem;
    }

    void* __wrap_linearRealloc(void* mem, size_t size)
    {
        __malloc_lock(_REENT);
        void* result = __real_linearRealloc(mem, size);
        __malloc_unlock(_REENT);
        return result;
    }

    size_t __wrap_linearGetSize(void* mem)
    {
        __malloc_lock(_REENT);
        const size_t size = __real_linearGetSize(mem);
        __malloc_unlock(_REENT);
        return size;
    }

    void __wrap_linearFree(void* mem)
    {
        __malloc_lock(_REENT);
        __real_linearFree(mem);
        __malloc_unlock(_REENT);
    }

    u32 __wrap_linearSpaceFree(void)
    {
        __malloc_lock(_REENT);
        const u32 free = __real_linearSpaceFree();
        __malloc_unlock(_REENT);
        return free;
    }
}

// ---------------------------------------------------------------------------------------------
// sbrk over the app heap only (replaces libsysbase's weak _sbrk_r).
//
// The libctru app heap is capped by its virtual window (~95 MB even in the 178 MB mode). The linear heap gets
// the rest of the memory. Until M9 malloc continued into a linear-heap segment when the app heap ran out. The
// large-allocation pool takes that memory now (pools.cpp), so newlib's arena stays in the app heap.
extern "C"
{
    size_t p3ds_linear_gpu_reserve = 24u * 1024u * 1024u; // linear memory left to the GPU by the first pool region

    static char* gSbrkCur = nullptr; // current break
    // Diagnostics (read by the watchdog, sbrk runs under the malloc lock and must not log).
    volatile uint32_t p3ds_sbrk_failures = 0;
    volatile int32_t p3ds_sbrk_last_fail_incr = 0;
    volatile uint32_t p3ds_sbrk_last_fail_linear_free = 0;

    void* _sbrk_r(struct _reent* r, ptrdiff_t incr)
    {
        if (gSbrkCur == nullptr)
            gSbrkCur = fake_heap_start;
        if (incr >= 0 ? (gSbrkCur + incr > fake_heap_end) : (gSbrkCur + incr < fake_heap_start))
        {
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

    // newlib's arena without mallinfo's walk: what sbrk handed out.
    size_t p3ds_heap_arena()
    {
        char* const cur = gSbrkCur;
        return cur == nullptr ? 0 : size_t(cur - fake_heap_start);
    }

    // The app heap malloc can use: libctru's main stack sits at its start (fake_heap_start is past it).
    size_t p3ds_heap_window()
    {
        return size_t(fake_heap_end - fake_heap_start);
    }
}

namespace p3ds
{
    namespace
    {
        PlatformInfo gInfo;
        uint32_t gRestoreCount = 0;

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

    void writeSmallFile(const char* path, const char* text)
    {
        FILE* f = fopen(path, "wb");
        if (f == nullptr)
            return;
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }

    // The previous two runs' logs stay: NAME-1.log (the last run), NAME-2.log (the one before). Several runs of a
    // hardware test then survive until the tester copies the folder (each launch used to overwrite them).
    static char gLogDir[96] = "sdmc:/openmw/logs";

    const char* logDir()
    {
        return gLogDir;
    }

    namespace
    {
        bool gDebugMode = true;
    }

    bool debugMode()
    {
        return gDebugMode;
    }

    static void rotateLogs()
    {
        static const char* const kNames[] = { "openmw3ds", "openmw", "MyGUI" };
        for (const char* name : kNames)
        {
            char current[96], older[96], oldest[96];
            snprintf(current, sizeof(current), "%s/%s.log", gLogDir, name);
            snprintf(older, sizeof(older), "%s/%s-1.log", gLogDir, name);
            snprintf(oldest, sizeof(oldest), "%s/%s-2.log", gLogDir, name);
            // FAT can't rename onto an existing file.
            remove(oldest);
            rename(older, oldest);
            rename(current, older);
        }
    }

    PlatformInfo init(const InitOptions& options)
    {
        const u64 mainTick = svcGetSystemTick();
        gDebugMode = options.debugMode;
        startTick(); // the game's time zero (timing.hpp): the log's timestamps and the bottom screen's timer
        paintMainStack();
        mkdir(kBaseDir, 0777);
        // The package ships the folder: creating one costs ~1.5 s on hardware, a stat ~5 ms.
        struct stat logDir;
        if (stat(kLogDir, &logDir) != 0)
            mkdir(kLogDir, 0777);
        if (options.logSubdir != nullptr && options.logSubdir[0] != '\0')
        {
            snprintf(gLogDir, sizeof(gLogDir), "%s/%s", kLogDir, options.logSubdir);
            if (stat(gLogDir, &logDir) != 0)
                mkdir(gLogDir, 0777);
        }
        remove(kAutorunDonePath);
        rotateLogs();
#ifdef OPENMW3DS_DEBUG
        if (stat("sdmc:/openmw/memprofile", &logDir) == 0)
            memprobe::enableThreadAccounting(); // diagnostics (see memprobe.hpp)
#endif

        textfb::setupBottomScreen();

        char logPath[128], crashPath[128];
        snprintf(logPath, sizeof(logPath), "%s/openmw3ds.log", gLogDir);
        snprintf(crashPath, sizeof(crashPath), "%s/openmw3ds-crash.log", gLogDir);
        log::init(logPath);
        crash::install(crashPath);

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
        // HOME Menu / sleep: the app is suspended (the main thread waits in aptMainLoop), which isn't a hang
        // (hardware test #25: 42 s in the HOME Menu, then closing, gave a hang report).
        static aptHookCookie aptCookie;
        aptHook(
            &aptCookie,
            [](APT_HookType hook, void*) {
                static const char* const kNames[] = { "suspend", "restore", "sleep", "wake up", "exit" };
                if (hook >= 0 && hook < APTHOOK_COUNT)
                    P3DS_LOGI("apt: %s", kNames[hook]);
                if (hook == APTHOOK_ONSUSPEND || hook == APTHOOK_ONSLEEP)
                    crash::setWatchdogPaused(true);
                else if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP || hook == APTHOOK_ONEXIT)
                    crash::setWatchdogPaused(false);
                if (hook == APTHOOK_ONRESTORE || hook == APTHOOK_ONWAKEUP)
                    __atomic_add_fetch(&gRestoreCount, 1u, __ATOMIC_RELAXED);
            },
            nullptr);
        return gInfo;
    }

    uint32_t restoreCount()
    {
        return __atomic_load_n(&gRestoreCount, __ATOMIC_RELAXED);
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
}
