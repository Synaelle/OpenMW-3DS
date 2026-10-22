#include "core/text.hpp"

#include <cstdint>

namespace setup
{
    namespace
    {
        // Windows-1252's 0x80-0x9F block (the rest of the upper half is Latin-1, the same code points).
        constexpr char16_t kHighBlock[32] = {
            0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, //
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F, //
            0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, //
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, //
        };

        void appendUtf8(std::string& out, char32_t code)
        {
            if (code < 0x80)
                out += char(code);
            else if (code < 0x800)
            {
                out += char(0xC0 | (code >> 6));
                out += char(0x80 | (code & 0x3F));
            }
            else
            {
                out += char(0xE0 | (code >> 12));
                out += char(0x80 | ((code >> 6) & 0x3F));
                out += char(0x80 | (code & 0x3F));
            }
        }
    }

    std::string cp1252ToUtf8(std::string_view text)
    {
        std::string out;
        out.reserve(text.size());
        for (const char c : text)
        {
            const uint8_t byte = uint8_t(c);
            if (byte >= 0x80 && byte < 0xA0)
                appendUtf8(out, kHighBlock[byte - 0x80]);
            else
                appendUtf8(out, byte);
        }
        return out;
    }

    std::string toLowerAscii(std::string_view text)
    {
        std::string out(text);
        for (char& c : out)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return out;
    }

    std::string trim(std::string_view text)
    {
        const auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        size_t begin = 0, end = text.size();
        while (begin < end && isSpace(text[begin]))
            ++begin;
        while (end > begin && isSpace(text[end - 1]))
            --end;
        return std::string(text.substr(begin, end - begin));
    }
}
