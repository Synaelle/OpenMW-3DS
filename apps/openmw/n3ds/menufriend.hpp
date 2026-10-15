#ifndef OPENMW_APPS_OPENMW_N3DS_MENUFRIEND_H
#define OPENMW_APPS_OPENMW_N3DS_MENUFRIEND_H

// 3DS: the MWGui windows the bottom-screen menus read and drive (n3ds/menus.cpp) declare OMW3DS_MENU_FRIEND in
// their class, until M8 draws OpenMW's own UI. Expands to nothing elsewhere.
#ifdef OPENMW_PLATFORM_3DS
namespace omw3ds
{
    struct MenuBridge;
}
#define OMW3DS_MENU_FRIEND friend struct ::omw3ds::MenuBridge;
#else
#define OMW3DS_MENU_FRIEND
#endif

#endif
