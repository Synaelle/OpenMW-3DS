#pragma once

#include <cstdint>

namespace p3ds::screenshot
{
    // Writes a 24-bit BMP from a 3DS-style rotated framebuffer: the buffer is stored column-major,
    // fbWidth=240 "rows" of fbHeight pixels (400 top / 320 bottom), first pixel = bottom-left.
    // Pixel format: BGR8 (3 bytes) or RGB565 (2 bytes).
    enum class Format
    {
        BGR8,
        RGB565,
    };
    bool saveRotatedFramebuffer(const char* path, const void* data, int screenWidth, int screenHeight, Format fmt);

    // Bottom screen framebuffer (as drawn by textfb) to BMP.
    bool saveBottomScreen(const char* path);

    // The player's screenshot (test #107, the Home tab's camera button): one 400x480 BMP, the top screen above the
    // bottom one (centred, black on its sides), as the 3DS shows them. `top` is the top screen in the rotated BGR8
    // layout above (240 x 400). The image is built now (the bottom screen as it shows now) and written by a
    // background thread: the SD card's file creation takes seconds on hardware. false: one is still being written
    // (or no memory). Nothing is done.
    bool saveBothScreensAsync(const char* path, const uint8_t* top);
    // A finished background save, once: 1 saved, -1 failed, 0 nothing new.
    int takeAsyncResult();
}
