#include "threedspath.hpp"

#if defined(OPENMW_PLATFORM_3DS)

namespace
{
    const std::filesystem::path sRoot = "sdmc:/openmw";
}

namespace Files
{
    ThreeDSPath::ThreeDSPath(const std::string& /*application_name*/) {}

    std::filesystem::path ThreeDSPath::getUserConfigPath() const
    {
        return sRoot / "config";
    }

    std::filesystem::path ThreeDSPath::getUserDataPath() const
    {
        return sRoot;
    }

    std::filesystem::path ThreeDSPath::getGlobalConfigPath() const
    {
        return sRoot;
    }

    std::filesystem::path ThreeDSPath::getLocalPath() const
    {
        return sRoot;
    }

    std::filesystem::path ThreeDSPath::getGlobalDataPath() const
    {
        return sRoot / "data";
    }

    std::filesystem::path ThreeDSPath::getCachePath() const
    {
        return sRoot / "cache";
    }

    std::vector<std::filesystem::path> ThreeDSPath::getInstallPaths() const
    {
        return {};
    }
}

#endif
