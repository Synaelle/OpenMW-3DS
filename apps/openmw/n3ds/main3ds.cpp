// Nintendo 3DS entry point (replaces main() in apps/openmw/main.cpp on this platform).
//
// Brings up the platform layer (log file, crash logger, hang watchdog, memory split, bottom-screen
// debug display) before handing over to OpenMW's unchanged runApplication(). Command-line options come
// from sdmc:/openmw/args.txt (one argument per line); the debug default boots straight into a new game
// with the default generated character in Seyda Neen:
//   --skip-menu --start "Seyda Neen"
// (--new-game is deliberately absent: it would start character generation and the intro movie.)

#include "frame3ds.hpp"
#include "sound3ds.hpp"

#include <platform3ds/crash.hpp>
#include <platform3ds/loadscreen.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/platform.hpp>

#include <components/debug/debugging.hpp>
#include <components/render3ds/render3ds.hpp>

#include <3ds.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#ifdef OPENMW3DS_DEBUG
#include <platform3ds/menuart.hpp>
#include <platform3ds/textfb.hpp>

#include "benchmark3ds.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>
#include <sstream>
#include <sys/stat.h>
#endif

int runApplication(int argc, char* argv[]); // apps/openmw/main.cpp

namespace
{
    constexpr const char* kArgsPath = "sdmc:/openmw/args.txt";

    // The start menu's choice (M15): Start boots the game's own main menu (a new game goes through character
    // creation); Debug's "Start (skip the intro)" keeps the test set-up of args.txt (--skip-menu --start <cell>
    // --script-run test-start.txt: the test character with its gold, skills, axe and spells). Automated runs skip
    // the intro.
    bool gSkipIntro = true;
    bool gBenchmark = false; // the start menu's third choice (benchmark3ds.hpp)
#ifdef OPENMW3DS_DEBUG
    omw3ds::benchmark::Set gBenchmarkSet = omw3ds::benchmark::Set::Morrowind;
#endif

    std::vector<std::string> loadArguments()
    {
        std::vector<std::string> args{ "openmw" };
        std::ifstream file(kArgsPath);
        if (file)
        {
            std::string line;
            while (std::getline(file, line))
            {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (!line.empty() && line[0] != '#')
                    args.push_back(line);
            }
            P3DS_LOGI("arguments from %s", kArgsPath);
            if (gBenchmark)
            {
                // A new game of its own (no test script, no character creation), started in Seyda Neen: the
                // benchmark takes it round from there. Nothing is saved. A fixed random seed: the same AI and weather
                // rolls each run (frame times still differ, so NPCs can still drift apart a little).
                args = { "openmw", "--skip-menu", "--start", "Seyda Neen", "--random-seed", "1" };
                P3DS_LOGI("benchmark: about 5 minutes, then the game quits");
            }
            else if (!gSkipIntro)
            {
                // The game's own start: no skipped menu, no start cell, no test script.
                std::vector<std::string> kept;
                for (size_t i = 0; i < args.size(); ++i)
                {
                    const std::string& a = args[i];
                    if (a == "--skip-menu" || a == "--new-game")
                        continue;
                    if ((a == "--start" || a == "--script-run") && i + 1 < args.size())
                    {
                        ++i;
                        continue;
                    }
                    kept.push_back(a);
                }
                args = std::move(kept);
                P3DS_LOGI("start normally: the main menu");
            }
        }
        else
        {
            args.insert(args.end(), { "--skip-menu", "--start", "Seyda Neen" });
            P3DS_LOGI("no %s: using debug defaults", kArgsPath);
        }
        for (const std::string& a : args)
            P3DS_LOGI("  arg: %s", a.c_str());
        return args;
    }

    const char* gContext = "openmw: startup";

#ifdef OPENMW3DS_DEBUG
    // ------------------------------------------------------------------ the start menu (M15)
    //
    // Shown before the platform is up (p3ds::init: the chosen start decides the debug mode and where the logs go),
    // drawn on the bottom screen by the CPU. "Start" is the game; "Debug" holds the test set-ups (skip the intro, the
    // benchmark, the hardware test's runs); "Options", "Check for updates", "Quit".
    //
    // Hardware tests: sdmc:/openmw/test-menu.txt lists the test's runs (the switch files each one needs), shown in
    // Debug as "Test run: <label>" (A goes to the next run); Debug's starts create the chosen run's switch files and
    // delete the others', a normal Start deletes them all:
    //   title <text>
    //   logs <prefix>
    //   run <label> | <switch files, separated by spaces, or nothing>
    struct TestRun
    {
        std::string mLabel;
        std::vector<std::string> mSwitches;
    };

