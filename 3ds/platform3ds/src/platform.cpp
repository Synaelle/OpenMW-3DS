#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/textfb.hpp>

#include <3ds.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

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

    void __system_allocateHeaps(void)
    {
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

namespace p3ds
{
    namespace
    {
        PlatformInfo gInfo;
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
        mkdir(kBaseDir, 0777);
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

        const mem::Stats ms = mem::query();
        P3DS_LOGI("platform3ds: %s, %s (title %016llX), speedup %s, syscore limit %d%%, autorun %s",
            isNew ? "New 3DS" : "Old 3DS", gInfo.launchedAsCia ? "cia" : "3dsx", (unsigned long long)gInfo.programId,
            gInfo.speedupEnabled ? "on" : "off", gInfo.syscoreLimit, gInfo.autorun ? "yes" : "no");
        P3DS_LOGI("memory: region %.1f MB (expected %d MB), image %.2f MB, heap %.1f MB, linear %.1f MB, vram free %.2f MB",
            mem::toMB(ms.regionTotal), int(mem::expectedRegion() >> 20), mem::toMB(ms.imageSize), mem::toMB(ms.heapTotal),
            mem::toMB(ms.linearTotal), mem::toMB(ms.vramFree));
        if (mem::overProvision(ms) > 0)
            P3DS_LOGW("memory region is %.1f MB larger than hardware: enforcing the hardware budget (%.1f MB heap)",
                mem::toMB(mem::overProvision(ms)), mem::toMB(mem::heapBudget(ms)));
        return gInfo;
    }

    void shutdown()
    {
        P3DS_LOGI("platform3ds: shutdown");
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
