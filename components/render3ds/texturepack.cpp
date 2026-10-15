#include "render3ds.hpp"

#include "gpumemory.hpp"

#include <components/debug/debuglog.hpp>

#include <platform3ds/log.hpp>

#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#ifndef GL_ETC1_RGB8_OES
#define GL_ETC1_RGB8_OES 0x8D64
#endif
#ifndef GL_COMPRESSED_RGBA8_ETC2_EAC
#define GL_COMPRESSED_RGBA8_ETC2_EAC 0x9278
#endif

namespace Render3DS
{
    namespace
    {
        // tools/cook_textures.py
        constexpr char kMagic[4] = { 'O', '3', 'T', 'P' };
        constexpr uint32_t kVersion = 1;

        struct PackHeader
        {
            char mMagic[4];
            uint32_t mVersion;
            uint32_t mCount;
            uint32_t mIndexOffset;
            uint32_t mDataOffset;
            uint32_t mReserved[3];
        };
        static_assert(sizeof(PackHeader) == 32);

        struct PackEntry
        {
            uint64_t mHash;
            uint32_t mOffset;
            uint32_t mSize;
            uint16_t mWidth;
            uint16_t mHeight;
            uint16_t mSrcWidth;
            uint16_t mSrcHeight;
            uint8_t mFormat;
            uint8_t mLevels;
            uint8_t mFlags;
            uint8_t mReserved0;
            uint32_t mReserved1;
        } __attribute__((packed));
        static_assert(sizeof(PackEntry) == 32);

        std::mutex gPackMutex;
        FILE* gPack = nullptr;
        std::vector<PackEntry> gIndex;

        LightLock& linearLock()
        {
            static LightLock lock = [] {
                LightLock l;
                LightLock_Init(&l);
                return l;
            }();
            return lock;
        }

        uint64_t fnv1a64(std::string_view text)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (unsigned char c : text)
            {
                h ^= c;
                h *= 0x100000001B3ull;
            }
            return h;
        }

