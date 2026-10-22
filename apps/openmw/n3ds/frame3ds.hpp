#ifndef OPENMW_APPS_OPENMW_N3DS_FRAME3DS_H
#define OPENMW_APPS_OPENMW_N3DS_FRAME3DS_H

// Per-frame 3DS hooks for the engine loop: bottom-screen display and frame-time log (debug tools in
// debugtools.cpp).

#include <functional>
#include <string>
#include <string>
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
    // Runs `call` at the start of the next engine frame, outside any drawing (the system keyboard, which is
    // an applet, is opened this way from a MyGUI dialog).
    void deferToFrame(std::function<void()> call);
    // The system keyboard (libctru's swkbd applet) over `text`: true when the player confirmed.
    bool systemKeyboard(const std::string& hint, std::string& text, size_t maxLength);

    // While loading (loading screen draws, the data-loading wait loop): services the HOME button and
    // keeps the bottom screen updating. A close request while loading exits immediately.
    void onLoadingTick();

    void shutdownBottomScreen();

    // Closes as if from the HOME Menu (autorun "close:N").
    void closeAsFromHomeMenu();

    // Leaves the process (main3ds.cpp): logs, bottom screen, renderer, platform, then _exit. Engine::go() calls it
    // once OpenMW has saved what it keeps (settings, Lua storage, input bindings), skipping the engine's teardown:
    // ~16 s of freeing the world on hardware after the HOME Menu's Close (test #25). The system reclaims the memory.
    [[noreturn]] void exitProcess(int code);

    // The player's screenshot (test #107: the Home tab's camera, shown with the start menu's experimental option,
    // screenshotButtonEnabled3DS): both screens in one BMP, sdmc:/openmw/screenshots/<date>_<time>.bmp, written in
    // the background; a message says when it is saved.
    void requestPlayerScreenshot3DS();
    bool screenshotButtonEnabled3DS(); // sdmc:/openmw/config/screenshot-button exists (read once)
}

#endif
