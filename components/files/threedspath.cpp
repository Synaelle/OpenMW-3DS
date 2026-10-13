#include "threedspath.hpp"

#if defined(OPENMW_PLATFORM_3DS)

namespace
{
    // A POSIX absolute path on the default device: std::filesystem (newlib) does not treat "sdmc:/..."
    // as absolute, so OpenMW would resolve it relative to the config directory. main3ds.cpp makes
    // sdmc the default device with chdir("sdmc:/").
    const std::filesystem::path sRoot = "/openmw";
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
