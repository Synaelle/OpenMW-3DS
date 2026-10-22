#include "gui/system.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <sys/wait.h>

// macOS and Linux. The folder picker goes through the system's own tools (osascript, zenity or kdialog), so the
// program needs no toolkit of its own.

namespace gui
{
    namespace
    {
        // Runs a command and gives its output (empty when it fails); `ran` tells whether it could run at all.
        std::string run(const std::string& command, bool* ran = nullptr)
        {
            std::string output;
            FILE* pipe = popen((command + " 2>/dev/null").c_str(), "r");
            if (pipe == nullptr)
            {
                if (ran)
                    *ran = false;
                return output;
            }
            std::array<char, 512> buffer;
            while (fgets(buffer.data(), int(buffer.size()), pipe) != nullptr)
                output += buffer.data();
            const int status = pclose(pipe);
            if (ran)
                *ran = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) != 127;
            while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                output.pop_back();
            return output;
        }

        std::string quoted(const std::string& text)
        {
            std::string out = "'";
            for (const char c : text)
                out += c == '\'' ? std::string("'\\''") : std::string(1, c);
            return out + "'";
        }
    }

    std::filesystem::path pickFolder(const std::string& title, const std::filesystem::path& start)
    {
#ifdef __APPLE__
        (void)start;
        const std::string output
            = run("osascript -e " + quoted("POSIX path of (choose folder with prompt \"" + title + "\")"));
        return output.empty() ? std::filesystem::path() : std::filesystem::path(output);
#else
        bool ran = false;
        std::string output = run("zenity --file-selection --directory --title=" + quoted(title) + " --filename="
                + quoted(start.string() + "/"),
            &ran);
        if (!ran)
            output = run("kdialog --getexistingdirectory " + quoted(start.string()) + " --title " + quoted(title));
        return output.empty() ? std::filesystem::path() : std::filesystem::path(output);
#endif
    }

    void openFolder(const std::filesystem::path& folder)
    {
#ifdef __APPLE__
        run("open " + quoted(folder.string()));
#else
        run("xdg-open " + quoted(folder.string()) + " &");
#endif
    }

    std::filesystem::path interfaceFont()
    {
        for (const char* path : { "/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                 "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf" })
        {
            std::error_code error;
            if (std::filesystem::is_regular_file(path, error))
                return path;
        }
        return {};
    }

    bool systemPrefersDark()
    {
#ifdef __APPLE__
        // The key only exists in dark mode: "Dark", or nothing (light) when the command runs.
        bool ran = false;
        const std::string style = run("defaults read -g AppleInterfaceStyle", &ran);
        if (!ran)
            return true;
        return style.find("Dark") != std::string::npos;
#else
        bool ran = false;
        const std::string scheme = run("gsettings get org.gnome.desktop.interface color-scheme", &ran);
        if (scheme.find("prefer-dark") != std::string::npos)
            return true;
        if (scheme.find("prefer-light") != std::string::npos)
            return false;
        if (const char* gtk = std::getenv("GTK_THEME"))
            return std::string(gtk).find("dark") != std::string::npos || std::string(gtk).find("Dark") != std::string::npos;
        const std::string theme = run("gsettings get org.gnome.desktop.interface gtk-theme");
        if (!theme.empty())
            return theme.find("dark") != std::string::npos || theme.find("Dark") != std::string::npos;
        return true;
#endif
    }

    bool canFollowThemeChanges()
    {
        return false;
    }

    void matchTitleBar(void*, bool)
    {
    }
}