    struct TestRuns
    {
        std::string mTitle;
        std::string mLogPrefix;
        std::vector<TestRun> mRuns;
        size_t mSelected = 0;
    };

    std::string trim(const std::string& text)
    {
        const size_t first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            return {};
        return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    }

    TestRuns readTestRuns()
    {
        TestRuns runs;
        std::ifstream file("sdmc:/openmw/test-menu.txt");
        std::string line;
        while (file && std::getline(file, line))
        {
            line = trim(line);
            if (line.rfind("title ", 0) == 0)
                runs.mTitle = trim(line.substr(6));
            else if (line.rfind("logs ", 0) == 0)
                runs.mLogPrefix = trim(line.substr(5));
            else if (line.rfind("run ", 0) == 0)
            {
                TestRun run;
                const std::string rest = line.substr(4);
                const size_t bar1 = rest.find('|');
                const size_t bar2 = bar1 == std::string::npos ? bar1 : rest.find('|', bar1 + 1);
                run.mLabel = trim(rest.substr(0, bar1));
                if (bar1 != std::string::npos)
                {
                    const size_t end = bar2 == std::string::npos ? rest.size() : bar2;
                    std::istringstream names(rest.substr(bar1 + 1, end - bar1 - 1));
                    std::string name;
                    while (names >> name)
                        run.mSwitches.push_back(name);
                }
                runs.mRuns.push_back(run);
            }
        }
        return runs;
    }

    // The chosen run's switch files exist afterwards, every other known one doesn't (`run` null: none, a normal
    // start). Past tests' switches are in the list too (hardware test #57: a "gpu-ui" left by test #56 kept the
    // deprecated GPU interface on).
    void applySwitches(const TestRuns& runs, const TestRun* run)
    {
        std::set<std::string> all{ "gpu-ui", "soft-gui", "lumiere", "lumiere-demo", "no-fills", "fills", "fills-top",
            "fills-bottom", "fills-opaque", "gui-on-scene", "gui-no-reupload", "ui3ds-demo", "no-m8ui", "cpu-skinning",
            "draw-census", "memwalk" };
        for (const TestRun& r : runs.mRuns)
            all.insert(r.mSwitches.begin(), r.mSwitches.end());
        for (const std::string& name : all)
        {
            const std::string path = "sdmc:/openmw/" + name;
            struct stat st;
            const bool exists = stat(path.c_str(), &st) == 0;
            const bool want = run != nullptr && std::find(run->mSwitches.begin(), run->mSwitches.end(), name)
                != run->mSwitches.end();
            if (want && !exists)
            {
                if (std::FILE* f = std::fopen(path.c_str(), "w"))
                    std::fclose(f);
            }
            else if (!want && exists)
                std::remove(path.c_str());
        }
    }

