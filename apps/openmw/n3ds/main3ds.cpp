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
#include <platform3ds/textfb.hpp>

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

    // The startup choice (debug builds, before the test runs): "Start normally" boots the game's own main menu
    // (M8: a new game goes through character creation); "Skip the intro" keeps the test set-up of args.txt
    // (--skip-menu --start <cell> --script-run test-start.txt: the test character with its gold, skills, axe
    // and spells), for testing anything but the menus. Automated runs skip the intro.
    bool gSkipIntro = true;

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
            if (!gSkipIntro)
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
    // Hardware tests: sdmc:/openmw/test-menu.txt (written for each test) lists the test's runs, and the tester picks
    // one on the bottom screen before OpenMW starts, instead of putting and removing empty switch files by hand.
    // The menu creates the chosen run's switch files in sdmc:/openmw/ and deletes the other runs' ones: the code
    // that uses a switch still just checks whether its file exists. Automated runs (autorun.txt) skip it.
    //   title <text>
    //   run <label> | <switch files, separated by spaces, or nothing>
    // Only the labels are shown (anything after a second '|' is ignored).
    struct TestRun
    {
        std::string mLabel;
        std::vector<std::string> mSwitches;
    };

    std::string trim(const std::string& text)
    {
        const size_t first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            return {};
        return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    }

    // Shown before p3ds::init: the chosen run's logs get their own folder (logs/<prefix>-<run>, "logs <prefix>"
    // in the menu file), so the platform isn't up yet (no log; the bottom screen is set up here). Returns that
    // folder's name (empty: the usual logs folder) and, in `chosenLine`, the line to log once logging starts.
    // A choice on the bottom screen before the platform is up (the test menu's look): the index chosen with
    // UP/DOWN and A, or by tapping a line twice.
    size_t pickOne(const char* title, const std::vector<std::string>& labels, const char* footer)
    {
        namespace tf = p3ds::textfb;
        tf::setupBottomScreen();
        size_t selected = 0;
        bool redraw = true;
        while (aptMainLoop())
        {
            if (redraw)
            {
                tf::clear(tf::color::black);
                tf::fillRows(0, 1, tf::color::header);
                tf::drawText(0, 0, title, tf::color::yellow, tf::color::header);
                for (size_t i = 0; i < labels.size(); ++i)
                {
                    const bool on = i == selected;
                    const std::string head = std::string(on ? "> " : "  ") + labels[i];
                    tf::drawText(0, 2 + int(i) * 2, head.c_str(), on ? tf::color::green : tf::color::white,
                        tf::color::black);
                }
                tf::fillRows(tf::kRows - 1, 1, tf::color::header);
                tf::drawText(0, tf::kRows - 1, footer, tf::color::yellow, tf::color::header);
                tf::present();
                redraw = false;
            }
            gspWaitForVBlank();
            hidScanInput();
            const u32 down = hidKeysDown();
            if (down & KEY_DOWN)
            {
                selected = (selected + 1) % labels.size();
                redraw = true;
            }
            if (down & KEY_UP)
            {
                selected = (selected + labels.size() - 1) % labels.size();
                redraw = true;
            }
            if (down & KEY_A)
                return selected;
            if (down & KEY_TOUCH)
            {
                touchPosition touch;
                hidTouchRead(&touch);
                const int row = touch.py / 8;
                for (size_t i = 0; i < labels.size(); ++i)
                    if (row >= 2 + int(i) * 2 && row <= 3 + int(i) * 2)
                    {
                        if (i == selected)
                            return selected;
                        selected = i;
                        redraw = true;
                    }
            }
        }
        return selected;
    }

    // The first question: the game's own start, or the test set-up (see gSkipIntro).
    void startChoice(std::string& chosenLine)
    {
        struct stat autorun;
        if (stat(p3ds::kAutorunPath, &autorun) == 0)
        {
            gSkipIntro = true;
            return;
        }
        const size_t choice = pickOne("OpenMW 3DS - how to start",
            { "Start normally (the main menu, a new character)", "Skip the intro: the test character (debug)" },
            "UP/DOWN choose, A start (or tap twice)");
        gSkipIntro = choice == 1;
        chosenLine = gSkipIntro ? "start: skip the intro (the test character)" : "start: normally (the main menu)";
    }

    std::string testMenu(std::string& chosenLine)
    {
        std::ifstream file("sdmc:/openmw/test-menu.txt");
        if (!file)
            return {};
        std::string title = "Test runs";
        std::string logPrefix;
        std::vector<TestRun> runs;
        std::string line;
        while (std::getline(file, line))
        {
            line = trim(line);
            if (line.rfind("title ", 0) == 0)
                title = trim(line.substr(6));
            else if (line.rfind("logs ", 0) == 0)
                logPrefix = trim(line.substr(5));
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
                runs.push_back(run);
            }
        }
        if (runs.empty())
            return {};
        struct stat autorun;
        if (stat(p3ds::kAutorunPath, &autorun) == 0)
        {
            chosenLine = "test menu: skipped (automated run)";
            return {};
        }

        namespace tf = p3ds::textfb;
        tf::setupBottomScreen();
        size_t selected = 0;
        std::vector<int> firstRow(runs.size()), lastRow(runs.size());
        bool redraw = true;
        bool chosen = false;
        while (!chosen && aptMainLoop())
        {
            if (redraw)
            {
                tf::clear(tf::color::black);
                tf::fillRows(0, 1, tf::color::header);
                tf::drawText(0, 0, title.c_str(), tf::color::yellow, tf::color::header);
                int row = 2;
                for (size_t i = 0; i < runs.size(); ++i)
                {
                    const bool on = i == selected;
                    firstRow[i] = row;
                    const std::string head = std::string(on ? "> " : "  ") + runs[i].mLabel;
                    tf::drawText(0, row, head.c_str(), on ? tf::color::green : tf::color::white, tf::color::black);
                    // Two text rows per run, so a run is easy to tap.
                    lastRow[i] = row + 1;
                    row += 2;
                }
                tf::fillRows(tf::kRows - 1, 1, tf::color::header);
                tf::drawText(0, tf::kRows - 1, "UP/DOWN choose, A start (or tap twice)", tf::color::yellow,
                    tf::color::header);
                tf::present();
                redraw = false;
            }
            gspWaitForVBlank();
            hidScanInput();
            const u32 down = hidKeysDown();
            if (down & KEY_DOWN)
            {
                selected = (selected + 1) % runs.size();
                redraw = true;
            }
            if (down & KEY_UP)
            {
                selected = (selected + runs.size() - 1) % runs.size();
                redraw = true;
            }
            if (down & KEY_A)
                chosen = true;
            if (down & KEY_TOUCH)
            {
                touchPosition touch;
                hidTouchRead(&touch);
                const int row = touch.py / 8;
                for (size_t i = 0; i < runs.size(); ++i)
                    if (row >= firstRow[i] && row <= lastRow[i])
                    {
                        chosen = i == selected;
                        selected = i;
                        redraw = true;
                    }
            }
        }

        // Every switch any run names, and the switches of past tests (hardware test #57: a "gpu-ui" left by
        // test #56's run A kept the deprecated GPU interface on): the chosen run's ones exist afterwards, the
        // others don't.
        std::set<std::string> all{ "gpu-ui", "soft-gui", "lumiere", "lumiere-demo", "no-fills", "fills", "fills-top",
            "fills-bottom", "fills-opaque", "gui-on-scene", "gui-no-reupload", "ui3ds-demo", "no-m8ui" };
        for (const TestRun& run : runs)
            all.insert(run.mSwitches.begin(), run.mSwitches.end());
        const std::vector<std::string>& wanted = runs[selected].mSwitches;
        for (const std::string& name : all)
        {
            const std::string path = "sdmc:/openmw/" + name;
            struct stat st;
            const bool exists = stat(path.c_str(), &st) == 0;
            const bool want = std::find(wanted.begin(), wanted.end(), name) != wanted.end();
            if (want && !exists)
            {
                if (std::FILE* f = std::fopen(path.c_str(), "w"))
                    std::fclose(f);
            }
            else if (!want && exists)
                std::remove(path.c_str());
        }
        // The run's log folder: the label up to its ':' ("A: ..." -> "A"), letters, digits and '-' only. A run
        // without one ("Normal game") keeps the usual folder.
        std::string folder;
        const std::string& label = runs[selected].mLabel;
        if (label.find(':') != std::string::npos)
        {
            for (char c : label.substr(0, label.find(':')))
                if (std::isalnum(static_cast<unsigned char>(c)) || c == '-')
                    folder += c;
                else if (c == ' ' && !folder.empty() && folder.back() != '-')
                    folder += '-';
            while (!folder.empty() && folder.back() == '-')
                folder.pop_back();
        }
        if (!logPrefix.empty()) // "Normal game" too: logs/test<N>-<date>-<time>[-<run>]
        {
            const std::time_t now = std::time(nullptr);
            char stamp[32];
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M", std::localtime(&now));
            folder = logPrefix + "-" + stamp + (folder.empty() ? "" : "-" + folder);
        }
        chosenLine = "test menu: run \"" + runs[selected].mLabel + "\" chosen (" + std::to_string(wanted.size())
            + " switch file(s)), logs in " + (folder.empty() ? std::string("logs/") : "logs/" + folder + "/");
        tf::clear(tf::color::black);
        tf::present();
        return folder;
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
    startChoice(startLine);
    std::string menuChoice;
    const std::string runLogs = testMenu(menuChoice);
    if (!runLogs.empty())
    {
        opts.logSubdir = runLogs.c_str();
        setenv("OPENMW3DS_LOG_DIR", ("/openmw/logs/" + runLogs).c_str(), 1); // OpenMW's openmw.log, MyGUI.log
    }
