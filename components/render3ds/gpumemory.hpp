#ifndef OPENMW_COMPONENTS_RENDER3DS_GPUMEMORY_H
#define OPENMW_COMPONENTS_RENDER3DS_GPUMEMORY_H

// Internal to components/render3ds.

#include <osg/Image>

#include <3ds/gpu/enums.h>

#include <cstddef>
#include <cstdint>

namespace Render3DS
{
    // linearAlloc/linearFree behind one lock: textures load on any thread (ImageManager), geometry is
    // uploaded on the main thread.
    void* gpuAlloc(size_t size);
    void gpuFree(void* ptr);

    // An image from the cooked pack. Its data (all mip levels, PICA tiled layout) lives in linear memory
    // and is what the GPU samples, so it is freed with the image.
    //
    // For OSG the image looks like a compressed texture of the cooked size: ETC1 is GL_ETC1_RGB8_OES,
    // ETC1A4 (same 16 bytes per 4x4 block) GL_COMPRESSED_RGBA8_ETC2_EAC, RGBA8 GL_RGBA. CPU-side readers
    // (getColor, etc.) only ever see in-bounds data that isn't in the layout they expect.
    class CookedImage : public osg::Image
    {
    public:
        CookedImage(void* data, uint32_t size, uint16_t width, uint16_t height, GPU_TEXCOLOR format,
            uint8_t levels, uint8_t flags);

        void* gpuData() const { return mGpuData; }
        uint32_t gpuSize() const { return mGpuSize; }
        GPU_TEXCOLOR gpuFormat() const { return mGpuFormat; }
        uint8_t levels() const { return mLevels; }
        uint8_t flags() const { return mFlags; } // tools/cook_textures.py FLAG_*

    protected:
        ~CookedImage() override;

    private:
        void* mGpuData;
        uint32_t mGpuSize;
        GPU_TEXCOLOR mGpuFormat;
        uint8_t mLevels;
        uint8_t mFlags;
    };
}

#endif
