#pragma once

#include <cstdint>

// Coarse heap profiler: attributes heap growth to a 32-bit key (e.g. an ESM record type FourCC).
//   { p3ds::memprobe::Scope s(key); ...allocations... }   // accumulates heap delta under key
//   p3ds::memprobe::report("label");                       // logs the table, largest first, and resets
// Also accumulates the time spent in each key (report shows ms). Cost: a few counter reads per scope.
namespace p3ds::memprobe
{
    void add(uint32_t key, int64_t bytes, uint64_t elapsedTicks = 0);
    uint32_t heapUsed();
    uint64_t ticks(); // system ticks (SYSCLOCK_ARM11 Hz)
    void report(const char* label);

    // Logs every thread's net operator-new balance (the watchdog calls this every 5 s).
    void logThreads();

    // Logs this thread's cumulative net allocations and the heap at a named point (startup profiling).
    void stage(const char* label);

    // Caller profile: bytes allocated per operator-new call site between start and stop, for the
    // calling thread or every thread (logs the top call sites; resolve with addr2line). An active
    // profile is also dumped automatically on out-of-memory.
    void callerProfileStart(bool allThreads = false);
    bool callerProfileActive();
    void callerProfileStop(const char* label);

    // Extra memory consumers the platform layer can't see (e.g. the Lua VM, which allocates with
    // realloc): reporters return bytes in use; the watchdog logs them with the heap stats.
    using Reporter = uint64_t (*)(void* ctx);
    void addReporter(const char* name, Reporter fn, void* ctx);
    void logReporters();

    class Scope
    {
    public:
        explicit Scope(uint32_t key)
            : mKey(key)
            , mStart(heapUsed())
            , mStartTick(ticks())
        {
        }
        ~Scope() { add(mKey, int64_t(heapUsed()) - int64_t(mStart), ticks() - mStartTick); }

    private:
        uint32_t mKey;
        uint32_t mStart;
        uint64_t mStartTick;
    };
}
