#include "core/decode.hpp"

#include "core/text.hpp"

#include <cstring>
#include <stdexcept>

#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"

namespace setup
{
    namespace
    {
        uint32_t get32(const uint8_t* p)
        {
            return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        uint16_t get16(const uint8_t* p)
        {
            return uint16_t(p[0] | p[1] << 8);
        }

        constexpr uint32_t kDdpfAlphaPixels = 0x1;
        constexpr uint32_t kDdpfFourCC = 0x4;
        constexpr uint32_t kDdpfRgb = 0x40;
        constexpr uint32_t kDdpfLuminance = 0x20000;

        // Pillow's BcnDecode.c.
        struct Rgba
        {
            uint8_t r, g, b, a;
        };

        Rgba decode565(uint16_t x)
        {
            int r = (x & 0xf800) >> 8;
            r |= r >> 5;
            int g = (x & 0x7e0) >> 3;
            g |= g >> 6;
            int b = (x & 0x1f) << 3;
            b |= b >> 5;
            return { uint8_t(r), uint8_t(g), uint8_t(b), 0xff };
        }

        void decodeBc1Color(Rgba* dst, const uint8_t* src, bool separateAlpha)
        {
            const uint16_t c0 = get16(src), c1 = get16(src + 2);
            Rgba p[4];
            p[0] = decode565(c0);
            p[1] = decode565(c1);
            const int r0 = p[0].r, g0 = p[0].g, b0 = p[0].b, r1 = p[1].r, g1 = p[1].g, b1 = p[1].b;
            if (c0 > c1 || separateAlpha)
            {
                p[2] = { uint8_t((2 * r0 + r1) / 3), uint8_t((2 * g0 + g1) / 3), uint8_t((2 * b0 + b1) / 3), 0xff };
                p[3] = { uint8_t((r0 + 2 * r1) / 3), uint8_t((g0 + 2 * g1) / 3), uint8_t((b0 + 2 * b1) / 3), 0xff };
            }
            else
            {
                p[2] = { uint8_t((r0 + r1) / 2), uint8_t((g0 + g1) / 2), uint8_t((b0 + b1) / 2), 0xff };
                p[3] = { 0, 0, 0, 0 };
            }
            for (int n = 0; n < 4; ++n)
                for (int o = 0; o < 4; ++o)
                    dst[n * 4 + o] = p[3 & (src[4 + n] >> (2 * o))];
        }

        void decodeBc3Alpha(Rgba* dst, const uint8_t* src)
        {
            const unsigned a0 = src[0], a1 = src[1];
            const int lut1 = src[2] | (src[3] << 8) | (src[4] << 16);
            const int lut2 = src[5] | (src[6] << 8) | (src[7] << 16);
            uint8_t a[8];
            a[0] = uint8_t(a0);
            a[1] = uint8_t(a1);
            if (a0 > a1)
            {
                for (int i = 0; i < 6; ++i)
                    a[2 + i] = uint8_t(((6 - i) * a0 + (1 + i) * a1) / 7);
            }
            else
            {
                for (int i = 0; i < 4; ++i)
                    a[2 + i] = uint8_t(((4 - i) * a0 + (1 + i) * a1) / 5);
                a[6] = 0;
                a[7] = 0xff;
            }
            for (int n = 0; n < 8; ++n)
                dst[n].a = a[7 & (lut1 >> (3 * n))];
            for (int n = 0; n < 8; ++n)
                dst[8 + n].a = a[7 & (lut2 >> (3 * n))];
        }

        Image decodeDxt(const uint8_t* data, size_t size, int width, int height, int format)
        {
            Image image(width, height);
            const size_t blockBytes = format == 1 ? 8 : 16;
            const int blocksX = (width + 3) / 4, blocksY = (height + 3) / 4;
            if (size < blockBytes * size_t(blocksX) * size_t(blocksY))
                throw std::runtime_error("truncated DDS");
            const uint8_t* src = data;
            Rgba block[16];
            for (int by = 0; by < blocksY; ++by)
                for (int bx = 0; bx < blocksX; ++bx, src += blockBytes)
                {
                    if (format == 1)
                        decodeBc1Color(block, src, false);
                    else if (format == 3)
                    {
                        decodeBc1Color(block, src + 8, true);
                        for (int n = 0; n < 16; ++n)
                        {
                            int av = 0xf & (src[n * 4 >> 3] >> ((n * 4) & 7));
                            block[n].a = uint8_t((av << 4) | av);
                        }
                    }
                    else
                    {
                        decodeBc1Color(block, src + 8, true);
                        decodeBc3Alpha(block, src);
                    }
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x)
                        {
                            const int px = bx * 4 + x, py = by * 4 + y;
                            if (px < width && py < height)
                                std::memcpy(image.at(px, py), &block[y * 4 + x], 4);
                        }
                }
            return image;
        }

