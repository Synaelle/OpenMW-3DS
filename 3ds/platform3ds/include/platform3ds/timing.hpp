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

    inline double secondsSinceBoot()
    {
        return double(ticks()) / double(SYSCLOCK_ARM11);
    }

    class ScopedTimer
    {
    public:
        explicit ScopedTimer(double& outMs)
            : mOut(outMs)
            , mStart(ticks())
        {
        }
        ~ScopedTimer() { mOut = ticksToMs(ticks() - mStart); }

    private:
        double& mOut;
        uint64_t mStart;
    };
}
