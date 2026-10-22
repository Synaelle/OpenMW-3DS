#include "core/fallbacks.hpp"

#include "core/text.hpp"

#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>

namespace setup
{
    namespace
    {
        constexpr const char* kKeys[] = {
#include "fallback_keys.inc"
        };
    }

    size_t fallbackKeyCount()
    {
        return std::size(kKeys);
    }

    std::vector<std::string> readFallbacks(const std::filesystem::path& morrowindIni)
    {
        std::ifstream file(morrowindIni, std::ios::binary);
        if (!file)
            throw std::runtime_error("can't read " + morrowindIni.string());
        const std::string raw((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // "Section:Key" -> every value, in file order (MwIniImporter::loadIniFile).
        std::map<std::string, std::vector<std::string>> values;
        std::string section;
        size_t start = 0;
        while (start < raw.size())
        {
            size_t end = raw.find('\n', start);
            if (end == std::string::npos)
                end = raw.size();
            const std::string line = trim(std::string_view(raw).substr(start, end - start));
            start = end + 1;
            if (line.empty() || line[0] == ';')
                continue;
            if (line.front() == '[' && line.back() == ']')
            {
                section = line.substr(1, line.size() - 2);
                continue;
            }
            const size_t equals = line.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = trim(std::string_view(line).substr(0, equals));
            if (key.empty())
                continue;
            values[section + ":" + key].push_back(cp1252ToUtf8(trim(std::string_view(line).substr(equals + 1))));
        }

        std::vector<std::string> lines;
        for (const char* key : kKeys)
        {
            const auto found = values.find(key);
            if (found == values.end())
                continue;
            std::string name = key;
            for (char& c : name)
                if (c == ' ' || c == ':')
                    c = '_';
            for (const std::string& value : found->second)
                lines.push_back("fallback=" + name + "," + value);
        }
        return lines;
    }
}
