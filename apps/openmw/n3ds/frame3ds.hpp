#ifndef OPENMW_APPS_OPENMW_N3DS_FRAME3DS_H
#define OPENMW_APPS_OPENMW_N3DS_FRAME3DS_H

// Per-frame 3DS hooks for the engine loop: bottom-screen debug display and frame-time log.
namespace osgViewer
{
    class Viewer;
}

namespace omw3ds
{
    // Before OpenMW starts (main3ds.cpp).
    void initBottomScreen(const char* status);

    // Engine::go(), once the viewer exists: enables OpenMW's per-subsystem frame timings (input, Lua,
    // mechanics, physics, ...) and OSG's update/cull/draw timings; logged every 5 s with the perf line.
    void setViewer(osgViewer::Viewer* viewer);

    // Once per Engine::frame(), main thread. Measures wall time between calls itself (OpenMW's frame
    // time is clamped for the simulation).
    void onEngineFrame();

    // While loading (loading screen draws, the data-loading wait loop): services the HOME button and
    // keeps the bottom screen updating. A close request while loading exits immediately.
    void onLoadingTick();

    void shutdownBottomScreen();

    // Time spent in parts of the frame OpenMW's own profiling doesn't cover (Lua GC, rendering traversal
    // overhead, ...); averaged per frame and logged as "perf/more" every 5 s. Label: string literal.
    unsigned long long phaseTicks();
    void addPhaseTime(const char* label, unsigned long long ticks);

    struct PhaseTimer
    {
        explicit PhaseTimer(const char* label)
            : mLabel(label)
            , mStart(phaseTicks())
        {
        }
        ~PhaseTimer() { addPhaseTime(mLabel, phaseTicks() - mStart); }
        PhaseTimer(const PhaseTimer&) = delete;
        PhaseTimer& operator=(const PhaseTimer&) = delete;

        const char* mLabel;
        unsigned long long mStart;
    };
}

#endif
