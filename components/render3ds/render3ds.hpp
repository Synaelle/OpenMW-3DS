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
        uint32_t mDebugSkipped = 0; // draws and particles left out by the debug switches (backend.cpp debugSkips)
        uint32_t mInsideEffects = 0; // effect meshes left out because the camera is inside them (insideEffect)
        uint32_t mGuiDraws = 0; // MyGUI batches drawn (both screens)
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
