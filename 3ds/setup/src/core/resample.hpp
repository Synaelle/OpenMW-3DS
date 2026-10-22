#pragma once

#include "core/image.hpp"

namespace setup
{
    enum class Filter
    {
        Bicubic,
        Lanczos,
    };

    // Image.resize as Pillow 12 does it (the cooker's reference): RGBA premultiplied by alpha around the resampling,
    // 22-bit fixed-point coefficients, the horizontal pass first, a pass only along an axis whose size changes. A
    // palette source is resized nearest-neighbour, as in Pillow.
    Image resize(const Image& image, int width, int height, Filter filter);
}
