#include "debugtools.hpp"

#include "frame3ds.hpp"
#include "hidinject.hpp"
#include "menus.hpp"

#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/screenshot.hpp>

#include "../mwbase/environment.hpp"
#include "../mwbase/luamanager.hpp"
#include "../mwbase/mechanicsmanager.hpp"
#include "../mwbase/statemanager.hpp"
#include "../mwbase/windowmanager.hpp"
#include "../mwbase/world.hpp"
#include "../mwgui/mode.hpp"
#include "../mwgui/savegamedialog.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwphysics/raycasting.hpp"
#include "../mwrender/camera.hpp"
#include "../mwworld/actionteleport.hpp"
#include "../mwworld/cell.hpp"
#include "../mwworld/cellstore.hpp"
#include "../mwworld/class.hpp"
#include "../mwworld/esmstore.hpp"
#include "../mwworld/globals.hpp"
#include "../mwworld/manualref.hpp"
#include "../mwworld/ptr.hpp"
#include "../mwworld/refdata.hpp"
#include "../mwworld/timestamp.hpp"

#include <MyGUI_ITexture.h> // SaveGameDialog holds a unique_ptr to one

#include <components/esm/position.hpp>
#include <components/esm/refid.hpp>
#include <components/esm/util.hpp>
#include <components/esm3/loadbook.hpp>
#include <components/esm3/loadcont.hpp>
#include <components/esm3/loaddial.hpp>
#include <components/esm3/loaddoor.hpp>
#include <components/esm3/loadnpc.hpp>
#include <components/esm3/loadregn.hpp>
#include <components/render3ds/render3ds.hpp>

