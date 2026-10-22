#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace setup
{
    // The TES3 BSA name hash (components/bsa/bsafile.cpp) of a lower-case path with backslashes.
    std::pair<uint32_t, uint32_t> tes3Hash(const std::string& name);

    struct BsaSummary
    {
        size_t files = 0;
        uint64_t dataBytes = 0;
    };

    // Packs the .wav and .mp3 files of `folders` (under `dataFiles`) into one TES3 BSA, as OpenMW reads it with
    // fallback-archive=: the sound3ds.bsa the port loads instead of ~7,200 loose files (indexing them took 12.8 s
    // on the console). Written to `out` through a temporary file. `progress` gets bytes written and the total.
    BsaSummary packSoundArchive(const std::filesystem::path& dataFiles, const std::vector<std::string>& folders,
        const std::filesystem::path& out, const std::function<void(uint64_t, uint64_t)>& progress = {});
}
