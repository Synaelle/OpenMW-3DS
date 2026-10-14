#ifndef OPENMW_APPS_OPENMW_N3DS_FRAME3DS_H
#define OPENMW_APPS_OPENMW_N3DS_FRAME3DS_H

// Per-frame 3DS hooks for the engine loop: bottom-screen debug display and frame-time log.
namespace omw3ds
{
    // Before OpenMW starts (main3ds.cpp).
    void initBottomScreen(const char* status);

    // Once per Engine::frame(), main thread. Measures wall time between calls itself (OpenMW's frame
    // time is clamped for the simulation).
    void onEngineFrame();

    void shutdownBottomScreen();
}

#endif
