#ifndef OPENMW_COMPONENTS_RENDER3DS_RENDER3DS_H
#define OPENMW_COMPONENTS_RENDER3DS_RENDER3DS_H

// Nintendo 3DS rendering backend (citro3d).
//
// OpenSceneGraph keeps the scene graph, update and cull; its GL calls go to a stub library. This backend
// replaces the draw: an OSG hook (deps/patches/osg/0003) hands every RenderStage to it, and it walks the
// culled render bins itself, emitting citro3d commands for each leaf (geometry uploaded once to linear
// memory, textures from the PC-cooked pack, OpenMW's lighting evaluated per vertex on the PICA).
// See LIMITATIONS.md section 4 ("option C").

#include <osg/ref_ptr>

#include <cstdint>
#include <string_view>

namespace osg
{
    class Image;
    class Texture;
}

namespace Render3DS
{
    // After gfxInit and p3ds::init (memory split). Sets up citro3d, the top-screen render target and the
    // OSG/window hooks. Returns false if citro3d could not start (the game then runs headless).
    bool init();
    void shutdown();
    // The HOME Menu closed the app: it owns the GPU now, so work submitted from here on never completes
    // (shutdown's C3D_Fini waited forever). Stops all drawing and makes shutdown skip the GPU teardown.
    void releaseGpuForExit();
    bool isActive();
    // Why rendering stopped (a GPU hang: the GPU core was reset and the game runs on headless), or nullptr.
    const char* failure();
    // Whether MyGUI is drawn (M8; off: the M6 text menus). While it isn't, its batches aren't built either (cull
    // time). Set before OpenMW creates its UI (the canvas size depends on it).
    void setDrawsGui(bool draws);
    bool drawsGui();

    // MyGUI's canvas has the console's shape: the top screen (400x240) at (0, 0) and the bottom screen (320x240)
    // centred under it at (40, 240). Its vertices arrive in the canvas's clip space; each screen draws them through
    // its own projection and the GPU clips what lies on the other screen.
    constexpr int kGuiCanvasWidth = 400;
    constexpr int kGuiCanvasHeight = 480;
    constexpr int kGuiBottomX = 40;
    constexpr int kGuiBottomY = 240;

    enum class GuiBlend : uint8_t
    {
        Alpha, // SRC_ALPHA, ONE_MINUS_SRC_ALPHA (MyGUI's default)
        Additive, // SRC_ALPHA, ONE (OpenMW's AdditiveLayer)
        Premultiplied, // ONE, ONE_MINUS_SRC_ALPHA (character preview)
    };

    // One MyGUI draw: triangles of MyGUI::Vertex (float x, y, z; colour bytes R, G, B, A; float u, v).
    struct GuiBatch
    {
        const osg::Texture* mTexture; // nullptr: vertex colour only
        const void* mVertices;
        uint32_t mCount;
        GuiBlend mBlend;
    };
    using GuiBatchSink = void (*)(const GuiBatch& batch);
    // Called once per frame while drawing, after the scene: hands over this frame's batches in drawing order.
    using GuiBatchSource = void (*)(GuiBatchSink sink);
    void setGuiSource(GuiBatchSource source);

    // Who draws the bottom screen: MyGUI through the GPU (true) or the CPU text display (false: the debug display,
    // the M6 text menus). Switching to the GPU redraws it at the next frame.
#ifdef OPENMW3DS_LUMIERE
    // Deprecated 18/10/2026, compiled only with OPENMW3DS_LUMIERE (see backend.cpp).
    // The port's own interface (backend.cpp, namespace lumiere): everything drawn from one texture, one draw per
    // screen; the game side is n3ds/hud3ds.cpp. Coordinates are canvas pixels (the top screen 400x240 at the
    // top, the bottom screen's 320x240 under it at x 40..360, y 240..480). A screen's vertices are rebuilt only
    // between begin() and the next begin(); the bottom screen is redrawn only when they changed.
    namespace Lumiere
    {
        struct Rgba
        {
            uint8_t r, g, b, a;
        };
        enum Screen
        {
            Top,
            Bottom
        };
        bool enabled(); // sdmc:/openmw/lumiere: the port's interface instead of MyGUI's
        bool active(); // enabled and the atlas loaded
        void begin(Screen screen);
        void image(Screen screen, std::string_view key, float x, float y, float w, float h, Rgba colour);
        void imageRotated(Screen screen, std::string_view key, float cx, float cy, float w, float h, float radians, Rgba colour);
        void rect(Screen screen, float x, float y, float w, float h, Rgba colour);
        float text(Screen screen, int font, float x, float y, std::string_view s, Rgba colour);
        float textWidth(int font, std::string_view s);
        void frame(Screen screen, float x, float y, float w, float h, const char* style);
        // key: the icon's normalized path without its extension ("icons/w/tx_axe_steel"); 16 or 32 px cooked icons
        void icon(Screen screen, std::string_view key, float x, float y, float size);
        void releaseIcons();
    }
#endif // OPENMW3DS_LUMIERE

    void setGuiOwnsBottomScreen(bool owns);
    bool guiOwnsBottomScreen();

    struct FrameStats
    {
        uint32_t mDraws = 0;
        uint32_t mTriangles = 0;
        uint32_t mLeaves = 0; // render leaves seen
        uint32_t mSkipped = 0; // leaves not drawn (unsupported drawable or primitive)
        uint32_t mUploadedBytes = 0; // geometry/texture data written to linear memory this frame
        uint32_t mGeometries = 0; // cached GPU geometries
        uint32_t mVerified = 0; // geometries drawn for the first time, each draw checked on the GPU
        uint32_t mLinearFreeKb = 0;
        float mEmitMs = 0; // CPU time walking the render graph and emitting commands
        float mGpuMs = 0; // PICA time of the previous frame
        float mCmdBufUsage = 0; // fraction of the GPU command buffer used
        uint32_t mLightSlotWrites = 0; // light slots uploaded (4 vectors each)
        uint32_t mShaderRuns[9] = {}; // vertex shader runs (vertices/indices drawn) by active point-light count
        uint32_t mProgramSwitches = 0; // shader variant changes between draws
        uint32_t mParticleVertices = 0; // particle quads built on the CPU this frame (6 vertices each)
        uint32_t mBadParticles = 0; // particles left out: non-finite corners (a zero-scale effect, see buildParticles)
        uint32_t mGuiDraws = 0; // GUI draws (both screens; batches with one texture and blend merged)
        uint32_t mGuiTextureBinds = 0; // distinct textures of the GUI passes, summed (one per screen: the UI atlas)
        uint32_t mGuiBottomFrames = 0; // frames in which the bottom screen was redrawn (0 or 1)
    };
    const FrameStats& getLastFrameStats();

    // Saves the next completed top-screen frame as a BMP (main thread).
    void requestCapture(const char* path);

    // PC-cooked texture pack (tools/cook_textures.py). Missing pack = every texture falls back to OSG's
    // loaders (DDS can't be displayed, see ImageManager).
    bool openTexturePack(const char* path);

    // A texture from the pack as an osg::Image whose data is already in PICA format in linear memory.
    // nullptr if the pack has no texture for this normalized VFS path (extension ignored).
    osg::ref_ptr<osg::Image> loadCookedImage(std::string_view normalizedPath);

    // The size of the original image a pack texture was cooked from (MyGUI measures skins in its pixels); false
    // for any other image.
    bool cookedSourceSize(const osg::Image& image, int& width, int& height);
}

#endif
