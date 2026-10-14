#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memstats.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/textfb.hpp>
#include <platform3ds/timing.hpp>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace p3ds
{
    void FrameTimer::addFrame(float frameMs)
    {
        mLast = frameMs;
        mSamples[mNext] = frameMs;
        mNext = (mNext + 1) % kWindow;
        if (mCount < kWindow)
            ++mCount;
    }

    void FrameTimer::fill(FrameStats& out) const
    {
        out.frameMs = mLast;
        if (mCount == 0)
            return;
        float sum = 0.f, worst = 0.f;
        for (int i = 0; i < mCount; ++i)
        {
            sum += mSamples[i];
            if (mSamples[i] > worst)
                worst = mSamples[i];
        }
        out.fps = sum > 0.f ? 1000.f * float(mCount) / sum : 0.f;
        out.fpsLow = worst > 0.f ? 1000.f / worst : 0.f;
    }

    namespace
    {
        constexpr int kLogFirstRow = 10;

        uint16_t levelColor(log::Level level)
        {
            switch (level)
            {
                case log::Level::Error:
                    return textfb::color::red;
                case log::Level::Warning:
                    return textfb::color::yellow;
                case log::Level::Info:
                    return textfb::color::white;
                default:
                    return textfb::color::grey;
            }
        }

        // Shadow of what is on screen, so a redraw only touches characters that changed.
        char gShadow[textfb::kRows][textfb::kCols];
        uint16_t gShadowFg[textfb::kRows][textfb::kCols];

        void invalidateShadow()
        {
            memset(gShadow, 0, sizeof(gShadow));
        }

        void row(int r, uint16_t fg, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
        void row(int r, uint16_t fg, const char* fmt, ...)
        {
            char buf[textfb::kCols + 1];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            // Pad to the full width so stale characters are overwritten without clearing.
            size_t n = strlen(buf);
            while (n < size_t(textfb::kCols))
                buf[n++] = ' ';
            buf[textfb::kCols] = '\0';
            for (int c = 0; c < textfb::kCols; ++c)
            {
                if (gShadow[r][c] == buf[c] && gShadowFg[r][c] == fg)
                    continue;
                gShadow[r][c] = buf[c];
                gShadowFg[r][c] = fg;
                textfb::drawText(c, r, buf + c, fg, textfb::color::black, 1);
            }
        }
    }

    void DebugBottomScreen::init()
    {
        mNeedsClear = true;
    }

    void DebugBottomScreen::shutdown()
    {
        textfb::clear(textfb::color::black);
        textfb::present();
    }

    void DebugBottomScreen::setVisible(bool visible)
    {
        if (visible == mVisible)
            return;
        mVisible = visible;
        mNeedsClear = true;
        mLastRedrawTick = 0;
    }

    void DebugBottomScreen::update(const GameSnapshot& snapshot)
    {
        if (!mVisible)
            return;
        // Copy what we need: the pointers in the snapshot are only valid during this call.
        if (snapshot.frame != nullptr)
            mFrameCopy = *snapshot.frame;
        mSnapshot = snapshot;
        mSnapshot.frame = &mFrameCopy;
        strncpy(mCell, snapshot.cellName ? snapshot.cellName : "-", sizeof(mCell) - 1);
        strncpy(mStatus, snapshot.status ? snapshot.status : "", sizeof(mStatus) - 1);
        mSnapshot.cellName = mCell;
        mSnapshot.status = mStatus;
    }

    void DebugBottomScreen::render()
    {
        const uint64_t now = ticks();
        if (mNeedsClear)
        {
            textfb::clear(textfb::color::black);
            invalidateShadow();
            if (!mVisible)
            {
                textfb::present();
                mNeedsClear = false;
                return;
            }
            mNeedsClear = false;
        }
        if (!mVisible)
            return;
        const uint64_t interval = uint64_t(double(SYSCLOCK_ARM11) / kRedrawHz);
        if (mLastRedrawTick != 0 && now - mLastRedrawTick < interval)
            return;
        mLastRedrawTick = now;

        const uint64_t t0 = ticks();
        redraw();
        textfb::present();
        mLastRedrawMs = float(ticksToMs(ticks() - t0));
    }

    void DebugBottomScreen::forceRedraw()
    {
        mLastRedrawTick = 0;
        render();
    }

    void DebugBottomScreen::redraw()
    {
        using namespace textfb;
        const FrameStats& f = mFrameCopy;
        const mem::Stats m = mem::query();
        const bool budgetOk = mem::checkBudget(m);
        const PlatformInfo& pi = info();

        char title[kCols + 1];
        snprintf(title, sizeof(title), " OpenMW3DS debug   %s %s  [SELECT]", pi.isNew3DS ? "N3DS" : "O3DS",
            pi.speedupEnabled ? "804MHz" : "268MHz");
        if (gShadow[0][0] == 0) // header is static: draw once per clear
        {
            fillRows(0, 1, color::header);
            drawText(0, 0, title, color::cyan, color::header);
            gShadow[0][0] = 1;
        }

        const uint16_t fpsColor = f.fps >= 25.f ? color::green : (f.fps >= 15.f ? color::yellow : color::red);
        row(1, fpsColor, "FPS %5.1f  low %5.1f  frame %6.2f ms", f.fps, f.fpsLow, f.frameMs);
        row(2, color::white, "CPU %6.2f ms  GPU %6.2f ms  dbg %.2f", f.cpuMs, f.gpuMs, mLastRedrawMs);
        row(3, color::white, "draws %5lu  tris %7lu  frame #%lu", (unsigned long)f.drawCalls,
            (unsigned long)f.triangles, (unsigned long)f.frameIndex);
        row(4, budgetOk ? color::white : color::red, "heap %6.1f / %6.1f MB (budget %.1f)", mem::toMB(m.heapUsed),
            mem::toMB(m.heapTotal), mem::toMB(mem::heapBudget(m)));
        row(5, color::white, "linear free %5.1f / %5.1f MB", mem::toMB(m.linearFree), mem::toMB(m.linearTotal));
        row(6, color::white, "vram free %4.2f MB  image %5.2f MB", mem::toMB(m.vramFree), mem::toMB(m.imageSize));
        row(7, color::white, "region %5.1f / %5.1f MB (hw %lu)", mem::toMB(m.regionUsed), mem::toMB(m.regionTotal),
            (unsigned long)(mem::expectedRegion() >> 20));
        row(8, color::cyan, "cell %.34s", mSnapshot.cellName);
        row(9, color::cyan, "pos %.0f %.0f %.0f  %.16s", mSnapshot.playerPos[0], mSnapshot.playerPos[1],
            mSnapshot.playerPos[2], mSnapshot.status);

        constexpr int kLogRows = kRows - kLogFirstRow - 1; // last row: watchdog heartbeat (crash.cpp)
        log::Line lines[kLogRows];
        const size_t n = log::copyLastLines(lines, kLogRows);
        for (int i = 0; i < kLogRows; ++i)
        {
            if (size_t(i) < n)
                row(kLogFirstRow + i, levelColor(lines[i].level), "%c %s", log::levelChar(lines[i].level), lines[i].text);
            else
                row(kLogFirstRow + i, color::black, " ");
        }
    }
}
