#ifndef OPENMW_COMPONENTS_RENDER3DS_GPUMEMORY_H
#define OPENMW_COMPONENTS_RENDER3DS_GPUMEMORY_H

// Internal to components/render3ds.

#include <osg/Image>

#include <3ds/gpu/enums.h>

#include <cstddef>
#include <cstdint>

namespace osg
{
    class Texture;
}

namespace Render3DS
{
    // linearAlloc/linearFree behind one lock: textures load on any thread (ImageManager), geometry is
    // uploaded on the main thread.
    void* gpuAlloc(size_t size);
    // Frees linear or VRAM memory (scene textures live in VRAM when it has room, see loadEntry).
    void gpuFree(void* ptr);
    // Reads the textures kept in VRAM again from the texture pack: applet transitions (the keyboard, HOME Menu)
    // write the screens' capture into VRAM. Main thread, between frames.
    void reloadVramTextures();

    // The first mipmapped texture of that format in the cooked pack (GPU self-test).
    osg::ref_ptr<osg::Image> loadCookedImageWithFormat(GPU_TEXCOLOR format);

    // An image from the cooked pack. Its data (all mip levels, PICA tiled layout) lives in linear memory
    // and is what the GPU samples, so it is freed with the image.
    //
    // For OSG the image looks like a compressed texture of the cooked size: ETC1 is GL_ETC1_RGB8_OES,
    // ETC1A4 (same 16 bytes per 4x4 block) GL_COMPRESSED_RGBA8_ETC2_EAC, RGBA8 GL_RGBA. CPU-side readers
    // (getColor, etc.) only ever see in-bounds data that isn't in the layout they expect.
    class CookedImage : public osg::Image
    {
    public:
        CookedImage(void* data, uint32_t size, uint16_t width, uint16_t height, uint16_t srcWidth,
            uint16_t srcHeight, GPU_TEXCOLOR format, uint8_t levels, uint8_t flags, uint8_t atlas = 0,
            uint32_t atlasPos = 0);

        void* gpuData() const { return mGpuData; }
        uint32_t gpuSize() const { return mGpuSize; }
        GPU_TEXCOLOR gpuFormat() const { return mGpuFormat; }
        uint8_t levels() const { return mLevels; }
        uint8_t flags() const { return mFlags; } // tools/cook_textures.py FLAG_*
        // The original image's size. MyGUI sizes skins with it and divides pixel coordinates by it.
        uint16_t sourceWidth() const { return mSrcWidth; }
        uint16_t sourceHeight() const { return mSrcHeight; }
        // UI images are padded, not resampled (FLAG_PADDED): the image fills sourceWidth x sourceHeight from the
        // top left, so texture coordinates over the image are scaled by source / cooked size.
        bool padded() const { return (mFlags & 4) != 0; }
        // Small UI images are also packed into a UI atlas (FLAG_ATLAS, uiAtlasTexture): the image's (u, v) is at
        // atlas texel (atlasX + u * sourceWidth, atlasY + v * sourceHeight), or with the axes swapped when it is
        // stored transposed (atlasX + v * sourceHeight, atlasY + u * sourceWidth). A 1-texel repeated-edge gutter
        // surrounds it. This image's own data is still the standalone texture (for skins that tile).
        bool inAtlas() const { return (mFlags & 8) != 0; }
        bool atlasTransposed() const { return (mFlags & 16) != 0; }
        bool atlasHalved() const { return (mFlags & 32) != 0; } // the atlas copy is half the size on each side
        uint8_t atlasIndex() const { return mAtlas; }
        uint16_t atlasX() const { return static_cast<uint16_t>(mAtlasPos & 0xFFFF); }
        uint16_t atlasY() const { return static_cast<uint16_t>(mAtlasPos >> 16); }

        // Set for an image in VRAM: where its data is in the texture pack (reloadVramTextures).
        void setPackOffset(uint32_t offset) { mPackOffset = offset; }
        uint32_t packOffset() const { return mPackOffset; }

    protected:
        ~CookedImage() override;

    private:
        uint32_t mPackOffset = 0;
        void* mGpuData;
        uint32_t mGpuSize;
        GPU_TEXCOLOR mGpuFormat;
        uint8_t mLevels;
        uint8_t mFlags;
        uint16_t mSrcWidth;
        uint16_t mSrcHeight;
        uint8_t mAtlas;
        uint32_t mAtlasPos;
    };

    // The UI atlas texture (pack entry "ui-atlas/<index>", loaded on first use and kept: clamped, bilinear, no
    // mipmaps), or nullptr when the pack has none. Render thread.
    osg::Texture* uiAtlasTexture(unsigned index);
}

#endif
