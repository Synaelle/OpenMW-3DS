#pragma once

#include <filesystem>
#include <vector>

namespace setup
{
    // Morrowind installations found on this computer, the usable ones (checkMorrowind finds no problem) first:
    // Steam's libraries, GOG and the original installer (their registry entries on Windows), then the usual folders.
    std::vector<std::filesystem::path> findMorrowind();

    // Where the output goes unless the player picks another folder: OpenMW-3DS on the desktop.
    std::filesystem::path defaultOutput();

    // The folder of the running program (the release puts the port's files next to it, in "port").
    std::filesystem::path programFolder();
}
