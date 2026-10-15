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

        // The whole file is built in memory and written at once: one small write per row took 3-7 s on the SD
        // card (hardware test #21).
        u8* file = static_cast<u8*>(calloc(1, sizeof(header) + imageBytes));
        if (file == nullptr)
            return false;
        memcpy(file, header, sizeof(header));
        // BMP row 0 is the bottom of the image. In the rotated framebuffer, screen pixel (x, y)
        // (y down) is at x*h + (h-1-y), so the bottom screen row y=h-1 is index x*h + 0.
        for (int by = 0; by < h; ++by)
        {
            const int y = h - 1 - by;
            u8* line = file + sizeof(header) + size_t(by) * size_t(rowBytes);
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
        }
        FILE* f = fopen(path, "wb");
        bool ok = f != nullptr;
        if (ok)
        {
            ok = fwrite(file, 1, sizeof(header) + imageBytes, f) == sizeof(header) + imageBytes;
            ok = fclose(f) == 0 && ok;
        }
        free(file);
        return ok;
    }

    bool saveBottomScreen(const char* path)
    {
        // The GPU may have written it (M8's GUI): drop stale cache lines first.
        GSPGPU_InvalidateDataCache(textfb::framebuffer(), textfb::kWidth * textfb::kHeight * sizeof(uint16_t));
        return saveRotatedFramebuffer(path, textfb::framebuffer(), textfb::kWidth, textfb::kHeight, Format::RGB565);
    }
}
