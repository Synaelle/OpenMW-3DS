#pragma once

#include <cstdint>

// Coarse heap profiler: attributes heap growth to a 32-bit key (e.g. an ESM record type FourCC).
//   { p3ds::memprobe::Scope s(key); ...allocations... }   // accumulates heap delta under key
//   p3ds::memprobe::report("label");                       // logs the table, largest first, and resets
// Cost: two mallinfo() calls per scope, fine for a diagnostic pass over ~100k records.
namespace p3ds::memprobe
{
    void add(uint32_t key, int64_t bytes);
    uint32_t heapUsed();
    void report(const char* label);

    // Logs every thread's net operator-new balance (the watchdog calls this every 5 s).
    void logThreads();

    // Logs this thread's cumulative net allocations and the heap at a named point (startup profiling).
    void stage(const char* label);

    // Caller profile for the calling thread: bytes allocated per operator-new call site between
    // start and stop (logs the top call sites; resolve with addr2line).
    void callerProfileStart();
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
        {
        }
        ~Scope() { add(mKey, int64_t(heapUsed()) - int64_t(mStart)); }

    private:
        uint32_t mKey;
        uint32_t mStart;
    };
}
