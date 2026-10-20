#pragma once

// The bottom screen while the game starts (M15), until the interface takes it over (textfb::gpuOwned): the game's
// name, what it is doing in plain words, a row of blocks lighting up in turn so the player sees it is alive, and the
// build in small letters. Drawn on the CPU into the framebuffer by the watchdog's thread every half second (tick),
// so it keeps moving while the main thread is busy in a long read, and costs nothing to the loading itself.
namespace p3ds::loadscreen
{
    // Draws the screen whole (the bottom screen in RGB565 mode: textfb::setupBottomScreen). `build`: a short line.
    void start(const char* build);
    bool active();

    // A startup stage (memprobe::stage's labels: "VFS registerArchives", "loadData: content files", ...), turned
    // into the words shown. Any thread.
    void noteStage(const char* label);

    // The watchdog's thread, every half second: the blocks and the words, redrawn.
    void tick();
}