#include <3ds.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace omw3ds::debug
{
    namespace
    {
        // autorun.txt scripted input: "btn:F,NAME,N" holds a button from game frame F for N frames (default 3);
        // "move:F,X,Y,N" / "look:F,X,Y,N" hold the circle pad / C-stick at X,Y (-1..1, +Y = up/forward);
        // "touch:F,X,Y,N" touches the bottom screen at pixel X,Y.
        struct ScriptedInput
        {
            unsigned mStart = 0;
            unsigned mFrames = 3;
            u32 mKeys = 0; // btn:
            int mStick = -1; // move: 0, look: 1, touch: 2
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
            forEach("touch:", parseStick(2));
        }

        void applyInputs(const std::vector<ScriptedInput>& inputs, unsigned frame)
        {
            if (inputs.empty())
                return;
            u32 held = 0;
            hidinject::Stick sticks[2];
            hidinject::Touch touch;
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
                {
                    const osg::Vec3f pos = MWBase::Environment::get()
                                               .getWorld()
                                               ->getPlayerPtr()
                                               .getRefData()
                                               .getPosition()
                                               .asVec3();
                    P3DS_LOGI("autorun: %s at pos %.0f %.0f %.0f", input.mText.c_str(), double(pos.x()),
                        double(pos.y()), double(pos.z()));
                }
                if (input.mStick < 0)
                    held |= input.mKeys;
                else if (input.mStick == 2)
                {
                    touch = hidinject::Touch{ true, u16(input.mX), u16(input.mY) };
                    held |= KEY_TOUCH;
                }
                else
                    sticks[input.mStick] = hidinject::Stick{ true, input.mX, input.mY };
            }
            hidinject::set(held, sticks[0], sticks[1], touch);
            p3ds::crash::setScriptedButtons(held); // the panic combination's watchdog
        }

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

        // autorun "talk:F": activate the nearest other actor (a dialogue with an NPC).
        void talkToNearestActor()
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
            MWWorld::Ptr nearest;
            float best = 0;
            player.getCell()->forEachType<ESM::NPC>([&](const MWWorld::Ptr& npc) {
                if (npc == player || !npc.getRefData().isEnabled() || npc.getRefData().getBaseNode() == nullptr)
                    return true;
                const float distance = (npc.getRefData().getPosition().asVec3() - playerPos).length2();
                if (nearest.isEmpty() || distance < best)
                {
                    nearest = npc;
                    best = distance;
                }
                return true;
            });
            if (nearest.isEmpty())
            {
                P3DS_LOGW("autorun: no one to talk to");
                return;
            }
            P3DS_LOGI("autorun: talking to %s", nearest.getCellRef().getRefId().toDebugString().c_str());
            MWBase::Environment::get().getLuaManager()->objectActivated(nearest, player);
        }

        // autorun "use:F,TEXT": activate the nearest object (in the scene) whose id contains TEXT (any case).
        void useNearest(std::string text)
        {
            const auto lower = [](std::string value) {
                for (char& c : value)
                    c = char(std::tolower(static_cast<unsigned char>(c)));
                return value;
            };
            text = lower(text);
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            const osg::Vec3f playerPos = player.getRefData().getPosition().asVec3();
            MWWorld::Ptr nearest;
            float best = 0;
            player.getCell()->forEach([&](const MWWorld::Ptr& ptr) {
                if (ptr == player || !ptr.getRefData().isEnabled() || ptr.getRefData().getBaseNode() == nullptr
                    || lower(ptr.getCellRef().getRefId().toDebugString()).find(text) == std::string::npos)
                    return true;
                const float distance = (ptr.getRefData().getPosition().asVec3() - playerPos).length2();
                if (nearest.isEmpty() || distance < best)
                {
                    nearest = ptr;
                    best = distance;
                }
                return true;
            });
            if (nearest.isEmpty())
            {
                P3DS_LOGW("autorun: nothing matching \"%s\" to use", text.c_str());
                return;
            }
            P3DS_LOGI("autorun: using %s", nearest.getCellRef().getRefId().toDebugString().c_str());
            MWBase::Environment::get().getLuaManager()->objectActivated(nearest, player);
        }

        // Automated runs, sdmc:/openmw/autorun.txt: "quit:N" quits after N game frames (tests shutdown),
        // "shot:N,M,..." saves the top screen at those frames (sdmc:/openmw/shot_top_N.bmp).
        struct Autorun
        {
            unsigned mQuitAt = 0;
            unsigned mCloseAt = 0; // "close:N": like closing from the HOME Menu after N frames
            std::vector<unsigned> mShots;
            std::vector<unsigned> mLoadViews; // "loading:N,...": both screens at the Nth tick of a load
            float mTurnDegrees = 0; // "turn:D": rotate the player by D degrees at frame 30
            std::vector<ScriptedInput> mInputs;
            std::vector<unsigned> mDoors; // "door:F,G,...": activate the nearest door at those frames
            std::vector<unsigned> mSaves; // "save:F": a quick save at frame F
            std::vector<unsigned> mLoads; // "load:F": the quick load at frame F
            std::vector<std::pair<unsigned, std::string>> mGuiModes; // "gui:F,<mode>": open a window at frame F
            unsigned mLuaProfileAt = 0; // "luaprof:F": write the Lua profiler table (needs "lua profiler = true")
            unsigned mPosAt = 0; // "pos:F,X,Y,Z": put the player at X Y Z (same worldspace) at frame F
            osg::Vec3f mPos;
            // "weather:F,N": at frame F, the player's region changes to weather N (0 clear, 1 cloudy, 2 foggy,
            // 3 overcast, 4 rain, 5 thunder, 6 ash, 7 blight, 8 snow, 9 blizzard), like the ChangeWeather
            // command: it transitions over a few seconds.
            unsigned mWeatherAt = 0;
            int mWeather = 0;
            // "view:F,X,Y,Z,YAW,PITCH,POV,CELL": at frame F, the player at X Y Z facing YAW, looking PITCH degrees
            // down, in first (1) or third (3) person, in CELL ("ext" or an interior, '_' for spaces): the token a
            // screenshot's "view" log line starts with (describeView).
            unsigned mViewAt = 0;
            float mView[5] = {};
            int mViewPov = 1;
            std::string mViewCell;
            unsigned mHourAt = 0; // "hour:F,H": at frame F, the game time is H o'clock
            float mHour = 0;
            unsigned mTeleportAt = 0; // "tp:F,N": at frame F, teleport N (from 0, sTeleports)
            int mTeleport = 0;
            unsigned mTalkAt = 0; // "talk:F": at frame F, talk to the nearest actor (activate it)
            // "use:F,TEXT" (any number): at frame F, activate the nearest object whose id contains TEXT
            std::vector<std::pair<unsigned, std::string>> mUses;
            std::vector<std::pair<unsigned, int>> mPicks; // "pick:F,N" (any number): touch menu option N at frame F
        };
        unsigned sLoadingTicks = 0; // since the last engine frame

        const Autorun& autorunScript()
        {
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
                    if (const char* q = std::strstr(buf, "weather:"))
                    {
                        char* end = nullptr;
                        result.mWeatherAt = unsigned(std::strtoul(q + 8, &end, 10));
                        if (*end == ',')
                            result.mWeather = int(std::strtol(end + 1, nullptr, 10));
                    }
                    if (const char* q = std::strstr(buf, "hour:"))
                    {
                        char* end = nullptr;
                        result.mHourAt = unsigned(std::strtoul(q + 5, &end, 10));
                        if (*end == ',')
                            result.mHour = std::strtof(end + 1, nullptr);
                        else
                            result.mHourAt = 0;
                    }
                    if (const char* q = std::strstr(buf, "talk:"))
                        result.mTalkAt = unsigned(std::strtoul(q + 5, nullptr, 10));
                    for (const char* q = std::strstr(buf, "use:"); q != nullptr; q = std::strstr(q + 4, "use:"))
                    {
                        char* end = nullptr;
                        const unsigned at = unsigned(std::strtoul(q + 4, &end, 10));
                        if (*end != ',')
                            continue;
                        const char* text = end + 1;
                        const char* textEnd = text;
                        while (*textEnd != '\0' && !std::isspace(static_cast<unsigned char>(*textEnd)))
                            ++textEnd;
                        result.mUses.emplace_back(at, std::string(text, textEnd));
                    }
                    for (const char* q = std::strstr(buf, "pick:"); q != nullptr; q = std::strstr(q + 5, "pick:"))
                    {
                        char* end = nullptr;
                        const unsigned at = unsigned(std::strtoul(q + 5, &end, 10));
                        if (*end == ',')
                            result.mPicks.emplace_back(at, int(std::strtol(end + 1, nullptr, 10)));
                    }
                    if (const char* q = std::strstr(buf, "tp:"))
                    {
                        char* end = nullptr;
                        result.mTeleportAt = unsigned(std::strtoul(q + 3, &end, 10));
                        if (*end == ',')
                            result.mTeleport = int(std::strtol(end + 1, nullptr, 10));
                        else
                            result.mTeleportAt = 0;
                    }
                    if (const char* q = std::strstr(buf, "view:"))
                    {
                        char* end = nullptr;
                        result.mViewAt = unsigned(std::strtoul(q + 5, &end, 10));
                        int fields = 0;
                        for (; fields < 5 && *end == ','; ++fields)
                            result.mView[fields] = std::strtof(end + 1, &end);
                        if (fields == 5 && *end == ',')
                            result.mViewPov = int(std::strtol(end + 1, &end, 10));
                        if (fields == 5 && *end == ',')
                        {
                            const char* cell = end + 1;
                            const char* cellEnd = cell;
                            while (*cellEnd != '\0' && !std::isspace(static_cast<unsigned char>(*cellEnd)))
                                ++cellEnd;
                            result.mViewCell.assign(cell, cellEnd);
                            std::replace(result.mViewCell.begin(), result.mViewCell.end(), '_', ' ');
                        }
                        if (result.mViewCell.empty())
                        {
                            P3DS_LOGW("autorun: can't parse the view: token");
                            result.mViewAt = 0;
                        }
                    }
                    if (const char* q = std::strstr(buf, "pos:"))
                    {
                        char* end = nullptr;
                        result.mPosAt = unsigned(std::strtoul(q + 4, &end, 10));
                        for (int i = 0; i < 3 && *end == ','; ++i)
                            result.mPos[i] = std::strtof(end + 1, &end);
                    }
                    if (const char* q = std::strstr(buf, "door:"))
                    {
                        char* end = const_cast<char*>(q + 5);
                        do
                            result.mDoors.push_back(unsigned(std::strtoul(end + (*end == ',' ? 1 : 0), &end, 10)));
                        while (*end == ',');
                    }
                    for (const char* q = std::strstr(buf, "gui:"); q != nullptr; q = std::strstr(q + 4, "gui:"))
                    {
                        char* end = nullptr;
                        const unsigned at = unsigned(std::strtoul(q + 4, &end, 10));
                        if (*end != ',')
                            continue;
                        std::string mode;
                        for (const char* c = end + 1; *c != '\0' && !std::isspace(static_cast<unsigned char>(*c)); ++c)
                            mode += *c;
                        result.mGuiModes.emplace_back(at, mode);
                    }
                    if (const char* q = std::strstr(buf, "save:"))
                        result.mSaves.push_back(unsigned(std::strtoul(q + 5, nullptr, 10)));
                    if (const char* q = std::strstr(buf, "load:"))
                        result.mLoads.push_back(unsigned(std::strtoul(q + 5, nullptr, 10)));
                    if (const char* q = std::strstr(buf, "loading:"))
                    {
                        char* end = const_cast<char*>(q + 8);
                        do
                            result.mLoadViews.push_back(
                                unsigned(std::strtoul(end + (*end == ',' ? 1 : 0), &end, 10)));
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
            return autorun;
        }

        // Exteriors: where the game puts the player on leaving a well-known building of the town (its door's
        // destination in Morrowind.esm). "coc <town>" goes to the middle of the town's first cell instead, the
        // hills west of Balmora. Interiors: like "coc".
        struct TeleportTarget
        {
            const char* mLabel;
            const char* mInterior; // nullptr: exterior, at mPos facing mYaw
            float mPos[3];
            float mYaw; // degrees
        };
        constexpr TeleportTarget sTeleports[] = {
            { "Seyda Neen", nullptr, { -11615, -70115, 316 }, 91.7f }, // Arrille's Tradehouse, front door
            { "Balmora", nullptr, { -21792, -12087, 882 }, 270.f }, // The Eight Plates, street door
            { "Ald-ruhn", nullptr, { -13071, 51975, 2182 }, 292.9f }, // The Rat In The Pot
            { "Vivec", nullptr, { 31477, -79741, 3348 }, 90.f }, // Foreign Quarter Plaza
            { "Caldera", nullptr, { -11304, 17656, 1330 }, 270.f }, // Shenk's Shovel
            { "Pelagiad", nullptr, { 2507, -56048, 1570 }, 134.6f }, // Halfway Tavern
            { "Sadrith Mora", nullptr, { 144071, 35904, 876 }, 270.f }, // Gateway Inn
            { "Council Club", "Balmora, Council Club", {}, 0.f }, // interior: lights and people
        };
    }

    // Where the player is and what the camera looks at, starting with autorun tokens that put the player
    // back there: "view:F,X,Y,Z,YAW,PITCH,POV,CELL" (degrees; POV 1 or 3 = first/third person; CELL "ext"
    // for exteriors, else the interior's name with '_' for spaces), "hour:F,H" and, outdoors, "weather:F,N".
    // Logged with every screenshot, so a view seen on the console can be recreated in the emulator. The
    // crosshair ray costs a few ms: screenshots only.
    std::string describeView(unsigned frame)
    {
        MWBase::World* world = MWBase::Environment::get().getWorld();
        if (world == nullptr)
            return "(no world yet)";
        const MWWorld::Ptr player = world->getPlayerPtr();
        if (player.isEmpty() || !player.isInCell())
            return "(no player yet)";
        const ESM::Position& p = player.getRefData().getPosition();
        const MWWorld::Cell& cell = *player.getCell()->getCell();
        std::string cellField = "ext";
        if (!cell.isExterior())
        {
            cellField = std::string(cell.getNameId());
            std::replace(cellField.begin(), cellField.end(), ' ', '_');
        }
        const bool firstPerson = world->isFirstPerson();
        char weather[32] = "";
        if (cell.isExterior()) // applied after the view (it needs the player in the region)
            std::snprintf(weather, sizeof(weather), " weather:%u,%d", frame + 30, world->getCurrentWeatherScriptId());
        char buf[768];
        int len = std::snprintf(buf, sizeof(buf), "view:%u,%.0f,%.0f,%.0f,%.1f,%.1f,%d,%s hour:%u,%.2f%s [%s]",
            frame, double(p.pos[0]), double(p.pos[1]), double(p.pos[2]), double(osg::RadiansToDegrees(p.rot[2])),
            double(osg::RadiansToDegrees(p.rot[0])), firstPerson ? 1 : 3, cellField.c_str(), frame,
            double(world->getTimeStamp().getHour()), weather, std::string(cell.getDescription()).c_str());
        if (const MWRender::Camera* camera = world->getCamera(); camera != nullptr && len > 0)
        {
            const osg::Vec3f eye = camera->getPosition();
            const osg::Vec3f forward = camera->getOrient() * osg::Vec3f(0, 1, 0);
            const osg::Vec3f end = eye + forward * 8192.f;
            MWPhysics::RayCastingResult hit;
            std::string target = "nothing within 8192 units";
            if (world->castRenderingRay(hit, eye, end, true, false, false))
            {
                char at[48];
                std::snprintf(at, sizeof(at), " at %.0f units", double((hit.mHitPos - eye).length()));
                target = hit.mHitObject.isEmpty() ? std::string("terrain") + at
                                                  : hit.mHitObject.getCellRef().getRefId().toDebugString() + " ("
                        + std::string(hit.mHitObject.getTypeDescription()) + ")" + at;
            }
            std::snprintf(buf + len, sizeof(buf) - len,
                " | %s person, eye %.0f %.0f %.0f, camera yaw %.1f pitch %.1f, crosshair: %s",
                firstPerson ? "first" : "third", double(eye.x()), double(eye.y()), double(eye.z()),
                double(osg::RadiansToDegrees(-camera->getYaw())),
                double(osg::RadiansToDegrees(-camera->getPitch())), target.c_str()); // the player's conventions
        }
        return buf;
    }

    void runAutorun(unsigned frame)
    {
        const Autorun& autorun = autorunScript();
        sLoadingTicks = 0;
        if (autorun.mQuitAt != 0 && frame == autorun.mQuitAt)
        {
            P3DS_LOGI("autorun: quitting after %u frames", autorun.mQuitAt);
            MWBase::Environment::get().getStateManager()->requestQuit();
        }
        if (autorun.mLuaProfileAt != 0 && frame == autorun.mLuaProfileAt)
        {
            const std::string table = MWBase::Environment::get().getLuaManager()->formatResourceUsageStats();
            if (std::FILE* f = std::fopen("sdmc:/openmw/lua_profile.txt", "w"))
            {
                std::fwrite(table.data(), 1, table.size(), f);
                std::fclose(f);
            }
            P3DS_LOGI("autorun: Lua profile written to sdmc:/openmw/lua_profile.txt (%zu bytes)", table.size());
        }
        if (autorun.mCloseAt != 0 && frame == autorun.mCloseAt)
        {
            P3DS_LOGI("autorun: closing as if from the HOME Menu after %u frames", autorun.mCloseAt);
            closeAsFromHomeMenu();
        }
        for (unsigned door : autorun.mDoors)
            if (door == frame)
                activateNearestDoor();
        for (unsigned at : autorun.mSaves)
            if (at == frame)
            {
                P3DS_LOGI("autorun: quick save");
                MWBase::Environment::get().getStateManager()->quickSave("Autorun");
            }
        for (unsigned at : autorun.mLoads)
            if (at == frame)
            {
                P3DS_LOGI("autorun: quick load");
                MWBase::Environment::get().getStateManager()->quickLoad();
            }
        for (const auto& [at, mode] : autorun.mGuiModes)
            if (at == frame)
            {
                static const std::pair<const char*, MWGui::GuiMode> modes[] = { { "race", MWGui::GM_Race },
                    { "class", MWGui::GM_Class }, { "classpick", MWGui::GM_ClassPick },
                    { "classcreate", MWGui::GM_ClassCreate }, { "classgen", MWGui::GM_ClassGenerate },
                    { "birth", MWGui::GM_Birth }, { "review", MWGui::GM_Review }, { "name", MWGui::GM_Name },
                    { "mainmenu", MWGui::GM_MainMenu }, { "journal", MWGui::GM_Journal },
                    { "rest", MWGui::GM_Rest }, { "levelup", MWGui::GM_Levelup },
                    { "quickkeys", MWGui::GM_QuickKeysMenu }, { "alchemy", MWGui::GM_Alchemy } };
                bool found = false;
                for (const auto& [name, gm] : modes)
                    if (mode == name)
                    {
                        P3DS_LOGI("autorun: opening the %s window", name);
                        MWBase::Environment::get().getWindowManager()->pushGuiMode(gm);
                        found = true;
                    }
                if (!found && (mode == "save" || mode == "load"))
                {
                    static std::unique_ptr<MWGui::SaveGameDialog> dialog; // as the main menu keeps its own
                    if (!dialog)
                        dialog = std::make_unique<MWGui::SaveGameDialog>();
                    P3DS_LOGI("autorun: the %s dialog", mode.c_str());
                    dialog->setLoadOrSave(mode == "load");
                    dialog->setVisible(true);
                    found = true;
                }
                if (!found && mode == "settings")
                {
                    P3DS_LOGI("autorun: the options window");
                    MWBase::Environment::get().getWindowManager()->toggleSettingsWindow();
                    found = true;
                }
                if (!found && mode == "container")
                {
                    const MWWorld::Ptr player = MWMechanics::getPlayer();
                    const osg::Vec3f at = player.getRefData().getPosition().asVec3();
                    MWWorld::Ptr nearest;
                    float best = 0.f;
                    player.getCell()->forEachType<ESM::Container>([&](const MWWorld::Ptr& ptr) {
                        const float d = (ptr.getRefData().getPosition().asVec3() - at).length();
                        if (nearest.isEmpty() || d < best)
                        {
                            nearest = ptr;
                            best = d;
                        }
                        return true;
                    });
                    if (!nearest.isEmpty())
                    {
                        P3DS_LOGI("autorun: opening '%s'", nearest.getCellRef().getRefId().toDebugString().c_str());
                        MWBase::Environment::get().getWindowManager()->pushGuiMode(MWGui::GM_Container, nearest);
                        found = true;
                    }
                }
                if (!found && (mode == "dialogue" || mode == "travel" || mode == "spellmaking" || mode == "training"
                                   || mode == "barter" || mode == "spellbuying" || mode == "merchantrepair"
                                   || mode == "enchanting"))
                {
                    const MWWorld::Ptr player = MWMechanics::getPlayer();
                    MWWorld::Ptr nearest;
                    float best = 0.f;
                    std::vector<MWWorld::Ptr> actors;
                    MWBase::Environment::get().getMechanicsManager()->getActorsInRange(
                        player.getRefData().getPosition().asVec3(), 2000.f, actors);
                    for (const MWWorld::Ptr& actor : actors)
                    {
                        if (actor == player)
                            continue;
                        // An NPC offering the service the window needs.
                        const int services = actor.getClass().getServices(actor);
                        if ((mode == "barter" && (services & ESM::NPC::AllItems) == 0)
                            || (mode == "spellbuying" && (services & ESM::NPC::Spells) == 0)
                            || (mode == "spellmaking" && (services & ESM::NPC::Spellmaking) == 0)
                            || (mode == "training" && (services & ESM::NPC::Training) == 0)
                            || (mode == "merchantrepair" && (services & ESM::NPC::Repair) == 0)
                            || (mode == "enchanting" && (services & ESM::NPC::Enchanting) == 0)
                            || (mode == "travel" && actor.getClass().isNpc()
                                && actor.get<ESM::NPC>()->mBase->getTransport().empty()))
                            continue;
                        const float d = (actor.getRefData().getPosition().asVec3() - player.getRefData().getPosition().asVec3()).length();
                        if (nearest.isEmpty() || d < best)
                        {
                            nearest = actor;
                            best = d;
                        }
                    }
                    if (!nearest.isEmpty())
                    {
                        P3DS_LOGI("autorun: %s with '%s'", mode.c_str(), nearest.getCellRef().getRefId().toDebugString().c_str());
                        const MWGui::GuiMode gm = mode == "travel" ? MWGui::GM_Travel
                            : mode == "spellmaking"                ? MWGui::GM_SpellCreation
                            : mode == "training"                   ? MWGui::GM_Training
                            : mode == "barter"                     ? MWGui::GM_Barter
                            : mode == "spellbuying"                ? MWGui::GM_SpellBuying
                            : mode == "merchantrepair"             ? MWGui::GM_MerchantRepair
                            : mode == "enchanting"                 ? MWGui::GM_Enchanting
                                                                   : MWGui::GM_Dialogue;
                        MWBase::Environment::get().getWindowManager()->pushGuiMode(gm, nearest);
                        found = true;
                    }
                }
                if (!found && (mode == "book" || mode == "scroll"))
                {
                    // The first book (or scroll) of the store, as a reference of its own, read like ActionRead.
                    static std::unique_ptr<MWWorld::ManualRef> ref; // alive while the window is up
                    const bool scroll = mode == "scroll";
                    for (const ESM::Book& book : MWBase::Environment::get().getESMStore()->get<ESM::Book>())
                    {
                        if ((book.mData.mIsScroll != 0) != scroll || book.mText.get().size() < 400)
                            continue;
                        ref = std::make_unique<MWWorld::ManualRef>(*MWBase::Environment::get().getESMStore(), book.mId);
                        P3DS_LOGI("autorun: reading %s '%s'", mode.c_str(), book.mId.toDebugString().c_str());
                        MWBase::Environment::get().getWindowManager()->pushGuiMode(
                            scroll ? MWGui::GM_Scroll : MWGui::GM_Book, ref->getPtr());
                        found = true;
                        break;
                    }
                }
                if (!found)
                    P3DS_LOGW("autorun: gui: unknown window \"%s\"", mode.c_str());
            }
        if (autorun.mWeatherAt != 0 && frame == autorun.mWeatherAt)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            const ESM::RefId region = player.isInCell() ? player.getCell()->getCell()->getRegion() : ESM::RefId();
            if (region.empty())
                P3DS_LOGW("autorun: weather: the player is in no region (an interior?)");
            else
            {
                world->changeWeather(region, ESM::Weather::indexToRefId(autorun.mWeather));
                P3DS_LOGI("autorun: weather %d in %s", autorun.mWeather, region.toDebugString().c_str());
            }
        }
        if (autorun.mPosAt != 0 && frame == autorun.mPosAt)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            world->moveObject(world->getPlayerPtr(), autorun.mPos);
            P3DS_LOGI("autorun: player moved to %.0f %.0f %.0f", double(autorun.mPos.x()), double(autorun.mPos.y()),
                double(autorun.mPos.z()));
        }
        if (autorun.mHourAt != 0 && frame == autorun.mHourAt)
        {
            MWBase::Environment::get().getWorld()->setGlobalFloat(MWWorld::Globals::sGameHour, autorun.mHour);
            P3DS_LOGI("autorun: game hour %.2f", double(autorun.mHour));
        }
        if (autorun.mViewAt != 0 && frame == autorun.mViewAt)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            ESM::Position position{};
            for (int i = 0; i < 3; ++i)
                position.pos[i] = autorun.mView[i];
            position.rot[0] = osg::DegreesToRadians(autorun.mView[4]);
            position.rot[2] = osg::DegreesToRadians(autorun.mView[3]);
            if (autorun.mViewCell == "ext")
            {
                const ESM::ExteriorCellLocation index
                    = ESM::positionToExteriorCellLocation(position.pos[0], position.pos[1]);
                world->changeToCell(ESM::RefId::esm3ExteriorCell(index.mX, index.mY), position, true);
            }
            else
                world->changeToInteriorCell(autorun.mViewCell, position, true);
            if ((autorun.mViewPov == 3) == world->isFirstPerson())
                world->togglePOV(true);
            P3DS_LOGI("autorun: view set: %.0f %.0f %.0f yaw %.1f pitch %.1f, %s person, cell %s",
                double(position.pos[0]), double(position.pos[1]), double(position.pos[2]), double(autorun.mView[3]),
                double(autorun.mView[4]), autorun.mViewPov == 3 ? "third" : "first", autorun.mViewCell.c_str());
        }
        if (autorun.mTeleportAt != 0 && frame == autorun.mTeleportAt)
            teleport(autorun.mTeleport);
        if (autorun.mTalkAt != 0 && frame == autorun.mTalkAt)
            talkToNearestActor();
        for (const auto& [at, text] : autorun.mUses)
            if (at == frame)
                useNearest(text);
        for (const auto& [at, option] : autorun.mPicks)
            if (at == frame)
            {
                P3DS_LOGI("autorun: menu option %d", option);
                pressMenuOption(option);
            }
        for (unsigned shot : autorun.mShots)
            if (shot == frame)
            {
                const std::string path = "sdmc:/openmw/shot_top_" + std::to_string(shot) + ".bmp";
                Render3DS::requestCapture(path.c_str());
                P3DS_LOGI("screenshot %s: %s", path.c_str(), describeView(frame).c_str());
                // The bottom screen too (its framebuffer holds the last frame drawn there: M8's GUI or the text
                // display).
                const std::string bottom = "sdmc:/openmw/shot_bottom_" + std::to_string(shot) + ".bmp";
                p3ds::screenshot::saveBottomScreen(bottom.c_str());
            }
        const float turn = frame == 30 ? autorun.mTurnDegrees : 0.f;
        if (turn != 0)
        {
            MWBase::World* world = MWBase::Environment::get().getWorld();
            const MWWorld::Ptr player = world->getPlayerPtr();
            osg::Vec3f rot = player.getRefData().getPosition().asRotationVec3();
            rot.z() += osg::DegreesToRadians(turn);
            world->rotateObject(player, rot, MWBase::RotationFlag_none);
            P3DS_LOGI("debug: player turned to %.0f degrees", double(osg::RadiansToDegrees(rot.z())));
        }
    }

    void onLoadingTickAutorun()
    {
        const unsigned tick = ++sLoadingTicks;
        for (unsigned view : autorunScript().mLoadViews)
            if (view == tick)
            {
                const std::string name = "load" + std::to_string(view);
                Render3DS::requestCapture(("sdmc:/openmw/shot_top_" + name + ".bmp").c_str());
                p3ds::screenshot::saveBottomScreen(("sdmc:/openmw/shot_bottom_" + name + ".bmp").c_str());
                P3DS_LOGI("autorun: loading screens saved (%s)", name.c_str());
            }
    }

    void applyScriptedInput(unsigned frame)
    {
        applyInputs(autorunScript().mInputs, frame);
    }

    void teleport(int index)
    {
        if (index < 0 || index >= int(std::size(sTeleports)))
            return;
        const TeleportTarget& target = sTeleports[index];
        MWBase::World* world = MWBase::Environment::get().getWorld();
        ESM::Position pos{};
        if (target.mInterior == nullptr)
        {
            for (int i = 0; i < 3; ++i)
                pos.pos[i] = target.mPos[i];
            pos.rot[2] = osg::DegreesToRadians(target.mYaw);
            const ESM::ExteriorCellLocation cell = ESM::positionToExteriorCellLocation(pos.pos[0], pos.pos[1]);
            P3DS_LOGI("debug: teleport to %s (%.0f %.0f %.0f)", target.mLabel, double(pos.pos[0]), double(pos.pos[1]),
                double(pos.pos[2]));
            world->changeToCell(ESM::RefId::esm3ExteriorCell(cell.mX, cell.mY), pos, true);
            return;
        }
        // MWScript's OpCOC for an interior (mwscript/cellextensions.cpp).
        if (const ESM::RefId refId = world->findInteriorPosition(target.mInterior, pos); !refId.empty())
        {
            P3DS_LOGI("debug: teleport to %s", target.mInterior);
            MWWorld::ActionTeleport(refId, pos, false).execute(world->getPlayerPtr());
            return;
        }
        P3DS_LOGW("debug: teleport: no cell %s", target.mInterior);
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
}
