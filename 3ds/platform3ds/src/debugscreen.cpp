#include <platform3ds/debugscreen.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/textfb.hpp>
#include <platform3ds/timing.hpp>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string_view>

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
        // Rows (8 px each, 30 in all, the last one is the watchdog's alive line, crash.cpp).
        // Game menu: title, body, pager, options (two columns of 2-row buttons: label, detail).
        constexpr int kMenuTitleRow = 2;
        constexpr int kMenuBodyRow = 3;
        constexpr int kMenuBodyRows = 11;
        constexpr int kMenuPagerRow = 14;
        constexpr int kMenuOptionRow = 15;
        constexpr int kMenuOptionRows = 5; // of 2 text rows each
        constexpr int kMenuOptionsPerPage = kMenuOptionRows * 2;
        constexpr int kMenuWrap = textfb::kCols - 1; // the last column shows the scroll marks
        // Screenshot button above the alive line: rows 26-28, 24 px tall.
        constexpr int kButtonRow = textfb::kRows - 4;
        constexpr int kButtonRows = 3;

        constexpr uint16_t kOptionDetailBg = textfb::rgb565(18, 24, 44);

        // Shadow of what is on screen, so a redraw only touches characters that changed.
        char gShadow[textfb::kRows][textfb::kCols];
        uint16_t gShadowFg[textfb::kRows][textfb::kCols];
        uint16_t gShadowBg[textfb::kRows][textfb::kCols];
        bool gStaticDrawn = false;

        void invalidateShadow()
        {
            memset(gShadow, 0, sizeof(gShadow));
            gStaticDrawn = false;
        }

        // Draws text at (col, r), padded with spaces to width, touching only the characters that changed.
        void cells(int r, int col, const char* text, int width, uint16_t fg, uint16_t bg)
        {
            const size_t len = strlen(text);
            for (int i = 0; i < width && col + i < textfb::kCols; ++i)
            {
                const int c = col + i;
                char ch = size_t(i) < len ? text[i] : ' ';
                if (static_cast<unsigned char>(ch) < 32 || static_cast<unsigned char>(ch) > 126)
                    ch = '?'; // the 8x8 font is ASCII
                if (gShadow[r][c] == ch && gShadowFg[r][c] == fg && gShadowBg[r][c] == bg)
                    continue;
                gShadow[r][c] = ch;
                gShadowFg[r][c] = fg;
                gShadowBg[r][c] = bg;
                textfb::drawText(c, r, &ch, fg, bg, 1);
            }
        }

        void row(int r, uint16_t fg, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
        void row(int r, uint16_t fg, const char* fmt, ...)
        {
            char buf[textfb::kCols + 1];
            va_list args;
            va_start(args, fmt);
            vsnprintf(buf, sizeof(buf), fmt, args);
            va_end(args);
            cells(r, 0, buf, textfb::kCols, fg, textfb::color::black);
        }

        // Word wrap to `width` columns. Paragraphs keep their own line breaks.
        void wrap(const std::string& text, int width, std::vector<std::string>& out)
        {
            size_t start = 0;
            while (start <= text.size())
            {
                size_t end = text.find('\n', start);
                if (end == std::string::npos)
                    end = text.size();
                std::string_view para(text.data() + start, end - start);
                if (para.empty())
                    out.emplace_back();
                while (!para.empty())
                {
                    if (int(para.size()) <= width)
                    {
                        out.emplace_back(para);
                        break;
                    }
                    size_t cut = para.rfind(' ', size_t(width));
                    if (cut == std::string_view::npos || cut == 0)
                        cut = size_t(width);
                    out.emplace_back(para.substr(0, cut));
                    para.remove_prefix(cut);
                    while (!para.empty() && para.front() == ' ')
                        para.remove_prefix(1);
                }
                start = end + 1;
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
        strncpy(mLooking, snapshot.lookingAt ? snapshot.lookingAt : "", sizeof(mLooking) - 1);
        strncpy(mState, snapshot.state ? snapshot.state : "", sizeof(mState) - 1);
        mSnapshot.cellName = mCell;
        mSnapshot.status = mStatus;
        mSnapshot.lookingAt = mLooking;
        mSnapshot.state = mState;
    }

    void DebugBottomScreen::setMenu(const MenuView* menu)
    {
        if (menu == nullptr)
        {
            if (mMenuActive)
            {
                mMenuActive = false;
                mMenu = MenuView{};
                mNeedsClear = true; // back to the debug rows
                mLastRedrawTick = 0;
            }
            return;
        }
        if (mMenuActive && *menu == mMenu)
            return;
        const bool newMenu = !mMenuActive || menu->mTitle != mMenu.mTitle;
        const bool optionsChanged = !mMenuActive || menu->mOptions != mMenu.mOptions;
        if (!mMenuActive)
            mNeedsClear = true; // the debug rows go
        mMenuActive = true;
        mMenu = *menu;
        mMenuLines.clear();
        for (const std::string& paragraph : mMenu.mBody)
            wrap(paragraph, kMenuWrap, mMenuLines);
        const int maxScroll = std::max(0, int(mMenuLines.size()) - kMenuBodyRows);
        if (newMenu || mMenu.mBodyFromEnd)
            mMenuScroll = mMenu.mBodyFromEnd ? maxScroll : 0;
        mMenuScroll = std::clamp(mMenuScroll, 0, maxScroll);
        if (newMenu || optionsChanged)
            mMenuPage = 0;
        mLastRedrawTick = 0; // show the change at once
    }

    int DebugBottomScreen::consumeMenuSelection()
    {
        const int selection = mMenuSelection;
        mMenuSelection = -1;
        return selection;
    }

    void DebugBottomScreen::onTouch(const TouchEvent& event)
    {
        if (!mVisible || !event.down)
            return;
        const int r = event.y / 8;
        const bool left = event.x < textfb::kWidth / 2;
        if (r >= kButtonRow && r < kButtonRow + kButtonRows)
            mScreenshotRequested = true;
        else if (mMenuActive)
        {
            if (r >= kMenuBodyRow && r < kMenuBodyRow + kMenuBodyRows)
            {
                // Upper half: back a page, lower half: forward a page.
                const int maxScroll = std::max(0, int(mMenuLines.size()) - kMenuBodyRows);
                const int step = kMenuBodyRows - 1;
                const bool up = r < kMenuBodyRow + kMenuBodyRows / 2;
                mMenuScroll = std::clamp(mMenuScroll + (up ? -step : step), 0, maxScroll);
            }
            else if (r == kMenuPagerRow)
            {
                const int pages = (int(mMenu.mOptions.size()) + kMenuOptionsPerPage - 1) / kMenuOptionsPerPage;
                mMenuPage = std::clamp(mMenuPage + (left ? -1 : 1), 0, std::max(0, pages - 1));
            }
            else if (r >= kMenuOptionRow && r < kMenuOptionRow + kMenuOptionRows * 2)
            {
                const int index
                    = mMenuPage * kMenuOptionsPerPage + (r - kMenuOptionRow) / 2 * 2 + (left ? 0 : 1);
                if (index < int(mMenu.mOptions.size()))
                    mMenuSelection = index;
            }
            mLastRedrawTick = 0;
        }
    }

    bool DebugBottomScreen::consumeScreenshotRequest()
    {
        const bool requested = mScreenshotRequested;
        mScreenshotRequested = false;
        return requested;
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

        redraw();
        textfb::present();
    }

    void DebugBottomScreen::forceRedraw()
    {
        mLastRedrawTick = 0;
        render();
    }

    // Header and buttons: drawn once per clear.
    void DebugBottomScreen::drawStatic()
    {
        using namespace textfb;
        const PlatformInfo& pi = info();
        char title[kCols + 1];
        // The build stamp ("YYYYMMDD-HHMMSS rev", Paris time): "build 14/10 21:12" (day/month) shows at a glance
        // which CIA is installed (the package README names the one it contains). The row is exactly 40 columns.
        char stamp[12] = "?";
        if (strlen(p3ds_build_id) >= 13)
            snprintf(stamp, sizeof(stamp), "%.2s/%.2s %.2s:%.2s", p3ds_build_id + 6, p3ds_build_id + 4,
                p3ds_build_id + 9, p3ds_build_id + 11);
        snprintf(title, sizeof(title), " OpenMW3DS %s %s build %s", pi.isNew3DS ? "N3DS" : "O3DS",
            pi.speedupEnabled ? "804MHz" : "268MHz", stamp);
        cells(0, 0, title, kCols, color::cyan, color::header);
        for (int r = kButtonRow; r < kButtonRow + kButtonRows; ++r)
            cells(r, 0, "", kCols, color::white, color::header);
        static const char kButton[] = "[ tap: SCREENSHOT ]";
        cells(kButtonRow + 1, (kCols - int(sizeof(kButton) - 1)) / 2, kButton, int(sizeof(kButton) - 1), color::white,
            color::header);
        gStaticDrawn = true;
    }

    void DebugBottomScreen::drawMenu()
    {
        using namespace textfb;
        cells(kMenuTitleRow, 0, mMenu.mTitle.c_str(), kCols, color::yellow, color::black);
        const int maxScroll = std::max(0, int(mMenuLines.size()) - kMenuBodyRows);
        for (int i = 0; i < kMenuBodyRows; ++i)
        {
            const int line = mMenuScroll + i;
            cells(kMenuBodyRow + i, 0, line < int(mMenuLines.size()) ? mMenuLines[line].c_str() : "", kMenuWrap,
                color::white, color::black);
            const char* mark = (i == 0 && mMenuScroll > 0) ? "^" : (i == kMenuBodyRows - 1 && mMenuScroll < maxScroll) ? "v" : "";
            cells(kMenuBodyRow + i, kMenuWrap, mark, 1, color::cyan, color::black);
        }
        const int count = int(mMenu.mOptions.size());
        const int pages = (count + kMenuOptionsPerPage - 1) / kMenuOptionsPerPage;
        char pager[64] = ""; // cells() cuts it at kCols
        if (pages > 1)
            snprintf(pager, sizeof(pager), "%s      options %d-%d of %d      %s", mMenuPage > 0 ? "[<]" : "   ",
                mMenuPage * kMenuOptionsPerPage + 1, std::min(count, (mMenuPage + 1) * kMenuOptionsPerPage), count,
                mMenuPage + 1 < pages ? "[>]" : "   ");
        cells(kMenuPagerRow, 0, pager, kCols, color::cyan, color::black);
        for (int slot = 0; slot < kMenuOptionsPerPage; ++slot)
        {
            const int index = mMenuPage * kMenuOptionsPerPage + slot;
            const int r = kMenuOptionRow + slot / 2 * 2;
            const int col = slot % 2 == 0 ? 0 : kCols / 2;
            if (index < count)
            {
                const MenuView::Option& option = mMenu.mOptions[index];
                char label[kCols + 1];
                snprintf(label, sizeof(label), " %s", option.mLabel.c_str());
                char detail[kCols + 1];
                snprintf(detail, sizeof(detail), " %s", option.mDetail.c_str());
                cells(r, col, label, kCols / 2 - 1, color::white, color::header);
                cells(r + 1, col, detail, kCols / 2 - 1, color::grey, kOptionDetailBg);
            }
            else
            {
                cells(r, col, "", kCols / 2 - 1, color::black, color::black);
                cells(r + 1, col, "", kCols / 2 - 1, color::black, color::black);
            }
        }
    }

    void DebugBottomScreen::redraw()
    {
        using namespace textfb;
        if (!gStaticDrawn)
            drawStatic();

#ifdef OPENMW3DS_DEBUG
        const FrameStats& f = mFrameCopy;
        const uint16_t fpsColor = f.fps >= 25.f ? color::green : (f.fps >= 15.f ? color::yellow : color::red);
        row(1, fpsColor, "FPS %5.1f  low %5.1f  frame %6.2f ms", f.fps, f.fpsLow, f.frameMs);
#endif
        if (mMenuActive)
        {
            drawMenu();
            return;
        }
#ifdef OPENMW3DS_DEBUG
        row(2, color::cyan, "cell %.34s", mSnapshot.cellName);
        row(3, color::cyan, "pos %.0f %.0f %.0f  yaw %.0f pitch %.0f", mSnapshot.playerPos[0],
            mSnapshot.playerPos[1], mSnapshot.playerPos[2], mSnapshot.yaw, mSnapshot.pitch);
        row(4, color::cyan, "look %.34s", mLooking[0] ? mLooking : "-");
#endif
        row(5, mSnapshot.stateAlert ? color::yellow : color::cyan, "%s", mState);
        row(6, color::grey, "%s", mStatus);
    }
}
