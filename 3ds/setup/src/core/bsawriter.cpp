#include "core/bsawriter.hpp"

#include "core/text.hpp"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace setup
{
    namespace fs = std::filesystem;

    std::pair<uint32_t, uint32_t> tes3Hash(const std::string& name)
    {
        const size_t half = name.size() / 2;
        uint32_t low = 0, high = 0;
        unsigned shift = 0;
        for (size_t i = 0; i < half; ++i)
        {
            low ^= uint32_t(uint8_t(name[i])) << (shift & 0x1F);
            shift += 8;
        }
        shift = 0;
        for (size_t i = half; i < name.size(); ++i)
        {
            const uint32_t temp = uint32_t(uint8_t(name[i])) << (shift & 0x1F);
            high ^= temp;
            const unsigned n = temp & 0x1F;
            if (n != 0)
                high = (high >> n) | (high << (32 - n));
            shift += 8;
        }
        return { low, high };
    }

    namespace
    {
        void put32(std::ofstream& out, uint32_t value)
        {
            const char bytes[4] = { char(value), char(value >> 8), char(value >> 16), char(value >> 24) };
            out.write(bytes, 4);
        }

        struct Entry
        {
            std::string name; // lower case, backslashes, relative to Data Files
            fs::path path;
            uint32_t size = 0;
            std::pair<uint32_t, uint32_t> hash;
        };
    }

    BsaSummary packSoundArchive(const fs::path& dataFiles, const std::vector<std::string>& folders, const fs::path& out,
        const std::function<void(uint64_t, uint64_t)>& progress)
    {
        std::vector<Entry> entries;
        for (const std::string& folder : folders)
        {
            const fs::path root = dataFiles / folder;
            if (!fs::is_directory(root))
                throw std::runtime_error("no " + folder + " folder in " + dataFiles.string());
            for (const fs::directory_entry& item : fs::recursive_directory_iterator(root))
            {
                if (!item.is_regular_file())
                    continue;
                const std::string extension = toLowerAscii(item.path().extension().string());
                if (extension != ".wav" && extension != ".mp3")
                    continue;
                std::string name = toLowerAscii(fs::relative(item.path(), dataFiles).string());
                std::replace(name.begin(), name.end(), '/', '\\');
                const uintmax_t size = item.file_size();
                if (size >= (uintmax_t(1) << 32))
                    throw std::runtime_error(item.path().string() + " is larger than 4 GB");
                entries.push_back({ name, item.path(), uint32_t(size), tes3Hash(name) });
            }
        }
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.hash < b.hash; });

        uint32_t namesSize = 0;
        uint64_t total = 0;
        for (const Entry& e : entries)
        {
            namesSize += uint32_t(e.name.size() + 1);
            total += e.size;
        }
        const uint32_t count = uint32_t(entries.size());
        const uint32_t dirSize = 12 * count + namesSize;
        if (12 + uint64_t(dirSize) + 8 * uint64_t(count) + total >= (uint64_t(1) << 32))
            throw std::runtime_error("the sound archive would be larger than 4 GB");

        fs::create_directories(out.parent_path());
        const fs::path part = out.string() + ".part";
        {
            std::ofstream file(part, std::ios::binary | std::ios::trunc);
            if (!file)
                throw std::runtime_error("can't write " + part.string());
            put32(file, 0x100);
            put32(file, dirSize);
            put32(file, count);
            uint32_t offset = 0;
            for (const Entry& e : entries)
            {
                put32(file, e.size);
                put32(file, offset);
                offset += e.size;
            }
            uint32_t nameOffset = 0;
            for (const Entry& e : entries)
            {
                put32(file, nameOffset);
                nameOffset += uint32_t(e.name.size() + 1);
            }
            for (const Entry& e : entries)
                file.write(e.name.c_str(), std::streamsize(e.name.size() + 1));
            for (const Entry& e : entries)
            {
                put32(file, e.hash.first);
                put32(file, e.hash.second);
            }
            std::vector<char> buffer;
            uint64_t written = 0;
            for (const Entry& e : entries)
            {
                std::ifstream in(e.path, std::ios::binary);
                buffer.resize(e.size);
                if (!in.read(buffer.data(), e.size) || in.gcount() != std::streamsize(e.size))
                    throw std::runtime_error(e.path.string() + " couldn't be read whole");
                file.write(buffer.data(), e.size);
                written += e.size;
                if (progress)
                    progress(written, total);
            }
            if (!file.flush())
                throw std::runtime_error("writing " + part.string() + " failed");
        }
        fs::rename(part, out);
        return { entries.size(), total };
    }
}
