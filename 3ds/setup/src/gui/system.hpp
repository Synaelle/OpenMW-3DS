#pragma once

#include <filesystem>
#include <string>

// What the window needs from the operating system.
namespace gui
{
    // The system's folder picker. Empty when the player cancels it.
    std::filesystem::path pickFolder(const std::string& title, const std::filesystem::path& start);

    // Shows a folder in the file manager.
    void openFolder(const std::filesystem::path& folder);

    // A font from the system for the interface (empty when none is found: the built-in one is used).
    std::filesystem::path interfaceFont();

    // Whether the system's apps are in dark mode: Windows' "app mode", macOS's appearance, GNOME's colour scheme or
    // the GTK theme on Linux. Dark when it can't be told.
    bool systemPrefersDark();

    // Whether systemPrefersDark is cheap enough to ask again while the window is open (Windows: a registry read).
    bool canFollowThemeChanges();

    // Gives the window's title bar the matching look where the system lets an app choose (Windows 10 and 11).
    // `nativeWindow` is the system's window handle (HWND on Windows).
    void matchTitleBar(void* nativeWindow, bool dark);
}
