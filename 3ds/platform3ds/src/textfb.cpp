#include <platform3ds/textfb.hpp>

extern "C" const u8 default_font_bin[]; // libctru console font: 256 glyphs, 8x8, 1 bpp

namespace p3ds::textfb
{
    // The 3DS LCD framebuffer is rotated: it is stored as 320 columns of 240 pixels, each column
    // running bottom to top. Screen pixel (x, y) with y pointing down lives at x*240 + (239 - y).
    static inline uint16_t* pixelPtr(uint16_t* fb, int x, int y)
    {
        return fb + x * kHeight + (kHeight - 1 - y);
    }

    void setupBottomScreen()
    {
        gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        clear(color::black);
        present();
    }

    uint16_t* framebuffer()
    {
        return reinterpret_cast<uint16_t*>(gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr));
    }

    void clear(uint16_t c)
    {
        uint16_t* fb = framebuffer();
        for (int i = 0; i < kWidth * kHeight; ++i)
            fb[i] = c;
    }

    void fillRows(int row, int rowCount, uint16_t c)
    {
        if (row < 0 || row >= kRows)
            return;
        if (row + rowCount > kRows)
            rowCount = kRows - row;
        uint16_t* fb = framebuffer();
        const int y0 = row * 8;
        const int y1 = y0 + rowCount * 8;
        for (int x = 0; x < kWidth; ++x)
        {
            // Rows are contiguous within a column (in reverse order).
            uint16_t* col = fb + x * kHeight;
            for (int y = y0; y < y1; ++y)
                col[kHeight - 1 - y] = c;
        }
    }

    void drawText(int col, int row, const char* text, uint16_t fg, uint16_t bg, int maxChars)
    {
        if (row < 0 || row >= kRows)
            return;
        uint16_t* fb = framebuffer();
        for (int i = 0; i < maxChars && text[i] != '\0'; ++i)
        {
            const int c = col + i;
            if (c < 0)
                continue;
            if (c >= kCols)
                break;
            const u8* glyph = default_font_bin + uint8_t(text[i]) * 8;
            for (int gy = 0; gy < 8; ++gy)
            {
                const u8 bits = glyph[gy];
                for (int gx = 0; gx < 8; ++gx)
                {
                    const bool on = (bits >> (7 - gx)) & 1; // bit 7 = leftmost pixel in libctru's font
                    *pixelPtr(fb, c * 8 + gx, row * 8 + gy) = on ? fg : bg;
                }
            }
        }
    }

    void present()
    {
        GSPGPU_FlushDataCache(framebuffer(), kWidth * kHeight * sizeof(uint16_t));
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
    }
}
