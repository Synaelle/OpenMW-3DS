#ifndef OPENMW_APPS_OPENMW_N3DS_BENCHMARK3DS_H
#define OPENMW_APPS_OPENMW_N3DS_BENCHMARK3DS_H

// The benchmark (debug builds: the start menu's third choice, or "benchmark" in an automated run's autorun.txt):
// a new game, then about 5 minutes through the places that weigh most on the frame rate, each with its weather and
// hour, the camera turning slowly round. Per place: frame times, draws, GPU time, memory and the load, logged and
// written to benchmark-<set>.txt in the run's log folder. Then the game quits. No save, no screenshot.
#ifndef OPENMW3DS_DEBUG
#error "benchmark3ds.hpp is only for builds with the 3DS debug tools (OPENMW3DS_DEBUG)"
#endif

namespace omw3ds::benchmark
{
    // Which places: Morrowind's towns, or the long tour. In the order of benchmark3ds.cpp's sSets. The expansions'
    // sets went with their support in M20 (back with M11, git history).
    enum class Set
    {
        Morrowind,
        // The long tour (the tester, 21/10): ~20 minutes through 32 places of Vvardenfell, towns and interiors,
        // with 3 top-screen screenshots at each. Zeste only (an automated run's "benchmark:tour"): never on the
        // console's menu.
        Tour,
    };
    // Before OpenMW starts (main3ds.cpp). Results go to benchmark-<set>.txt in the run's log folder. firstStage:
    // where to start (an automated run's "benchmark@N", N from 1: profiling one place in Zeste).
    void enable(Set set, int firstStage = 0);
    bool enabled();

    // Once per engine frame (frame3ds.cpp).
    void onFrame();
}

#endif
