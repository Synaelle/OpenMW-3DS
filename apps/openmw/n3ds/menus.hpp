#ifndef OPENMW_APPS_OPENMW_N3DS_MENUS_H
#define OPENMW_APPS_OPENMW_N3DS_MENUS_H

#include "../mwgui/mode.hpp"

namespace p3ds
{
    class DebugBottomScreen;
}

namespace omw3ds
{
    // Bottom-screen text menus for OpenMW's menus (M6), kept as the emergency fallback (sdmc:/openmw/no-m8ui,
    // M20). Each engine frame: applies the option
    // the player touched, then shows the menu the game has open (an interactive message box first, else the top GUI
    // mode), or none. OpenMW's own windows keep all the state and logic. The menus read and drive them
    // (n3ds/menufriend.hpp).
    void refreshMenus(p3ds::DebugBottomScreen& screen);

    // As if the player touched option `index` of the menu shown (autorun "pick:", debug builds).
    void pressMenuOption(int index);

    const char* guiModeName(MWGui::GuiMode mode);
}

#endif
