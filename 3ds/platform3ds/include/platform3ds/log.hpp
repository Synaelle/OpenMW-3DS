#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Leveled logger: lines go to an in-RAM ring (for the bottom screen and crash reports) and to a
// pending buffer that a low-priority thread flushes to the SD card. Synchronous SD writes per line
// are far too slow (the Vita port measured 13-22 ms each), so file output is batched.
namespace p3ds::log
{
    // Same numbering as OpenMW's Debug::Level so the two can be bridged directly.
    enum class Level : uint8_t
    {
        Error = 1,
        Warning = 2,
        Info = 3,
        Verbose = 4,
        Debug = 5,
    };

    constexpr size_t kLineLen = 128;
    constexpr size_t kRingLines = 128;

    // Opens the log file (truncating it) and starts the flush thread. Safe to call once.
    bool init(const char* path, Level fileLevel = Level::Debug);
    void shutdown();

    void setLevel(Level fileLevel);

    // Sync mode: every line is written and flushed to the SD card before write() returns.
    // Slow (several ms per line on hardware) but survives a hard freeze. Use for hardware tests.
    void setSyncMode(bool sync);

    void write(Level level, std::string_view msg);
    void writef(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

    // Forces pending text to the file (blocking).
    void flush();

    // Incremented on every logged line; lets consumers redraw only when something changed.
    uint32_t generation();

    struct Line
    {
        Level level;
        char text[kLineLen];
    };

    // Copies up to maxLines most recent lines, oldest first. Returns the count copied.
    size_t copyLastLines(Line* out, size_t maxLines);

    // Crash-path access: no locking, no allocation. Calls fn for up to maxLines most recent lines.
    void forEachLineUnsafe(size_t maxLines, void (*fn)(const Line&, void*), void* ctx);

    char levelChar(Level level);
}

#define P3DS_LOGE(...) ::p3ds::log::writef(::p3ds::log::Level::Error, __VA_ARGS__)
#define P3DS_LOGW(...) ::p3ds::log::writef(::p3ds::log::Level::Warning, __VA_ARGS__)
#define P3DS_LOGI(...) ::p3ds::log::writef(::p3ds::log::Level::Info, __VA_ARGS__)
#define P3DS_LOGV(...) ::p3ds::log::writef(::p3ds::log::Level::Verbose, __VA_ARGS__)
#define P3DS_LOGD(...) ::p3ds::log::writef(::p3ds::log::Level::Debug, __VA_ARGS__)
