#include <platform3ds/screenshot.hpp>
#include <platform3ds/textfb.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace p3ds::screenshot
{
    namespace
    {
        void put16(u8* p, uint16_t v)
        {
            p[0] = u8(v);
            p[1] = u8(v >> 8);
        }
        void put32(u8* p, uint32_t v)
        {
            put16(p, uint16_t(v));
            put16(p + 2, uint16_t(v >> 16));
        }
    }

    bool saveRotatedFramebuffer(const char* path, const void* data, int w, int h, Format fmt)
    {
        const int rowBytes = (w * 3 + 3) & ~3;
        const uint32_t imageBytes = uint32_t(rowBytes * h);
        u8 header[54] = {};
        header[0] = 'B';
        header[1] = 'M';
        put32(header + 2, 54 + imageBytes);
        put32(header + 10, 54);
        put32(header + 14, 40);
        put32(header + 18, uint32_t(w));
        put32(header + 22, uint32_t(h)); // positive: bottom-up rows
        put16(header + 26, 1);
        put16(header + 28, 24);
        put32(header + 34, imageBytes);

        FILE* f = fopen(path, "wb");
        if (f == nullptr)
            return false;
        fwrite(header, 1, sizeof(header), f);

        u8* line = static_cast<u8*>(malloc(size_t(rowBytes)));
        if (line == nullptr)
        {
            fclose(f);
            return false;
        }
        memset(line, 0, size_t(rowBytes));
        // BMP row 0 is the bottom of the image. In the rotated framebuffer, screen pixel (x, y)
        // (y down) is at x*h + (h-1-y), so the bottom screen row y=h-1 is index x*h + 0.
        for (int by = 0; by < h; ++by)
        {
            const int y = h - 1 - by;
            for (int x = 0; x < w; ++x)
            {
                const int idx = x * h + (h - 1 - y);
                u8* dst = line + x * 3;
                if (fmt == Format::BGR8)
                {
                    const u8* src = static_cast<const u8*>(data) + idx * 3;
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                }
                else
                {
                    const uint16_t px = static_cast<const uint16_t*>(data)[idx];
                    dst[0] = u8((px & 0x1F) << 3);
                    dst[1] = u8(((px >> 5) & 0x3F) << 2);
                    dst[2] = u8(((px >> 11) & 0x1F) << 3);
                }
            }
            fwrite(line, 1, size_t(rowBytes), f);
        }
        free(line);
        fclose(f);
        return true;
    }

    bool saveBottomScreen(const char* path)
    {
        return saveRotatedFramebuffer(path, textfb::framebuffer(), textfb::kWidth, textfb::kHeight, Format::RGB565);
    }
}
