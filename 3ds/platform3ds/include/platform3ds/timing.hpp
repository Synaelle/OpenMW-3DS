#pragma once

#include <3ds.h>
#include <cstdint>

namespace p3ds
{
    // The system tick runs at SYSCLOCK_ARM11 (~268 MHz) regardless of the New 3DS CPU speedup.
    inline uint64_t ticks()
    {
        return svcGetSystemTick();
    }

    inline double ticksToMs(uint64_t t)
    {
        return double(t) * 1000.0 / double(SYSCLOCK_ARM11);
    }

    // The tick when the game started (the first call, in the platform's startup): the system tick counts from the
    // console's boot (the tester, 20/10: the bottom screen's timer started at ~350 s).
    inline uint64_t startTick()
    {
        static const uint64_t start = ticks();
        return start;
    }

    // Seconds since the game started (the log's timestamps, the bottom screen's timer).
    inline double secondsSinceBoot()
    {
        return double(ticks() - startTick()) / double(SYSCLOCK_ARM11);
    }
}
