#pragma once

#include "core/image.hpp"

namespace setup
{
    // Colours the fully transparent texels with the alpha-weighted colour of the visible texels nearby (up to 8
    // texels away, wrapping like a repeating texture), then the texture's average colour. The encoder replaces the
    // colour of alpha-0 texels with black, which bilinear filtering and mipmaps blend into the visible edges (dark
    // fringes, black raindrops). Unless `keepAlphaZero` (RGBA8, where alpha 1 would show), those texels get alpha 1
    // instead, which keeps their colour and still encodes as 0 in ETC1A4's 4-bit alpha.
    // The arithmetic follows the Python cooker's numpy code in float32, operation for operation.
    Image bleedTransparent(const Image& image, bool keepAlphaZero);
}
