#include "frame3ds.hpp"
#include "hidinject.hpp"

#include <platform3ds/debugscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/platform.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwgui/mode.hpp"
#include "../mwmechanics/creaturestats.hpp"
#include "../mwmechanics/drawstate.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"

#include "../profile.hpp"

#include <components/esm/position.hpp>
#include <components/render3ds/render3ds.hpp>
#include <components/settings/values.hpp>

#include <osg/Stats>
#include <osgViewer/Viewer>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loaddial.hpp>
#include <components/esm3/loaddoor.hpp>

#include <3ds.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sys/stat.h>

namespace omw3ds
{
    namespace
    {
        p3ds::DebugBottomScreen gBottom;
        p3ds::FrameTimer gTimer;
        p3ds::FrameStats gStats;
        bool gBottomReady = false;

        // Cell name / position / player state are refreshed a few times a second only (string work).
        std::string gCellName = "-";
        float gPos[3] = {};
        char gPlayerLine[48] = "";
        char gStateLine[48] = "";
        bool gStateAlert = false;
        unsigned gFramesSinceInfo = 0;

        // Frame-time log every 5 s.
        double gLogAccum = 0;
        unsigned gLogFrames = 0;
        float gLogWorst = 0;

        // Renderer counters summed between perf lines.
        struct RenderSums
        {
            double mDraws = 0, mTriangles = 0, mLeaves = 0, mSkipped = 0, mUploadKb = 0, mEmitMs = 0, mGpuMs = 0;
            float mGpuWorst = 0;
            float mCmdBufPeak = 0;
            unsigned mVerified = 0;
            double mLightSlots = 0;
        } gRender;

        // Per-subsystem frame timings from osg::Stats (previous frame), summed between perf lines.
        struct Timing
        {
            std::string mLabel;
            std::string mAttribute;
            bool mCamera; // camera stats (cull/draw) instead of viewer stats
            double mSum = 0;
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
                if (stats != nullptr && stats->getAttribute(frame - 1, t.mAttribute, value))
                    t.mSum += value;
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

        // Once: read a few texts that ESM::LazyString left in the content file, to check offsets and the
        // code page conversion in a headless run (books and dialogue can't be opened yet).
        void lazyStringSelfCheck()
        {
            const MWWorld::ESMStore& store = *MWBase::Environment::get().getESMStore();
            int shown = 0;
            for (const ESM::Book& book : store.get<ESM::Book>())
            {
                const std::string& text = book.mText.get();
                P3DS_LOGI("lazy text: book %s: %zu bytes \"%.70s\"", book.mId.toDebugString().c_str(), text.size(),
                    text.c_str());
                if (++shown == 2)
                    break;
            }
            shown = 0;
            for (const ESM::Dialogue& dialogue : store.get<ESM::Dialogue>())
            {
                for (const ESM::DialInfo& info : dialogue.mInfo)
                {
                    if (info.mResponse.empty())
                        continue;
                    P3DS_LOGI("lazy text: %s: \"%.70s\"", dialogue.mStringId.c_str(), info.mResponse.get().c_str());
                    if (++shown == 2)
                        return;
                }
            }
        }

        const char* guiModeName(MWGui::GuiMode mode)
        {
            switch (mode)
            {
                case MWGui::GM_Inventory:
                    return "inventory";
                case MWGui::GM_Container:
                    return "container";
                case MWGui::GM_Companion:
                    return "companion";
                case MWGui::GM_MainMenu:
                    return "main menu";
                case MWGui::GM_Journal:
                    return "journal";
                case MWGui::GM_Scroll:
                case MWGui::GM_Book:
                    return "book";
                case MWGui::GM_Dialogue:
                    return "dialogue";
                case MWGui::GM_Barter:
                    return "barter";
                case MWGui::GM_Rest:
                    return "rest";
                case MWGui::GM_Levelup:
                    return "level up";
                case MWGui::GM_Loading:
                case MWGui::GM_LoadingWallpaper:
                    return "loading";
                case MWGui::GM_QuickKeysMenu:
                    return "quick keys";
                case MWGui::GM_None:
                    return "message";
                default:
                    return "service";
            }
        }

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
                gPos[i] = pos.pos[i];

