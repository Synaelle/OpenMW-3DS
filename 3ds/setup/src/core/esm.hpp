#pragma once

#include <filesystem>
#include <string>

namespace setup
{
    // The language of a Morrowind.esm, read from its own interface words (the game settings sYes, sNo and sCancel)
    // rather than a list of file hashes (the English game exists in several versions). "English", the name of
    // another language it recognises ("German", "French"...), or empty when it can't be told (an unreadable file,
    // a language it doesn't know).
    std::string esmLanguage(const std::filesystem::path& esm);
}
