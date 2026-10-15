#pragma once

#include <platform3ds/memstats.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Replaceable bottom-screen module. Today: DebugBottomScreen. Later: the in-game UI
// (inventory, map, spells, journal). The game loop only talks to IBottomScreen.
namespace p3ds
{
    struct FrameStats
    {
        uint32_t frameIndex = 0;
        float fps = 0.f; // averaged over the last ~0.5 s
        float fpsLow = 0.f; // worst frame in the window, as FPS
        float frameMs = 0.f; // last frame wall time
        float cpuMs = 0.f; // citro3d CPU-side processing time
        float gpuMs = 0.f; // citro3d GPU drawing time
        uint32_t drawCalls = 0;
        uint32_t triangles = 0;
    };

    // What the game exposes to the bottom screen each frame. Pointers must stay valid for the call.
    struct GameSnapshot
    {
        const FrameStats* frame = nullptr;
        const char* cellName = nullptr;
        float playerPos[3] = { 0.f, 0.f, 0.f };
        float yaw = 0.f; // degrees, 0 = north, clockwise
        float pitch = 0.f; // degrees, + = looking down
        const char* lookingAt = nullptr; // the object in reach under the crosshair
        const char* status = nullptr; // free-form one-liner (renderer failure, test app status)
        const char* state = nullptr; // stance, view, open menu, death
        bool stateAlert = false; // the state needs the player's attention (a menu we can't draw yet, death)
    };

    // A game menu as text on the bottom screen (until M8's UI): a title, body paragraphs (wrapped by the screen) and
    // touch options. The game rebuilds it a few times a second; the screen keeps its scroll position and option page
    // while the content is unchanged.
    struct MenuView
    {
        struct Option
        {
            std::string mLabel;
            std::string mDetail; // second line (count, price, state), may be empty
            bool operator==(const Option&) const = default;
        };
        std::string mTitle;
        std::vector<std::string> mBody;
        std::vector<Option> mOptions;
        bool mBodyFromEnd = false; // show the end of the body first (dialogue history), else the start (books)
        bool operator==(const MenuView&) const = default;
    };

    struct TouchEvent
    {
        int x = 0;
        int y = 0;
        bool down = false;
    };

    class IBottomScreen
    {
    public:
        virtual ~IBottomScreen() = default;

        virtual void init() = 0;
        virtual void shutdown() = 0;

        // Called every frame; must be cheap. Implementations decide when to actually redraw.
        virtual void update(const GameSnapshot& snapshot) = 0;
        // Called every frame after the top screen frame was submitted.
        virtual void render() = 0;

        virtual void onTouch(const TouchEvent& event) {}

        virtual void setVisible(bool visible) = 0;
        virtual bool isVisible() const = 0;
    };

    // Rolling FPS / frame time accounting for FrameStats.
    class FrameTimer
    {
    public:
        // Call once per frame with the time the frame took.
        void addFrame(float frameMs);
        void fill(FrameStats& out) const;

    private:
        static constexpr int kWindow = 32;
        float mSamples[kWindow] = {};
        int mCount = 0;
        int mNext = 0;
        float mLast = 0.f;
    };
}