    // A debug run's log folder: logs/<date>-<time>[-<test>-<run>] (the run: its label up to ':', "A: ..." -> "A").
    std::string debugLogFolder(const TestRuns& runs)
    {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        std::string folder = stamp;
        if (!runs.mRuns.empty() && !runs.mLogPrefix.empty())
        {
            std::string run;
            const std::string& label = runs.mRuns[runs.mSelected].mLabel;
            for (char c : label.substr(0, label.find(':')))
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '-')
                    run += c;
            folder += "-" + runs.mLogPrefix + (run.empty() ? "" : "-" + run);
        }
        return folder;
    }

    namespace menu
    {
        namespace tf = p3ds::textfb;
        namespace art = p3ds::menuart;

        // The loading screen (platform3ds/loadscreen.cpp) keeps this layout.
        constexpr int kTitleY = 14;
        constexpr int kHeaderLineY = 46;
        constexpr int kItemsY = 58;
        constexpr int kItemHeight = 25;
        constexpr int kItemX = art::kMargin + 12;
        constexpr int kFooterLineY = tf::kHeight - 30;
        constexpr int kMessageY = kFooterLineY - 22;
        constexpr int kFooterY = tf::kHeight - 24;

        struct Item
        {
            std::string mLabel;
            bool mEnabled = true;
        };

        // One page: the title, the items (UP/DOWN and A, or a tap on one), a message line and the controls. Returns
        // the item chosen, or -1 for B.
        int page(const std::string& title, const std::vector<Item>& items, std::string& message, size_t& selected,
            bool canGoBack)
        {
            bool redraw = true;
            while (aptMainLoop())
            {
                if (redraw)
                {
                    // Design 4 of docs/start-menu-designs.png: flat, left-aligned, MysticCards (platform3ds/menuart.cpp).
                    art::background();
                    art::text(art::kMargin, kTitleY, "OpenMW 3DS", art::Font::Title, art::colour::title);
                    if (!title.empty())
                        art::rightAligned(kTitleY + 8, title.c_str(), art::Font::Small, art::colour::dim);
                    art::separator(kHeaderLineY);
                    for (size_t i = 0; i < items.size(); ++i)
                    {
                        const int y = kItemsY + int(i) * kItemHeight;
                        if (i == selected)
                            art::mark(y, art::lineHeight(art::Font::Text));
                        art::text(kItemX, y, items[i].mLabel.c_str(), art::Font::Text,
                            !items[i].mEnabled ? art::colour::disabled
                                : i == selected ? art::colour::selected
                                                : art::colour::text);
                    }
                    if (!message.empty())
                        art::text(kItemX, kMessageY, message.c_str(), art::Font::Text, art::colour::title);
                    art::separator(kFooterLineY);
                    art::text(art::kMargin, kFooterY, canGoBack ? "A choose     B back" : "A choose", art::Font::Small,
                        art::colour::dim);
                    art::rightAligned(kFooterY, OPENMW3DS_VERSION, art::Font::Small, art::colour::dim);
                    tf::present();
                    redraw = false;
                }
                gspWaitForVBlank();
                hidScanInput();
                const u32 down = hidKeysDown();
                if (down & KEY_DOWN)
                {
                    selected = (selected + 1) % items.size();
                    redraw = true;
                }
                if (down & KEY_UP)
                {
                    selected = (selected + items.size() - 1) % items.size();
                    redraw = true;
                }
                if (down & KEY_A)
                    return int(selected);
                if ((down & KEY_B) && canGoBack)
                    return -1;
                if (down & KEY_TOUCH)
                {
                    touchPosition touch;
                    hidTouchRead(&touch);
                    for (size_t i = 0; i < items.size(); ++i)
                    {
                        const int y = kItemsY + int(i) * kItemHeight - 2;
                        if (touch.py >= y && touch.py < y + kItemHeight)
                        {
                            selected = i;
                            return int(i);
                        }
                    }
                }
            }
            return -1;
        }
    }

    enum class Start
    {
        Game, // the main menu, debug tools off
        DebugGame, // straight into the game with the test character (--skip-menu)
        Benchmark,
        Quit,
    };

    Start startMenu(TestRuns& runs, std::string& chosenLine)
    {
        std::string message;
        size_t mainSelected = 0;
        while (true)
        {
            const int choice = menu::page("",
                { { "Start" }, { "Debug" }, { "Options" }, { "Check for updates" }, { "Quit" } }, message, mainSelected,
                false);
            message.clear();
            if (choice == 0)
            {
                chosenLine = "start: the game (debug tools off)";
                return Start::Game;
            }
            if (choice == 3)
            {
                message = "No update information yet.";
                continue;
            }
            if (choice == 4 || choice < 0)
                return Start::Quit;
            if (choice == 2)
            {
                // Options: one choice each for now (the start menu's later choices, LIMITATIONS.md M15).
                size_t selected = 0;
                std::string note;
                while (true)
                {
                    const int option = menu::page("Options",
                        { { "Renderer: Citro3D" }, { "Layout: 3DS" }, { "Interface: MyGUI (CPU)" }, { "Back" } }, note,
                        selected, true);
                    if (option < 0 || option == 3)
                        break;
                    note = option == 0 ? "The only renderer for now."
                        : option == 1  ? "\"Like on PC\" is coming."
                                       : "Lumiere-new is coming.";
                }
                continue;
            }
            // Debug.
            size_t selected = 0;
            std::string note;
            while (true)
            {
                std::vector<menu::Item> items{ { "Start (skip the intro)" }, { "Benchmark" } };
                if (!runs.mRuns.empty())
                    items.push_back({ "Test run: " + runs.mRuns[runs.mSelected].mLabel });
                items.push_back({ "Back" });
                const int option = menu::page("Debug", items, note, selected, true);
                note.clear();
                if (option < 0 || option == int(items.size()) - 1)
                    break;
                if (option == 2)
                {
                    runs.mSelected = (runs.mSelected + 1) % runs.mRuns.size();
                    continue;
                }
                const std::string run = runs.mRuns.empty() ? std::string()
                                                           : " (test run " + runs.mRuns[runs.mSelected].mLabel + ")";
                if (option == 0)
                {
                    chosenLine = "start: debug, skip the intro" + run;
                    return Start::DebugGame;
                }
                // Benchmark: Morrowind's places, or an installed expansion's.
                std::vector<menu::Item> sets{ { "Morrowind" } };
                std::vector<omw3ds::benchmark::Set> setIds{ omw3ds::benchmark::Set::Morrowind };
                if (access("sdmc:/openmw/data/Tribunal.esm", F_OK) == 0)
                {
                    sets.push_back({ "Tribunal (Mournhold)" });
                    setIds.push_back(omw3ds::benchmark::Set::Tribunal);
                }
                if (access("sdmc:/openmw/data/Bloodmoon.esm", F_OK) == 0)
                {
                    sets.push_back({ "Bloodmoon (Solstheim)" });
                    setIds.push_back(omw3ds::benchmark::Set::Bloodmoon);
                }
                int set = 0;
                if (setIds.size() > 1)
                {
                    size_t setSelected = 0;
                    std::string length = "About 5 minutes, then the game quits.";
                    set = menu::page("Benchmark", sets, length, setSelected, true);
                    if (set < 0)
                        continue;
                }
                gBenchmarkSet = setIds[set];
                chosenLine = "start: benchmark, " + sets[set].mLabel + run;
                return Start::Benchmark;
            }
        }
    }
