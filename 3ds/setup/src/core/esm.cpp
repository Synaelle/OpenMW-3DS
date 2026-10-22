#include "core/esm.hpp"

#include <fstream>
#include <map>
#include <vector>

namespace setup
{
    namespace
    {
        uint32_t get32(const unsigned char* p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        // The game setting's string, without its trailing NULs.
        std::string trimmed(const char* data, size_t size)
        {
            std::string text(data, size);
            while (!text.empty() && text.back() == '\0')
                text.pop_back();
            return text;
        }
    }

    std::string esmLanguage(const std::filesystem::path& esm)
    {
        std::ifstream file(esm, std::ios::binary);
        if (!file)
            return {};
        // TES3 records: 16-byte header (type, size, 8 bytes), then subrecords (type, size, data). The game settings
        // come early in Morrowind.esm (within its first ~1,500 records): stop when the three are found, or after
        // enough records.
        std::map<std::string, std::string> words;
        std::vector<char> data;
        for (int record = 0; record < 20000 && words.size() < 3; ++record)
        {
            unsigned char header[16];
            if (!file.read(reinterpret_cast<char*>(header), 16))
                break;
            const uint32_t size = get32(header + 4);
            if (std::string(reinterpret_cast<char*>(header), 4) != "GMST")
            {
                file.seekg(size, std::ios::cur);
                continue;
            }
            data.resize(size);
            if (!file.read(data.data(), std::streamsize(size)))
                break;
            std::string id, value;
            for (size_t p = 0; p + 8 <= data.size();)
            {
                const std::string type(&data[p], 4);
                const uint32_t length = get32(reinterpret_cast<const unsigned char*>(&data[p + 4]));
                if (p + 8 + length > data.size())
                    break;
                if (type == "NAME")
                    id = trimmed(&data[p + 8], length);
                else if (type == "STRV")
                    value = trimmed(&data[p + 8], length);
                p += 8 + length;
            }
            if (id == "sYes" || id == "sNo" || id == "sCancel")
                words[id] = value;
        }
        const std::string yes = words["sYes"];
        if (yes == "Yes" && words["sNo"] == "No" && words["sCancel"] == "Cancel")
            return "English";
        // The game's own translations (their single-byte encodings: Windows-1252, -1250 or -1251).
        static const std::map<std::string, std::string> others = {
            { "Ja", "German" },
            { "Oui", "French" },
            { "S\xEC", "Italian" },
            { "S\xED", "Spanish" },
            { "Tak", "Polish" },
            { "\xC4\xE0", "Russian" },
        };
        const auto found = others.find(yes);
        return found == others.end() ? std::string() : found->second;
    }
}
