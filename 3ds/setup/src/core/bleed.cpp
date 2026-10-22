#include "core/bleed.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace setup
{
    namespace
    {
        // numpy's pairwise summation of a float32 array (the 1-D sum of the alpha weights).
        float pairwiseSum(const float* a, size_t n)
        {
            if (n < 8)
            {
                float res = 0.f;
                for (size_t i = 0; i < n; ++i)
                    res += a[i];
                return res;
            }
            if (n <= 128)
            {
                float r[8];
                for (int j = 0; j < 8; ++j)
                    r[j] = a[j];
                size_t i = 8;
                for (; i < n - (n % 8); i += 8)
                    for (int j = 0; j < 8; ++j)
                        r[j] += a[i + j];
                float res = ((r[0] + r[1]) + (r[2] + r[3])) + ((r[4] + r[5]) + (r[6] + r[7]));
                for (; i < n; ++i)
                    res += a[i];
                return res;
            }
            size_t n2 = n / 2;
            n2 -= n2 % 8;
            return pairwiseSum(a, n2) + pairwiseSum(a + n2, n - n2);
        }
    }

    Image bleedTransparent(const Image& image, bool keepAlphaZero)
    {
        const int w = image.width, h = image.height;
        const size_t n = size_t(w) * size_t(h);
        std::vector<char> clear(n);
        size_t clearCount = 0;
        for (size_t i = 0; i < n; ++i)
        {
            clear[i] = image.pixels[i * 4 + 3] == 0;
            clearCount += size_t(clear[i]);
        }
        if (clearCount == 0 || clearCount == n)
            return image;

        std::vector<float> weight(n), color(n * 3);
        for (size_t i = 0; i < n; ++i)
        {
            weight[i] = float(image.pixels[i * 4 + 3]) / 255.0f;
            for (int c = 0; c < 3; ++c)
                color[i * 3 + c] = float(image.pixels[i * 4 + c]) * weight[i];
        }
        std::vector<char> filled(n);
        for (size_t i = 0; i < n; ++i)
            filled[i] = !clear[i];

        std::vector<float> colorSum(n * 3), weightSum(n);
        std::vector<char> grow(n);
        for (int iteration = 0; iteration < 8; ++iteration)
        {
            if (std::all_of(filled.begin(), filled.end(), [](char f) { return f != 0; }))
                break;
            std::fill(colorSum.begin(), colorSum.end(), 0.f);
            std::fill(weightSum.begin(), weightSum.end(), 0.f);
            // np.roll(a, (dy, dx)): out[y][x] = a[y - dy][x - dx], wrapping; summed in the loops' order.
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    for (int y = 0; y < h; ++y)
                    {
                        const int sy = ((y - dy) % h + h) % h;
                        for (int x = 0; x < w; ++x)
                        {
                            const int sx = ((x - dx) % w + w) % w;
                            const size_t o = size_t(y) * size_t(w) + size_t(x), s = size_t(sy) * size_t(w) + size_t(sx);
                            for (int c = 0; c < 3; ++c)
                                colorSum[o * 3 + c] += color[s * 3 + c];
                            weightSum[o] += weight[s];
                        }
                    }
            bool any = false;
            for (size_t i = 0; i < n; ++i)
            {
                grow[i] = !filled[i] && weightSum[i] > 0.f;
                any |= grow[i] != 0;
            }
            if (!any)
                break;
            for (size_t i = 0; i < n; ++i)
            {
                if (!grow[i])
                    continue;
                const float newWeight = weightSum[i] / 18.0f;
                for (int c = 0; c < 3; ++c)
                    color[i * 3 + c] = colorSum[i * 3 + c] / weightSum[i] * newWeight;
                weight[i] = newWeight;
                filled[i] = 1;
            }
        }

        // The visible texels' alpha-weighted mean colour, for the texels nothing reached: the colour sums run
        // sequentially (numpy reduces along the outer axis), the weights' sum pairwise (a 1-D reduction).
        float meanSum[3] = { 0.f, 0.f, 0.f };
        std::vector<float> visibleWeights;
        visibleWeights.reserve(n - clearCount);
        for (size_t i = 0; i < n; ++i)
        {
            if (clear[i])
                continue;
            const float a = float(image.pixels[i * 4 + 3]) / 255.0f;
            visibleWeights.push_back(a);
            for (int c = 0; c < 3; ++c)
                meanSum[c] += float(image.pixels[i * 4 + c]) * a;
        }
        const float total = std::max(1e-6f, pairwiseSum(visibleWeights.data(), visibleWeights.size()));
        float mean[3];
        for (int c = 0; c < 3; ++c)
            mean[c] = meanSum[c] / total;

        Image out = image;
        for (size_t i = 0; i < n; ++i)
        {
            if (!clear[i])
                continue;
            for (int c = 0; c < 3; ++c)
            {
                const float v = filled[i] ? color[i * 3 + c] / std::max(weight[i], 1e-6f) : mean[c];
                out.pixels[i * 4 + c] = uint8_t(std::clamp(std::nearbyint(v), 0.f, 255.f));
            }
            if (!keepAlphaZero)
                out.pixels[i * 4 + 3] = 1;
        }
        return out;
    }
}
