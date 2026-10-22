#pragma once

#include "core/image.hpp"

#include <cstdint>
#include <vector>

namespace setup
{
    // GPU_TEXCOLOR values (libctru).
    enum class PicaFormat : uint8_t
    {
        Rgba8 = 0x0,
        Etc1 = 0xC,
        Etc1A4 = 0xD,
    };

    int bitsPerPixel(PicaFormat format);

    // Must be called once before encoding ETC1 (the encoder's tables).
    void initPicaEncoder();

    // Encodes an image (power-of-two sides of at least 8, bottom row first: the cooker flips it) as the 3DS GPU
    // reads it, with tex3ds's arithmetic (devkitPro's texture tool, the cooker's reference): every pixel composited
    // over transparent black first (alpha 0 texels become black), mip levels down to a side of 8 when `mipmaps`
    // (each one the alpha-weighted average of the base level's blocks, as ImageMagick's box filter gives it), ETC1
    // at medium quality in 8x8 tiles of four 4x4 blocks (ETC1A4: each block's 4-bit alpha first), RGBA8 in Morton
    // order, A B G R. Returns the levels back to back; `levels` gets their count.
    std::vector<uint8_t> encodePica(const Image& image, PicaFormat format, bool mipmaps, int& levels);
}
