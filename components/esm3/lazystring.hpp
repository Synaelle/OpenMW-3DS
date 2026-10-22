#ifndef OPENMW_COMPONENTS_ESM3_LAZYSTRING_H
#define OPENMW_COMPONENTS_ESM3_LAZYSTRING_H

// Nintendo 3DS: a string subrecord that stays in the content file until first read.
//
// Dialogue responses and book texts are ~8 MB of Morrowind.esm, but only the few on screen are ever
// needed. load() records where the subrecord is (file, offset, size) and skips it. get() reads it back
// from the SD card on first use, converts it to UTF-8 exactly like ESMReader::getHString, and caches it.
// Main thread only (dialogue, journal, books, Lua with "lua num threads = 0").

#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace ESM
{
    class ESMReader;

    class LazyString
    {
    public:
        LazyString() = default;
        LazyString(const LazyString& other) { *this = other; }
        LazyString(LazyString&&) noexcept = default;
        LazyString& operator=(const LazyString& other);
        LazyString& operator=(LazyString&&) noexcept = default;
        LazyString& operator=(std::string value);

        // Records the current H-string subrecord (its name already read) and skips its data.
        void load(ESMReader& esm);

        const std::string& get() const;
        operator const std::string&() const { return get(); }
        operator std::string_view() const { return get(); }

        // Doesn't touch the file. A lazily stored subrecord is non-empty unless it is 0 bytes long.
        bool empty() const { return mCache ? mCache->empty() : mSize == 0; }
        void clear();

    private:
        mutable std::unique_ptr<std::string> mCache; // value once known
        uint32_t mOffset = 0; // of the subrecord data in the content file
        uint16_t mSize = 0; // bytes (subrecord size)
        uint8_t mFile = 0; // index into the registry of content files (lazystring.cpp)
        bool mLazy = false; // mOffset/mSize/mFile are valid
    };

    inline std::ostream& operator<<(std::ostream& stream, const LazyString& value)
    {
        return stream << value.get();
    }
}

#endif