        // "textures/tx_wood.dds" -> "textures/tx_wood" (the pack key has no extension).
        std::string_view stripExtension(std::string_view path)
        {
            const size_t dot = path.find_last_of('.');
            const size_t slash = path.find_last_of('/');
            if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash))
                return path.substr(0, dot);
            return path;
        }

        GLenum glFormatFor(GPU_TEXCOLOR format)
        {
            switch (format)
            {
                case GPU_ETC1:
                    return GL_ETC1_RGB8_OES;
                case GPU_ETC1A4:
                    return GL_COMPRESSED_RGBA8_ETC2_EAC;
                default:
                    return GL_RGBA;
            }
        }

        unsigned bitsPerPixel(GPU_TEXCOLOR format)
        {
            switch (format)
            {
                case GPU_ETC1:
                    return 4;
                case GPU_ETC1A4:
                    return 8;
                default:
                    return 32;
            }
        }
    }

    void* gpuAlloc(size_t size)
    {
        LightLock_Lock(&linearLock());
        void* ptr = linearAlloc(size);
        LightLock_Unlock(&linearLock());
        return ptr;
    }

    void gpuFree(void* ptr)
    {
        if (ptr == nullptr)
            return;
        LightLock_Lock(&linearLock());
        linearFree(ptr);
        LightLock_Unlock(&linearLock());
    }

    CookedImage::CookedImage(void* data, uint32_t size, uint16_t width, uint16_t height, uint16_t srcWidth,
        uint16_t srcHeight, GPU_TEXCOLOR format, uint8_t levels, uint8_t flags)
        : mGpuData(data)
        , mGpuSize(size)
        , mGpuFormat(format)
        , mLevels(levels)
        , mFlags(flags)
        , mSrcWidth(srcWidth)
        , mSrcHeight(srcHeight)
    {
        const GLenum glFormat = glFormatFor(format);
        setImage(width, height, 1, glFormat, glFormat, GL_UNSIGNED_BYTE, static_cast<unsigned char*>(data),
            osg::Image::NO_DELETE);
        setOrigin(osg::Image::TOP_LEFT);
        if (levels > 1)
        {
            osg::Image::MipmapDataType offsets;
            uint32_t offset = 0;
            unsigned w = width, h = height;
            for (unsigned level = 1; level < levels; ++level)
            {
                offset += w * h * bitsPerPixel(format) / 8;
                offsets.push_back(offset);
                w /= 2;
                h /= 2;
            }
            setMipmapLevels(offsets);
        }
    }

    CookedImage::~CookedImage()
    {
        gpuFree(mGpuData);
    }

    bool openTexturePack(const char* path)
    {
        std::lock_guard<std::mutex> lock(gPackMutex);
        FILE* file = std::fopen(path, "rb");
        if (file == nullptr)
        {
            P3DS_LOGW("render3ds: no texture pack at %s (textures will be missing)", path);
            return false;
        }
        PackHeader header;
        if (std::fread(&header, sizeof(header), 1, file) != 1 || std::memcmp(header.mMagic, kMagic, 4) != 0
            || header.mVersion != kVersion)
        {
            P3DS_LOGE("render3ds: %s is not a version %lu texture pack", path, (unsigned long)kVersion);
            std::fclose(file);
            return false;
        }
        std::vector<PackEntry> index(header.mCount);
        if (std::fseek(file, static_cast<long>(header.mIndexOffset), SEEK_SET) != 0
            || std::fread(index.data(), sizeof(PackEntry), index.size(), file) != index.size())
        {
            P3DS_LOGE("render3ds: %s: truncated index", path);
            std::fclose(file);
            return false;
        }
        // Every texture is one read straight into linear memory: no stdio buffering in between.
        std::setvbuf(file, nullptr, _IONBF, 0);
        gPack = file;
        gIndex = std::move(index);
        P3DS_LOGI("render3ds: texture pack %s: %u textures", path, (unsigned)gIndex.size());
        return true;
    }

    namespace
    {
        osg::ref_ptr<osg::Image> loadEntry(const PackEntry& entry, std::string_view name)
        {
            void* data = gpuAlloc(entry.mSize);
            if (data == nullptr)
            {
                Log(Debug::Error) << "render3ds: out of linear memory for texture " << name << " (" << entry.mSize
                                  << " bytes, " << linearSpaceFree() / 1024 << " KB free)";
                return nullptr;
            }
            // No dirty cache lines may be written back over what the file system writes.
            GSPGPU_FlushDataCache(data, entry.mSize);
            bool ok;
            {
                std::lock_guard<std::mutex> lock(gPackMutex);
                ok = std::fseek(gPack, static_cast<long>(entry.mOffset), SEEK_SET) == 0
                    && std::fread(data, 1, entry.mSize, gPack) == entry.mSize;
            }
            if (!ok)
            {
                Log(Debug::Error) << "render3ds: failed to read " << name << " from the texture pack";
                gpuFree(data);
                return nullptr;
            }
            GSPGPU_FlushDataCache(data, entry.mSize);

            osg::ref_ptr<osg::Image> image = new CookedImage(data, entry.mSize, entry.mWidth, entry.mHeight,
                entry.mSrcWidth, entry.mSrcHeight, static_cast<GPU_TEXCOLOR>(entry.mFormat), entry.mLevels,
                entry.mFlags);
            image->setFileName(std::string(name));
            return image;
        }
    }

    bool cookedSourceSize(const osg::Image& image, int& width, int& height)
    {
        const auto* cooked = dynamic_cast<const CookedImage*>(&image);
        if (cooked == nullptr || cooked->sourceWidth() == 0 || cooked->sourceHeight() == 0)
            return false;
        width = cooked->sourceWidth();
        height = cooked->sourceHeight();
        return true;
    }

    osg::ref_ptr<osg::Image> loadCookedImage(std::string_view normalizedPath)
    {
        if (gPack == nullptr)
            return nullptr;
        const uint64_t hash = fnv1a64(stripExtension(normalizedPath));
        const auto it = std::lower_bound(gIndex.begin(), gIndex.end(), hash,
            [](const PackEntry& entry, uint64_t value) { return entry.mHash < value; });
        if (it == gIndex.end() || it->mHash != hash)
            return nullptr;
        return loadEntry(*it, normalizedPath);
    }

    osg::ref_ptr<osg::Image> loadCookedImageWithFormat(GPU_TEXCOLOR format)
    {
        for (const PackEntry& entry : gIndex)
            if (entry.mFormat == format && entry.mLevels > 1)
                return loadEntry(entry, "(self-test texture)");
        return nullptr;
    }
}
