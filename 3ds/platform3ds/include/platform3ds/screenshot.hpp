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
}
