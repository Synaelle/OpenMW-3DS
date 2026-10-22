#include <platform3ds/menuart.hpp>
#include <platform3ds/textfb.hpp>

#include <3ds.h>

#include <algorithm>
#include <cstring>

namespace p3ds::menuart
{
    namespace
    {
        struct MenuGlyph
        {
            int8_t mLeft;
            int8_t mTop;
            uint8_t mWidth;
            uint8_t mHeight;
            uint8_t mAdvance;
            uint32_t mOffset;
        };
#include "menufont.inc"
#include "menuemblem.inc"
#include "menuspin.inc"
#include "menubuttons.inc"

        constexpr int kWidth = textfb::kWidth;
        constexpr int kHeight = textfb::kHeight;

        // The framebuffer is rotated: screen pixel (x, y) is at x * 240 + (239 - y).
        inline uint16_t* pixel(uint16_t* fb, int x, int y)
        {
            return fb + x * kHeight + (kHeight - 1 - y);
        }

        inline uint16_t blend(uint16_t under, uint16_t over, unsigned alpha)
        {
            const unsigned ur = under >> 11, ug = (under >> 5) & 63, ub = under & 31;
            const unsigned or_ = over >> 11, og = (over >> 5) & 63, ob = over & 31;
            const unsigned r = (ur * (255 - alpha) + or_ * alpha) / 255;
            const unsigned g = (ug * (255 - alpha) + og * alpha) / 255;
            const unsigned b = (ub * (255 - alpha) + ob * alpha) / 255;
            return uint16_t((r << 11) | (g << 5) | b);
        }

        void fill(uint16_t* fb, int x0, int y0, int w, int h, uint16_t colour)
        {
            const int x1 = std::min(kWidth, x0 + w), y1 = std::min(kHeight, y0 + h);
            for (int x = std::max(0, x0); x < x1; ++x)
                for (int y = std::max(0, y0); y < y1; ++y)
                    *pixel(fb, x, y) = colour;
        }

        const MenuGlyph* glyphs(Font font)
        {
            return font == Font::Title ? kTitleGlyphs : font == Font::Text ? kTextGlyphs : kSmallGlyphs;
        }
        const uint8_t* coverage(Font font)
        {
            return font == Font::Title ? kTitleCoverage : font == Font::Text ? kTextCoverage : kSmallCoverage;
        }

        void drawGlyphs(uint16_t* fb, int x, int y, const char* str, Font font, uint16_t colour)
        {
            const MenuGlyph* table = glyphs(font);
            const uint8_t* alpha = coverage(font);
            for (const char* c = str; *c != '\0'; ++c)
            {
                const unsigned code = static_cast<unsigned char>(*c);
                if (code < 32 || code > 126)
                    continue;
                const MenuGlyph& g = table[code - 32];
                for (int gy = 0; gy < g.mHeight; ++gy)
                    for (int gx = 0; gx < g.mWidth; ++gx)
                    {
                        const unsigned a = alpha[g.mOffset + gy * g.mWidth + gx];
                        const int px = x + g.mLeft + gx, py = y + g.mTop + gy;
                        if (a == 0 || px < 0 || px >= kWidth || py < 0 || py >= kHeight)
                            continue;
                        uint16_t* p = pixel(fb, px, py);
                        *p = blend(*p, colour, a);
                    }
                x += g.mAdvance;
            }
        }
    }

    void background()
    {
        restore(0, 0, kWidth, kHeight);
    }

    void topScreen()
    {
        static bool drawn = false;
        if (drawn || textfb::gpuOwned())
            return;
        drawn = true;
        // The top screen as gfxInitDefault leaves it (BGR8, double buffered, 400 x 240 stored as 400 columns of
        // 240): both buffers, so swapping shows the picture whichever is current.
        const uint8_t bg[3] = { colour::backgroundRgb[2], colour::backgroundRgb[1], colour::backgroundRgb[0] }; // BGR
        constexpr int kTopWidth = 400;
        const int x0 = (kTopWidth - kEmblemSize) / 2, y0 = (kHeight - kEmblemSize) / 2;
        for (int buffer = 0; buffer < 2; ++buffer)
        {
            uint8_t* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
            for (int i = 0; i < kTopWidth * kHeight; ++i)
                std::memcpy(fb + i * 3, bg, 3);
            // Screen row y is column index 239 - y: the emblem's rows y0 .. y0 + size - 1 start at index
            // 240 - y0 - size.
            for (int x = 0; x < kEmblemSize; ++x)
                std::memcpy(fb + ((x0 + x) * kHeight + (kHeight - y0 - kEmblemSize)) * 3,
                    kEmblem + x * kEmblemSize * 3, kEmblemSize * 3);
            GSPGPU_FlushDataCache(fb, kTopWidth * kHeight * 3);
            gfxScreenSwapBuffers(GFX_TOP, false);
            gspWaitForVBlank();
        }
    }

    namespace
    {
        constexpr int kTopWidth = 400;
        constexpr int kTopMargin = 16;
        int gSpinX = 0, gSpinY = 0; // the emblem's top-left on the top screen (topLoading)

        inline uint8_t* topPixel(uint8_t* fb, int x, int y)
        {
            return fb + (x * kHeight + (kHeight - 1 - y)) * 3; // BGR8, rotated as the bottom screen
        }

