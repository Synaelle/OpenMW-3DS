#include "core/detect.hpp"

#include "core/prepare.hpp"
#include "core/text.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <regex>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#endif

namespace setup
{
    namespace fs = std::filesystem;

    namespace
    {
#ifdef _WIN32
        // A string value from the registry, in both registry views (32-bit installers write to WOW6432Node).
        fs::path registryPath(HKEY root, const wchar_t* key, const wchar_t* value)
        {
            for (const DWORD view : { DWORD(KEY_WOW64_32KEY), DWORD(KEY_WOW64_64KEY) })
            {
                wchar_t buffer[2048];
                DWORD size = sizeof(buffer);
                if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | view, nullptr, buffer, &size) == ERROR_SUCCESS)
                    return fs::path(buffer);
            }
            return {};
        }
#endif

        // Steam's library folders (steamapps/libraryfolders.vdf: "path" entries), Steam's own folder first.
        std::vector<fs::path> steamLibraries(const fs::path& steam)
        {
            std::vector<fs::path> libraries;
            if (steam.empty())
                return libraries;
            libraries.push_back(steam);
            std::ifstream file(steam / "steamapps" / "libraryfolders.vdf", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            const std::regex pattern(R"re("path"\s*"([^"]+)")re");
            for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it)
            {
                std::string path = (*it)[1].str();
                // VDF escapes backslashes.
                for (size_t at = path.find("\\\\"); at != std::string::npos; at = path.find("\\\\", at + 1))
                    path.erase(at, 1);
                libraries.push_back(pathFromUtf8(path));
            }
            return libraries;
        }
    }

    std::vector<fs::path> findMorrowind()
    {
        std::vector<fs::path> candidates;
#ifdef _WIN32
        fs::path steam = registryPath(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
        if (steam.empty())
            steam = registryPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
        for (const fs::path& library : steamLibraries(steam))
            candidates.push_back(library / "steamapps" / "common" / "Morrowind");
        candidates.push_back(registryPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games\\1435828767", L"path"));
        candidates.push_back(registryPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\GOG.com\\Games\\1440163901", L"path"));
        candidates.push_back(
            registryPath(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Bethesda Softworks\\Morrowind", L"Installed Path"));
        for (const char* folder : { "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Morrowind",
                 "C:\\GOG Games\\Morrowind", "C:\\Program Files (x86)\\GOG Galaxy\\Games\\Morrowind",
                 "C:\\Program Files (x86)\\Bethesda Softworks\\Morrowind" })
            candidates.push_back(folder);
#else
        if (const char* home = std::getenv("HOME"))
        {
            for (const fs::path& steam :
                { fs::path(home) / ".steam" / "steam", fs::path(home) / ".local" / "share" / "Steam",
                    fs::path(home) / "Library" / "Application Support" / "Steam" })
                for (const fs::path& library : steamLibraries(steam))
                    candidates.push_back(library / "steamapps" / "common" / "Morrowind");
            candidates.push_back(fs::path(home) / "GOG Games" / "Morrowind");
        }
#endif
        std::vector<fs::path> found;
        for (const fs::path& candidate : candidates)
        {
            std::error_code error;
            if (candidate.empty() || !fs::is_directory(candidate, error))
                continue;
            const fs::path normal = fs::weakly_canonical(candidate, error);
            const fs::path path = error ? candidate : normal;
            if (std::find(found.begin(), found.end(), path) == found.end())
                found.push_back(path);
        }
        std::stable_partition(found.begin(), found.end(), [](const fs::path& p) { return checkMorrowind(p).empty(); });
        return found;
    }

    fs::path defaultOutput()
    {
#ifdef _WIN32
        PWSTR desktop = nullptr;
        fs::path result;
        if (SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop) == S_OK)
            result = fs::path(desktop) / "OpenMW-3DS";
        CoTaskMemFree(desktop);
        if (!result.empty())
            return result;
#endif
        if (const char* home = std::getenv("HOME"))
            return fs::path(home) / "Desktop" / "OpenMW-3DS";
        return fs::current_path() / "OpenMW-3DS";
    }

    fs::path programFolder()
    {
#ifdef _WIN32
        wchar_t buffer[MAX_PATH * 4];
        const DWORD length = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
        if (length > 0 && length < std::size(buffer))
            return fs::path(buffer).parent_path();
#else
        // An AppImage runs from a temporary mount: its own folder is where the .AppImage file is.
        if (const char* appImage = std::getenv("APPIMAGE"))
            return fs::path(appImage).parent_path();
        std::error_code error;
        const fs::path self = fs::read_symlink("/proc/self/exe", error);
        if (!error)
            return self.parent_path();
#endif
        return fs::current_path();
    }
}
