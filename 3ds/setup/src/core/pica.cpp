#include "core/pica.hpp"

#include "rg_etc1/rg_etc1.h"

#include <mutex>
#include <stdexcept>

namespace setup
{
    namespace
    {
        // A level as tex3ds holds it: ImageMagick quanta (0..65535, not rounded: an HDRI build) per channel.
        struct Level
        {
            int width = 0, height = 0;
            std::vector<double> rgba;

            const double* at(int x, int y) const { return &rgba[(size_t(y) * size_t(width) + size_t(x)) * 4]; }
        };

        constexpr double kQuantumRange = 65535.0;

        // quantum_to_bits<bits> (tex3ds quantum.h): (1 << bits) * v / (QuantumRange + 1), truncated.
        template <int bits>
        uint8_t toBits(double v)
        {
            const double r = double(1 << bits) * v / (kQuantumRange + 1);
            return r <= 0 ? 0 : uint8_t(r);
        }

        // load_image: the image over a transparent one. Alpha 0 leaves black; otherwise the colour is unchanged.
        Level baseLevel(const Image& image)
        {
            Level level;
            level.width = image.width;
            level.height = image.height;
            level.rgba.resize(image.pixels.size());
            for (size_t i = 0; i < image.pixels.size(); i += 4)
            {
                const uint8_t a = image.pixels[i + 3];
                for (int c = 0; c < 3; ++c)
                    level.rgba[i + c] = a == 0 ? 0.0 : image.pixels[i + c] * 257.0;
                level.rgba[i + 3] = a * 257.0;
            }
            return level;
        }

        // ImageMagick's resize with the box filter from the base level to width x height (a whole power of two
        // smaller): each texel the average of its block, the colours weighted by alpha.
        Level boxLevel(const Level& base, int width, int height)
        {
            Level level;
            level.width = width;
            level.height = height;
            level.rgba.resize(size_t(width) * size_t(height) * 4);
            const int fx = base.width / width, fy = base.height / height;
            const double count = double(fx) * double(fy);
            for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                {
                    double alphaWeight = 0, color[3] = { 0, 0, 0 }, alpha = 0;
                    for (int by = 0; by < fy; ++by)
                        for (int bx = 0; bx < fx; ++bx)
                        {
                            const double* p = base.at(x * fx + bx, y * fy + by);
                            const double a = p[3] / kQuantumRange;
                            alphaWeight += a;
                            for (int c = 0; c < 3; ++c)
                                color[c] += a * p[c];
                            alpha += p[3];
                        }
                    double* out = &level.rgba[(size_t(y) * size_t(width) + size_t(x)) * 4];
                    for (int c = 0; c < 3; ++c)
                        out[c] = alphaWeight > 0 ? color[c] / alphaWeight : 0.0;
                    out[3] = alpha / count;
                }
            return level;
        }

        void encodeEtc1(const Level& level, bool alpha, std::vector<uint8_t>& out)
        {
            rg_etc1::etc1_pack_params params;
            params.clear();
            params.m_quality = rg_etc1::cMediumQuality;
            for (int ty = 0; ty < level.height; ty += 8)
                for (int tx = 0; tx < level.width; tx += 8)
                    for (int j = 0; j < 8; j += 4)
                        for (int i = 0; i < 8; i += 4)
                        {
                            uint8_t block[4 * 4 * 4];
                            uint8_t packed[8];
                            uint8_t alphaBits[8] = {};
                            for (int y = 0; y < 4; ++y)
                                for (int x = 0; x < 4; ++x)
                                {
                                    const double* p = level.at(tx + i + x, ty + j + y);
                                    uint8_t* b = &block[y * 16 + x * 4];
                                    b[0] = toBits<8>(p[0]);
                                    b[1] = toBits<8>(p[1]);
                                    b[2] = toBits<8>(p[2]);
                                    b[3] = 0xFF;
                                    // 4-bit alpha, the X and Y axes swapped.
                                    alphaBits[2 * x + y / 2] |= uint8_t(toBits<4>(p[3]) << ((y & 1) ? 4 : 0));
                                }
                            rg_etc1::pack_etc1_block(packed, reinterpret_cast<const unsigned*>(block), params);
                            if (alpha)
                                out.insert(out.end(), alphaBits, alphaBits + 8);
                            // rg_etc1 writes big-endian; the GPU reads little-endian.
                            for (int b = 7; b >= 0; --b)
                                out.push_back(packed[b]);
                        }
        }

