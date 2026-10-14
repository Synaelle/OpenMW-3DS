#pragma once

#include <platform3ds/bottomscreen.hpp>

namespace p3ds
{
    // CPU-drawn debug HUD for the bottom screen: FPS/frame time, GPU/CPU ms, memory (heap/linear/
    // VRAM/region), current cell, player state and the last log lines, plus a touch button bar.
    // Redraws at most kRedrawHz, so its cost is a few hundred microseconds every 250 ms, and zero
    // when hidden.
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

        // Redraws immediately, ignoring the rate limit (progress display during long blocking steps).
        void forceRedraw();

        // Cost of the last redraw, for self-profiling.
        float lastRedrawMs() const { return mLastRedrawMs; }

    private:
        void redraw();

        GameSnapshot mSnapshot;
        FrameStats mFrameCopy;
        char mCell[64] = {};
        char mStatus[64] = {};
        char mPlayer[64] = {};
        char mState[64] = {};
        bool mScreenshotRequested = false;
        bool mVisible = true;
        bool mNeedsClear = true;
        uint64_t mLastRedrawTick = 0;
        float mLastRedrawMs = 0.f;
    };
}
