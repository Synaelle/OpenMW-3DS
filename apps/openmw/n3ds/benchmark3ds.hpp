#ifndef OPENMW_APPS_OPENMW_N3DS_BENCHMARK3DS_H
#define OPENMW_APPS_OPENMW_N3DS_BENCHMARK3DS_H

// The benchmark (debug builds: the start menu's third choice, or "benchmark" in an automated run's autorun.txt):
// a new game, then about 5 minutes through the places that weigh most on the frame rate, each with its weather and
// hour, the camera turning slowly round. Per place: frame times, draws, GPU time, memory and the load, logged and
// written to benchmark-<set>.txt in the run's log folder; then the game quits. No save, no screenshot.
#ifndef OPENMW3DS_DEBUG
#error "benchmark3ds.hpp is only for builds with the 3DS debug tools (OPENMW3DS_DEBUG)"
#endif

namespace omw3ds::benchmark
{
    // Which places: Morrowind's, or an expansion's (Mournhold, Solstheim: installed expansions only).
    enum class Set
    {
        Morrowind,
        Tribunal,
        Bloodmoon,
    };
    // Before OpenMW starts (main3ds.cpp). Results go to benchmark-<set>.txt in the run's log folder.
    void enable(Set set);
    bool enabled();

    // Once per engine frame (frame3ds.cpp).
    void onFrame();
}

#endif
