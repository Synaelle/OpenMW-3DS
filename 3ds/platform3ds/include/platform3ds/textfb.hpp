#pragma once

#include <3ds.h>
#include <cstdint>

// Minimal CPU text renderer for the bottom screen framebuffer (RGB565, 320x240, 8x8 font from
// libctru). Allocation-free so the crash handler can use it.
namespace p3ds::textfb
{
    constexpr int kWidth = 320;
    constexpr int kHeight = 240;
    constexpr int kCols = kWidth / 8; // 40
    constexpr int kRows = kHeight / 8; // 30

    constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
    {
        return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    namespace color
    {
        constexpr uint16_t black = rgb565(0, 0, 0);
        constexpr uint16_t white = rgb565(235, 235, 235);
        constexpr uint16_t grey = rgb565(140, 140, 140);
        constexpr uint16_t red = rgb565(255, 80, 80);
        constexpr uint16_t yellow = rgb565(255, 220, 90);
        constexpr uint16_t green = rgb565(110, 230, 110);
        constexpr uint16_t cyan = rgb565(110, 210, 240);
        constexpr uint16_t darkRed = rgb565(90, 0, 0);
        constexpr uint16_t header = rgb565(30, 40, 70);
    }

    // Puts the bottom screen in single-buffered RGB565 mode. Call once after gfxInit.
    void setupBottomScreen();

    // Current bottom framebuffer (rotated 240x320 layout).
    uint16_t* framebuffer();

    void clear(uint16_t color);
    void fillRows(int row, int rowCount, uint16_t color);
    // Draws up to maxChars characters (or until NUL) at character cell (col,row).
    void drawText(int col, int row, const char* text, uint16_t fg, uint16_t bg, int maxChars = kCols);

    // Flushes the CPU cache for the framebuffer and presents it.
    void present();
}