#endif
    p3ds::init(opts);
#ifdef OPENMW3DS_DEBUG
    if (!startLine.empty())
        P3DS_LOGI("%s", startLine.c_str());
    if (!menuChoice.empty())
        P3DS_LOGI("%s", menuChoice.c_str());
#endif
    p3ds::crash::setContextString(&gContext);
    // Fed by Engine::frame() and LoadingScreen::draw() (see p3ds::crash::kick calls in openmw).
    p3ds::crash::startWatchdog(60, p3ds::info().syscoreLimit > 0 ? 1 : 0);

    const std::string starting = std::string("(starting OpenMW) ") + Render3DS::interfacePath();
    omw3ds::initBottomScreen(starting.c_str());

    // Top screen: citro3d draws what OSG culls (components/render3ds). Textures come from the pack cooked
    // on the PC by tools/cook_textures.py.
    if (!Render3DS::init())
        P3DS_LOGE("citro3d renderer failed to start: running headless");
    // M8's UI (MyGUI drawn on both screens). While M8 is being built, sdmc:/openmw/no-m8ui goes back to M6's text
    // screens (the bottom screen's debug display and menus).
    Render3DS::setDrawsGui(access("sdmc:/openmw/no-m8ui", F_OK) != 0);
    Render3DS::openTexturePack("sdmc:/openmw/cooked/textures.o3tp");

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
