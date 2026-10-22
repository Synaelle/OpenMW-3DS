#ifndef OPENMW_APPS_OPENMW_N3DS_DEBUGTOOLS_H
#define OPENMW_APPS_OPENMW_N3DS_DEBUGTOOLS_H

// 3DS debug tools (debug builds only: OPENMW3DS_DEBUG, platform3ds/CMakeLists.txt, this file and
// debugtools.cpp aren't compiled without it): the autorun script of automated emulator runs and its scripted
// input, and replayable view lines for screenshots.
#ifndef OPENMW3DS_DEBUG
#error "debugtools.hpp is only for builds with the 3DS debug tools (OPENMW3DS_DEBUG)"
#endif

#include <string>

namespace omw3ds::debug
{
    // Once per engine frame, before input is scanned: what sdmc:/openmw/autorun.txt asks for at this frame
    // (quit, doors, screenshots, weather, hour, view, ...).
    void runAutorun(unsigned frame);

    // Each loading tick (frame3ds.cpp onLoadingTick): autorun "loading:N,..." saves both screens at the Nth tick of
    // a load (counted from the last engine frame) as sdmc:/openmw/shot_top_loadN.bmp and shot_bottom_loadN.bmp.
    void onLoadingTickAutorun();

    // Right after hidScanInput(): the autorun script's buttons and sticks for this frame (n3ds/hidinject.cpp).
    void applyScriptedInput(unsigned frame);

    // Where the player is and what the camera looks at, starting with autorun tokens that put the player back
    // there ("view:F,... hour:F,H weather:F,N"): logged with every screenshot.
    std::string describeView(unsigned frame);

    // Autorun "tp:F,N": to a town (at a well-known building's door) or an interior.
    void teleport(int index);
}

#endif
