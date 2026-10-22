#ifndef OPENMW_COMPONENTS_RENDER3DS_RENDER3DS_H
#define OPENMW_COMPONENTS_RENDER3DS_RENDER3DS_H

// Nintendo 3DS rendering backend (citro3d).
//
// OpenSceneGraph keeps the scene graph, update and cull; its GL calls go to a stub library. This backend
// replaces the draw: an OSG hook (deps/patches/osg/0003) hands every RenderStage to it, and it walks the
// culled render bins itself, emitting citro3d commands for each leaf (geometry uploaded once to linear
// memory, textures from the PC-cooked pack, OpenMW's lighting evaluated per vertex on the PICA).
// See LIMITATIONS.md section 4 ("option C").

#include <osg/Matrixf>
#include <osg/Referenced>
#include <osg/ref_ptr>

#include <cstdint>
#include <string_view>
#include <vector>

namespace osg
{
    class Geometry;
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
    // Which path draws the interface, for the debug display: "ui CPU", or the deprecated GPU one.
    const char* interfacePath();
    bool interfaceByCpu(); // the CPU path (the port's one) rather than the deprecated GPU one

    // While a loading screen is up (hardware test #54: the cell being torn down and rebuilt drew smears on the
    // top screen), the top screen keeps its last frame: no clear, no scene draws; the interface still draws.
    void setHoldTopScreen(bool hold);
    // Both screens' interface drawn whole again: after a system applet (the keyboard), whose transition writes
    // the screens' capture into VRAM over the top overlay texture (test #63).
    void invalidateInterface();
    // MyGUI laid its layers out again this frame (myguirendermanager.cpp): its batches may differ from the last
    // ones. Without it, the backend knows they're the same and skips its fingerprint of them.
    void noteGuiCollected();
    // Only the HUD is up (in game, no menu, no page; n3ds/hud3ds.cpp): its layout may then run at most every 3rd
    // frame (myguirendermanager.cpp).
    void setHudOnly(bool hudOnly);
    bool hudOnly();

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
        uint32_t mSkinnedDraws = 0; // draws skinned on the GPU (batches of skinned meshes)
        // Draws by kind (M10, fewer draws): sky passes, terrain layers, particle systems, small (under 64
        // triangles) and other scene geometry. Skinned ones are mSkinnedDraws.
        uint32_t mSkyDraws = 0, mTerrainDraws = 0, mParticleDraws = 0, mSmallDraws = 0, mOtherDraws = 0;
        uint32_t mSmallCulled = 0; // parts of models left out, smaller than the small feature culling size
        uint32_t mParticleVertices = 0; // particle quads built on the CPU this frame (6 vertices each)
        uint32_t mBadParticles = 0; // particles left out: non-finite corners (a zero-scale effect, see buildParticles)
        uint32_t mWeatherSkipped = 0; // rain drops and storm particles left out near the eye (perf batch 1)
        uint32_t mFlameReplays = 0; // particle systems drawn from a recorded loop (perf batch 2)
        uint32_t mSameState = 0; // draws that kept the previous draw's state (perf batch 4)
        uint32_t mGuiDraws = 0; // GUI draws (both screens; batches with one texture and blend merged)
        uint32_t mGuiTextureBinds = 0; // distinct textures of the GUI passes, summed (one per screen: the UI atlas)
        uint32_t mGuiTopFrames = 0; // frames that redrew the top screen's interface texture
        uint32_t mGuiBottomFrames = 0; // frames in which the bottom screen was redrawn (0 or 1)
    };
    const FrameStats& getLastFrameStats();

    // Saves the next completed top-screen frame as a BMP (main thread).
    void requestCapture(const char* path);
    // The player's screenshot: both screens in one BMP at the next frame, written in the background (main thread).
    // playerCaptureStarted: true once, when the save began (p3ds::screenshot::takeAsyncResult then tells its end).
    void requestPlayerCapture(const char* path);
    bool playerCaptureStarted();

    // PC-cooked texture pack (tools/cook_textures.py). Missing pack = every texture falls back to OSG's
    // loaders (DDS can't be displayed, see ImageManager).
    bool openTexturePack(const char* path);
    // An expansion's pack (tools/cook_textures.py --addon: its BSA's textures only), after the main one: its textures
    // override the main pack's. false when the file isn't there (the expansion isn't installed).
    bool openAddonTexturePack(const char* path);

    // A texture from the pack as an osg::Image whose data is already in PICA format in linear memory.
    // nullptr if the pack has no texture for this normalized VFS path (extension ignored).
    osg::ref_ptr<osg::Image> loadCookedImage(std::string_view normalizedPath);

    // The size of the original image a pack texture was cooked from (MyGUI measures skins in its pixels); false
    // for any other image.
    bool cookedSourceSize(const osg::Image& image, int& width, int& height);

    // GPU skinning (M10). A skinned mesh (sceneutil/riggeometry.cpp) split into batches of at most kSkinBones
    // bones, each batch drawn by the skinned shader variant (skin1.v.pica) with its bones' matrices as uniforms:
    // the mesh's vertices are uploaded once, shared by every actor wearing it, instead of skinned on the CPU and
    // uploaded again every frame. Built once per mesh, read-only afterwards.
    constexpr int kSkinBones = 14;
    struct SkinBatch
    {
        std::vector<uint16_t> mBones; // palette slot -> the mesh's bone (RigGeometry's bone list)
        std::vector<uint16_t> mVertices; // the batch's vertices: source vertex index
        std::vector<uint8_t> mSlots; // 4 per vertex: palette slot x 3 (the slot's first row); unused slots 0
        std::vector<uint8_t> mWeights; // 4 per vertex: 0..255, summing to 255
        std::vector<uint16_t> mIndices; // triangle list, batch vertex indices
    };
    struct SkinMesh : osg::Referenced
    {
        osg::ref_ptr<const osg::Geometry> mSource; // bind-pose arrays: positions, normals, colors, texcoords
        std::vector<SkinBatch> mBatches;
    };
    // This frame's pose of one skinned drawable, set as the drawn geometry's user data by RigGeometry's cull: one
    // matrix per bone of the mesh, bone space to the drawable's space (OSG row-vector order, as RigGeometry's CPU
    // skinning applies them). The backend draws the geometry from mMesh with these instead of its own vertices.
    struct SkinPose : osg::Referenced
    {
        osg::ref_ptr<const SkinMesh> mMesh;
        std::vector<osg::Matrixf> mBones;
    };
    // Whether skinned meshes are drawn by the skinned shader; otherwise RigGeometry skins them on the CPU.
    bool gpuSkinning();

    // Performance batch 1 (docs/MW3DS_STUDY.md), each on unless its file is on the SD card (read once, at the first
    // call), so a hardware run can compare with and without:
    //   thin rain and faded storm clouds near the eye (sdmc:/openmw/no-thinweather);
    //   distant actors' bones updated every 2nd/3rd/4th frame by distance (sdmc:/openmw/no-posecadence);
    //   scenery parts culled at 10 pixels instead of the setting's 6 (sdmc:/openmw/no-smallcull).
    bool thinWeather();
    bool poseCadence();
    bool smallCullBoost();
    // Perf batch 2: still objects' flames simulated once per kind and replayed (sdmc:/openmw/no-flameloops).
    bool flameLoops();
    // Perf batch 4: a draw under the previous draw's state skips its state, textures and material
    // (sdmc:/openmw/no-samestate).
    bool sameStateSkip();
    // M19: a local map tile's camera can go: its texture is drawn and copied out (or never will be).
    bool mapTileReady(const osg::Texture* texture);
    // Every how many frames a skeleton this far from the camera gets its pose updated (1: every frame).
    inline int poseEvery(float distance)
    {
        return distance < 800.f ? 1 : distance < 1600.f ? 2 : distance < 3000.f ? 3 : 4;
    }
}

#endif