#endif
}

int main(int, char**)
{
    gfxInitDefault();
    p3ds::InitOptions opts;
    opts.ciaUniqueId = 0xF6D3E; // must match apps/openmw packaging UNIQUE_ID
#ifdef OPENMW3DS_DEBUG
    // Before p3ds::init (the run decides where the logs go) and the watchdog (waiting here isn't a hang).
    std::string startLine;
    std::string runLogs;
    struct stat autorun;
    if (stat(p3ds::kAutorunPath, &autorun) == 0)
    {
        // An automated (emulator) run: the test set-up, or the benchmark its autorun.txt asks for; the usual logs.
        std::ifstream file(p3ds::kAutorunPath);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        gBenchmark = text.find("benchmark") != std::string::npos;
        gBenchmarkSet = text.find("benchmark:tribunal") != std::string::npos ? omw3ds::benchmark::Set::Tribunal
            : text.find("benchmark:bloodmoon") != std::string::npos      ? omw3ds::benchmark::Set::Bloodmoon
                                                                          : omw3ds::benchmark::Set::Morrowind;
        startLine = gBenchmark ? "start: automated run, benchmark" : "start: automated run";
    }
    else
    {
        p3ds::textfb::setupBottomScreen();
        p3ds::menuart::topScreen();
        TestRuns runs = readTestRuns();
        // Each page is drawn whole into the hidden buffer, then shown (the tester, test #97: drawing into the shown
        // one flashed the screen on every change). The loading screen draws on top of what is shown: single again.
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
        const Start start = startMenu(runs, startLine);
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        if (start == Start::Quit)
        {
            gfxExit();
            return 0;
        }
        gSkipIntro = start != Start::Game;
        gBenchmark = start == Start::Benchmark;
        opts.debugMode = start != Start::Game;
        // A normal start runs without any test switch; a debug one with the chosen run's.
        applySwitches(runs, opts.debugMode && !runs.mRuns.empty() ? &runs.mRuns[runs.mSelected] : nullptr);
        // A debug run's logs get a folder of their own (creating one costs ~1.5 s on hardware: not for a normal
        // start, whose logs stay in logs/, rotated).
        if (opts.debugMode)
            runLogs = debugLogFolder(runs);
    }
    if (gBenchmark)
        omw3ds::benchmark::enable(gBenchmarkSet);
    if (!runLogs.empty())
    {
        opts.logSubdir = runLogs.c_str();
        setenv("OPENMW3DS_LOG_DIR", ("/openmw/logs/" + runLogs).c_str(), 1); // OpenMW's openmw.log, MyGUI.log
    }
#endif
    p3ds::init(opts);
#ifdef OPENMW3DS_DEBUG
    if (!startLine.empty())
        P3DS_LOGI("%s, logs in logs/%s", startLine.c_str(), runLogs.c_str());
#endif
    p3ds::crash::setContextString(&gContext);
    // Fed by Engine::frame() and LoadingScreen::draw() (see p3ds::crash::kick calls in openmw).
    p3ds::crash::startWatchdog(60, p3ds::info().syscoreLimit > 0 ? 1 : 0);

    // The bottom screen while the game starts (M15): the game's name, what it does, a moving row of blocks
    // (platform3ds/loadscreen.cpp, drawn by the watchdog's thread). The M6 text screens (sdmc:/openmw/no-m8ui) keep
    // the debug display instead.
    if (access("sdmc:/openmw/no-m8ui", F_OK) == 0)
    {
        const std::string starting = std::string("(starting OpenMW) ") + Render3DS::interfacePath();
        omw3ds::initBottomScreen(starting.c_str());
    }
    else
    {
        char build[64] = OPENMW3DS_VERSION;
        if (std::strlen(p3ds_build_id) >= 13)
            std::snprintf(build, sizeof(build), "%s - build %.2s/%.2s %.2s:%.2s", OPENMW3DS_VERSION,
                p3ds_build_id + 6, p3ds_build_id + 4, p3ds_build_id + 9, p3ds_build_id + 11);
        p3ds::loadscreen::start(build);
    }

    // Top screen: citro3d draws what OSG culls (components/render3ds). Textures come from the pack cooked
    // on the PC by tools/cook_textures.py.
    if (!Render3DS::init())
        P3DS_LOGE("citro3d renderer failed to start: running headless");
    // M8's UI (MyGUI drawn on both screens). While M8 is being built, sdmc:/openmw/no-m8ui goes back to M6's text
    // screens (the bottom screen's debug display and menus).
    Render3DS::setDrawsGui(access("sdmc:/openmw/no-m8ui", F_OK) != 0);
    Render3DS::openTexturePack("sdmc:/openmw/cooked/textures.o3tp");
    // M11: an installed expansion's own textures (its ESM in the data folder, as main.cpp loads it).
    if (access("sdmc:/openmw/data/Tribunal.esm", F_OK) == 0)
        Render3DS::openAddonTexturePack("sdmc:/openmw/cooked/tribunal.o3tp");
    if (access("sdmc:/openmw/data/Bloodmoon.esm", F_OK) == 0)
        Render3DS::openAddonTexturePack("sdmc:/openmw/cooked/bloodmoon.o3tp");

    // Make the SD card the default device so plain "/openmw/..." paths work: std::filesystem (newlib)
    // does not consider "sdmc:/..." absolute, and OpenMW resolves relative paths against config dirs.
    chdir("sdmc:/");
    // std::filesystem::temp_directory_path() (used by the crash-catcher setup) needs an existing dir.
    setenv("TMPDIR", "/openmw/cache", 1);
    setenv("HOME", "/openmw", 1);
    // SDL is only used for events, joystick, touch and timers. Its n3ds video driver would take over
    // the screens (gfxInit) and has no OpenGL, so use the dummy driver.
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    // OSG's incremental compile pass would "upload" every new texture and geometry to the stub GL: CPU work
    // for nothing (the citro3d backend uploads on first draw).
    setenv("OPENMW_DONT_PRECOMPILE", "1", 1);
    // Physics steps at 60 Hz by default: two steps per frame at the 30 fps cap, 17-26 ms of each 33 ms frame on
    // core 2 in Seyda Neen (hardware test #21), where the work queue thread loading models also runs. One step
    // per frame halves it; actor movement uses swept shapes, so the longer step doesn't let actors tunnel.
    setenv("OPENMW_PHYSICS_FPS", "30", 1);

    std::vector<std::string> args = loadArguments();
    std::vector<char*> argv;
    for (std::string& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    gContext = "openmw: runApplication";
    // Normally doesn't return: Engine::go() ends with omw3ds::exitProcess.
    const int ret = Debug::wrapApplication(&runApplication, int(args.size()), argv.data(), "OpenMW");
    omw3ds::exitProcess(ret);
}

void omw3ds::exitProcess(int code)
{
    std::cout.flush(); // OpenMW's log (openmw.log): its streams are never closed on this path
    std::cerr.flush();
    P3DS_LOGI("OpenMW exited with %d", code);
    if (p3ds::info().autorun)
        p3ds::writeSmallFile(p3ds::kAutorunDonePath, "done\n"); // run-azahar.ps1 waits for it
    // Each step is named for the watchdog: a hang here gets a crash report that says where.
    p3ds::crash::kick("shutdown: sound");
    MWSound::shutdownSound3DS();
    p3ds::crash::kick("shutdown: bottom screen");
    omw3ds::shutdownBottomScreen();
    p3ds::crash::kick("shutdown: render3ds");
    Render3DS::shutdown();
    p3ds::crash::kick("shutdown: platform");
    p3ds::shutdown();
    gfxExit();
    // No static destructors (OpenMW's objects still exist); libctru still closes its services.
    _exit(code);
}
