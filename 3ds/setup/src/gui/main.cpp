// The setup app's window: two folders (Morrowind, the output) and Prepare, then a progress bar, then what to do
// with the folder. Plain, dark or light as the system is (the tester chose the look, 22/10/2026). Dear ImGui over
// SDL2's renderer.

#include "core/detect.hpp"
#include "core/prepare.hpp"
#include "core/text.hpp"
#include "core/update.hpp"
#include "gui/system.hpp"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2.h"
#include "imgui/imgui_impl_sdlrenderer2.h"

#include <SDL.h>
#include <SDL_syswm.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <objbase.h>
#endif

namespace
{
    namespace fs = std::filesystem;

    ImVec4 colour(int r, int g, int b)
    {
        return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.f);
    }

    // The palette: the approved mockup's dark one (the text brighter: the tester, more readable), and its light
    // twin for systems in light mode (the tester: follow the system, dark when unknown). kLight is the one strong
    // button (light on the dark theme, dark on the light one).
    ImVec4 kBackground, kField, kButton, kButtonHover, kBorder, kTitle, kText, kLabel, kLight, kLightHover, kProblem,
        kLink, kSelection;
    bool gDark = true;

    void setPalette(bool dark)
    {
        gDark = dark;
        if (dark)
        {
            kBackground = colour(18, 17, 17);
            kField = colour(34, 32, 30);
            kButton = colour(44, 41, 38);
            kButtonHover = colour(58, 54, 50);
            kBorder = colour(52, 49, 46);
            kTitle = colour(244, 240, 232);
            kText = colour(228, 222, 212);
            kLabel = colour(182, 175, 165);
            kLight = colour(226, 220, 208);
            kLightHover = colour(240, 235, 225);
            kProblem = colour(232, 146, 126);
            kLink = colour(214, 186, 132);
            kSelection = colour(80, 74, 66);
        }
        else
        {
            kBackground = colour(246, 243, 238);
            kField = colour(231, 226, 218);
            kButton = colour(221, 215, 205);
            kButtonHover = colour(208, 201, 190);
            kBorder = colour(207, 200, 189);
            kTitle = colour(26, 24, 22);
            kText = colour(40, 37, 34);
            kLabel = colour(96, 90, 82);
            kLight = colour(44, 40, 36);
            kLightHover = colour(64, 59, 54);
            kProblem = colour(160, 58, 38);
            kLink = colour(128, 88, 24);
            kSelection = colour(200, 186, 160);
        }
    }

    const char* kVersion = "0.1.1";
    constexpr float kBodySize = 22.f;
    constexpr float kTitleSize = 28.f;
    float gScale = 1.f; // the display's DPI over 96

    enum class State
    {
        Ready,
        Working,
        Done,
        Failed,
    };

    struct App
    {
        State state = State::Ready;
        char morrowind[1024] = {};
        char output[1024] = {};
        bool checked = false; // the Morrowind folder was checked at least once (an empty one too)
        std::string checkedPath; // the Morrowind folder last checked, and what was found
        std::vector<std::string> problems;
        std::string notice; // shown under the button (a problem with what was entered)

        std::thread worker;
        std::atomic<bool> cancel{ false };
        std::atomic<bool> finished{ false };
        std::mutex mutex;
        setup::Progress progress; // under mutex
        std::string error; // under mutex, when it failed
        bool cancelled = false; // under mutex
        std::string newerVersion; // under mutex: a newer release on GitHub, once the check has answered
        std::thread updateCheck;
    };

    fs::path portFolder()
    {
        return setup::programFolder() / "port";
    }

    // The folder made inside an output folder the player chooses (the tester: never straight into the folder
    // picked). The default output, the port folder itself, takes the game's files as it is.
    std::string outputFolderName()
    {
        return std::string("OpenMW-3DS-") + kVersion;
    }

    fs::path effectiveOutput(const fs::path& chosen)
    {
        std::error_code error;
        if (chosen.empty() || fs::equivalent(chosen, portFolder(), error) || chosen.filename() == outputFolderName())
            return chosen;
        return chosen / outputFolderName();
    }

    void setPath(char* buffer, size_t size, const fs::path& path)
    {
        const std::u8string text = path.u8string();
        std::strncpy(buffer, reinterpret_cast<const char*>(text.c_str()), size - 1);
        buffer[size - 1] = '\0';
    }

    fs::path getPath(const char* buffer)
    {
        return fs::path(std::u8string(reinterpret_cast<const char8_t*>(buffer)));
    }

    void startPreparing(App& app)
    {
        setup::Options options;
        options.morrowind = getPath(app.morrowind);
        options.output = effectiveOutput(getPath(app.output));
        setPath(app.output, sizeof(app.output), options.output);
        options.port = portFolder();
        options.version = kVersion;
        options.cancel = &app.cancel;
        app.cancel = false;
        app.finished = false;
        app.error.clear();
        app.cancelled = false;
        app.progress = { "Starting", 0.0 };
        app.state = State::Working;
        app.worker = std::thread([&app, options] {
            try
            {
                setup::prepare(options, [&app](const setup::Progress& p) {
                    std::lock_guard<std::mutex> lock(app.mutex);
                    app.progress = p;
                });
            }
            catch (const setup::Cancelled&)
            {
                std::lock_guard<std::mutex> lock(app.mutex);
                app.cancelled = true;
            }
            catch (const std::exception& e)
            {
                std::lock_guard<std::mutex> lock(app.mutex);
                app.error = e.what();
            }
            app.finished = true;
        });
    }

    // A labelled folder line: the path (editable) and Browse. `inside`: what a picked folder becomes (the output's
    // own folder in it).
    void folderField(const char* label, const char* id, char* buffer, size_t size, const char* pickerTitle,
        fs::path (*inside)(const fs::path&) = nullptr)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kLabel);
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
        const float browseWidth = ImGui::CalcTextSize("Browse").x + ImGui::GetStyle().FramePadding.x * 4;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - browseWidth - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText(id, buffer, size);
        ImGui::SameLine();
        ImGui::PushID(id);
        if (ImGui::Button("Browse", ImVec2(browseWidth, 0)))
        {
            const fs::path picked = gui::pickFolder(pickerTitle, getPath(buffer));
            if (!picked.empty())
                setPath(buffer, size, inside != nullptr ? inside(picked) : picked);
        }
        ImGui::PopID();
    }

    // A button's width: its text and padding, wider for the light one.
    float buttonWidth(const char* label, bool primary)
    {
        return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2 + (primary ? 24.f * gScale : 0.f);
    }

    // The light button (one per view).
    bool primaryButton(const char* label)
    {
        ImGui::PushStyleColor(ImGuiCol_Button, kLight);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kLightHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kLight);
        ImGui::PushStyleColor(ImGuiCol_Text, kBackground);
        const bool pressed = ImGui::Button(label, ImVec2(buttonWidth(label, true), 0));
        ImGui::PopStyleColor(4);
        return pressed;
    }

    // Moves the cursor so that buttons of these widths end at the right edge.
    void alignRight(float width)
    {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - width);
    }

    void drawReady(App& app)
    {
        folderField("Morrowind folder", "##morrowind", app.morrowind, sizeof(app.morrowind),
            "Choose the Morrowind folder");
        if (!app.checked || app.checkedPath != app.morrowind)
        {
            app.checked = true;
            app.checkedPath = app.morrowind;
            app.problems = app.checkedPath.empty() ? std::vector<std::string>{ "Choose the folder Morrowind is installed in." }
                                                   : setup::checkMorrowind(getPath(app.morrowind));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, app.problems.empty() ? kLabel : kProblem);
        ImGui::TextWrapped("%s", app.problems.empty() ? "The English Morrowind is here." : app.problems.front().c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        folderField("Output folder", "##output", app.output, sizeof(app.output), "Choose where to put the files",
            effectiveOutput);

        ImGui::Spacing();
        ImGui::Spacing();
        if (!app.notice.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kProblem);
            ImGui::TextWrapped("%s", app.notice.c_str());
            ImGui::PopStyleColor();
        }
        alignRight(buttonWidth("Prepare", true));
        if (primaryButton("Prepare"))
        {
            app.notice.clear();
            if (!app.problems.empty())
                app.notice = app.problems.front();
            else if (app.output[0] == '\0')
                app.notice = "Choose an output folder.";
            else if (!fs::is_directory(setup::programFolder() / "port"))
                app.notice = "The port's files are missing: keep the \"port\" folder next to this program.";
            else
                startPreparing(app);
        }
    }

    void drawWorking(App& app)
    {
        setup::Progress progress;
        {
            std::lock_guard<std::mutex> lock(app.mutex);
            progress = app.progress;
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        if (app.cancel)
            ImGui::TextUnformatted("Cancelling...");
        else
            ImGui::Text("%s, %d%%", progress.step.c_str(), int(progress.fraction * 100.0));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        // A thin bar.
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = 4.f * gScale;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), ImGui::GetColorU32(kButton), height / 2);
        draw->AddRectFilled(at, ImVec2(at.x + width * float(progress.fraction), at.y + height), ImGui::GetColorU32(kLight),
            height / 2);
        ImGui::Dummy(ImVec2(width, height));
        ImGui::Spacing();
        ImGui::Spacing();
        alignRight(buttonWidth("Cancel", false));
        if (!app.cancel && ImGui::Button("Cancel"))
            app.cancel = true;

        if (app.finished)
        {
            app.worker.join();
            std::lock_guard<std::mutex> lock(app.mutex);
            if (app.cancelled)
            {
                app.state = State::Ready;
                app.notice = "Cancelled. The output folder is incomplete: prepare it again before copying it.";
            }
            else
                app.state = app.error.empty() ? State::Done : State::Failed;
        }
    }

    void drawDone(App& app, bool& quit)
    {
        // Compact (the tester): the folder, then three short steps.
        ImGui::PushStyleColor(ImGuiCol_Text, kLabel);
        ImGui::TextUnformatted("Ready in");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, kTitle);
        ImGui::TextWrapped("%s", app.output);
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        ImGui::TextWrapped("1. Copy its contents to your SD card's root (slow over Wi-Fi for now).");
        // The installers, each a link to its latest release (the tester).
        ImGui::TextUnformatted("2. Install cias/openmw3ds.cia with");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("FBI", "https://github.com/Steveice10/FBI/releases/latest");
        ImGui::SameLine();
        ImGui::TextUnformatted("or");
        ImGui::SameLine();
        ImGui::TextLinkOpenURL("Cement", "https://github.com/Synaelle/cement/releases/latest");
        ImGui::SameLine(0, 0);
        ImGui::TextUnformatted(".");
        ImGui::TextWrapped("3. Start OpenMW. The first launch takes a while.");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Spacing();
        alignRight(buttonWidth("Open folder", false) + ImGui::GetStyle().ItemSpacing.x + buttonWidth("Close", true));
        if (ImGui::Button("Open folder"))
            gui::openFolder(getPath(app.output));
        ImGui::SameLine();
        if (primaryButton("Close"))
            quit = true;
    }

    void drawFailed(App& app)
    {
        std::string error;
        {
            std::lock_guard<std::mutex> lock(app.mutex);
            error = app.error;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kProblem);
        ImGui::TextWrapped("The folder couldn't be prepared.");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, kText);
        ImGui::TextWrapped("%s", error.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Spacing();
        alignRight(buttonWidth("Back", false));
        if (ImGui::Button("Back"))
            app.state = State::Ready;
    }

    void applyStyle(float scale)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        style.WindowPadding = ImVec2(28, 24);
        style.FramePadding = ImVec2(10, 7);
        style.ItemSpacing = ImVec2(8, 8);
        style.FrameRounding = 4;
        style.WindowRounding = 0;
        style.WindowBorderSize = 0;
        style.FrameBorderSize = 0;
        style.ScrollbarSize = 10;
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = kBackground;
        c[ImGuiCol_Text] = kText;
        c[ImGuiCol_TextDisabled] = kLabel;
        c[ImGuiCol_FrameBg] = kField;
        c[ImGuiCol_FrameBgHovered] = kField;
        c[ImGuiCol_FrameBgActive] = kField;
        c[ImGuiCol_Button] = kButton;
        c[ImGuiCol_ButtonHovered] = kButtonHover;
        c[ImGuiCol_ButtonActive] = kButton;
        c[ImGuiCol_Border] = kBorder;
        c[ImGuiCol_TextSelectedBg] = kSelection;
        c[ImGuiCol_TextLink] = kLink;
        c[ImGuiCol_NavCursor] = kLabel;
        c[ImGuiCol_InputTextCursor] = kText;
        style.ScaleAllSizes(scale);
        style.FontSizeBase = kBodySize;
        style.FontScaleDpi = scale;
    }
}

