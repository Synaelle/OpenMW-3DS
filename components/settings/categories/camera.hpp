#ifndef OPENMW_COMPONENTS_SETTINGS_CATEGORIES_CAMERA_H
#define OPENMW_COMPONENTS_SETTINGS_CATEGORIES_CAMERA_H

#include <components/settings/sanitizerimpl.hpp>
#include <components/settings/settingvalue.hpp>

#include <osg/Math>
#include <osg/Vec2f>
#include <osg/Vec3f>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace Settings
{
    struct CameraCategory : WithIndex
    {
        using WithIndex::WithIndex;

        SettingValue<float> mNearClip{ mIndex, "Camera", "near clip", makeMaxSanitizerFloat(0.005f) };
        SettingValue<bool> mSmallFeatureCulling{ mIndex, "Camera", "small feature culling" };
        SettingValue<float> mSmallFeatureCullingPixelSize{ mIndex, "Camera", "small feature culling pixel size",
            makeMaxStrictSanitizerFloat(0) };
        // 3DS (Options > Visuals, the tester, test #105): how far small details stay drawn. 2 High: OpenMW's PC
        // size (2 pixels); 1 Medium: the 3DS's own size (the small feature culling size above, settings-3ds.cfg);
        // 0 Low: the parts of scenery under 10 pixels left out too (render3ds backend), the most frame rate.
        SettingValue<int> mSmallDetails3DS{ mIndex, "Camera", "small details", makeClampSanitizerInt(0, 2) };
        // The small feature culling size in use: [Camera] small feature culling pixel size, or 2 on High.
        float smallFeaturePixels3DS() const
        {
            return mSmallDetails3DS >= 2 ? std::min(2.f, mSmallFeatureCullingPixelSize.get())
                                         : mSmallFeatureCullingPixelSize.get();
        }
        SettingValue<float> mViewingDistance{ mIndex, "Camera", "viewing distance", makeMaxStrictSanitizerFloat(0) };
        SettingValue<float> mFieldOfView{ mIndex, "Camera", "field of view", makeClampSanitizerFloat(1, 179) };
        SettingValue<float> mFirstPersonFieldOfView{ mIndex, "Camera", "first person field of view",
            makeClampSanitizerFloat(1, 179) };
        SettingValue<bool> mReverseZ{ mIndex, "Camera", "reverse z" };
    };
}

#endif