        void topGlyphs(uint8_t* fb, int x, int y, const char* str, Font font, const uint8_t rgb[3])
        {
            const MenuGlyph* table = glyphs(font);
            const uint8_t* alpha = coverage(font);
            for (const char* c = str; *c != '\0'; ++c)
            {
                const unsigned code = static_cast<unsigned char>(*c);
                if (code < 32 || code > 126)
                    continue;
                const MenuGlyph& g = table[code - 32];
                for (int gy = 0; gy < g.mHeight; ++gy)
                    for (int gx = 0; gx < g.mWidth; ++gx)
                    {
                        const unsigned a = alpha[g.mOffset + gy * g.mWidth + gx];
                        const int px = x + g.mLeft + gx, py = y + g.mTop + gy;
                        if (a == 0 || px < 0 || px >= kTopWidth || py < 0 || py >= kHeight)
                            continue;
                        uint8_t* p = topPixel(fb, px, py);
                        for (int k = 0; k < 3; ++k) // BGR
                            p[k] = uint8_t((p[k] * (255 - a) + rgb[2 - k] * a) / 255);
                    }
                x += g.mAdvance;
            }
        }
    }

    void topLoading()
    {
        if (textfb::gpuOwned())
            return;
        gfxSetDoubleBuffering(GFX_TOP, false);
        uint8_t* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        std::memset(fb, 0, kTopWidth * kHeight * 3); // black: the renderer's start-up clears it to black anyway
        GSPGPU_FlushDataCache(fb, kTopWidth * kHeight * 3);
        topSpin(0);
    }

    void topSpin(int frame)
    {
        // "[emblem] Loading" at the bottom right, the word centred on the emblem's height. Drawn whole each time
        // (area cleared, frame, word): the renderer's start-up, between two frames, clears the screen.
        static constexpr const char* kWord = "Loading";
        static constexpr uint8_t kWordRgb[3] = { 214, 208, 196 }; // colour::title
        const int textW = textWidth(kWord, Font::Text);
        const int textX = kTopWidth - kTopMargin - textW;
        gSpinX = textX - 10 - kSpinSize;
        gSpinY = kHeight - kTopMargin - kSpinSize;
        uint8_t* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, nullptr, nullptr);
        const int areaW = kTopWidth - gSpinX;
        for (int x = gSpinX; x < kTopWidth; ++x)
            std::memset(topPixel(fb, x, kHeight - 1), 0, kSpinSize * 3 + kTopMargin * 3); // rows gSpinY..239
        const uint8_t* src = kSpin + ((frame % kSpinFrames + kSpinFrames) % kSpinFrames) * kSpinSize * kSpinSize * 3;
        // Screen row y is column index 239 - y: the frame's rows gSpinY .. gSpinY + size - 1 start at 240 - gSpinY - size.
        for (int x = 0; x < kSpinSize; ++x)
            std::memcpy(fb + ((gSpinX + x) * kHeight + (kHeight - gSpinY - kSpinSize)) * 3, src + x * kSpinSize * 3,
                kSpinSize * 3);
        topGlyphs(fb, textX, gSpinY + (kSpinSize - lineHeight(Font::Text)) / 2, kWord, Font::Text, kWordRgb);
        GSPGPU_FlushDataCache(topPixel(fb, gSpinX, kHeight - 1), areaW * kHeight * 3);
    }

    void topLoadingEnd()
    {
        gfxSetDoubleBuffering(GFX_TOP, true);
    }

    void restore(int x, int y, int w, int h)
    {
        if (textfb::gpuOwned())
            return;
        fill(textfb::framebuffer(), x, y, w, h, colour::background);
    }

    int textWidth(const char* str, Font font)
    {
        int width = 0;
        for (const char* c = str; *c != '\0'; ++c)
        {
            const unsigned code = static_cast<unsigned char>(*c);
            if (code >= 32 && code <= 126)
                width += glyphs(font)[code - 32].mAdvance;
        }
        return width;
    }

    int lineHeight(Font font)
    {
        return font == Font::Title ? kTitleLineHeight : font == Font::Text ? kTextLineHeight : kSmallLineHeight;
    }

    void text(int x, int y, const char* str, Font font, uint16_t colour)
    {
        if (textfb::gpuOwned())
            return;
        drawGlyphs(textfb::framebuffer(), x, y, str, font, colour);
    }

    void rightAligned(int y, const char* str, Font font, uint16_t colour)
    {
        text(kWidth - kMargin - textWidth(str, font), y, str, font, colour);
    }

    int button(int x, int y, char which, uint16_t colour)
    {
        const uint8_t* alpha = which == 'B' ? kButtonB : kButtonA;
        const int size = which == 'B' ? kButtonBSize : kButtonASize;
        if (textfb::gpuOwned())
            return size;
        uint16_t* fb = textfb::framebuffer();
        for (int gy = 0; gy < size; ++gy)
            for (int gx = 0; gx < size; ++gx)
            {
                const unsigned a = alpha[gy * size + gx];
                const int px = x + gx, py = y + gy;
                if (a == 0 || px < 0 || px >= kWidth || py < 0 || py >= kHeight)
                    continue;
                uint16_t* p = pixel(fb, px, py);
                *p = blend(*p, colour, a);
            }
        return size;
    }

    void separator(int y)
    {
        if (textfb::gpuOwned())
            return;
        fill(textfb::framebuffer(), kMargin, y, kWidth - 2 * kMargin, 1, colour::line);
    }

    void mark(int y, int height)
    {
        if (textfb::gpuOwned())
            return;
        fill(textfb::framebuffer(), kMargin, y, 3, height, colour::accent);
    }
}