namespace
{
    void* nativeWindow(SDL_Window* window)
    {
#ifdef _WIN32
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(window, &info))
            return info.info.win.window;
#else
        (void)window;
#endif
        return nullptr;
    }
}

// Testing: --screenshot <file.png> [--state ready|working|done|failed] [--theme dark|light] draws that state once,
// off screen, into a PNG (sample progress and messages), then quits.
int main(int argc, char** argv)
{
    // "--state run" really prepares (into --output), then pictures the window once it has finished.
    std::string screenshot, shotState = "ready", shotTheme, shotOutput;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        if (std::strcmp(argv[i], "--screenshot") == 0)
            screenshot = argv[i + 1];
        else if (std::strcmp(argv[i], "--state") == 0)
            shotState = argv[i + 1];
        else if (std::strcmp(argv[i], "--theme") == 0)
            shotTheme = argv[i + 1];
        else if (std::strcmp(argv[i], "--output") == 0)
            shotOutput = argv[i + 1];
    }
    const bool shot = !screenshot.empty();
    setPalette(shotTheme.empty() ? gui::systemPrefersDark() : shotTheme != "light");
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    // SDL_HINT_WINDOWS_DPI_AWARENESS (SDL 2.24+), by its name: older SDL2s (Linux distributions) don't define it.
    SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0)
    {
        std::fprintf(stderr, "The window can't open: %s\n", SDL_GetError());
        if (!shot)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenMW for New 3DS setup", SDL_GetError(), nullptr);
        return 1;
    }
    float dpi = 96.f;
    if (SDL_GetDisplayDPI(0, &dpi, nullptr, nullptr) != 0 || dpi <= 0)
        dpi = 96.f;
    const float scale = dpi / 96.f;
    gScale = scale;
    SDL_Window* window = SDL_CreateWindow("OpenMW for New 3DS setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        int(700 * scale), int(400 * scale),
        SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE | (shot ? SDL_WINDOW_HIDDEN : 0));
    SDL_Renderer* renderer = window == nullptr
        ? nullptr
        : SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    // No GPU (a virtual machine, a remote desktop): the software renderer is plenty for this window.
    if (window != nullptr && renderer == nullptr)
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (window == nullptr || renderer == nullptr)
    {
        std::fprintf(stderr, "The window can't open: %s\n", SDL_GetError());
        if (!shot)
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "OpenMW for New 3DS setup", SDL_GetError(), nullptr);
        return 1;
    }
    SDL_SetWindowMinimumSize(window, int(560 * scale), int(380 * scale));
    gui::matchTitleBar(nativeWindow(window), gDark);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no imgui.ini next to the program
    applyStyle(scale);
    // One font, drawn at any size (Dear ImGui 1.92+ bakes sizes on demand).
    const fs::path font = gui::interfaceFont();
    if (font.empty() || io.Fonts->AddFontFromFileTTF(font.string().c_str(), kBodySize) == nullptr)
        io.Fonts->AddFontDefault();
    ImGui::GetStyle().FontSizeBase = kBodySize;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    App app;
    const std::vector<fs::path> found = setup::findMorrowind();
    if (!found.empty())
        setPath(app.morrowind, sizeof(app.morrowind), found.front());
    // The port folder itself by default (the tester): the game's files are added to it, the CIA is already there.
    setPath(app.output, sizeof(app.output), portFolder());
    // Is there a newer release? Asked once in the background (no connection: nothing shown). Pictures (testing) ask
    // only with OPENMW3DS_SETUP_REPOSITORY set, and wait for the answer.
    const auto checkForUpdate = [&app] {
        const std::string latest = setup::latestReleaseVersion();
        if (!latest.empty() && setup::olderThan(kVersion, latest))
        {
            std::lock_guard<std::mutex> lock(app.mutex);
            app.newerVersion = latest;
        }
    };
    if (!shot)
        app.updateCheck = std::thread(checkForUpdate);
    else if (std::getenv("OPENMW3DS_SETUP_REPOSITORY") != nullptr)
        checkForUpdate();

    SDL_Texture* target = nullptr;
    int frames = 0;
    if (shot)
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_TARGET, w, h);
        SDL_SetRenderTarget(renderer, target);
        if (shotState == "working")
        {
            app.state = State::Working;
            app.progress = { "Preparing the textures", 0.42 };
        }
        else if (shotState == "done")
            app.state = State::Done;
        else if (shotState == "failed")
        {
            app.state = State::Failed;
            app.error = "can't write C:/Users/you/Desktop/OpenMW-3DS/openmw/data/sound3ds.bsa.part";
        }
        else if (shotState == "run")
        {
            if (!shotOutput.empty())
                setPath(app.output, sizeof(app.output), setup::pathFromUtf8(shotOutput));
            startPreparing(app);
        }
    }

    bool quit = false;
    Uint32 lastThemeCheck = SDL_GetTicks();
    while (!quit)
    {
        // Follow the system switching between light and dark while the window is open (where that's cheap to ask).
        if (!shot && gui::canFollowThemeChanges() && SDL_GetTicks() - lastThemeCheck > 2000)
        {
            lastThemeCheck = SDL_GetTicks();
            const bool dark = gui::systemPrefersDark();
            if (dark != gDark)
            {
                setPalette(dark);
                applyStyle(scale);
                ImGui::GetStyle().FontSizeBase = kBodySize;
                gui::matchTitleBar(nativeWindow(window), dark);
            }
        }
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                quit = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)
                quit = true;
        }
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("setup", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings
                | ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushFont(nullptr, kTitleSize);
        ImGui::PushStyleColor(ImGuiCol_Text, kTitle);
        ImGui::TextUnformatted("OpenMW for New 3DS setup");
        ImGui::PopStyleColor();
        ImGui::PopFont();
        {
            std::string newer;
            {
                std::lock_guard<std::mutex> lock(app.mutex);
                newer = app.newerVersion;
            }
            if (!newer.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kProblem);
                ImGui::TextUnformatted(("Version " + newer + " is out, this is " + kVersion + ".").c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::TextLinkOpenURL("Download it",
                    (std::string("https://github.com/") + setup::kRepository + "/releases/latest").c_str());
            }
        }
        ImGui::Spacing();
        ImGui::Spacing();
        switch (app.state)
        {
            case State::Ready:
                drawReady(app);
                break;
            case State::Working:
                drawWorking(app);
                break;
            case State::Done:
                drawDone(app, quit);
                break;
            case State::Failed:
                drawFailed(app);
                break;
        }
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, Uint8(kBackground.x * 255), Uint8(kBackground.y * 255),
            Uint8(kBackground.z * 255), 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        if (shot)
        {
            // A few frames for the layout and the font sizes to settle (a real run: until it has finished), then the
            // picture.
            if (app.state == State::Working)
            {
                frames = 0;
                SDL_Delay(50);
                continue;
            }
            if (++frames < 4)
                continue;
            int w = 0, h = 0;
            SDL_QueryTexture(target, nullptr, nullptr, &w, &h);
            std::vector<unsigned char> pixels(size_t(w) * size_t(h) * 4);
            SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_ABGR8888, pixels.data(), w * 4);
            stbi_write_png(screenshot.c_str(), w, h, 4, pixels.data(), w * 4);
            if (FILE* info = std::fopen((screenshot + ".txt").c_str(), "w"))
            {
                std::fprintf(info, "dpi %.1f scale %.3f font size %.2f base %.2f dpi factor %.3f display %.0fx%.0f\n", dpi,
                    scale, ImGui::GetFontSize(), ImGui::GetStyle().FontSizeBase, ImGui::GetStyle().FontScaleDpi,
                    io.DisplaySize.x, io.DisplaySize.y);
                std::fclose(info);
            }
            break;
        }
        SDL_RenderPresent(renderer);
        // Idle without input, but keep drawing the progress while working.
        if (app.state != State::Working)
            SDL_WaitEventTimeout(nullptr, 250);
    }

    if (app.updateCheck.joinable())
        app.updateCheck.join(); // a few seconds at most (the request's time limit)
    if (app.worker.joinable())
    {
        app.cancel = true;
        app.worker.join();
    }
    if (target != nullptr)
        SDL_DestroyTexture(target);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
