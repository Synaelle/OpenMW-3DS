#include "render3ds.hpp"

#include "gpumemory.hpp"

#include <components/debug/debuglog.hpp>

#include <platform3ds/log.hpp>

#include <osg/Texture2D>

#include <3ds.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
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
        // 2: the UI atlas (FLAG_ATLAS entries, "ui-atlas/<n>"). 3: one 512x256 atlas with the reserved icon and map
        // regions. 4: the TrueType fonts' glyphs in it (the font and glyph tables).
        constexpr uint32_t kVersion = 4;

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
            uint8_t mAtlas; // FLAG_ATLAS: which "ui-atlas/<n>" entry also holds this image
            uint32_t mAtlasPos; // FLAG_ATLAS: x | y << 16 in that atlas
        } __attribute__((packed));
        static_assert(sizeof(PackEntry) == 32);

        // The texture pack (cooked/textures.o3tp: Morrowind's textures and the interface atlas). The expansions'
        // packs of their own went with their support in M20 (back with M11, git history).
        std::mutex gPackMutex;
        FILE* gPack = nullptr;
        std::vector<PackEntry> gIndex; // sorted by hash

        // The pack's entry of a hash, or nullptr.
        const PackEntry* findEntry(uint64_t hash)
        {
            const auto it = std::lower_bound(gIndex.begin(), gIndex.end(), hash,
                [](const PackEntry& e, uint64_t value) { return e.mHash < value; });
            return it != gIndex.end() && it->mHash == hash ? &*it : nullptr;
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
        return linearAlloc(size); // locked by platform3ds's wrapper
    }

    namespace
    {
        // VRAM (6 MB at 0x1F000000): the render targets and the top screen's interface overlay take ~1 MB. Scene
        // textures fill the rest when it has room, which leaves that much linear memory to the heap's large pool
        // (M9). vramAlloc has no lock: one here.
        constexpr uintptr_t kVramBegin = 0x1F000000, kVramEnd = 0x1F600000;
        constexpr size_t kVramMargin = 384 * 1024; // left free for render targets created later
        std::mutex gVramMutex;
        std::vector<CookedImage*> gVramImages; // reloaded after applets

        bool inVram(const void* ptr)
        {
            return uintptr_t(ptr) >= kVramBegin && uintptr_t(ptr) < kVramEnd;
        }
    }

    void gpuFree(void* ptr)
    {
        if (ptr == nullptr)
            return;
        if (inVram(ptr))
        {
            std::lock_guard<std::mutex> lock(gVramMutex);
            vramFree(ptr);
            return;
        }
        linearFree(ptr);
    }

    CookedImage::CookedImage(void* data, uint32_t size, uint16_t width, uint16_t height, uint16_t srcWidth,
        uint16_t srcHeight, GPU_TEXCOLOR format, uint8_t levels, uint8_t flags, uint8_t atlas, uint32_t atlasPos)
        : mGpuData(data)
        , mGpuSize(size)
        , mGpuFormat(format)
        , mLevels(levels)
        , mFlags(flags)
        , mSrcWidth(srcWidth)
        , mSrcHeight(srcHeight)
        , mAtlas(atlas)
        , mAtlasPos(atlasPos)
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
        if (inVram(mGpuData))
        {
            std::lock_guard<std::mutex> lock(gVramMutex);
            gVramImages.erase(std::find(gVramImages.begin(), gVramImages.end(), this));
        }
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
        // The header's reserved words held the atlas fonts and the GPU interface's reserved atlas region (packs
        // cooked before M20): not read any more.
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
        // Scene textures (not interface images: the CPU interface reads those) go to VRAM through a heap buffer
        // (the file system can't write VRAM). nullptr when VRAM has no room.
        // The CPU writes VRAM since packaging/app.rsf maps it writable (it was read-only: hardware test #71 faulted
        // in the memcpy below).
        constexpr bool kTexturesInVram = true;

        CookedImage* loadIntoVram(const PackEntry& entry)
        {
            if (!kTexturesInVram || (entry.mFlags & (4 | 8)) != 0)
                return nullptr;
            void* vram = nullptr;
            {
                std::lock_guard<std::mutex> lock(gVramMutex);
                if (vramSpaceFree() < entry.mSize + kVramMargin)
                    return nullptr;
                vram = vramAlloc(entry.mSize);
            }
            if (vram == nullptr)
                return nullptr;
            std::vector<uint8_t> staging;
            bool ok = false;
            try
            {
                staging.resize(entry.mSize);
                std::lock_guard<std::mutex> lock(gPackMutex);
                FILE* file = gPack;
                ok = std::fseek(file, static_cast<long>(entry.mOffset), SEEK_SET) == 0
                    && std::fread(staging.data(), 1, entry.mSize, file) == entry.mSize;
            }
            catch (const std::bad_alloc&)
            {
            }
            if (!ok)
            {
                gpuFree(vram);
                return nullptr;
            }
            std::memcpy(vram, staging.data(), entry.mSize);
            GSPGPU_FlushDataCache(vram, entry.mSize);
            auto* image = new CookedImage(vram, entry.mSize, entry.mWidth, entry.mHeight, entry.mSrcWidth,
                entry.mSrcHeight, static_cast<GPU_TEXCOLOR>(entry.mFormat), entry.mLevels, entry.mFlags,
                entry.mAtlas, entry.mAtlasPos);
            image->setPackOffset(entry.mOffset);
            std::lock_guard<std::mutex> lock(gVramMutex);
            gVramImages.push_back(image);
            return image;
        }

        // Texture tier-down, the last resort (M9): when linear memory runs low (the heap's large pool took it, or
        // the GPU did), a scene texture loads from its second mip level, a quarter of the memory.
        PackEntry tierDown(const PackEntry& entry)
        {
            constexpr size_t kLowLinear = 6u * 1024u * 1024u; // under the large pool's floor: the GPU itself is short
            if ((entry.mFlags & (4 | 8)) != 0 || entry.mLevels < 2 || entry.mWidth < 16 || entry.mHeight < 16
                || linearSpaceFree() >= kLowLinear)
                return entry;
            const uint32_t top = uint32_t(entry.mWidth) * entry.mHeight
                * bitsPerPixel(static_cast<GPU_TEXCOLOR>(entry.mFormat)) / 8;
            if (top >= entry.mSize)
                return entry;
            PackEntry smaller = entry;
            smaller.mOffset += top;
            smaller.mSize -= top;
            smaller.mWidth /= 2;
            smaller.mHeight /= 2;
            smaller.mLevels -= 1;
            static unsigned count = 0;
            if (count++ % 50 == 0)
                P3DS_LOGW("render3ds: linear memory low (%u KB): textures load at half size (%u so far)",
                    unsigned(linearSpaceFree() / 1024), count);
            return smaller;
        }

        osg::ref_ptr<osg::Image> loadEntry(const PackEntry& fullEntry, std::string_view name)
        {
            const PackEntry entry = tierDown(fullEntry);
            if (CookedImage* inVramImage = loadIntoVram(entry))
            {
                osg::ref_ptr<osg::Image> image = inVramImage;
                image->setFileName(std::string(name));
                return image;
            }
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
                FILE* file = gPack;
                ok = std::fseek(file, static_cast<long>(entry.mOffset), SEEK_SET) == 0
                    && std::fread(data, 1, entry.mSize, file) == entry.mSize;
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
                entry.mFlags, entry.mAtlas, entry.mAtlasPos);
            image->setFileName(std::string(name));
            return image;
        }
    }

    void reloadVramTextures()
    {
        std::lock_guard<std::mutex> vramLock(gVramMutex);
        std::vector<uint8_t> staging;
        size_t bytes = 0;
        for (CookedImage* image : gVramImages)
        {
            try
            {
                staging.resize(image->gpuSize());
            }
            catch (const std::bad_alloc&)
            {
                break;
            }
            std::lock_guard<std::mutex> lock(gPackMutex);
            FILE* file = gPack;
            if (std::fseek(file, static_cast<long>(image->packOffset()), SEEK_SET) == 0
                && std::fread(staging.data(), 1, image->gpuSize(), file) == image->gpuSize())
            {
                std::memcpy(image->gpuData(), staging.data(), image->gpuSize());
                GSPGPU_FlushDataCache(image->gpuData(), image->gpuSize());
                bytes += image->gpuSize();
            }
        }
        P3DS_LOGI("render3ds: %u textures in VRAM read again (%u KB)", unsigned(gVramImages.size()),
            unsigned(bytes / 1024));
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
        const PackEntry* entry = findEntry(fnv1a64(stripExtension(normalizedPath)));
        if (entry == nullptr)
            return nullptr;
        return loadEntry(*entry, normalizedPath);
    }

    namespace
    {
        std::vector<osg::ref_ptr<osg::Texture2D>> gAtlasTextures;
        std::vector<bool> gAtlasMissing;
    }

    osg::Texture* uiAtlasTexture(unsigned index)
    {
        if (index >= gAtlasTextures.size())
        {
            gAtlasTextures.resize(index + 1);
            gAtlasMissing.resize(index + 1, false);
        }
        if (gAtlasTextures[index] != nullptr)
            return gAtlasTextures[index].get();
        if (gAtlasMissing[index])
            return nullptr;
        const std::string key = "ui-atlas/" + std::to_string(index);
        osg::ref_ptr<osg::Image> image = loadCookedImage(key);
        if (image == nullptr)
        {
            gAtlasMissing[index] = true;
            P3DS_LOGW("render3ds: no UI atlas %u in the texture pack (interface textures bound one by one)", index);
            return nullptr;
        }
        osg::ref_ptr<osg::Texture2D> texture = new osg::Texture2D(image);
        texture->setWrap(osg::Texture::WRAP_S, osg::Texture::CLAMP_TO_EDGE);
        texture->setWrap(osg::Texture::WRAP_T, osg::Texture::CLAMP_TO_EDGE);
        texture->setFilter(osg::Texture::MIN_FILTER, osg::Texture::LINEAR);
        texture->setFilter(osg::Texture::MAG_FILTER, osg::Texture::LINEAR);
        gAtlasTextures[index] = texture;
        P3DS_LOGI("render3ds: UI atlas %u: %dx%d", index, image->s(), image->t());
        return texture.get();
    }

    osg::ref_ptr<osg::Image> loadCookedImageWithFormat(GPU_TEXCOLOR format)
    {
        for (const PackEntry& entry : gIndex)
            if (entry.mFormat == format && entry.mLevels > 1)
                return loadEntry(entry, "(self-test texture)");
        return nullptr;
    }
}
