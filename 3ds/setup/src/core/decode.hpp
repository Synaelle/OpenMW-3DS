#pragma once

#include "core/image.hpp"

#include <string>
#include <vector>

namespace setup
{
    // Decodes a texture file (.dds, .tga, .bmp, .png, .jpg) to RGBA as Pillow does (the cooker's reference): DDS
    // DXT1/DXT3/DXT5, uncompressed (by its channel masks) and luminance, the others through stb_image. `name` gives
    // the format by its extension. Throws std::runtime_error when the file can't be decoded.
    Image decodeImage(const std::vector<uint8_t>& data, const std::string& name);

    // An uncompressed DDS (no FourCC): the cooker keeps small ones in RGBA8 (ETC1 artifacts show on them).
    bool ddsIsUncompressed(const std::vector<uint8_t>& data);

    // A 16-bit TGA with one attribute bit: Morrowind ignores that alpha (as OpenMW's ImageManager does).
    bool tgaIgnoresAlpha(const std::vector<uint8_t>& data);
}
