#include "core/image.hpp"

#include <algorithm>
#include <cstring>

namespace setup
{
    Image flipVertical(const Image& image)
    {
        Image out(image.width, image.height);
        out.mode = image.mode;
        const size_t row = size_t(image.width) * 4;
        for (int y = 0; y < image.height; ++y)
            std::memcpy(out.at(0, image.height - 1 - y), image.at(0, y), row);
        return out;
    }

    Image transposed(const Image& image)
    {
        Image out(image.height, image.width);
        out.mode = image.mode;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                std::memcpy(out.at(y, x), image.at(x, y), 4);
        return out;
    }

    Image cropped(const Image& image, int w, int h)
    {
        Image out(w, h);
        out.mode = image.mode;
        for (int y = 0; y < h; ++y)
            std::memcpy(out.at(0, y), image.at(0, y), size_t(w) * 4);
        return out;
    }

    Image paddedEdge(const Image& image, int w, int h)
    {
        return paddedEdgeAround(image, 0, 0, w - image.width, h - image.height);
    }

    Image paddedEdgeAround(const Image& image, int left, int top, int right, int bottom)
    {
        Image out(image.width + left + right, image.height + top + bottom);
        out.mode = image.mode;
        for (int y = 0; y < out.height; ++y)
        {
            const int sy = std::clamp(y - top, 0, image.height - 1);
            for (int x = 0; x < out.width; ++x)
            {
                const int sx = std::clamp(x - left, 0, image.width - 1);
                std::memcpy(out.at(x, y), image.at(sx, sy), 4);
            }
        }
        return out;
    }
}
