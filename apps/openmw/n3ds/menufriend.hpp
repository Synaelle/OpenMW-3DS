#ifndef OPENMW_APPS_OPENMW_N3DS_MENUFRIEND_H
#define OPENMW_APPS_OPENMW_N3DS_MENUFRIEND_H

// 3DS: the MWGui windows the bottom-screen menus read and drive (n3ds/menus.cpp) declare OMW3DS_MENU_FRIEND in
// their class, until M8 draws OpenMW's own UI; the ones M8's HUD reads (n3ds/hud3ds.cpp) OMW3DS_HUD_FRIEND.
// Both expand to nothing elsewhere.
#ifdef OPENMW_PLATFORM_3DS
namespace omw3ds
{
    struct MenuBridge;
    struct Hud3DS;
}
#define OMW3DS_MENU_FRIEND friend struct ::omw3ds::MenuBridge;
#define OMW3DS_HUD_FRIEND friend struct ::omw3ds::Hud3DS;
#else
#define OMW3DS_MENU_FRIEND
#define OMW3DS_HUD_FRIEND
#endif

#endif
