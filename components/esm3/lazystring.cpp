#include "lazystring.hpp"

#include "esmreader.hpp"

#include <components/debug/debuglog.hpp>
#include <components/toutf8/toutf8.hpp>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace ESM
{
    namespace
    {
        // Content files that hold lazy strings, by ESMReader index. Filled while loading (loader thread),
        // read afterwards (main thread).
        struct SourceFile
        {
            std::string mPath;
            std::FILE* mHandle = nullptr; // opened on first read, kept open (opening costs ms on SD)
        };
        std::vector<SourceFile> sFiles;
        std::mutex sFilesMutex;
        const ToUTF8::StatelessUtf8Encoder* sEncoder = nullptr;

        bool registerFile(const ESMReader& esm)
        {
            const int index = esm.getIndex();
            if (index < 0 || index >= std::numeric_limits<uint8_t>::max())
                return false;
            // Only immutable content files: a save game (e.g. a book created at runtime) can be
            // overwritten later, which would leave the offset pointing at other data.
            std::string ext = esm.getName().extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".esm" && ext != ".esp" && ext != ".omwgame" && ext != ".omwaddon")
                return false;
            std::lock_guard lock(sFilesMutex);
            if (sFiles.size() <= static_cast<std::size_t>(index))
                sFiles.resize(index + 1);
            if (sFiles[index].mPath.empty())
            {
                if (sFiles[index].mHandle != nullptr)
                {
                    std::fclose(sFiles[index].mHandle);
                    sFiles[index].mHandle = nullptr;
                }
                sFiles[index].mPath = esm.getName().string();
            }
            else if (sFiles[index].mPath != esm.getName().string())
                return false; // index reused for another file: keep it simple, load eagerly
            if (esm.getEncoder() != nullptr)
                sEncoder = &esm.getEncoder()->getStatelessEncoder();
            return true;
        }

        std::string readFromFile(uint8_t file, uint32_t offset, uint16_t size)
        {
            std::string raw(size, '\0');
            {
                std::lock_guard lock(sFilesMutex);
                SourceFile& source = sFiles.at(file);
                if (source.mHandle == nullptr)
                    source.mHandle = std::fopen(source.mPath.c_str(), "rb");
                if (source.mHandle == nullptr || std::fseek(source.mHandle, static_cast<long>(offset), SEEK_SET) != 0
                    || std::fread(raw.data(), 1, size, source.mHandle) != size)
                {
                    Log(Debug::Error) << "LazyString: failed to read " << size << " bytes at " << offset << " from "
                                      << source.mPath;
                    return {};
                }
            }
            // Same as ESMReader::getStringView: stop at the first NUL, then convert to UTF-8.
            raw.resize(strnlen(raw.data(), raw.size()));
            if (sEncoder == nullptr)
                return raw;
            std::string buffer;
            return std::string(sEncoder->getUtf8(raw, ToUTF8::BufferAllocationPolicy::FitToRequiredSize, buffer));
        }
    }

    LazyString& LazyString::operator=(const LazyString& other)
    {
        if (this == &other)
            return *this;
        mCache = other.mCache ? std::make_unique<std::string>(*other.mCache) : nullptr;
        mOffset = other.mOffset;
        mSize = other.mSize;
        mFile = other.mFile;
        mLazy = other.mLazy;
        return *this;
    }

    LazyString& LazyString::operator=(std::string value)
    {
        mCache = std::make_unique<std::string>(std::move(value));
        mLazy = false;
        return *this;
    }

    void LazyString::load(ESMReader& esm)
    {
        // The subrecord name has been read; next come its uint32 size and the data.
        const std::size_t sizeField = esm.getFileOffset();
        esm.skipHSub();
        const std::size_t dataStart = sizeField + sizeof(uint32_t);
        const std::size_t size = esm.getFileOffset() - dataStart;

        mCache.reset();
        mLazy = false;
        if (size <= std::numeric_limits<uint16_t>::max() && dataStart <= std::numeric_limits<uint32_t>::max()
            && registerFile(esm))
        {
            mOffset = static_cast<uint32_t>(dataStart);
            mSize = static_cast<uint16_t>(size);
            mFile = static_cast<uint8_t>(esm.getIndex());
            mLazy = true;
            return;
        }
        // Save games, huge texts, too many content files: read it now.
        const std::string path = esm.getName().string();
        std::FILE* f = std::fopen(path.c_str(), "rb");
        std::string raw(size, '\0');
        if (f != nullptr && std::fseek(f, static_cast<long>(dataStart), SEEK_SET) == 0)
            raw.resize(std::fread(raw.data(), 1, size, f));
        if (f != nullptr)
            std::fclose(f);
        raw.resize(strnlen(raw.data(), raw.size()));
        std::string buffer;
        mCache = std::make_unique<std::string>(
            esm.getEncoder() != nullptr
                ? std::string(esm.getEncoder()->getStatelessEncoder().getUtf8(
                    raw, ToUTF8::BufferAllocationPolicy::FitToRequiredSize, buffer))
                : raw);
    }

    const std::string& LazyString::get() const
    {
        if (!mCache)
            mCache = std::make_unique<std::string>(mLazy ? readFromFile(mFile, mOffset, mSize) : std::string());
        return *mCache;
    }

    void LazyString::clear()
    {
        mCache.reset();
        mLazy = false;
        mSize = 0;
    }
}
