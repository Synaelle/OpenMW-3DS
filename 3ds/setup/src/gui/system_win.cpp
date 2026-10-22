#include "gui/system.hpp"

#include "core/text.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

namespace gui
{
    std::filesystem::path pickFolder(const std::string& title, const std::filesystem::path& start)
    {
        std::filesystem::path result;
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return result;
        DWORD flags = 0;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        const std::wstring wideTitle = setup::pathFromUtf8(title).wstring();
        dialog->SetTitle(wideTitle.c_str());
        std::error_code error;
        std::filesystem::path folder = start;
        while (!folder.empty() && !std::filesystem::is_directory(folder, error) && folder != folder.parent_path())
            folder = folder.parent_path();
        IShellItem* startItem = nullptr;
        if (!folder.empty()
            && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&startItem))))
        {
            dialog->SetFolder(startItem);
            startItem->Release();
        }
        if (SUCCEEDED(dialog->Show(nullptr)))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    result = path;
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
        return result;
    }

    void openFolder(const std::filesystem::path& folder)
    {
        ShellExecuteW(nullptr, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    std::filesystem::path interfaceFont()
    {
        wchar_t windows[MAX_PATH];
        const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
        if (length == 0 || length >= MAX_PATH)
            return {};
        const std::filesystem::path font = std::filesystem::path(windows) / "Fonts" / "segoeui.ttf";
        std::error_code error;
        return std::filesystem::is_regular_file(font, error) ? font : std::filesystem::path();
    }

    bool systemPrefersDark()
    {
        // Settings > Personalisation > Colours > "Choose your default app mode": 0 dark, 1 light.
        DWORD light = 0;
        DWORD size = sizeof(light);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size)
            != ERROR_SUCCESS)
            return true;
        return light == 0;
    }

    bool canFollowThemeChanges()
    {
        return true;
    }

    void matchTitleBar(void* nativeWindow, bool dark)
    {
        if (nativeWindow == nullptr)
            return;
        const BOOL value = dark ? TRUE : FALSE;
        // DWMWA_USE_IMMERSIVE_DARK_MODE: 20 since Windows 10 20H1, 19 before it.
        if (FAILED(DwmSetWindowAttribute(static_cast<HWND>(nativeWindow), 20, &value, sizeof(value))))
            DwmSetWindowAttribute(static_cast<HWND>(nativeWindow), 19, &value, sizeof(value));
    }
}
