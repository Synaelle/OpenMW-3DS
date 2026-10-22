#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_HUD_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_HUD_H

#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct HUDCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<bool> mCrosshair{ mIndex, "HUD", "crosshair" };
        SettingValue<bool> mMouseEmulationCursor{ mIndex, "HUD", "mouse emulation cursor" };
#ifdef OPENMW_PLATFORM_3DS
        // Small health, magicka and fatigue bars without values at the top screen's bottom, as in Skyrim
        // (Options > Preferences, the tester 19/10). The bottom screen's bars hide meanwhile (test #78).
        SettingValue<bool> mTopBars3DS{ mIndex, "HUD", "top bars" };
#endif
    };
}

#endif
