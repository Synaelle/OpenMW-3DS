#ifndef COMPONENTS_FILES_THREEDSPATH_H
#define COMPONENTS_FILES_THREEDSPATH_H

#if defined(OPENMW_PLATFORM_3DS)

#include <filesystem>
#include <string>
#include <vector>

namespace Files
{
    /// Nintendo 3DS: everything lives on the SD card under sdmc:/openmw.
    ///   sdmc:/openmw/config     openmw.cfg, settings.cfg (user)
    ///   sdmc:/openmw/           user data (saves/, screenshots/)
    ///   sdmc:/openmw/cache      caches
    ///   sdmc:/openmw/resources  OpenMW resources (vfs, l10n, built-in Lua scripts)
    ///   sdmc:/openmw/data       Morrowind "Data Files"
    /// Directory creation costs ~1.5 s each on real SD cards, so the SD package ships these
    /// directories pre-created.
    struct ThreeDSPath
    {
        ThreeDSPath(const std::string& application_name);

        std::filesystem::path getUserConfigPath() const;
        std::filesystem::path getUserDataPath() const;
        std::filesystem::path getGlobalConfigPath() const;
        std::filesystem::path getLocalPath() const;
        std::filesystem::path getGlobalDataPath() const;
        std::filesystem::path getCachePath() const;
        std::vector<std::filesystem::path> getInstallPaths() const;
    };
}

#endif

#endif
