#ifndef OPENMW_APPS_OPENMW_N3DS_HUD3DS_H
#define OPENMW_APPS_OPENMW_N3DS_HUD3DS_H

// Nintendo 3DS HUD (M8, docs/M8_UI_IDEAS.md): what the bottom-screen HUD adds to OpenMW's
// (packaging/ui3ds/mygui/openmw_hud.layout), and where things sit on MyGUI's two-screen canvas
// (components/render3ds/render3ds.hpp: the bottom screen is at (40, 240)).

#include <MyGUI_Types.h>

namespace omw3ds
{
    // The target panel: the bottom screen's area between the boxes and the quick keys, where the crosshair's
    // tooltip goes (mwgui/tooltips.cpp) instead of the middle of the top screen.
    MyGUI::IntCoord targetPanel3DS();

    // Once per frame while MyGUI is drawn (frame3ds.cpp): the quick-key strip mirrors OpenMW's quick keys 1-9.
    void updateHud3DS();
}

#endif
