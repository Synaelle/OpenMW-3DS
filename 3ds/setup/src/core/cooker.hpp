#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace setup
{
    struct CookOptions
    {
        std::filesystem::path dataFiles; // the game's Data Files (its archives and loose files), read only
        std::vector<std::string> archives = { "Morrowind.bsa" }; // in load order
        std::filesystem::path engineVfs; // OpenMW's own resources/vfs, read first (the port's files)
        std::filesystem::path portInterface; // the port's 3DS interface (openmw/ui3ds), read last (the port's files)
        std::filesystem::path out; // the pack, textures.o3tp
        int threads = 0; // 0: one per CPU core
        size_t limit = 0; // cook only the first N textures (testing)
        std::filesystem::path keyList; // when set: each entry's hash and key, one per line (testing)
        const std::atomic<bool>* cancel = nullptr; // set by another thread: the cooking stops, nothing is written
    };

    struct CookSummary
    {
        size_t textures = 0;
        size_t atlasImages = 0;
        uint64_t bytes = 0;
    };

    // Cooks the textures for the 3DS's GPU into one pack (format 4, read by the port's texturepack.cpp), as the
    // port's tools/cook_textures.py does: the same choices, sizes, formats and bytes. `progress` gets the textures
    // done and the total.
    CookSummary cookTextures(const CookOptions& options, const std::function<void(size_t, size_t)>& progress = {});

    // Debug: the steps of one texture (its decoded image, the image handed to the encoder) as raw RGBA files in
    // `folder`, named <step>_<width>x<height>.rgba.
    void dumpTextureSteps(const CookOptions& options, const std::string& key, const std::filesystem::path& folder);
}
