#pragma once

#include <cstdint>
#include <vector>

namespace setup
{
    // How the source stored its pixels, where it changes what Pillow does with them (resizing a palette image is
    // nearest-neighbour, as in Pillow).
    enum class SourceMode
    {
        Rgba,
        Rgb,
        Luminance,
        LuminanceAlpha,
        Palette,
    };

    // 8-bit RGBA, top row first.
    struct Image
    {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> pixels;
        SourceMode mode = SourceMode::Rgba;

        Image() = default;
        Image(int w, int h)
            : width(w)
            , height(h)
            , pixels(size_t(w) * size_t(h) * 4, 0)
        {
        }

        uint8_t* at(int x, int y) { return &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4]; }
        const uint8_t* at(int x, int y) const { return &pixels[(size_t(y) * size_t(width) + size_t(x)) * 4]; }
    };

    Image flipVertical(const Image& image);
    Image transposed(const Image& image);
    // The top-left w x h of the image.
    Image cropped(const Image& image, int w, int h);
    // Grows the image to w x h by repeating its last column and row (numpy's pad mode "edge").
    Image paddedEdge(const Image& image, int w, int h);
    // Pads with `left`/`top`/`right`/`bottom` texels repeating the edge on each side.
    Image paddedEdgeAround(const Image& image, int left, int top, int right, int bottom);
}
