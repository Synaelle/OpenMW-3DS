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

    // The per-thread net balance (heapUsed, logThreads, stage's "thread net") costs a size lookup on every
    // new and delete: off unless sdmc:/openmw/memprofile exists (platform init turns it on).
    void enableThreadAccounting();
    bool threadAccounting();

    // Logs this thread's cumulative net allocations and the heap at a named point (startup profiling).
    void stage(const char* label);

    // Caller profile: bytes allocated per operator-new call site between start and stop, for the
    // calling thread or every thread (logs the top call sites, resolve with addr2line). An active
    // profile is also dumped automatically on out-of-memory.
    void callerProfileStart(bool allThreads = false);
    bool callerProfileActive();
    void callerProfileStop(const char* label);
    // The same, ranked by number of allocations (per-frame churn: what allocates and frees every frame).
    void callerProfileStopByCount(const char* label);

    // Extra memory consumers the platform layer can't see (e.g. the Lua VM, which allocates with
    // realloc): reporters return bytes in use. The watchdog logs them with the heap stats.
    using Reporter = uint64_t (*)(void* ctx);
    void addReporter(const char* name, Reporter fn, void* ctx);
    void logReporters();

    class Scope;
}

// Frame phase timing and detail captures are debug tools (OPENMW3DS_DEBUG, platform3ds/CMakeLists.txt): without
// it they compile to nothing.
#ifdef OPENMW3DS_DEBUG
// Frame phase timing: time spent in named parts of the frame, summed (thread-safe, any thread) and
// logged per frame as a "perf/more" line by the game's frame hook. Label: a string literal (keyed by
// pointer). Cost: two system tick reads and an uncontended lock per phase.
namespace p3ds::perf
{
    void add(const char* label, uint64_t ticks);
    // Logs "perf/more: label ms ..." averaged over `frames`, and resets.
    void logAndReset(unsigned frames);
    // Once per frame: the phases' time in the frame just ended, appended to `out` as " label ms" (those of at least
    // 2 ms, largest first) when `report` (a spike: frame3ds.cpp), then cleared either way.
    void endFrame(bool report, char* out, unsigned size);

    class Phase
    {
    public:
        explicit Phase(const char* label)
            : mLabel(label)
            , mStart(memprobe::ticks())
        {
        }
        ~Phase() { add(mLabel, memprobe::ticks() - mStart); }
        Phase(const Phase&) = delete;
        Phase& operator=(const Phase&) = delete;

    private:
        const char* mLabel;
        uint64_t mStart;
    };
}

// Detail capture: while a capture is open on a thread (e.g. around one object insertion), Detail scopes on
// that thread add their time and count to a small table. end() formats it, largest first. Scopes nest (a
// parent's time includes its children's). Label: a string literal (keyed by pointer). Cost when no capture is
// open: one thread-local read.
namespace p3ds::perf::detail
{
    void begin();
    // Closes the capture. Writes " label ms/count ..." (entries of at least 1 ms) to out.
    void end(char* out, unsigned size);
    bool active();
    void add(const char* label, uint64_t ticks);
    // While set (this thread), resource managers log each cache miss by name (OMW3DS_LOG_MISS).
    void setLogMisses(bool enabled);
    bool logMisses();

    class Scope
    {
    public:
        explicit Scope(const char* label)
            : mLabel(active() ? label : nullptr)
            , mStart(mLabel ? memprobe::ticks() : 0)
        {
        }
        ~Scope()
        {
            if (mLabel)
                add(mLabel, memprobe::ticks() - mStart);
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        const char* mLabel;
        uint64_t mStart;
    };
}
#else
namespace p3ds::perf
{
    inline void add(const char*, uint64_t) {}
    inline void logAndReset(unsigned) {}
    inline void endFrame(bool, char* out, unsigned size)
    {
        if (size > 0)
            out[0] = '\0';
    }

    class Phase
    {
    public:
        explicit Phase(const char*) {}
    };
}

namespace p3ds::perf::detail
{
    inline void begin() {}
    inline void end(char* out, unsigned size)
    {
        if (size > 0)
            out[0] = '\0';
    }
    inline bool active()
    {
        return false;
    }
    inline void add(const char*, uint64_t) {}
    inline void setLogMisses(bool) {}
    inline bool logMisses()
    {
        return false;
    }

    class Scope
    {
    public:
        explicit Scope(const char*) {}
    };
}
#endif

namespace p3ds::memprobe
{
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