        Image decodeDds(const std::vector<uint8_t>& file)
        {
            if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0 || get32(&file[4]) != 124)
                throw std::runtime_error("not a DDS file");
            const uint8_t* header = &file[8]; // after the magic and the header size
            const int height = int(get32(header + 4));
            const int width = int(get32(header + 8));
            const uint32_t pfFlags = get32(header + 72), fourCC = get32(header + 76), bitCount = get32(header + 80);
            const uint8_t* data = file.data() + 128;
            const size_t size = file.size() - 128;
            if (pfFlags & kDdpfRgb)
            {
                const bool alpha = (pfFlags & kDdpfAlphaPixels) != 0;
                const int maskCount = alpha ? 4 : 3;
                uint32_t masks[4] = {};
                for (int i = 0; i < maskCount; ++i)
                    masks[i] = get32(header + 84 + 4 * i);
                // Pillow's DdsRgbDecoder: each channel shifted down and scaled by a float division, truncated.
                int offsets[4] = {};
                uint32_t totals[4] = {};
                for (int i = 0; i < maskCount; ++i)
                {
                    int offset = 0;
                    if (masks[i] != 0)
                        while (((masks[i] >> (offset + 1)) << (offset + 1)) == masks[i])
                            ++offset;
                    offsets[i] = offset;
                    totals[i] = masks[i] >> offset;
                }
                const size_t bytes = bitCount / 8;
                if (bytes == 0 || bytes > 4 || size < bytes * size_t(width) * size_t(height))
                    throw std::runtime_error("unsupported or truncated uncompressed DDS");
                Image image(width, height);
                image.mode = alpha ? SourceMode::Rgba : SourceMode::Rgb;
                const uint8_t* p = data;
                for (int i = 0; i < width * height; ++i, p += bytes)
                {
                    uint32_t value = 0;
                    for (size_t b = 0; b < bytes; ++b)
                        value |= uint32_t(p[b]) << (8 * b);
                    uint8_t* out = &image.pixels[size_t(i) * 4];
                    for (int c = 0; c < maskCount; ++c)
                        out[c] = totals[c] ? uint8_t(int((double((value & masks[c]) >> offsets[c]) / totals[c]) * 255))
                                           : 0;
                    if (!alpha)
                        out[3] = 255;
                }
                return image;
            }
            if (pfFlags & kDdpfLuminance)
            {
                const bool alpha = bitCount == 16 && (pfFlags & kDdpfAlphaPixels);
                if (bitCount != 8 && !alpha)
                    throw std::runtime_error("unsupported luminance DDS");
                const size_t bytes = alpha ? 2 : 1;
                if (size < bytes * size_t(width) * size_t(height))
                    throw std::runtime_error("truncated DDS");
                Image image(width, height);
                image.mode = alpha ? SourceMode::LuminanceAlpha : SourceMode::Luminance;
                for (int i = 0; i < width * height; ++i)
                {
                    uint8_t* out = &image.pixels[size_t(i) * 4];
                    out[0] = out[1] = out[2] = data[i * bytes];
                    out[3] = alpha ? data[i * bytes + 1] : 255;
                }
                return image;
            }
            if (pfFlags & kDdpfFourCC)
            {
                int format = 0;
                if (fourCC == 0x31545844) // DXT1
                    format = 1;
                else if (fourCC == 0x33545844) // DXT3
                    format = 3;
                else if (fourCC == 0x35545844) // DXT5
                    format = 5;
                else
                    throw std::runtime_error("unsupported DDS compression");
                return decodeDxt(data, size, width, height, format);
            }
            throw std::runtime_error("unsupported DDS pixel format");
        }

        Image decodeStb(const std::vector<uint8_t>& file)
        {
            int w = 0, h = 0, channels = 0;
            stbi_uc* pixels = stbi_load_from_memory(file.data(), int(file.size()), &w, &h, &channels, 4);
            if (pixels == nullptr)
                throw std::runtime_error(std::string("can't decode: ") + stbi_failure_reason());
            Image image(w, h);
            std::memcpy(image.pixels.data(), pixels, image.pixels.size());
            stbi_image_free(pixels);
            image.mode = channels == 4 ? SourceMode::Rgba
                : channels == 3        ? SourceMode::Rgb
                : channels == 2        ? SourceMode::LuminanceAlpha
                                       : SourceMode::Luminance;
            return image;
        }
    }

    Image decodeImage(const std::vector<uint8_t>& data, const std::string& name)
    {
        const std::string lower = toLowerAscii(name);
        if (lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".dds") == 0)
            return decodeDds(data);
        return decodeStb(data);
    }

    bool ddsIsUncompressed(const std::vector<uint8_t>& data)
    {
        return data.size() >= 88 && std::memcmp(data.data(), "DDS ", 4) == 0 && get32(&data[84]) == 0;
    }

    bool tgaIgnoresAlpha(const std::vector<uint8_t>& data)
    {
        if (data.size() < 18)
            return false;
        const uint8_t imageType = data[2];
        const uint8_t depth = (imageType == 1 || imageType == 9) ? data[7] : data[16];
        return depth == 16 && (data[17] & 0x0F) == 1;
    }
}
