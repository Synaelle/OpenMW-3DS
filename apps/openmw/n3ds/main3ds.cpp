// Nintendo 3DS entry point (replaces main() in apps/openmw/main.cpp on this platform).
//
// Brings up the platform layer (log file, crash logger, hang watchdog, memory split, bottom-screen
// debug display) before handing over to OpenMW's unchanged runApplication(). Command-line options come
// from sdmc:/openmw/args.txt (one argument per line); the debug default boots straight into a new game
// with the default generated character in Seyda Neen:
//   --skip-menu --start "Seyda Neen"
// (--new-game is deliberately absent: it would start character generation and the intro movie.)

#include "frame3ds.hpp"

#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/platform.hpp>

#include <components/debug/debugging.hpp>
#include <components/render3ds/render3ds.hpp>

#include <3ds.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

int runApplication(int argc, char* argv[]); // apps/openmw/main.cpp

namespace
{
    constexpr const char* kArgsPath = "sdmc:/openmw/args.txt";

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
}

int main(int, char**)
{
    gfxInitDefault();
    p3ds::InitOptions opts;
    opts.ciaUniqueId = 0xF6D3E; // must match apps/openmw packaging UNIQUE_ID
    p3ds::init(opts);
    p3ds::crash::setContextString(&gContext);
    // Fed by Engine::frame() and LoadingScreen::draw() (see p3ds::crash::kick calls in openmw).
    p3ds::crash::startWatchdog(60, p3ds::info().syscoreLimit > 0 ? 1 : 0);

    omw3ds::initBottomScreen("(starting OpenMW)");

    // Top screen: citro3d draws what OSG culls (components/render3ds). Textures come from the pack cooked
    // on the PC by tools/cook_textures.py.
    if (!Render3DS::init())
        P3DS_LOGE("citro3d renderer failed to start: running headless");
    // M8's UI (MyGUI drawn on both screens) while it's being built: on with sdmc:/openmw/m8ui, otherwise the M6
    // text menus.
    Render3DS::setDrawsGui(access("sdmc:/openmw/m8ui", F_OK) == 0);
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
