#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace setup
{
    struct Options
    {
        std::filesystem::path morrowind; // the Morrowind installation (Morrowind.ini and Data Files inside)
        std::filesystem::path output; // what the player copies to the root of the 3DS's SD card
        std::filesystem::path port; // the port's own files from the release, laid out as on the SD card
        std::string version; // the release, written to openmw/VERSION
        const std::atomic<bool>* cancel = nullptr; // set by another thread: prepare() stops with Cancelled
    };

    // Thrown when Options::cancel was set. The output folder is left incomplete.
    struct Cancelled : std::runtime_error
    {
        Cancelled()
            : std::runtime_error("Cancelled")
        {
        }
    };

    struct Progress
    {
        std::string step; // what is being done, for the player
        double fraction = 0; // of the whole preparation
    };

    // The problems found in the Morrowind folder (empty: it can be used).
    std::vector<std::string> checkMorrowind(const std::filesystem::path& morrowind);

    // Prepares the output folder. Throws std::runtime_error with a message for the player on failure. The game's
    // files are only read.
    void prepare(const Options& options, const std::function<void(const Progress&)>& progress);
}
