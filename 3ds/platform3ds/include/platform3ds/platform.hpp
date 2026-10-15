#pragma once

#include <cstdint>

extern "C" const char* const p3ds_build_id; // "YYYYMMDD-HHMMSS <git rev>", regenerated every build

namespace p3ds
{
    // Root of all port files on the SD card.
    constexpr const char* kBaseDir = "sdmc:/openmw";
    // Every log in one folder: the port log, crash reports and OpenMW's own openmw.log (main.cpp).
    constexpr const char* kLogDir = "sdmc:/openmw/logs";
    constexpr const char* kLogPath = "sdmc:/openmw/logs/openmw3ds.log";
    constexpr const char* kCrashPath = "sdmc:/openmw/logs/openmw3ds-crash.log";
    constexpr const char* kAutorunPath = "sdmc:/openmw/autorun.txt";
    constexpr const char* kAutorunDonePath = "sdmc:/openmw/autorun.done";

    struct InitOptions
    {
        bool enableSpeedup = true; // New 3DS: 804 MHz + L2 cache
        int syscoreTimeLimitPercent = 30; // core 1 time slice (0 = leave core 1 unused)
        uint32_t ciaUniqueId = 0; // UniqueId from the .rsf; used to tell .cia launches from .3dsx
    };

    struct PlatformInfo
    {
        bool isNew3DS = false;
        bool speedupEnabled = false;
        int syscoreLimit = 0;
        bool autorun = false;
        bool launchedAsCia = false; // false for .3dsx (Homebrew Launcher or emulator direct boot)
        uint64_t programId = 0;
    };

    // Brings up directories, logging, crash handling and CPU configuration. Call right after
    // gfxInit (the crash screen needs the bottom framebuffer).
    PlatformInfo init(const InitOptions& options = {});
    void shutdown();

    const PlatformInfo& info();

    // Main thread stack high-water mark (the stack is painted at init). Returns false once the
    // bottom of the stack has been overwritten, i.e. it overflowed or is about to.
    bool mainStackUsage(unsigned& peakBytes, unsigned& sizeBytes);
    bool mainStackIntact(); // cheap: checks only the bottom words

    bool fileExists(const char* path);
    // Reads a small text file into buf (NUL-terminated). Returns false if missing.
    bool readSmallFile(const char* path, char* buf, unsigned bufSize);
    void writeSmallFile(const char* path, const char* text);

    // Best core for a worker thread: 2 on New 3DS (if the exheader allows it), else 1, else 0.
    int preferredWorkerCore();
}
