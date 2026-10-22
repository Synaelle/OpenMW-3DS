#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace setup
{
    // OpenMW's fallback values from a Morrowind.ini, as its mwiniimporter writes them: for each key it imports
    // ("Section:Key"), every value the file gives, as "fallback=Section_Key,value" with spaces turned into '_'.
    // The file is read as Windows-1252 and the lines come out in UTF-8.
    std::vector<std::string> readFallbacks(const std::filesystem::path& morrowindIni);

    // How many keys OpenMW imports (for the summary).
    size_t fallbackKeyCount();
}
