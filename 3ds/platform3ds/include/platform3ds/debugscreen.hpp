#pragma once

#include <platform3ds/bottomscreen.hpp>

namespace p3ds
{
    // Bottom-screen debug display: build, then (debug builds) FPS, cell, position and view, the object in reach,
    // then the game state, the screenshot button, and the watchdog's alive line at the very bottom (crash.cpp).
    // Everything else is in the log. While a game menu is open (setMenu), it takes the middle rows: title, body
    // text (touch its upper/lower half to scroll), options (touch to select, two per row, 10 per page). Redraws at
    // most kRedrawHz, only the characters that changed: a few hundred microseconds every 250 ms, and zero when
    // hidden.
    class DebugBottomScreen final : public IBottomScreen
    {
    public:
        static constexpr float kRedrawHz = 4.f;

        void init() override;
        void shutdown() override;
        void update(const GameSnapshot& snapshot) override;
        void render() override;
        void onTouch(const TouchEvent& event) override;
        void setVisible(bool visible) override;
        bool isVisible() const override { return mVisible; }

        // True once after the SCREENSHOT button was tapped.
        bool consumeScreenshotRequest();

        // Game menu (rows 2-25, in place of the debug rows) while set; nullptr: none.
        // consumeMenuSelection returns the touched option's index once, else -1.
        void setMenu(const MenuView* menu);
        int consumeMenuSelection();
        bool menuActive() const { return mMenuActive; }

        // Everything again at the next render (the screen was drawn by something else meanwhile: M8's GUI).
        void redrawAll()
        {
            mNeedsClear = true;
            mLastRedrawTick = 0;
        }

        // Redraws immediately, ignoring the rate limit (progress display during long blocking steps).
        void forceRedraw();

    private:
        void redraw();
        void drawStatic();
        void drawMenu();

        GameSnapshot mSnapshot;
        FrameStats mFrameCopy;
        char mCell[64] = {};
        char mStatus[64] = {};
        char mLooking[64] = {};
        char mState[64] = {};
        bool mMenuActive = false;
        MenuView mMenu;
        std::vector<std::string> mMenuLines; // the body, wrapped
        int mMenuScroll = 0; // first body line shown
        int mMenuPage = 0; // options page
        int mMenuSelection = -1;
        bool mScreenshotRequested = false;
        bool mVisible = true;
        bool mNeedsClear = true;
        uint64_t mLastRedrawTick = 0;
    };
}
