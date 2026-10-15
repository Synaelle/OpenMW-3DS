#ifndef OPENMW_APPS_OPENMW_N3DS_HUD3DS_H
#define OPENMW_APPS_OPENMW_N3DS_HUD3DS_H

// Nintendo 3DS HUD and bottom-screen tabs (M8, docs/M8_UI_IDEAS.md): what the bottom-screen HUD adds to OpenMW's
// (packaging/ui3ds/mygui/openmw_hud.layout), the tab bar (Inventory, Magic, Stats: pages over the HUD panel that
// don't pause the game), and where things sit on MyGUI's two-screen canvas (components/render3ds/render3ds.hpp: the
// bottom screen is at (40, 240)).
//
// Pages: Inventory is the 3DS's own (openmw_3ds_items.layout: touch an item, then Equip/Use, Drop, Quick key);
// Magic and Stats are OpenMW's windows with 3DS layouts, shown under the tab bar.

#include <string>
#include <string_view>

#include <MyGUI_Types.h>

namespace omw3ds
{
    // OpenMW's default font at book size (components/fontloader): about 10 pixels high instead of 16.
    inline constexpr std::string_view kSmallFont3DS = "Journalbook DefaultFont";

    // The crosshair's tooltip (mwgui/tooltips.cpp) is a one-line label at the very top of the top screen, in
    // kSmallFont3DS, instead of OpenMW's full tooltip in the middle of the screen: where it goes, and its text from
    // the tooltip's caption and lines ("Wooden Door to Seyda Neen", "Iron Dagger, Weight: 10, Value: 5").
    MyGUI::IntPoint targetLabel3DS(const MyGUI::IntSize& size);
    std::string targetLabelText3DS(const std::string& caption, const std::string& text);

    // Once per frame while MyGUI is drawn (frame3ds.cpp): the quick-key strip mirrors OpenMW's quick keys 1-9; the
    // tab bar and its pages show in game mode while the GUI draws the bottom screen.
    void updateHud3DS();

    // The inventory key (B) and anything else opening the inventory: the Inventory tab instead of OpenMW's inventory
    // menu (which pauses the game). False if there are no tabs (OpenMW's own layouts).
    bool toggleInventoryTab3DS();
}

#endif
