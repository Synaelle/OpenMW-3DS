#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace setup
{
    // A path from UTF-8 text (std::filesystem::u8path is deprecated in C++20).
    inline std::filesystem::path pathFromUtf8(std::string_view text)
    {
        return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
    }

    // Windows-1252 (the encoding of English and Western Morrowind files) to UTF-8.
    std::string cp1252ToUtf8(std::string_view text);

    // Lower case, ASCII only (game file names).
    std::string toLowerAscii(std::string_view text);

    std::string trim(std::string_view text);
}
