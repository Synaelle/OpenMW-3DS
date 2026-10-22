#include "frame3ds.hpp"
#include "hud3ds.hpp"
#include "memrelief.hpp"
#include "menus.hpp"
#include "sound3ds.hpp"
#ifdef OPENMW3DS_DEBUG
#include "benchmark3ds.hpp"
#include "debugtools.hpp"

extern "C"
{
#include <3ds/applets/swkbd.h>
}
#endif

#include <platform3ds/crash.hpp>
#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/screenshot.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwgui/mode.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwphysics/raycasting.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"

#include "../profile.hpp"

#include <components/esm/position.hpp>
#include <components/render3ds/render3ds.hpp>
#include <components/settings/values.hpp>

#include <osg/Stats>
#include <osgViewer/Viewer>

#include <MyGUI_InputManager.h>
#include <MyGUI_ILayer.h>
#include <MyGUI_Widget.h>

#include <3ds.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace omw3ds
{
    namespace
    {
        p3ds::DebugBottomScreen gBottom;
        p3ds::FrameTimer gTimer;
        p3ds::FrameStats gStats;
        bool gBottomReady = false;
        bool gScreenshotRequested = false; // M8's Debug tab
        int gPlayerShotWait = 0; // frames left for the player's screenshot to start (requestPlayerScreenshot3DS)

        // Cell name / position / player state are refreshed a few times a second only (string work).
        std::string gCellName = "-";
        float gPos[3] = {};
        float gRot[3] = {}; // player rotation (radians): x pitch, z yaw
        char gLooking[64] = ""; // the object in reach under the crosshair
        char gStateLine[48] = "";
        bool gStateAlert = false;
        unsigned gFramesSinceInfo = 0;

        // Frame-time log every 5 s.
        double gLogAccum = 0;
        unsigned gLogFrames = 0;
        float gLogWorst = 0;

#ifdef OPENMW3DS_DEBUG
        // Renderer counters summed between perf lines.
        struct RenderSums
        {
            double mDraws = 0, mTriangles = 0, mLeaves = 0, mSkipped = 0, mUploadKb = 0, mEmitMs = 0, mGpuMs = 0;
            float mGpuWorst = 0;
            float mCmdBufPeak = 0;
            unsigned mVerified = 0;
            double mLightSlots = 0;
            double mShaderRuns[9] = {};
            double mProgramSwitches = 0;
            double mParticleVertices = 0;
            double mSky = 0, mTerrain = 0, mParticles = 0, mSkinned = 0, mSmall = 0, mOther = 0, mSmallCulled = 0,
                   mWeatherSkipped = 0, mFlameReplays = 0, mSameState = 0;
        } gRender;

        // Per-subsystem frame timings from osg::Stats (previous frame), summed between perf lines.
        struct Timing
        {
            std::string mLabel;
            std::string mAttribute;
            bool mCamera; // camera stats (cull/draw) instead of viewer stats
            double mSum = 0;
            double mLast = 0; // the frame just ended (a spike's report)
        };
        osgViewer::Viewer* gViewer = nullptr;
        std::vector<Timing> gTimings;
        unsigned gTimingFrames = 0;

        void accumulateTimings()
        {
            if (gViewer == nullptr)
                return;
            // Resource::StatsHandler turns "engine" off on every event while its overlay is hidden; turn it
            // back on before Engine::frame's ScopedProfiles run (the handler runs later, in eventTraversal).
            gViewer->getViewerStats()->collectStats("engine", true);
            const unsigned frame = gViewer->getFrameStamp()->getFrameNumber();
            if (frame == 0)
                return;
            osg::Stats* viewerStats = gViewer->getViewerStats();
            osg::Stats* cameraStats = gViewer->getCamera()->getStats();
            for (Timing& t : gTimings)
            {
                osg::Stats* stats = t.mCamera ? cameraStats : viewerStats;
                double value = 0;
                t.mLast = 0;
                if (stats != nullptr && stats->getAttribute(frame - 1, t.mAttribute, value))
                {
                    t.mSum += value;
                    t.mLast = value;
                }
            }
            ++gTimingFrames;
        }

        void logTimings()
        {
            p3ds::perf::logAndReset(gTimingFrames); // perf/more (engine.cpp OMW3DS_PHASE, sceneutil, ...)
            if (gTimingFrames == 0)
                return;
            std::string line = "perf/ms:";
            char item[48];
            for (Timing& t : gTimings)
            {
                const double ms = t.mSum * 1000.0 / gTimingFrames;
                if (ms >= 0.05)
                {
                    std::snprintf(item, sizeof(item), " %s %.1f", t.mLabel.c_str(), ms);
                    line += item;
                }
                t.mSum = 0;
            }
            gTimingFrames = 0;
            P3DS_LOGI("%s", line.c_str());
        }
#endif

        void refreshWorldInfo()
        {
            const MWBase::Environment& env = MWBase::Environment::get();
            MWBase::World* world = env.getWorld();
            if (world == nullptr)
                return;
            const MWWorld::Ptr player = world->getPlayerPtr();
            if (player.isEmpty() || !player.isInCell())
                return;
            gCellName = player.getCell()->getCell()->getDescription();
            const ESM::Position& pos = player.getRefData().getPosition();
            for (int i = 0; i < 3; ++i)
            {
                gPos[i] = pos.pos[i];
                gRot[i] = pos.rot[i];
            }

#ifdef OPENMW3DS_DEBUG
            // The object in reach under the crosshair: this frame's focus ray (World::updateFocusObject).
            const MWWorld::Ptr focus = world->getFocusRay().mHitObject;
            if (focus.isEmpty())
                gLooking[0] = '\0';
            else
            {
                const std::string name(focus.getClass().getName(focus));
                const std::string id = focus.getCellRef().getRefId().toString();
                std::snprintf(gLooking, sizeof(gLooking), "%s%s%s%s", name.c_str(), name.empty() ? "" : " [",
                    id.c_str(), name.empty() ? "" : "]");
            }
#endif

            // No HUD until M8 (menus are on the bottom screen, menus.cpp): say what the game is waiting for.
            const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
            MWBase::WindowManager* windows = env.getWindowManager();
            if (stats.isDead())
            {
                std::snprintf(gStateLine, sizeof(gStateLine), "DEAD (load a save below)");
                gStateAlert = true;
            }
            else if (windows->isGuiMode())
            {
                std::snprintf(gStateLine, sizeof(gStateLine), "menu: %s",
                    windows->isConsoleMode() ? "console" : guiModeName(windows->getMode()));
                gStateAlert = true;
            }
            else
            {
                const MWMechanics::DrawState draw = stats.getDrawState();
                std::snprintf(gStateLine, sizeof(gStateLine), "%s%s  %s",
                    draw == MWMechanics::DrawState::Weapon     ? "weapon ready"
                        : draw == MWMechanics::DrawState::Spell ? "magic ready"
                                                                : "hands down",
                    env.getMechanicsManager()->isSneaking(player) ? "  sneaking" : "",
                    world->isFirstPerson() ? "1st person" : "3rd person");
                gStateAlert = false;
            }
            static std::string lastState;
            if (lastState != gStateLine)
            {
                lastState = gStateLine;
                P3DS_LOGI("state: %s", gStateLine);
            }
        }
    }

    std::string debugScreenText3DS()
    {
        std::string text;
        for (int r = 0; r <= 6; ++r)
        {
            const uint16_t colour = p3ds::DebugBottomScreen::textRowColour(r);
            char tag[8];
            std::snprintf(tag, sizeof(tag), "#%02X%02X%02X", ((colour >> 11) & 31) * 255 / 31,
                ((colour >> 5) & 63) * 255 / 63, (colour & 31) * 255 / 31);
            std::string row = p3ds::DebugBottomScreen::textRow(r);
            row.erase(0, row.find_first_not_of(' ')); // the title's margin
            for (size_t at = row.find('#'); at != std::string::npos; at = row.find('#', at + 2))
                row.insert(at, 1, '#'); // "##" is a '#' in MyGUI text
            text += tag + row + "\n";
        }
        return text;
    }

    void requestScreenshot3DS()
    {
        gScreenshotRequested = true;
    }

    bool screenshotButtonEnabled3DS()
    {
        static const bool enabled = access("sdmc:/openmw/config/screenshot-button", F_OK) == 0;
        return enabled;
    }

    void requestPlayerScreenshot3DS()
    {
        // At most one every 500 ms (the tester, test #107: no spamming the button).
        static u64 last = 0;
        const u64 now = osGetTime();
        if (gPlayerShotWait > 0 || (last != 0 && now - last < 500))
            return;
        last = now;
        mkdir("sdmc:/openmw/screenshots", 0777); // may be missing (empty folders and FTP copies)
        // Named by the console's clock: a new game session doesn't write over the last one's pictures.
        const time_t clock = time(nullptr);
        const tm* t = localtime(&clock);
        char path[96];
        std::snprintf(path, sizeof(path), "sdmc:/openmw/screenshots/%04d-%02d-%02d_%02d-%02d-%02d.bmp",
            t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
        for (int n = 2; access(path, F_OK) == 0 && n < 100; ++n) // twice in a second
            std::snprintf(path, sizeof(path), "sdmc:/openmw/screenshots/%04d-%02d-%02d_%02d-%02d-%02d_%d.bmp",
                t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec, n);
        Render3DS::requestPlayerCapture(path);
        gPlayerShotWait = 3; // taken at the next frame's start
        P3DS_LOGI("screenshot (player): %s", path);
    }

    bool exteriorsEnabled()
    {
        static const bool enabled = [] {
            struct stat st;
            const bool disabled = stat("sdmc:/openmw/no-exteriors", &st) == 0;
            if (disabled)
                P3DS_LOGW("exteriors disabled (sdmc:/openmw/no-exteriors)");
            return !disabled;
        }();
        return enabled;
    }

    void initBottomScreen(const char* status)
    {
        gBottom.init();
        gBottomReady = true;
        p3ds::GameSnapshot snap;
        snap.cellName = status;
        snap.status = status;
        gBottom.update(snap);
        gBottom.forceRedraw();
    }

    namespace
    {
        // The touch screen as MyGUI's mouse, in canvas coordinates (the bottom screen's place in it:
        // Render3DS::kGuiBottomX/Y). Held touches move; touching and lifting press and release the left button.
#ifdef OPENMW3DS_DEBUG
        // What a touch reached (scripted touches in autorun tests): the widget, its root and the root's layer.
        void logGuiTouch(MyGUI::InputManager& input, int x, int y, bool taken)
        {
            const MyGUI::Widget* focus = input.getMouseFocusWidget();
            const MyGUI::Widget* root = focus;
            while (root != nullptr && root->getParent() != nullptr)
                root = root->getParent();
            auto describe = [](const MyGUI::Widget* widget) {
                if (widget == nullptr)
                    return std::string("(none)");
                return std::string(widget->getTypeName()) + " '" + widget->getName() + "' "
                    + widget->getAbsoluteCoord().print();
            };
            const MyGUI::ILayer* layer = root != nullptr ? root->getLayer() : nullptr;
            std::string chain;
            for (const MyGUI::Widget* w = focus != nullptr ? focus->getParent() : nullptr; w != nullptr && w != root;
                 w = w->getParent())
                chain += " < " + std::string(w->getTypeName()) + " '" + w->getName() + "'";
            P3DS_LOGI("gui touch %d,%d: %s%s in %s on %s%s", x, y, describe(focus).c_str(), chain.c_str(),
                describe(root).c_str(), layer != nullptr ? layer->getName().c_str() : "-", taken ? "" : " (not taken)");
        }
#endif

        void injectTouchToGui()
        {
            MyGUI::InputManager* input = MyGUI::InputManager::getInstancePtr();
            if (input == nullptr)
                return;
            static bool sDown = false;
            static int sX = 0, sY = 0;
            const bool down = (hidKeysHeld() & KEY_TOUCH) != 0;
            if (down)
            {
                touchPosition touch;
                hidTouchRead(&touch);
                sX = touch.px + Render3DS::kGuiBottomX;
                sY = touch.py + Render3DS::kGuiBottomY;
                input->injectMouseMove(sX, sY, 0);
                if (!sDown)
                {
                    const bool taken = input->injectMousePress(sX, sY, MyGUI::MouseButton::Left);
#ifdef OPENMW3DS_DEBUG
                    logGuiTouch(*input, sX, sY, taken);
#else
                    (void)taken;
#endif
                }
            }
            else if (sDown)
                input->injectMouseRelease(sX, sY, MyGUI::MouseButton::Left);
            sDown = down;
        }

        // HOME menu / power button: SDL only services APT in its 3DS video driver, which we don't use.
        // aptMainLoop() blocks while the app is suspended and returns false when it must close.
        void closeRequested(bool loading, const char* who = "APT");

        void serviceApt(bool loading)
        {
            if (!aptMainLoop())
                closeRequested(loading);
            // The panic combination (L+R+START+SELECT, read by crash.cpp's watchdog): as if closed from the HOME
            // Menu. If this thread is frozen, the watchdog leaves by itself.
            else if (p3ds::crash::panicRequested())
                closeRequested(loading, "panic");
        }

        // The HOME Menu (or autorun "close:N", or the panic combination) asked the app to close.
        void closeRequested(bool loading, const char* who)
        {
            static bool quitRequested = false;
            if (quitRequested)
                return;
            quitRequested = true;
            p3ds::crash::kick("exiting");
            Render3DS::releaseGpuForExit();
            if (!loading)
            {
                P3DS_LOGI("%s: close requested, quitting", who);
                MWBase::Environment::get().getStateManager()->requestQuit();
                return;
            }
            // OpenMW can't abort a load; the user wants out now.
            P3DS_LOGI("%s: close requested while loading, exiting", who);
            p3ds::shutdown();
            svcExitProcess();
        }
    }

    void closeAsFromHomeMenu()
    {
        closeRequested(false);
    }

    void setViewer(osgViewer::Viewer* viewer)
    {
        // The settings that matter for performance, as actually loaded (the SD card may hold an older
        // settings.cfg than the package).
        P3DS_LOGI("settings: physics async threads %d, lua threads %d, lua gc steps %d, viewing distance %.0f, "
                  "preload %s, actors range %.0f",
            int(Settings::physics().mAsyncNumThreads), int(Settings::lua().mLuaNumThreads),
            int(Settings::lua().mGcStepsPerFrame),
            double(Settings::camera().mViewingDistance), Settings::cells().mPreloadEnabled ? "on" : "off",
            double(Settings::game().mActorsProcessingRange));
#ifdef OPENMW3DS_DEBUG
        gViewer = viewer;
        osg::Stats* stats = viewer->getViewerStats();
        stats->collectStats("engine", true); // OMW::ScopedProfile in Engine::frame
        stats->collectStats("event", true);
        stats->collectStats("update", true);
        if (osg::Stats* cameraStats = viewer->getCamera()->getStats())
            cameraStats->collectStats("rendering", true); // cull/draw
        gTimings.clear();
        OMW::forEachUserStatsValue([](const OMW::UserStats& value) {
            gTimings.push_back(Timing{ value.mLabel, value.mTaken, false });
        });
        gTimings.push_back(Timing{ "event", "Event traversal time taken", false });
        gTimings.push_back(Timing{ "update", "Update traversal time taken", false });
        gTimings.push_back(Timing{ "cull", "Cull traversal time taken", true });
        gTimings.push_back(Timing{ "draw", "Draw traversal time taken", true });
#endif
    }

    void onLoadingTick()
    {
        serviceApt(true);
#ifdef OPENMW3DS_DEBUG
        debug::onLoadingTickAutorun();
#endif
        if (Render3DS::drawsGui())
            onLoadingTick3DS();
        if (!gBottomReady)
            return;
        p3ds::GameSnapshot snap;
        snap.frame = &gStats;
        snap.cellName = gCellName == "-" ? "(loading)" : gCellName.c_str();
        static const std::string loading = std::string("loading - ") + Render3DS::interfacePath();
        snap.status = loading.c_str();
        gBottom.update(snap);
        gBottom.render(); // rate-limited internally
    }

    namespace
    {
        std::vector<std::function<void()>> gDeferred;
    }

    void deferToFrame(std::function<void()> call)
    {
        gDeferred.push_back(std::move(call));
    }

    uint32_t gRestoresSeen = 0; // p3ds::restoreCount() last handled (onEngineFrame)

    bool systemKeyboard(const std::string& hint, std::string& text, size_t maxLength)
    {
        SwkbdState keyboard;
        swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, static_cast<int>(maxLength));
        swkbdSetHintText(&keyboard, hint.c_str());
        swkbdSetInitialText(&keyboard, text.c_str());
        swkbdSetFeatures(&keyboard, SWKBD_DARKEN_TOP_SCREEN);
        std::vector<char> buffer(maxLength * 4 + 1, 0);
        const SwkbdButton button = swkbdInputText(&keyboard, buffer.data(), buffer.size());
        P3DS_LOGI("system keyboard: %s", button == SWKBD_BUTTON_CONFIRM ? "confirmed" : "cancelled");
        Render3DS::invalidateInterface(); // the applet's transition clobbers the overlay texture (test #63)
        // The applet's suspend and restore are handled here: onEngineFrame must not read the VRAM textures again
        // (test #94: 1.3 s more after every keyboard).
        gRestoresSeen = p3ds::restoreCount();
        if (button != SWKBD_BUTTON_CONFIRM)
            return false;
        text = buffer.data();
        return true;
    }

    void onEngineFrame()
    {
        serviceApt(false);
        if (p3ds::restoreCount() != gRestoresSeen)
        {
            gRestoresSeen = p3ds::restoreCount();
            Render3DS::invalidateInterface(); // back from the HOME Menu: VRAM textures and the overlay redone
        }
        omw3ds::memrelief::onFrame();
#ifdef OPENMW3DS_DEBUG
        // M10: what allocates every frame (the Zeste profile of 20/10: allocation ~17-23% of the main thread's
        // instructions): the main thread's operator-new call sites over 300 frames of play, ranked by count. Only
        // with sdmc:/openmw/memprofile (callerProfileStart is a no-op without it).
        {
            static unsigned playFrames = 0;
            if (MWBase::Environment::get().getStateManager()->getState() == MWBase::StateManager::State_Running)
            {
                ++playFrames;
                if (playFrames == 300)
                    p3ds::memprobe::callerProfileStart();
                else if (playFrames == 600)
                    p3ds::memprobe::callerProfileStopByCount("300 frames of play, main thread");
            }
        }
#endif
        if (!gDeferred.empty())
        {
            std::vector<std::function<void()>> calls;
            calls.swap(gDeferred);
            for (auto& call : calls)
                call();
        }
        static uint64_t lastTick = 0;
        const uint64_t now = svcGetSystemTick();
        if (lastTick == 0)
        {
            lastTick = now;
            return;
        }
        const float frameSeconds = float(double(now - lastTick) / SYSCLOCK_ARM11);
        lastTick = now;
        const float ms = frameSeconds * 1000.f;
        gTimer.addFrame(ms);
        ++gStats.frameIndex;
#ifdef OPENMW3DS_DEBUG
        accumulateTimings();
        {
            // A spike (the 0.6-2.4 s freezes of tests #99-#101's first stage): that frame's subsystems and phases.
            const bool spike = ms >= 300.f && p3ds::debugMode();
            char phases[400];
            p3ds::perf::endFrame(spike, phases, sizeof(phases));
            if (spike)
            {
                std::string systems;
                char item[48];
                for (const Timing& t : gTimings)
                    if (t.mLast * 1000.0 >= 2.0)
                    {
                        std::snprintf(item, sizeof(item), " %s %.0f", t.mLabel.c_str(), t.mLast * 1000.0);
                        systems += item;
                    }
                P3DS_LOGW("spike: %.0f ms frame [%s]; systems:%s; phases:%s", double(ms), gCellName.c_str(),
                    systems.c_str(), phases);
            }
        }
        if (gStats.frameIndex == 100)
            debug::lazyStringSelfCheck();
        debug::runAutorun(gStats.frameIndex);
        benchmark::onFrame();
#endif

        // Buttons: SDL's 3DS joystick driver reads hidKeysDown/Up but only its (unused) video driver scans,
        // so scan once per frame here. All buttons are game controls (bindingsmanager.cpp); debug
        // functions are on the touch screen.
        hidScanInput();
#ifdef OPENMW3DS_DEBUG
        debug::applyScriptedInput(gStats.frameIndex);
#endif
        // Screenshot combination, L + D-pad Left (D-pad Left does nothing in game; L alone sneaks): the same as the
        // Debug tab's button, for when the screens can't be read (hardware test #28).
        {
            constexpr u32 kScreenshotKeys = KEY_L | KEY_DLEFT;
            if ((hidKeysHeld() & kScreenshotKeys) == kScreenshotKeys && (hidKeysDown() & kScreenshotKeys) != 0)
                gScreenshotRequested = true;
        }
        // M8: MyGUI draws the bottom screen, except while an M6 text menu shows there (until each window has its
        // M8 version); the text display then gets the whole screen back.
        // Without the text display (M15: the loading screen instead, platform3ds/loadscreen.cpp), MyGUI takes the
        // bottom screen from the first frame.
        if (Render3DS::drawsGui())
        {
            const bool guiBottom = !gBottomReady || !gBottom.menuActive();
            if (guiBottom != Render3DS::guiOwnsBottomScreen())
            {
                Render3DS::setGuiOwnsBottomScreen(guiBottom);
                if (!guiBottom)
                    gBottom.redrawAll();
            }
        }
        if (Render3DS::drawsGui())
            updateHud3DS();
#ifdef OPENMW3DS_LUMIERE
        if (Render3DS::guiOwnsBottomScreen() && Render3DS::Lumiere::active())
        {
            touchPosition touch{};
            const bool down = (hidKeysHeld() & KEY_TOUCH) != 0;
            if (down)
                hidTouchRead(&touch);
            touchLumiere(touch.px, touch.py, down);
        }
        else
#endif // OPENMW3DS_LUMIERE
        if (Render3DS::guiOwnsBottomScreen())
            injectTouchToGui();
        else if (gBottomReady && (hidKeysDown() & KEY_TOUCH))
        {
            touchPosition touch;
            hidTouchRead(&touch);
            gBottom.onTouch(p3ds::TouchEvent{ touch.px, touch.py, true });
        }
        if (gBottomReady && (gBottom.consumeScreenshotRequest() || std::exchange(gScreenshotRequested, false)))
        {
            static unsigned count = 0;
            if (count == 0)
                mkdir("sdmc:/openmw/screenshots", 0777); // may be missing (empty folders and FTP copies)
            // Both screens: the top one from the render target (next frame), the bottom one as it shows now.
            char path[64];
            std::snprintf(path, sizeof(path), "sdmc:/openmw/screenshots/top_%03u.bmp", count);
            Render3DS::requestCapture(path);
            char bottomPath[64];
            std::snprintf(bottomPath, sizeof(bottomPath), "sdmc:/openmw/screenshots/bottom_%03u.bmp", count++);
            p3ds::screenshot::saveBottomScreen(bottomPath);
#ifdef OPENMW3DS_DEBUG
            P3DS_LOGI("screenshot %s: %s", path, debug::describeView(gStats.frameIndex).c_str());
#else
            P3DS_LOGI("screenshot %s", path);
#endif
        }
        // The player's screenshot: started (or not: one still being written), then saved or not.
        if (gPlayerShotWait > 0)
        {
            if (Render3DS::playerCaptureStarted())
                gPlayerShotWait = 0;
            else if (--gPlayerShotWait == 0)
                MWBase::Environment::get().getWindowManager()->messageBox("#{OMW3DS:ScreenshotBusy}");
        }
        if (const int saved = p3ds::screenshot::takeAsyncResult(); saved != 0)
        {
            P3DS_LOGI("screenshot (player): %s", saved > 0 ? "saved" : "failed");
            MWBase::Environment::get().getWindowManager()->messageBox(
                saved > 0 ? "#{OMW3DS:ScreenshotSaved}" : "#{OMW3DS:ScreenshotFailed}");
        }
        if (gBottomReady)
            refreshMenus(gBottom);

        const Render3DS::FrameStats& render = Render3DS::getLastFrameStats();
        gStats.drawCalls = render.mDraws;
        gStats.triangles = render.mTriangles;
        gStats.cpuMs = render.mEmitMs;
        gStats.gpuMs = render.mGpuMs;
#ifdef OPENMW3DS_DEBUG
        gRender.mDraws += render.mDraws;
        gRender.mTriangles += render.mTriangles;
        gRender.mLeaves += render.mLeaves;
        gRender.mSkipped += render.mSkipped;
        gRender.mUploadKb += render.mUploadedBytes / 1024.0;
        gRender.mEmitMs += render.mEmitMs;
        gRender.mGpuMs += render.mGpuMs;
        gRender.mGpuWorst = std::max(gRender.mGpuWorst, render.mGpuMs);
        gRender.mCmdBufPeak = std::max(gRender.mCmdBufPeak, render.mCmdBufUsage);
        gRender.mVerified += render.mVerified;
        gRender.mLightSlots += render.mLightSlotWrites;
        for (int i = 0; i < 9; ++i)
            gRender.mShaderRuns[i] += render.mShaderRuns[i];
        gRender.mProgramSwitches += render.mProgramSwitches;
        gRender.mParticleVertices += render.mParticleVertices;
        gRender.mSky += render.mSkyDraws;
        gRender.mTerrain += render.mTerrainDraws;
        gRender.mParticles += render.mParticleDraws;
        gRender.mSkinned += render.mSkinnedDraws;
        gRender.mSmall += render.mSmallDraws;
        gRender.mOther += render.mOtherDraws;
        gRender.mSmallCulled += render.mSmallCulled;
        gRender.mWeatherSkipped += render.mWeatherSkipped;
        gRender.mFlameReplays += render.mFlameReplays;
        gRender.mSameState += render.mSameState;
#endif

        gLogAccum += frameSeconds;
        ++gLogFrames;
        if (ms > gLogWorst)
            gLogWorst = ms;
        if (gLogAccum >= 5.0)
        {
            P3DS_LOGI("perf: %u frames in %.1f s, %.1f fps, avg %.1f ms, worst %.1f ms [%s] pos %.0f %.0f %.0f "
                      "yaw %.0f pitch %.0f",
                gLogFrames, gLogAccum, gLogFrames / gLogAccum, gLogAccum * 1000.0 / gLogFrames, gLogWorst,
                gCellName.c_str(), double(gPos[0]), double(gPos[1]), double(gPos[2]),
                double(osg::RadiansToDegrees(gRot[2])), double(osg::RadiansToDegrees(gRot[0])));
            if (const MWSound::SoundStats3DS sound = MWSound::takeSoundStats3DS(); sound.mActive)
                P3DS_LOGI("sound: %d sounds, %d streams, effects %zu KB, stream decoding %.1f ms/s, %u underruns, "
                          "%u skipped (no free channel)",
                    sound.mSounds, sound.mStreams, sound.mBufferBytes / 1024, sound.mDecodeMs / gLogAccum,
                    sound.mUnderruns, sound.mSkipped);
#ifdef OPENMW3DS_DEBUG
            if (p3ds::debugMode()) // the detailed lines: the start menu's Debug choices only (M15)
                logTimings();
            if (Render3DS::isActive() && p3ds::debugMode())
            {
                const double n = gLogFrames;
                P3DS_LOGI("gpu: per frame %.0f draws, %.0f tris, %.0f leaves (%.0f skipped), emit %.1f ms, "
                          "PICA %.1f ms (worst %.1f), uploads %.0f KB/frame, %lu geometries, linear free %lu KB, "
                          "cmdbuf peak %.0f%%, %u new objects GPU-checked, %.0f light slot uploads/frame",
                    gRender.mDraws / n, gRender.mTriangles / n, gRender.mLeaves / n, gRender.mSkipped / n,
                    gRender.mEmitMs / n, gRender.mGpuMs / n, double(gRender.mGpuWorst), gRender.mUploadKb / n,
                    (unsigned long)render.mGeometries, (unsigned long)render.mLinearFreeKb,
                    double(gRender.mCmdBufPeak) * 100.0, gRender.mVerified, gRender.mLightSlots / n);
                const double* runs = gRender.mShaderRuns;
                P3DS_LOGI("gpu/vs: vertex shader runs per frame by point lights: 0: %.0f 1: %.0f 2: %.0f 3: %.0f "
                          "4: %.0f 5: %.0f 6: %.0f 7: %.0f 8: %.0f (particles %.0f); %.0f shader switches/frame",
                    runs[0] / n, runs[1] / n, runs[2] / n, runs[3] / n, runs[4] / n, runs[5] / n, runs[6] / n,
                    runs[7] / n, runs[8] / n, gRender.mParticleVertices / n, gRender.mProgramSwitches / n);
                P3DS_LOGI("gpu/draws: per frame sky %.0f, terrain %.0f, particles %.0f, skinned %.0f, small (<64 "
                          "triangles) %.0f, other %.0f; %.0f parts under the small feature size left out, %.0f "
                          "weather particles faded near the eye, %.0f flames replayed from a loop, %.0f draws kept the previous "
                          "draw's state",
                    gRender.mSky / n, gRender.mTerrain / n, gRender.mParticles / n, gRender.mSkinned / n,
                    gRender.mSmall / n, gRender.mOther / n, gRender.mSmallCulled / n, gRender.mWeatherSkipped / n,
                    gRender.mFlameReplays / n, gRender.mSameState / n);
            }
            gRender = RenderSums{};
#endif
            gLogAccum = 0;
            gLogFrames = 0;
            gLogWorst = 0;
        }

        // Where the player is (the perf lines' cell and position), with or without the text display.
        if (++gFramesSinceInfo >= 8)
        {
            gFramesSinceInfo = 0;
            refreshWorldInfo();
        }
        if (!gBottomReady)
            return;
        gTimer.fill(gStats);
        p3ds::GameSnapshot snap;
        snap.frame = &gStats;
        snap.cellName = gCellName.c_str();
        for (int i = 0; i < 3; ++i)
            snap.playerPos[i] = gPos[i];
        snap.yaw = osg::RadiansToDegrees(gRot[2]);
        snap.pitch = osg::RadiansToDegrees(gRot[0]);
        snap.lookingAt = gLooking;
        snap.state = gStateLine;
        snap.stateAlert = gStateAlert;
        snap.status = Render3DS::isActive() ? Render3DS::interfacePath()
            : Render3DS::failure()          ? Render3DS::failure()
                                            : "headless (renderer failed to start)";
        gBottom.update(snap);
        p3ds::perf::Phase phase("bottomScreen");
        gBottom.render(); // rate-limited internally
    }

    void shutdownBottomScreen()
    {
        if (gBottomReady)
            gBottom.shutdown();
        gBottomReady = false;
    }
}