        // tex3ds swizzle.cpp: the 8x8 tile's texels moved into Morton order by rotating these foursomes, then
        // swapping four pairs.
        const int* mortonOrder()
        {
            static int order[64];
            static std::once_flag once;
            std::call_once(once, [] {
                static const unsigned char table[][4] = {
                    { 2, 8, 16, 4 }, { 3, 9, 17, 5 }, { 6, 10, 24, 20 }, { 7, 11, 25, 21 },
                    { 14, 26, 28, 22 }, { 15, 27, 29, 23 }, { 34, 40, 48, 36 }, { 35, 41, 49, 37 },
                    { 38, 42, 56, 52 }, { 39, 43, 57, 53 }, { 46, 58, 60, 54 }, { 47, 59, 61, 55 },
                };
                int p[64];
                for (int i = 0; i < 64; ++i)
                    p[i] = i;
                for (const auto& e : table)
                {
                    const int tmp = p[e[0]];
                    p[e[0]] = p[e[1]];
                    p[e[1]] = p[e[2]];
                    p[e[2]] = p[e[3]];
                    p[e[3]] = tmp;
                }
                std::swap(p[12], p[18]);
                std::swap(p[13], p[19]);
                std::swap(p[44], p[50]);
                std::swap(p[45], p[51]);
                for (int i = 0; i < 64; ++i)
                    order[i] = p[i]; // output slot i holds the tile texel p[i] (y * 8 + x)
            });
            return order;
        }

        void encodeRgba8(const Level& level, std::vector<uint8_t>& out)
        {
            const int* order = mortonOrder();
            for (int ty = 0; ty < level.height; ty += 8)
                for (int tx = 0; tx < level.width; tx += 8)
                    for (int slot = 0; slot < 64; ++slot)
                    {
                        const int t = order[slot];
                        const double* p = level.at(tx + t % 8, ty + t / 8);
                        out.push_back(toBits<8>(p[3]));
                        out.push_back(toBits<8>(p[2]));
                        out.push_back(toBits<8>(p[1]));
                        out.push_back(toBits<8>(p[0]));
                    }
        }
    }

    int bitsPerPixel(PicaFormat format)
    {
        return format == PicaFormat::Rgba8 ? 32 : format == PicaFormat::Etc1 ? 4 : 8;
    }

    void initPicaEncoder()
    {
        static std::once_flag once;
        std::call_once(once, [] { rg_etc1::pack_etc1_block_init(); });
    }

    std::vector<uint8_t> encodePica(const Image& image, PicaFormat format, bool mipmaps, int& levels)
    {
        if (image.width < 8 || image.height < 8 || image.width % 8 || image.height % 8)
            throw std::runtime_error("texture sides must be multiples of 8");
        const Level base = baseLevel(image);
        std::vector<uint8_t> out;
        out.reserve(size_t(image.width) * size_t(image.height) * size_t(bitsPerPixel(format)) / 8 * 4 / 3 + 64);
        levels = 0;
        int w = image.width, h = image.height;
        while (true)
        {
            const Level level = levels == 0 ? Level() : boxLevel(base, w, h);
            const Level& current = levels == 0 ? base : level;
            if (format == PicaFormat::Rgba8)
                encodeRgba8(current, out);
            else
                encodeEtc1(current, format == PicaFormat::Etc1A4, out);
            ++levels;
            if (!mipmaps || w <= 8 || h <= 8)
                break;
            w /= 2;
            h /= 2;
        }
        return out;
    }
}
