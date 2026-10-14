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
}

#endif
