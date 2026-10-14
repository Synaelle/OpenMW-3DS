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
    // Whether MyGUI is drawn. While it isn't, its batches aren't built either (cull time).
    bool drawsGui();

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
}

#endif
