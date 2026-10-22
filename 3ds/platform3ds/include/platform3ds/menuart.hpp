#pragma once

#include <cstdint>

// The look of the start menu and the loading screen (M15), drawn on the CPU before the engine and its interface
// exist. Bottom screen (textfb's RGB565 framebuffer): flat and left-aligned, like a console's system menu - a warm
// black background, thin separator lines, a short gold mark beside the current line, and text in OpenMW's
// MysticCards (a free look-alike of Morrowind's font, SIL Open Font License 1.1; tools/make_menu_font.py),
// anti-aliased. Top screen: the HOME Menu icon's emblem (tools/make_menu_emblem.py).
namespace p3ds::menuart
{
    enum class Font
    {
        Title, // 23 px
        Text, // 17 px
        Small, // 13 px
    };

    constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
    {
        return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    // Bone and ash on ebony black, a dark red mark (test #107).
    namespace colour
    {
        // Test #107 (the tester: darker, no yellow): "Ebony & bone", docs/start-menu-palette-a.png, its footer lighter.
        // The background as bytes too: the top screen (BGR8) is filled with it around the emblem, which
        // tools/make_menu_emblem.py blends over the same colour (test #107: a box showed when the two differed).
        constexpr uint8_t backgroundRgb[3] = { 9, 8, 8 };
        constexpr uint16_t background = rgb(backgroundRgb[0], backgroundRgb[1], backgroundRgb[2]);
        constexpr uint16_t line = rgb(56, 52, 49);
        constexpr uint16_t accent = rgb(176, 58, 46);
        constexpr uint16_t title = rgb(214, 208, 196);
        constexpr uint16_t selected = rgb(244, 240, 232);
        constexpr uint16_t text = rgb(170, 164, 154);
        constexpr uint16_t dim = rgb(146, 140, 132);
        constexpr uint16_t disabled = rgb(84, 80, 76);
    }

    // The margin left and right of everything.
    constexpr int kMargin = 24;

    // The whole screen.
    void background();
    // The top screen: the emblem of the HOME Menu icon, centred on the same background. Once; the engine's first
    // frame replaces it.
    void topScreen();
    // The background again in a rectangle (under text about to be redrawn).
    void restore(int x, int y, int w, int h);

    // Text from (x, y) (the line's top), blended in `colour`.
    void text(int x, int y, const char* str, Font font, uint16_t colour);
    // Text ending at the right margin.
    void rightAligned(int y, const char* str, Font font, uint16_t colour);
    int textWidth(const char* str, Font font);
    int lineHeight(Font font);

    // A button's glyph ('A' or 'B': the in-game hints' icons, tools/make_button_icons.py), 16x16 from (x, y), in
    // `colour`. Returns its width.
    int button(int x, int y, char which, uint16_t colour);

    // A separator line between the margins.
    void separator(int y);
    // The short gold mark beside the current line, `height` pixels tall, at the left margin.
    void mark(int y, int height);
}