            // No HUD and no menus are drawn until M8: say what the game is waiting for.
            const MWMechanics::CreatureStats& stats = player.getClass().getCreatureStats(player);
            std::snprintf(gPlayerLine, sizeof(gPlayerLine), "HP %.0f/%.0f  MP %.0f/%.0f  FT %.0f/%.0f",
                double(stats.getHealth().getCurrent()), double(stats.getHealth().getModified()),
                double(stats.getMagicka().getCurrent()), double(stats.getMagicka().getModified()),
                double(stats.getFatigue().getCurrent()), double(stats.getFatigue().getModified()));
            MWBase::WindowManager* windows = env.getWindowManager();
            if (stats.isDead())
            {
                std::snprintf(gStateLine, sizeof(gStateLine), "DEAD (no load menu yet: HOME, close)");
                gStateAlert = true;
            }
            else if (windows->isGuiMode())
            {
                std::snprintf(gStateLine, sizeof(gStateLine), "menu: %s - not drawn, B closes",
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

    namespace
    {
        // autorun.txt scripted input: "btn:F,NAME,N" holds a button from game frame F for N frames (default 3);
        // "move:F,X,Y,N" / "look:F,X,Y,N" hold the circle pad / C-stick at X,Y (-1..1, +Y = up/forward).
        struct ScriptedInput
        {
            unsigned mStart = 0;
            unsigned mFrames = 3;
            u32 mKeys = 0; // btn:
            int mStick = -1; // move: 0, look: 1
            float mX = 0, mY = 0;
            std::string mText;
        };

        u32 keyFromName(const std::string& name)
        {
            static const struct
            {
                const char* mName;
                u32 mKey;
            } keys[] = { { "a", KEY_A }, { "b", KEY_B }, { "x", KEY_X }, { "y", KEY_Y }, { "l", KEY_L },
                { "r", KEY_R }, { "zl", KEY_ZL }, { "zr", KEY_ZR }, { "start", KEY_START }, { "select", KEY_SELECT },
                { "up", KEY_DUP }, { "down", KEY_DDOWN }, { "left", KEY_DLEFT }, { "right", KEY_DRIGHT } };
            for (const auto& key : keys)
                if (name == key.mName)
                    return key.mKey;
            return 0;
        }

        void parseScriptedInput(const char* text, std::vector<ScriptedInput>& out)
        {
            auto forEach = [&](const char* tag, auto&& parse) {
                for (const char* q = std::strstr(text, tag); q != nullptr; q = std::strstr(q + 1, tag))
                {
                    ScriptedInput input;
                    const char* end = q;
                    while (*end != '\0' && !std::isspace(static_cast<unsigned char>(*end)))
                        ++end;
                    input.mText.assign(q, end);
                    if (parse(q + std::strlen(tag), input))
                        out.push_back(std::move(input));
                    else
                        P3DS_LOGW("autorun: can't parse \"%s\"", input.mText.c_str());
                }
            };
            forEach("btn:", [](const char* p, ScriptedInput& input) {
                char* end = nullptr;
                input.mStart = unsigned(std::strtoul(p, &end, 10));
                if (*end != ',')
                    return false;
                std::string name;
                for (++end; std::isalpha(static_cast<unsigned char>(*end)); ++end)
                    name += char(std::tolower(static_cast<unsigned char>(*end)));
                if (*end == ',')
                    input.mFrames = unsigned(std::strtoul(end + 1, nullptr, 10));
                input.mKeys = keyFromName(name);
                return input.mKeys != 0;
            });
            auto parseStick = [](int stick) {
                return [stick](const char* p, ScriptedInput& input) {
                    char* end = nullptr;
                    input.mStick = stick;
                    input.mStart = unsigned(std::strtoul(p, &end, 10));
                    if (*end != ',')
                        return false;
                    input.mX = std::strtof(end + 1, &end);
                    if (*end != ',')
                        return false;
                    input.mY = std::strtof(end + 1, &end);
                    if (*end == ',')
                        input.mFrames = unsigned(std::strtoul(end + 1, nullptr, 10));
                    return true;
                };
            };
            forEach("move:", parseStick(0));
            forEach("look:", parseStick(1));
        }

        void applyScriptedInput(const std::vector<ScriptedInput>& inputs, unsigned frame)
        {
            if (inputs.empty())
                return;
            u32 held = 0;
            hidinject::Stick sticks[2];
            // SDL takes the first value an axis reports as its rest position and drops it; a scripted stick goes
            // straight to its target, so its first move would be lost. Report a tiny value once, first.
            static bool primed = false;
            if (!primed)
            {
                sticks[0] = sticks[1] = hidinject::Stick{ true, 1.f / 160.f, 1.f / 160.f };
                primed = true;
            }
            for (const ScriptedInput& input : inputs)
            {
                if (frame < input.mStart || frame >= input.mStart + input.mFrames)
                    continue;
                if (frame == input.mStart)
                    P3DS_LOGI("autorun: %s at pos %.0f %.0f %.0f", input.mText.c_str(), double(gPos[0]),
                        double(gPos[1]), double(gPos[2]));
                if (input.mStick < 0)
                    held |= input.mKeys;
                else
                    sticks[input.mStick] = hidinject::Stick{ true, input.mX, input.mY };
            }
            hidinject::set(held, sticks[0], sticks[1]);
        }
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

    namespace
    {
        // autorun "door:F": activate the nearest door that leads somewhere (an interior unless exteriors are
        // enabled), as if the player pressed A on it (tests cell changes without scripting a walk to the door).
        void activateNearestDoor()
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
            MWWorld::Ptr nearest;
            float best = 0;
            player.getCell()->forEachType<ESM::Door>([&](const MWWorld::Ptr& door) {
                if (!door.getCellRef().getTeleport())
                    return true;
                if (!exteriorsEnabled() && door.getCellRef().getDestCell().is<ESM::ESM3ExteriorCellRefId>())
                    return true; // refused anyway (ActionTeleport)
                const float distance = (door.getRefData().getPosition().asVec3() - playerPos).length2();
                if (nearest.isEmpty() || distance < best)
                {
                    nearest = door;
                    best = distance;
                }
                return true;
            });
            if (nearest.isEmpty())
            {
                P3DS_LOGW("autorun: no door to activate");
                return;
            }
            P3DS_LOGI("autorun: activating door %s (to %s)", nearest.getCellRef().getRefId().toDebugString().c_str(),
                nearest.getCellRef().getDestCell().toDebugString().c_str());
            MWBase::Environment::get().getLuaManager()->objectActivated(nearest, player);
        }
    }

    void initBottomScreen(const char* status)
    {
        gBottom.init();
        gBottomReady = true;
        p3ds::GameSnapshot snap;
        snap.cellName = status;
        gBottom.update(snap);
        gBottom.forceRedraw();
    }

    namespace
    {
        // HOME menu / power button: SDL only services APT in its 3DS video driver, which we don't use.
        // aptMainLoop() blocks while the app is suspended and returns false when it must close.
        void closeRequested(bool loading);

        void serviceApt(bool loading)
        {
            if (!aptMainLoop())
                closeRequested(loading);
        }

        // The HOME Menu (or autorun "close:N") asked the app to close.
        void closeRequested(bool loading)
        {
            static bool quitRequested = false;
            if (quitRequested)
                return;
            quitRequested = true;
            Render3DS::releaseGpuForExit();
            if (!loading)
            {
                P3DS_LOGI("APT: close requested, quitting");
                MWBase::Environment::get().getStateManager()->requestQuit();
                return;
            }
            // OpenMW can't abort a load; the user wants out now.
            P3DS_LOGI("APT: close requested while loading, exiting");
            p3ds::shutdown();
            svcExitProcess();
        }
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
    }

    void onLoadingTick()
    {
        serviceApt(true);
        if (!gBottomReady)
            return;
        p3ds::GameSnapshot snap;
        snap.frame = &gStats;
        snap.cellName = gCellName == "-" ? "(loading)" : gCellName.c_str();
        snap.status = "loading";
        gBottom.update(snap);
        gBottom.render(); // rate-limited internally
    }

    void onEngineFrame()
    {
        serviceApt(false);
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
        accumulateTimings();
        if (++gStats.frameIndex == 100)
            lazyStringSelfCheck();

        // Automated runs, sdmc:/openmw/autorun.txt: "quit:N" quits after N game frames (tests shutdown),
        // "shot:N,M,..." saves the top screen at those frames (sdmc:/openmw/shot_top_N.bmp).
        struct Autorun
        {
            unsigned mQuitAt = 0;
            unsigned mCloseAt = 0; // "close:N": like closing from the HOME Menu after N frames
            std::vector<unsigned> mShots;
            float mTurnDegrees = 0; // "turn:D": rotate the player by D degrees at frame 30
            std::vector<ScriptedInput> mInputs;
            std::vector<unsigned> mDoors; // "door:F,G,...": activate the nearest door at those frames
            unsigned mLuaProfileAt = 0; // "luaprof:F": write the Lua profiler table (needs "lua profiler = true")
        };
        static const Autorun autorun = [] {
            Autorun result;
            if (std::FILE* f = std::fopen("sdmc:/openmw/autorun.txt", "r"))
            {
                char buf[4096] = {};
                std::fread(buf, 1, sizeof(buf) - 1, f);
                std::fclose(f);
                if (const char* q = std::strstr(buf, "quit:"))
                    result.mQuitAt = unsigned(std::strtoul(q + 5, nullptr, 10));
                if (const char* q = std::strstr(buf, "close:"))
                    result.mCloseAt = unsigned(std::strtoul(q + 6, nullptr, 10));
                if (const char* q = std::strstr(buf, "turn:"))
                    result.mTurnDegrees = std::strtof(q + 5, nullptr);
                parseScriptedInput(buf, result.mInputs);
                if (const char* q = std::strstr(buf, "luaprof:"))
                    result.mLuaProfileAt = unsigned(std::strtoul(q + 8, nullptr, 10));
                if (const char* q = std::strstr(buf, "door:"))
                {
                    char* end = const_cast<char*>(q + 5);
                    do
                        result.mDoors.push_back(unsigned(std::strtoul(end + (*end == ',' ? 1 : 0), &end, 10)));
                    while (*end == ',');
                }
                if (const char* q = std::strstr(buf, "shot:"))
                {
                    char* end = const_cast<char*>(q + 5);
                    do
                        result.mShots.push_back(unsigned(std::strtoul(end + (*end == ',' ? 1 : 0), &end, 10)));
                    while (*end == ',');
                }
            }
            return result;
        }();
        if (autorun.mQuitAt != 0 && gStats.frameIndex == autorun.mQuitAt)
        {
            P3DS_LOGI("autorun: quitting after %u frames", autorun.mQuitAt);
            MWBase::Environment::get().getStateManager()->requestQuit();
        }
        if (autorun.mLuaProfileAt != 0 && gStats.frameIndex == autorun.mLuaProfileAt)
        {
            const std::string table = MWBase::Environment::get().getLuaManager()->formatResourceUsageStats();
            if (std::FILE* f = std::fopen("sdmc:/openmw/lua_profile.txt", "w"))
            {
                std::fwrite(table.data(), 1, table.size(), f);
                std::fclose(f);
            }
            P3DS_LOGI("autorun: Lua profile written to sdmc:/openmw/lua_profile.txt (%zu bytes)", table.size());
        }
        if (autorun.mCloseAt != 0 && gStats.frameIndex == autorun.mCloseAt)
        {
            P3DS_LOGI("autorun: closing as if from the HOME Menu after %u frames", autorun.mCloseAt);
            closeRequested(false);
        }
        for (unsigned door : autorun.mDoors)
            if (door == gStats.frameIndex)
                activateNearestDoor();
        for (unsigned shot : autorun.mShots)
            if (shot == gStats.frameIndex)
                Render3DS::requestCapture(("sdmc:/openmw/shot_top_" + std::to_string(shot) + ".bmp").c_str());
        // Buttons: SDL's 3DS joystick driver reads hidKeysDown/Up but only its (unused) video driver scans,
        // so scan once per frame here. All buttons are game controls (bindingsmanager.cpp); debug
        // functions are on the touch screen.
        hidScanInput();
        applyScriptedInput(autorun.mInputs, gStats.frameIndex);
        if (gBottomReady && (hidKeysDown() & KEY_TOUCH))
        {
            touchPosition touch;
            hidTouchRead(&touch);
            gBottom.onTouch(p3ds::TouchEvent{ touch.px, touch.py, true });
        }
        const float turn = gStats.frameIndex == 30 ? autorun.mTurnDegrees : 0.f;
        if (turn != 0)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            osg::Vec3f rot = player.getRefData().getPosition().asRotationVec3();
            rot.z() += osg::DegreesToRadians(turn);
            world->rotateObject(player, rot, MWBase::RotationFlag_none);
            P3DS_LOGI("debug: player turned to %.0f degrees", double(osg::RadiansToDegrees(rot.z())));
        }
        if (gBottomReady && gBottom.consumeScreenshotRequest())
        {
            static unsigned count = 0;
            if (count == 0)
                mkdir("sdmc:/openmw/screenshots", 0777); // may be missing (empty folders and FTP copies)
            char path[64];
            std::snprintf(path, sizeof(path), "sdmc:/openmw/screenshots/top_%03u.bmp", count++);
            Render3DS::requestCapture(path);
        }

        const Render3DS::FrameStats& render = Render3DS::getLastFrameStats();
        gStats.drawCalls = render.mDraws;
        gStats.triangles = render.mTriangles;
        gStats.cpuMs = render.mEmitMs;
        gStats.gpuMs = render.mGpuMs;
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

        gLogAccum += frameSeconds;
        ++gLogFrames;
        if (ms > gLogWorst)
            gLogWorst = ms;
        if (gLogAccum >= 5.0)
        {
            P3DS_LOGI("perf: %u frames in %.1f s, %.1f fps, avg %.1f ms, worst %.1f ms [%s] pos %.0f %.0f %.0f",
                gLogFrames, gLogAccum, gLogFrames / gLogAccum, gLogAccum * 1000.0 / gLogFrames, gLogWorst,
                gCellName.c_str(), double(gPos[0]), double(gPos[1]), double(gPos[2]));
            logTimings();
            if (Render3DS::isActive())
            {
                const double n = gLogFrames;
                P3DS_LOGI("gpu: per frame %.0f draws, %.0f tris, %.0f leaves (%.0f skipped), emit %.1f ms, "
                          "PICA %.1f ms (worst %.1f), uploads %.0f KB/frame, %lu geometries, linear free %lu KB, "
                          "cmdbuf peak %.0f%%, %u new objects GPU-checked, %.0f light slot uploads/frame",
                    gRender.mDraws / n, gRender.mTriangles / n, gRender.mLeaves / n, gRender.mSkipped / n,
                    gRender.mEmitMs / n, gRender.mGpuMs / n, double(gRender.mGpuWorst), gRender.mUploadKb / n,
                    (unsigned long)render.mGeometries, (unsigned long)render.mLinearFreeKb,
                    double(gRender.mCmdBufPeak) * 100.0, gRender.mVerified, gRender.mLightSlots / n);
            }
            gRender = RenderSums{};
            gLogAccum = 0;
            gLogFrames = 0;
            gLogWorst = 0;
        }

        if (!gBottomReady)
            return;
        if (++gFramesSinceInfo >= 8)
        {
            gFramesSinceInfo = 0;
            refreshWorldInfo();
        }
        gTimer.fill(gStats);
        p3ds::GameSnapshot snap;
        snap.frame = &gStats;
        snap.cellName = gCellName.c_str();
        for (int i = 0; i < 3; ++i)
            snap.playerPos[i] = gPos[i];
        snap.player = gPlayerLine;
        snap.state = gStateLine;
        snap.stateAlert = gStateAlert;
        snap.status = Render3DS::isActive() ? "citro3d"
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
