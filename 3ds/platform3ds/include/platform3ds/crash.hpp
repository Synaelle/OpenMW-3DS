#pragma once

// Crash logger built on libctru's per-thread exception handler (threadOnException).
// On a CPU exception, abort(), std::terminate or a final out-of-memory it writes
// <crash path> with registers, a backtrace (frame-pointer walk + stack scan), memory stats and
// the last log lines, then shows a short message on the bottom screen.
// Resolve addresses with scripts/symbolize.ps1 <crash.log> <app.elf>.
namespace p3ds::crash
{
    // Call once from the main thread, after log::init. Installs the handler for the main thread,
    // std::terminate and new_handler (with a small emergency reserve).
    void install(const char* crashLogPath);

    // libctru exception handlers are per-thread: call at the start of every thread you create.
    void installForCurrentThread();

    // When false (automated runs), the crash screen exits after ~2 s instead of waiting for START.
    void setWaitForInput(bool wait);

    // Optional context string included in reports (e.g. current cell). Must point to static storage
    // that stays valid; it is read without locking.
    void setContextString(const char* const* ctx);

    [[noreturn]] void fatal(const char* reason);

    // Hang watchdog: a high-priority thread (core 1 if available) that writes a "hang" report naming
    // the last step passed to kick() and exits to the HOME Menu if kick() isn't called for `seconds`.
    // Turns software hangs into a report instead of a frozen console needing a forced power-off.
    bool startWatchdog(int seconds, int core);
    void setWatchdogTimeout(int seconds); // 0 disables (e.g. while a dialog legitimately blocks)
    // Call regularly from the main loop / long operations. `step` must point to static storage.
    void kick(const char* step = nullptr);
}
