#include "render3ds.hpp"

#include "gpumemory.hpp"

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/material.hpp>
#include <components/sceneutil/riggeometry.hpp>
#include <components/settings/values.hpp>

#include <osg/AlphaFunc>
#include <osg/BlendFunc>
#include <osg/Camera>
#include <osg/ColorMask>
#include <osg/CullFace>
#include <osg/Depth>
#include <osg/Geometry>
#include <osg/Observer>
#include <osg/TexMat>
#include <osgParticle/ParticleSystem>
#include <osg/Texture2D>
#include <osg/Uniform>
#include <osg/View>
#include <osgUtil/RenderBin>
#include <osgUtil/RenderLeaf>
#include <osgUtil/RenderStage>
#include <osgUtil/StateGraph>

#include <platform3ds/crash.hpp>
#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/platform.hpp>
#include <platform3ds/screenshot.hpp>
#include <platform3ds/textfb.hpp>

#include <citro3d.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <unordered_set>

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <map>
#include <tuple>
#include <osg/observer_ptr>
#include <osg/MatrixTransform>
#include <osg/UserDataContainer>
#include <unordered_map>
#include <vector>

#include "probe_shbin.h"
#include "scene_shbin.h"

namespace SDLUtil
{
    extern void (*gSwapBuffersHook3DS)(); // components/sdlutil/sdlgraphicswindow_3ds.cpp
}

extern "C"
{
    extern u32 __ctru_linear_heap; // platform3ds/src/platform.cpp (__system_allocateHeaps)
    extern u32 __ctru_linear_heap_size;
}

namespace Render3DS
{
    namespace
    {
        constexpr u32 kDisplayTransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0)
            | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)
            | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);

        constexpr int kMaxLights = 8; // scene.v.pica lights[32]

        // Vertex shader input registers. Attribute index == register (see setupAttributes).
        enum Attribute
        {
            Attr_Position = 0,
            Attr_Normal = 1,
            Attr_Color = 2,
            Attr_TexCoord = 3,
            Attr_Count
        };

        bool gActive = false;
        C3D_RenderTarget* gTarget = nullptr;
        DVLB_s* gDvlb = nullptr;
        shaderProgram_s gProgram;
        shaderProgram_s gProbeProgram; // the M0 test app's shader (self-test baseline)
        shaderProgram_s gFlatProgram; // minimal shader on the scene vertex layout (self-test)
        // gProgram's variants that evaluate fewer point lights (scene1.v.pica: slot 0 only, scene0.v.pica: none).
        // Same code upload and uniform layout as gProgram (one DVLB, gen_scene_shader.py). Off if the layouts
        // don't match.
        shaderProgram_s gProgram1;
        shaderProgram_s gProgram0;
        shaderProgram_s gProgram2; // 2 point lights (perf batch 2, gProgram evaluates 4)
        bool gVariant2 = false;
        constexpr int kDrawLights = 4; // lights a draw is lit by at most: gProgram's (perf batch 2)
        bool gVariants = false;
        DVLB_s* gProbeDvlb = nullptr; // the probe shader's own DVLB (M10: its room in gDvlb went to gProgramSkin)
        shaderProgram_s gProgramSkin; // skinned on the GPU, sun + 1 light (skin1.v.pica)
        bool gSkinVariant = false;

        // Set when the GPU hung: the GPU core was reset and nothing is drawn for the rest of the run, so the game
        // keeps running (headless) and HOME closes it normally instead of a frozen console.
        const char* gFailure = nullptr;
        bool gInFrame = false;
        bool gHoldTop = false; // setHoldTopScreen: the scene's target kept as it is (a loading screen is up)

        // What texture unit 1 holds when no blend map is needed. citro3d's C3D_TexBind dereferences the texture
        // for units 1 and 2 without a null check (it refuses non-2D types there), so unit 1 is never "unbound":
        // it gets this 8x8 texture, which the TexEnv stage ignores and whose fetches always hit the texture cache.
        C3D_Tex gUnusedTex1;
        bool gUnusedTex1Ready = false;

        struct UniformLocations
        {
            int mProjection, mModelView, mTexMat, mTexMat1, mSunDir, mSunDiffuse, mSunAmbient, mMatDiffuse,
                mMatAmbient, mMatEmission, mColorSel, mLighting, mLights, mBones;
        } gLoc;

        FrameStats gStats;
        FrameStats gLastStats;
        uint32_t gFrame = 0; // backend frame counter (resolved-state cache stamp)
        float gNear = 1, gFar = 1000; // current perspective projection (fog LUT)

        // Vertex and index buffers are addressed by the PICA as 28-bit offsets from a base address. citro3d's
        // default base 0x18000000 reaches physical 0x27FFFFFF. The linear heap is at 0x20000000 on hardware too
        // (test #10), so the default is kept (exactly what the M0 test app used). The base only moves if the heap
        // ever ends beyond reach.
        u32 gLinearBasePa = 0;
        u32 gBufferBase = 0x18000000;

        // Draw-by-draw verification of the first frames that draw anything: each draw is submitted alone and
        // waited for, so a draw that hangs the GPU is named in the log instead of freezing the console.
        int gVerifyFramesLeft = 2;
        bool gVerifyThisFrame = false;

        // The draw-by-draw checks (the first frames, and every new object's first draw) cost a GPU wait each:
        // 2282 of them in test #72, seconds added to every cell load. The GPU hangs they were made for (tests #9,
        // #10) are long fixed: off unless sdmc:/openmw/gpu-verify exists (debug builds). The startup self-test stays.
        bool gpuVerify()
        {
#ifndef OPENMW3DS_DEBUG
            return false;
#endif
            static const bool enabled = [] {
                std::FILE* f = std::fopen("sdmc:/openmw/gpu-verify", "r");
                if (f != nullptr)
                    std::fclose(f);
                P3DS_LOGI("render3ds: draw-by-draw GPU checks %s", f != nullptr ? "on (sdmc:/openmw/gpu-verify)" : "off");
                return f != nullptr;
            }();
            return enabled;
        }
        uint32_t gVerifiedDraws = 0;

        bool gWarnedPrimitive = false;
        bool gWarnedArray = false;

        // ------------------------------------------------------------------ matrices

        // OSG matrices are row-vector (v * M). PICA uniforms are rows of the column-vector matrix: transpose.
        void setRows(C3D_FVec* rows, const osg::Matrixf& m, int count)
        {
            for (int i = 0; i < count; ++i)
            {
                rows[i].x = m(0, i);
                rows[i].y = m(1, i);
                rows[i].z = m(2, i);
                rows[i].w = m(3, i);
            }
        }

        bool gPerspective = true;
        bool gRotatedTarget = true; // drawing on a screen (rotated 90 degrees), not into a texture
        float gSmallFeaturePixels = 0.f; // [Camera] small feature culling pixel size, 0 = off (read every frame)

        // GL clip space -> PICA clip space. The top screen framebuffer is rotated 90 degrees (as citro3d's
        // Mtx_PerspTilt: x' = y, y' = -x). Depth, with the convention 0 = near, 1 = far (GL's LESS works as is):
        // - perspective: W-buffering, z' = -w / far so depth = eye distance / far. Linear depth gives the fog LUT
        //   (128 entries indexed by depth) uniform resolution. With z/w and a 1-unit near plane everything past
        //   ~120 units would share its first entry.
        // - orthographic: z-buffering, GL's [-w, w] squeezed into the PICA's [-w, 0], depth = z/w + 1.
        void setProjection(const osg::Matrixf& projection)
        {
            C3D_FVec p[4];
            setRows(p, projection, 4);
            C3D_FVec* out = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mProjection, 4);
            if (gRotatedTarget)
            {
                out[0] = p[1];
                out[1] = FVec4_Scale(p[0], -1.f);
            }
            else // a texture (render-to-texture): not the screens' rotated framebuffer
            {
                out[0] = p[0];
                out[1] = p[1];
            }
            out[3] = p[3];
            gPerspective = projection(3, 3) == 0.f;
            if (gPerspective)
            {
                // osg::Matrix::getPerspective
                gNear = projection(3, 2) / (projection(2, 2) - 1.f);
                gFar = projection(3, 2) / (projection(2, 2) + 1.f);
                // W-buffer: z' = -(w - near) / (far - near), depth = -z' = (w - near) / (far - near), linear in the
                // distance. The PICA clips at z' > 0, i.e. at w = near. With z' = -w / far (until hardware test #19) it
                // clipped at w = 0: polygons reaching behind the camera (the water plane's 30000-unit quads when
                // looking down) were cut at w ~ 0, their screen positions blew up and the fixed-point rasterizer
                // dropped part of them: half of the water missing, flashing as the view moved. Emulators clip in
                // floating point and don't show it.
                const float range = gFar - gNear;
                out[2] = FVec4_Scale(FVec4_Add(FVec4_Scale(p[3], -1.f), FVec4_New(0.f, 0.f, 0.f, gNear)), 1.f / range);
                C3D_DepthMap(false, -1.f, 0.f);
            }
            else
            {
                out[2] = FVec4_Scale(FVec4_Subtract(p[2], p[3]), 0.5f);
                C3D_DepthMap(true, 1.f, 1.f);
            }
        }

        // ------------------------------------------------------------------ deferred deletion

        // GPU objects die with their OSG objects (osg::Observer). OSG objects can be deleted on any thread
        // (OpenMW's UnrefQueue), so deletions are queued and processed on the main thread at frame start,
        // after the GPU finished the previous frame.
        class GpuObject : public osg::Observer
        {
        public:
            const osg::Referenced* mOwner = nullptr;
            void objectDeleted(void*) override;
            virtual void release() = 0; // frees GPU memory. Called on the main thread
        };

        std::mutex gPendingMutex;
        std::vector<GpuObject*> gPendingDeletes;

        void GpuObject::objectDeleted(void*)
        {
            std::lock_guard<std::mutex> lock(gPendingMutex);
            gPendingDeletes.push_back(this);
        }

        // A render-to-texture camera's color texture, drawn by the GPU (the character preview, the tester 20/10:
        // the race window showed no head): a VRAM texture with its own depth buffer, freed with the OSG texture
        // (the race window drops its preview when it closes).
        // One 256x256 target reserved at startup (test #85: allocated when the race window opened, the VRAM was
        // full of the scene's textures and the preview stayed empty). The race window is the only user.
        struct RttReserve
        {
            C3D_Tex mTex{};
            C3D_RenderTarget* mTarget = nullptr;
            bool mInUse = false;
        } gRttReserve;

        // M19: the local map's tiles (the minimap and the map window, black since M8, off in 0.1.0 and kept for when
        // the minimap comes back): each drawn in turn into one
        // reserved 128x128 VRAM target, then copied by the CPU into a texture of its own (RGB565 in linear memory,
        // 32 KB) once the GPU is done. LocalMap keeps a tile's camera until mapTileReady (one tile at a time).
        struct MapScratch
        {
            C3D_Tex mTex{};
            C3D_RenderTarget* mTarget = nullptr;
            const void* mBusy = nullptr; // the RttTarget drawn into it, copied out at the next beginFrame
        } gMapScratch;
        constexpr int kMapTileSize = 128; // [Map] local map resolution on the 3DS (settings-3ds.cfg)

        struct RttTarget : GpuObject
        {
            C3D_Tex* mTex = nullptr; // nullptr: no target (not drawn, not shown)
            C3D_RenderTarget* mTarget = nullptr;
            bool mDrawn = false; // drawn this frame: the CPU interface reads it once the GPU is done (next frame)
            bool mReady = false; // a finished image: until then the interface shows nothing (VRAM leftovers, #88)
            bool mMapTile = false; // a local map tile: mTex is its own copy (gMapScratch)
            bool mGaveUp = false; // a map tile that can't be drawn (no scratch, another size): LocalMap stops asking
            int mBlankRetries = 0; // a map tile that came out all black, drawn again (the first frame after a load)
            int mPrepareFrames = 0; // a map tile: frames spent preparing its objects before drawing it (prepareMapBin)
            unsigned mPrepared = 0; // ... and how many objects and textures were prepared
            void release() override
            {
                if (mTex == &gRttReserve.mTex)
                    gRttReserve.mInUse = false;
                if (gMapScratch.mBusy == this)
                    gMapScratch.mBusy = nullptr;
                if (mMapTile && mTex != nullptr)
                {
                    C3D_TexDelete(mTex);
                    delete mTex;
                    mTex = nullptr;
                }
            }
        };
        std::unordered_map<const osg::Texture*, RttTarget*> gRttTargets;

        void bumpTexture(const C3D_Tex* tex);

        // The scratch target's image (RGBA8, tiled: A, B, G, R) into the tile's own RGB565 texture, texel for texel
        // (the same tiled order), once the GPU drew it (called at the next frame's start).
        void copyMapTile(RttTarget& rtt)
        {
            if (gMapScratch.mBusy != &rtt)
                return;
            gMapScratch.mBusy = nullptr;
            if (rtt.mTex == nullptr)
            {
                rtt.mTex = new C3D_Tex;
                if (!C3D_TexInit(rtt.mTex, kMapTileSize, kMapTileSize, GPU_RGB565))
                {
                    delete rtt.mTex;
                    rtt.mTex = nullptr;
                    rtt.mGaveUp = true;
                    P3DS_LOGW("render3ds: no linear memory for a local map tile");
                    return;
                }
            }
            GSPGPU_InvalidateDataCache(gMapScratch.mTex.data, gMapScratch.mTex.size);
            const uint8_t* in = static_cast<const uint8_t*>(gMapScratch.mTex.data);
            uint16_t* out = static_cast<uint16_t*>(rtt.mTex->data);
            unsigned long brightness = 0; // all black: see below
            // Texel for texel: the GPU drew the tile the way the interface reads it (OpenMW flips render-to-texture
            // images in its widgets, the character preview's too).
            for (int i = 0; i < kMapTileSize * kMapTileSize; ++i, in += 4)
            {
                out[i] = uint16_t((in[3] >> 3) << 11 | (in[2] >> 2) << 5 | (in[1] >> 3));
                brightness += in[3] + in[2] + in[1];
            }
            // All black (the clear colour, nothing drawn): the first frame after a loading screen (the interior's tile
            // at a game's start) doesn't reach the GPU. Drawn again, twice at most (a tile of an interior's padding is
            // black for real).
            if (brightness == 0 && rtt.mBlankRetries < 2)
            {
                ++rtt.mBlankRetries;
                return;
            }
            GSPGPU_FlushDataCache(rtt.mTex->data, rtt.mTex->size);
            bumpTexture(rtt.mTex);
            rtt.mReady = true;
        }

        // ------------------------------------------------------------------ geometry

        struct GpuPrimitive
        {
            GPU_Primitive_t mMode;
            uint32_t mCount;
            uint32_t mFirst; // vertex (arrays) or byte offset into the index buffer (elements)
            int mIndexType; // -1 = arrays, else C3D_UNSIGNED_BYTE / C3D_UNSIGNED_SHORT
            uint32_t mTriangles;
        };

        class GpuGeometry : public GpuObject
        {
        public:
            void* mVbo = nullptr;
            uint32_t mVboSize = 0;
            uint32_t mVertexCount = 0;
            u32 mVboOffset = 0; // physical address - vertex buffer base (GPUREG_ATTRIBBUFFER0_OFFSET)
            void* mIbo = nullptr;
            uint32_t mIboSize = 0;
            std::vector<GpuPrimitive> mPrimitives;

            // Change detection.
            const osg::Array* mArrays[Attr_Count] = {};
            unsigned mModified[Attr_Count] = {};
            unsigned mPrimitiveSignature = 0;
            bool mValid = false;
            bool mVerified = false; // its draws completed on the GPU once (draw-by-draw check on first use)

            void release() override
            {
                gpuFree(mVbo);
                gpuFree(mIbo);
                mVbo = mIbo = nullptr;
            }
        };

        std::unordered_map<const osg::Geometry*, GpuGeometry*> gGeometries;

        // ------------------------------------------------------------------ textures

        class GpuTexture : public GpuObject
        {
        public:
            C3D_Tex mTex{};
            void* mOwnedData = nullptr; // converted images. Cooked images own their data
            const osg::Image* mImage = nullptr;
            unsigned mImageModified = 0;
            int mLevels = 1;
            bool mValid = false;

            void release() override
            {
                gpuFree(mOwnedData);
                mOwnedData = nullptr;
            }
        };

        std::unordered_map<const osg::Texture*, GpuTexture*> gTextures;
        // M10: the last lookup (consecutive draws mostly share a texture, an unordered_map lookup divides by its
        // bucket count, in software on this CPU). Map values stay put on insert. Erasing clears it.
        const osg::Texture* gLastTextureKey = nullptr;
        GpuTexture** gLastTextureSlot = nullptr;

        bool forgetSkin(GpuObject* object); // a GpuSkin (GPU skinning, below): out of gSkins

        void processPendingDeletes()
        {
            std::vector<GpuObject*> pending;
            {
                std::lock_guard<std::mutex> lock(gPendingMutex);
                pending.swap(gPendingDeletes);
            }
            for (GpuObject* object : pending)
            {
                if (auto* geometry = dynamic_cast<GpuGeometry*>(object))
                {
                    auto it = gGeometries.find(static_cast<const osg::Geometry*>(object->mOwner));
                    if (it != gGeometries.end() && it->second == geometry)
                        gGeometries.erase(it);
                }
                else if (forgetSkin(object))
                {
                }
                else if (auto* rtt = dynamic_cast<RttTarget*>(object))
                {
                    auto it = gRttTargets.find(static_cast<const osg::Texture*>(object->mOwner));
                    if (it != gRttTargets.end() && it->second == rtt)
                        gRttTargets.erase(it);
                }
                else if (auto* texture = dynamic_cast<GpuTexture*>(object))
                {
                    auto it = gTextures.find(static_cast<const osg::Texture*>(object->mOwner));
                    if (it != gTextures.end() && it->second == texture)
                        gTextures.erase(it);
                    gLastTextureKey = nullptr;
                    gLastTextureSlot = nullptr;
                }
                object->release();
                delete object;
            }
        }

        // ------------------------------------------------------------------ resolved state

        template <class T>
        struct Slot
        {
            const T* mValue = nullptr;
            unsigned mFlags = 0;

            // osg::State semantics: a parent's OVERRIDE wins unless the child's value is PROTECTED.
            void merge(const T* value, unsigned flags)
            {
                if ((mFlags & osg::StateAttribute::OVERRIDE) && !(flags & osg::StateAttribute::PROTECTED))
                    return;
                mValue = value;
                mFlags = flags;
            }
        };

        struct ModeSlot
        {
            bool mOn = false;
            unsigned mFlags = 0;

            void merge(unsigned value)
            {
                if ((mFlags & osg::StateAttribute::OVERRIDE) && !(value & osg::StateAttribute::PROTECTED))
                    return;
                mOn = (value & osg::StateAttribute::ON) != 0;
                mFlags = value;
            }
        };

        struct ResolvedState
        {
            Slot<osg::Texture> mTexture;
            Slot<osg::TexMat> mTexMat;
            Slot<osg::Texture> mTexture1; // unit 1: a blend map only on terrain layers (see blendMapOf)
            Slot<osg::Uniform> mTexMat0; // terrain layer tiling (uniform "texMat0", else mTexMat)
            Slot<osg::Uniform> mTexMat1; // terrain blend map placement
            Slot<osg::Uniform> mBlendMap; // sampler uniform "blendMap": only terrain layers have it
            Slot<SceneUtil::Material> mMaterial;
            Slot<osg::BlendFunc> mBlendFunc;
            Slot<osg::AlphaFunc> mAlphaFunc;
            Slot<osg::Depth> mDepth;
            Slot<osg::CullFace> mCullFace;
            Slot<osg::ColorMask> mColorMask;
            ModeSlot mBlend;
            ModeSlot mDepthTest;
            ModeSlot mCullFaceMode;
            Slot<osg::Uniform> mLightBuffer;
            Slot<osg::Uniform> mPointLightCount;
            Slot<osg::Uniform> mSunPosition;
            Slot<osg::Uniform> mSunDiffuse;
            Slot<osg::Uniform> mSunAmbient;
            Slot<osg::Uniform> mAlpha;
            Slot<osg::Uniform> mActorFade;
            Slot<osg::Uniform> mFogStart;
            Slot<osg::Uniform> mFogEnd;
            Slot<osg::Uniform> mFogColor;
            // Sky shader uniforms (files/shaders/compatibility/sky.frag)
            Slot<osg::Uniform> mSkyPass; // "pass"
            Slot<osg::Uniform> mDiffuseColor;
            Slot<osg::Uniform> mOpacity;
            Slot<osg::Uniform> mMoonBlend;
            Slot<osg::Uniform> mAtmosphereFade;
        };

        // The terrain layer's blend map. Other textures on unit 1 (an enchantment glow, a NIF's dark, detail or glow
        // map) aren't drawn: taken for blend maps, they made objects translucent.
        const osg::Texture* blendMapOf(const ResolvedState& rs)
        {
            return rs.mBlendMap.mValue != nullptr ? rs.mTexture1.mValue : nullptr;
        }

        // Resolved state of a StateGraph node, recomputed once per frame (memoized through the parent chain).
        struct StateGraphTag : public osg::Referenced
        {
            uint32_t mFrame = ~0u;
            uint32_t mIndex = 0;
        };

        std::vector<ResolvedState> gResolved;
        uint32_t gResolvedCount = 0;

        void applyStateSet(ResolvedState& rs, const osg::StateSet& ss)
        {
            for (const auto& [key, pair] : ss.getAttributeList())
            {
                const osg::StateAttribute* attr = pair.first.get();
                const unsigned flags = pair.second;
                switch (key.first)
                {
                    case osg::StateAttribute::MATERIAL:
                        if (auto* material = dynamic_cast<const SceneUtil::Material*>(attr))
                            rs.mMaterial.merge(material, flags);
                        break;
                    case osg::StateAttribute::BLENDFUNC:
                        if (key.second == 0)
                            rs.mBlendFunc.merge(static_cast<const osg::BlendFunc*>(attr), flags);
                        break;
                    case osg::StateAttribute::ALPHAFUNC:
                        rs.mAlphaFunc.merge(static_cast<const osg::AlphaFunc*>(attr), flags);
                        break;
                    case osg::StateAttribute::DEPTH:
                        rs.mDepth.merge(static_cast<const osg::Depth*>(attr), flags);
                        break;
                    case osg::StateAttribute::CULLFACE:
                        rs.mCullFace.merge(static_cast<const osg::CullFace*>(attr), flags);
                        break;
                    case osg::StateAttribute::COLORMASK:
                        if (key.second == 0) // not an indexed ColorMaski (normals render target)
                            rs.mColorMask.merge(static_cast<const osg::ColorMask*>(attr), flags);
                        break;
                    default:
                        break;
                }
            }

            for (const auto& [mode, value] : ss.getModeList())
            {
                switch (mode)
                {
                    case GL_BLEND:
                        rs.mBlend.merge(value);
                        break;
                    case GL_DEPTH_TEST:
                        rs.mDepthTest.merge(value);
                        break;
                    case GL_CULL_FACE:
                        rs.mCullFaceMode.merge(value);
                        break;
                    default:
                        break;
                }
            }

            const osg::StateSet::TextureAttributeList& textures = ss.getTextureAttributeList();
            if (!textures.empty())
            {
                for (const auto& [key, pair] : textures[0])
                {
                    if (key.first == osg::StateAttribute::TEXTURE)
                        rs.mTexture.merge(static_cast<const osg::Texture*>(pair.first.get()), pair.second);
                    else if (key.first == osg::StateAttribute::TEXMAT)
                        rs.mTexMat.merge(static_cast<const osg::TexMat*>(pair.first.get()), pair.second);
                }
            }
            if (textures.size() > 1)
            {
                for (const auto& [key, pair] : textures[1])
                    if (key.first == osg::StateAttribute::TEXTURE)
                        rs.mTexture1.merge(static_cast<const osg::Texture*>(pair.first.get()), pair.second);
            }

            for (const auto& [name, pair] : ss.getUniformList())
            {
                const osg::Uniform* uniform = pair.first.get();
                const unsigned flags = pair.second;
                switch (name[0])
                {
                    case 'L':
                        if (name == std::string_view("LightBuffer"))
                            rs.mLightBuffer.merge(uniform, flags);
                        break;
                    case 'P':
                        if (name == std::string_view("PointLightCount"))
                            rs.mPointLightCount.merge(uniform, flags);
                        break;
                    case 's':
                        if (name == std::string_view("sun.position"))
                            rs.mSunPosition.merge(uniform, flags);
                        else if (name == std::string_view("sun.diffuse"))
                            rs.mSunDiffuse.merge(uniform, flags);
                        else if (name == std::string_view("sun.ambient"))
                            rs.mSunAmbient.merge(uniform, flags);
                        break;
                    case 'f':
                        if (name == std::string_view("fog.start"))
                            rs.mFogStart.merge(uniform, flags);
                        else if (name == std::string_view("fog.end"))
                            rs.mFogEnd.merge(uniform, flags);
                        else if (name == std::string_view("fog.color"))
                            rs.mFogColor.merge(uniform, flags);
                        break;
                    case 'a':
                        if (name == std::string_view("alpha"))
                            rs.mAlpha.merge(uniform, flags);
                        else if (name == std::string_view("actorFade"))
                            rs.mActorFade.merge(uniform, flags);
                        else if (name == std::string_view("atmosphereFade"))
                            rs.mAtmosphereFade.merge(uniform, flags);
                        break;
                    case 'p':
                        if (name == std::string_view("pass"))
                            rs.mSkyPass.merge(uniform, flags);
                        break;
                    case 'b':
                        if (name == std::string_view("blendMap"))
                            rs.mBlendMap.merge(uniform, flags);
                        break;
                    case 'd':
                        if (name == std::string_view("diffuseColor"))
                            rs.mDiffuseColor.merge(uniform, flags);
                        break;
                    case 'o':
                        if (name == std::string_view("opacity"))
                            rs.mOpacity.merge(uniform, flags);
                        break;
                    case 'm':
                        if (name == std::string_view("moonBlend"))
                            rs.mMoonBlend.merge(uniform, flags);
                        break;
                    case 't':
                        if (name == std::string_view("texMat0"))
                            rs.mTexMat0.merge(uniform, flags);
                        else if (name == std::string_view("texMat1"))
                            rs.mTexMat1.merge(uniform, flags);
                        break;
                    default:
                        break;
                }
            }
        }

        const ResolvedState& resolve(osgUtil::StateGraph* sg)
        {
            auto* tag = static_cast<StateGraphTag*>(sg->getUserData());
            if (tag == nullptr)
            {
                tag = new StateGraphTag;
                sg->setUserData(tag);
            }
            if (tag->mFrame == gFrame)
                return gResolved[tag->mIndex];

            ResolvedState state;
            if (sg->_parent != nullptr)
                state = resolve(sg->_parent);
            else
                state.mDepthTest.mOn = true; // SceneView's global defaults: depth test on (LESS), blending off
            if (const osg::StateSet* ss = sg->getStateSet())
                applyStateSet(state, *ss);

            if (gResolvedCount == gResolved.size())
                gResolved.resize(gResolved.size() * 2 + 64);
            tag->mFrame = gFrame;
            tag->mIndex = gResolvedCount++;
            gResolved[tag->mIndex] = state;
            return gResolved[tag->mIndex];
        }

        // ------------------------------------------------------------------ uploads

        void flushToGpu(const void* data, size_t size)
        {
            GSPGPU_FlushDataCache(data, size);
            gStats.mUploadedBytes += static_cast<uint32_t>(size);
        }

        GPU_Primitive_t toPicaMode(GLenum mode, bool& ok)
        {
            ok = true;
            switch (mode)
            {
                case GL_TRIANGLES:
                    return GPU_TRIANGLES;
                case GL_TRIANGLE_STRIP:
                    return GPU_TRIANGLE_STRIP;
                case GL_TRIANGLE_FAN:
                    return GPU_TRIANGLE_FAN;
                default:
                    ok = false;
                    return GPU_TRIANGLES;
            }
        }

        uint32_t triangleCount(GLenum mode, uint32_t count)
        {
            if (mode == GL_TRIANGLES)
                return count / 3;
            return count >= 3 ? count - 2 : 0;
        }

        const osg::Array* perVertex(const osg::Array* array, unsigned vertexCount)
        {
            if (array == nullptr || array->getBinding() != osg::Array::BIND_PER_VERTEX
                || array->getNumElements() < vertexCount)
                return nullptr;
            return array;
        }

        // Every geometry gets the same layout, with defaults for missing arrays: no fixed attributes and one
        // attribute setup per frame (fixed attributes were one of the suspects of the hardware GPU hang).
        // The bone slots and weights are GPU skinning's (skin1.v.pica v4, v5). Every vertex carries them (unused
        // elsewhere) so the whole frame keeps one attribute layout. Test #93: switching the layout between skinned
        // and other draws mid-frame (6 attributes, then 4) made meshes and the interface flash on hardware, as
        // reconfiguring the vertex stages under in-flight draws did before (the fireball, gIdentityIbo below).
        struct Vertex
        {
            float mPosition[3];
            float mNormal[3];
            uint8_t mColor[4]; // 0..255, the shader scales by 1/255
            float mTexCoord[2];
            uint8_t mSlots[4]; // GPU skinning: palette slot x 3
            uint8_t mWeights[4]; // GPU skinning: 0..255
        };
        static_assert(sizeof(Vertex) == 44);
        using SkinVertex = Vertex;

        uint8_t toByte(float v)
        {
            return static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
        }

        // Geometry drawn only as GL_QUADS arrays: every quad has its own 4 vertices.
        bool onlyArrayQuads(const osg::Geometry& geometry)
        {
            if (geometry.getNumPrimitiveSets() == 0)
                return false;
            for (const auto& set : geometry.getPrimitiveSetList())
                if (set->getMode() != osg::PrimitiveSet::QUADS
                    || set->getType() != osg::PrimitiveSet::DrawArraysPrimitiveType)
                    return false;
            return true;
        }

        // OpenMW's water plane repeats its texture 900 times. Near the player the texture coordinates are ~450,
        // where the PICA's float24 (16-bit mantissa) is off by ~1 texel: noisy sampling, sparkles and shimmering on
        // the water (hardware test #18). Each quad has its own vertices, so its coordinates can lose their common
        // whole part, which a repeating texture doesn't see.
        void rebaseQuadTexCoords(Vertex* vertices, uint32_t count)
        {
            for (uint32_t q = 0; q + 4 <= count; q += 4)
            {
                Vertex* quad = vertices + q;
                for (int c = 0; c < 2; ++c)
                {
                    float lowest = quad[0].mTexCoord[c];
                    for (int k = 1; k < 4; ++k)
                        lowest = std::min(lowest, quad[k].mTexCoord[c]);
                    const float shift = std::floor(lowest);
                    if (shift != 0.f)
                        for (int k = 0; k < 4; ++k)
                            quad[k].mTexCoord[c] -= shift;
                }
            }
        }

        // An osg::Geometry's arrays, read as the scene vertex layout (Vertex).
        struct VertexSource
        {
            const osg::Vec3Array* mPositions = nullptr;
            const osg::Vec3Array* mNormals = nullptr;
            const osg::Vec4Array* mColors4f = nullptr;
            const osg::Vec4ubArray* mColors4ub = nullptr;
            const osg::Vec2Array* mTexCoords = nullptr;
            uint8_t mOverall[4] = { 255, 255, 255, 255 }; // BIND_OVERALL colors (or none: white) for every vertex

            // false: no Vec3Array vertex array (not drawn)
            bool init(const osg::Geometry& geometry)
            {
                mPositions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
                if (mPositions == nullptr || mPositions->empty())
                {
                    if (!gWarnedArray)
                        Log(Debug::Warning) << "render3ds: geometry without a Vec3Array vertex array (not drawn)";
                    gWarnedArray = true;
                    return false;
                }
                const unsigned count = mPositions->size();
                mNormals = dynamic_cast<const osg::Vec3Array*>(perVertex(geometry.getNormalArray(), count));
                const osg::Array* colors = perVertex(geometry.getColorArray(), count);
                mColors4f = dynamic_cast<const osg::Vec4Array*>(colors);
                mColors4ub = dynamic_cast<const osg::Vec4ubArray*>(colors);
                mTexCoords = dynamic_cast<const osg::Vec2Array*>(perVertex(geometry.getTexCoordArray(0), count));
                if (const osg::Array* c = geometry.getColorArray();
                    c && c->getBinding() == osg::Array::BIND_OVERALL && c->getNumElements() > 0)
                {
                    if (auto* c4f = dynamic_cast<const osg::Vec4Array*>(c))
                        for (int k = 0; k < 4; ++k)
                            mOverall[k] = toByte((*c4f)[0][k]);
                    else if (auto* c4ub = dynamic_cast<const osg::Vec4ubArray*>(c))
                        std::memcpy(mOverall, &(*c4ub)[0], 4);
                }
                return true;
            }

            void fill(Vertex& out, unsigned i) const
            {
                std::memcpy(out.mPosition, &(*mPositions)[i], 12);
                if (mNormals)
                    std::memcpy(out.mNormal, &(*mNormals)[i], 12);
                else
                {
                    out.mNormal[0] = out.mNormal[1] = 0.f;
                    out.mNormal[2] = 1.f;
                }
                if (mColors4f)
                    for (int k = 0; k < 4; ++k)
                        out.mColor[k] = toByte((*mColors4f)[i][k]);
                else if (mColors4ub)
                    std::memcpy(out.mColor, &(*mColors4ub)[i], 4);
                else
                    std::memcpy(out.mColor, mOverall, 4);
                if (mTexCoords)
                    std::memcpy(out.mTexCoord, &(*mTexCoords)[i], 8);
                else
                    out.mTexCoord[0] = out.mTexCoord[1] = 0.f;
            }
        };

        bool buildVertices(GpuGeometry& gpu, const osg::Geometry& geometry)
        {
            VertexSource source;
            if (!source.init(geometry))
                return false;
            const osg::Vec3Array* positions = source.mPositions;
            const unsigned count = positions->size();

            const uint32_t size = count * sizeof(Vertex);
            if (gpu.mVbo == nullptr || gpu.mVboSize < size || gpu.mVboSize > size * 2)
            {
                gpuFree(gpu.mVbo);
                gpu.mVbo = gpuAlloc(size);
                gpu.mVboSize = size;
                if (gpu.mVbo == nullptr)
                {
                    Log(Debug::Error) << "render3ds: out of linear memory for a " << size << " byte vertex buffer";
                    return false;
                }
            }

            auto* out = static_cast<Vertex*>(gpu.mVbo);
            for (unsigned i = 0; i < count; ++i, ++out)
                source.fill(*out, i);
            if (source.mTexCoords && onlyArrayQuads(geometry))
                rebaseQuadTexCoords(reinterpret_cast<Vertex*>(gpu.mVbo), count);
            flushToGpu(gpu.mVbo, size);

            gpu.mVertexCount = count;
            gpu.mVboOffset = osConvertVirtToPhys(gpu.mVbo) - gBufferBase;
            gpu.mArrays[Attr_Position] = positions;
            gpu.mArrays[Attr_Normal] = geometry.getNormalArray();
            gpu.mArrays[Attr_Color] = geometry.getColorArray();
            gpu.mArrays[Attr_TexCoord] = geometry.getTexCoordArray(0);
            for (int a = 0; a < Attr_Count; ++a)
                gpu.mModified[a] = gpu.mArrays[a] ? gpu.mArrays[a]->getModifiedCount() : 0;
            return true;
        }

        unsigned primitiveSignature(const osg::Geometry& geometry)
        {
            unsigned signature = geometry.getNumPrimitiveSets() * 0x9E3779B1u;
            for (const auto& set : geometry.getPrimitiveSetList())
                signature = signature * 31 + set->getModifiedCount() + static_cast<unsigned>(set->getMode());
            return signature;
        }

        // The PICA hangs on some malformed draws that the emulator ignores. Keep only sane ones: at least one whole
        // primitive, triangle lists in whole triangles, every vertex in range.
        uint32_t saneCount(GLenum mode, uint32_t count)
        {
            if (mode == GL_TRIANGLES)
                count -= count % 3;
            return count >= 3 ? count : 0;
        }

        bool gWarnedRange = false;

        template <class Index>
        bool indicesInRange(const Index* indices, size_t count, uint32_t vertexCount)
        {
            for (size_t i = 0; i < count; ++i)
                if (indices[i] >= vertexCount)
                {
                    if (!gWarnedRange)
                        Log(Debug::Warning) << "render3ds: skipping primitives with vertex index " << unsigned(indices[i])
                                            << " >= " << vertexCount << " vertices";
                    gWarnedRange = true;
                    return false;
                }
            return true;
        }

        bool buildPrimitives(GpuGeometry& gpu, const osg::Geometry& geometry)
        {
            gpu.mPrimitives.clear();
            // Index data of every DrawElements set in one allocation (each start 4-byte aligned).
            uint32_t indexBytes = 0;
            for (const auto& set : geometry.getPrimitiveSetList())
            {
                if (const osg::DrawElements* elements = set->getDrawElements())
                {
                    const uint32_t bytes = elements->getNumIndices() * (elements->getType() == osg::PrimitiveSet::DrawElementsUBytePrimitiveType ? 1 : 2);
                    indexBytes += (bytes + 3) & ~3u;
                }
                else if (set->getMode() == osg::PrimitiveSet::QUADS
                    && set->getType() == osg::PrimitiveSet::DrawArraysPrimitiveType)
                {
                    const uint32_t quads = static_cast<const osg::DrawArrays*>(set.get())->getCount() / 4;
                    indexBytes += (quads * 6 * 2 + 3) & ~3u;
                }
            }
            if (indexBytes > 0 && (gpu.mIbo == nullptr || gpu.mIboSize < indexBytes))
            {
                gpuFree(gpu.mIbo);
                gpu.mIbo = gpuAlloc(indexBytes);
                gpu.mIboSize = indexBytes;
                if (gpu.mIbo == nullptr)
                {
                    Log(Debug::Error) << "render3ds: out of linear memory for a " << indexBytes << " byte index buffer";
                    return false;
                }
            }

            uint32_t offset = 0;
            for (const auto& set : geometry.getPrimitiveSetList())
            {
                if (set->getMode() == osg::PrimitiveSet::QUADS)
                {
                    // The PICA has no quads (OpenMW's water plane is GL_QUADS): two indexed triangles per quad.
                    if (set->getType() != osg::PrimitiveSet::DrawArraysPrimitiveType)
                        continue;
                    auto* arrays = static_cast<const osg::DrawArrays*>(set.get());
                    const uint32_t quads = arrays->getCount() > 0 ? arrays->getCount() / 4 : 0;
                    const uint32_t first = arrays->getFirst() > 0 ? arrays->getFirst() : 0;
                    if (quads == 0 || first + quads * 4 > gpu.mVertexCount || first + quads * 4 > 65536)
                        continue;
                    auto* out16 = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(gpu.mIbo) + offset);
                    for (uint32_t q = 0; q < quads; ++q)
                    {
                        const uint16_t v = static_cast<uint16_t>(first + q * 4);
                        const uint16_t triangles[6] = { v, uint16_t(v + 1), uint16_t(v + 2), v, uint16_t(v + 2),
                            uint16_t(v + 3) };
                        std::memcpy(out16 + q * 6, triangles, sizeof(triangles));
                    }
                    const uint32_t count = quads * 6;
                    gpu.mPrimitives.push_back({ GPU_TRIANGLES, count, offset, C3D_UNSIGNED_SHORT, quads * 2 });
                    offset += (count * 2 + 3) & ~3u;
                    continue;
                }
                bool ok;
                const GPU_Primitive_t mode = toPicaMode(set->getMode(), ok);
                if (!ok)
                {
                    if (!gWarnedPrimitive)
                        Log(Debug::Warning) << "render3ds: skipping primitive mode " << set->getMode()
                                            << " (points/lines are not drawn)";
                    gWarnedPrimitive = true;
                    continue;
                }
                uint8_t* indexOut = static_cast<uint8_t*>(gpu.mIbo) + offset;
                switch (set->getType())
                {
                    case osg::PrimitiveSet::DrawArraysPrimitiveType:
                    {
                        auto* arrays = static_cast<const osg::DrawArrays*>(set.get());
                        const uint32_t count = saneCount(set->getMode(), arrays->getCount());
                        if (count > 0 && arrays->getFirst() >= 0
                            && static_cast<uint32_t>(arrays->getFirst()) + count <= gpu.mVertexCount)
                            gpu.mPrimitives.push_back({ mode, count, static_cast<uint32_t>(arrays->getFirst()), -1,
                                triangleCount(set->getMode(), count) });
                        break;
                    }
                    case osg::PrimitiveSet::DrawArrayLengthsPrimitiveType:
                    {
                        auto* lengths = static_cast<const osg::DrawArrayLengths*>(set.get());
                        uint32_t first = lengths->getFirst();
                        for (GLint length : *lengths)
                        {
                            const uint32_t count = saneCount(set->getMode(), length);
                            if (count > 0 && first + count <= gpu.mVertexCount)
                                gpu.mPrimitives.push_back(
                                    { mode, count, first, -1, triangleCount(set->getMode(), count) });
                            first += length;
                        }
                        break;
                    }
                    case osg::PrimitiveSet::DrawElementsUBytePrimitiveType:
                    {
                        auto* elements = static_cast<const osg::DrawElementsUByte*>(set.get());
                        const uint32_t count = saneCount(set->getMode(), elements->size());
                        if (count == 0 || !indicesInRange(&elements->front(), count, gpu.mVertexCount))
                            break;
                        std::memcpy(indexOut, elements->getDataPointer(), count);
                        gpu.mPrimitives.push_back(
                            { mode, count, offset, C3D_UNSIGNED_BYTE, triangleCount(set->getMode(), count) });
                        offset += (count + 3) & ~3u;
                        break;
                    }
                    case osg::PrimitiveSet::DrawElementsUShortPrimitiveType:
                    {
                        auto* elements = static_cast<const osg::DrawElementsUShort*>(set.get());
                        const uint32_t count = saneCount(set->getMode(), elements->size());
                        if (count == 0 || !indicesInRange(&elements->front(), count, gpu.mVertexCount))
                            break;
                        std::memcpy(indexOut, elements->getDataPointer(), count * 2);
                        gpu.mPrimitives.push_back(
                            { mode, count, offset, C3D_UNSIGNED_SHORT, triangleCount(set->getMode(), count) });
                        offset += (count * 2 + 3) & ~3u;
                        break;
                    }
                    case osg::PrimitiveSet::DrawElementsUIntPrimitiveType:
                    {
                        auto* elements = static_cast<const osg::DrawElementsUInt*>(set.get());
                        const uint32_t count = saneCount(set->getMode(), elements->size());
                        if (count == 0 || !indicesInRange(&elements->front(), count, gpu.mVertexCount))
                            break;
                        auto* out16 = reinterpret_cast<uint16_t*>(indexOut);
                        for (size_t i = 0; i < count; ++i)
                            out16[i] = static_cast<uint16_t>((*elements)[i]);
                        gpu.mPrimitives.push_back(
                            { mode, count, offset, C3D_UNSIGNED_SHORT, triangleCount(set->getMode(), count) });
                        offset += (count * 2 + 3) & ~3u;
                        break;
                    }
                    default:
                        break;
                }
            }
            if (offset > 0)
                flushToGpu(gpu.mIbo, offset);
            gpu.mPrimitiveSignature = primitiveSignature(geometry);
            return true;
        }

        GpuGeometry* geometryFor(const osg::Geometry& geometry)
        {
            GpuGeometry*& gpu = gGeometries[&geometry];
            if (gpu == nullptr)
            {
                gpu = new GpuGeometry;
                gpu->mOwner = &geometry;
                geometry.addObserver(gpu);
            }
            else if (gpu->mValid)
            {
                // Skinned/morphed geometry rewrites its arrays: re-upload what changed.
                bool verticesChanged = geometry.getVertexArray() != gpu->mArrays[Attr_Position]
                    || geometry.getNormalArray() != gpu->mArrays[Attr_Normal]
                    || geometry.getColorArray() != gpu->mArrays[Attr_Color]
                    || geometry.getTexCoordArray(0) != gpu->mArrays[Attr_TexCoord];
                for (int a = 0; a < Attr_Count && !verticesChanged; ++a)
                    verticesChanged = gpu->mArrays[a] && gpu->mArrays[a]->getModifiedCount() != gpu->mModified[a];
                const uint32_t oldVertexCount = gpu->mVertexCount;
                if (verticesChanged)
                    gpu->mValid = buildVertices(*gpu, geometry);
                if (gpu->mValid
                    && (primitiveSignature(geometry) != gpu->mPrimitiveSignature || gpu->mVertexCount != oldVertexCount))
                    gpu->mValid = buildPrimitives(*gpu, geometry);
                return gpu->mValid ? gpu : nullptr;
            }
            gpu->mValid = buildVertices(*gpu, geometry) && buildPrimitives(*gpu, geometry);
            return gpu->mValid ? gpu : nullptr;
        }

        // ------------------------------------------------------------------ GPU skinning (M10)

        // A skinned mesh's batches in linear memory, one after the other: built once, shared by every drawable
        // wearing the mesh (render3ds.hpp SkinMesh), freed with it.
        class GpuSkin : public GpuObject
        {
        public:
            struct Batch
            {
                u32 mVboOffset; // physical address - vertex buffer base
                const uint16_t* mIndices;
                uint32_t mCount;
            };
            void* mVbo = nullptr;
            uint16_t* mIbo = nullptr;
            std::vector<Batch> mBatches;
            bool mValid = false;

            void release() override
            {
                gpuFree(mVbo);
                gpuFree(mIbo);
                mVbo = nullptr;
                mIbo = nullptr;
            }
        };

        std::unordered_map<const SkinMesh*, GpuSkin*> gSkins;

        bool forgetSkin(GpuObject* object)
        {
            auto* skin = dynamic_cast<GpuSkin*>(object);
            if (skin == nullptr)
                return false;
            auto it = gSkins.find(static_cast<const SkinMesh*>(object->mOwner));
            if (it != gSkins.end() && it->second == skin)
                gSkins.erase(it);
            return true;
        }

        bool buildSkin(GpuSkin& gpu, const SkinMesh& mesh)
        {
            VertexSource source;
            if (mesh.mSource == nullptr || !source.init(*mesh.mSource))
                return false;
            const unsigned sourceCount = source.mPositions->size();
            size_t vertices = 0, indices = 0;
            for (const SkinBatch& batch : mesh.mBatches)
            {
                vertices += batch.mVertices.size();
                indices += batch.mIndices.size();
            }
            if (vertices == 0 || indices == 0)
                return false;
            gpu.mVbo = gpuAlloc(vertices * sizeof(SkinVertex));
            gpu.mIbo = static_cast<uint16_t*>(gpuAlloc(indices * sizeof(uint16_t)));
            if (gpu.mVbo == nullptr || gpu.mIbo == nullptr)
            {
                Log(Debug::Error) << "render3ds: out of linear memory for a skinned mesh (" << vertices
                                  << " vertices); drawn on the CPU";
                return false;
            }
            auto* out = static_cast<SkinVertex*>(gpu.mVbo);
            uint16_t* index = gpu.mIbo;
            for (const SkinBatch& batch : mesh.mBatches)
            {
                gpu.mBatches.push_back({ static_cast<u32>(osConvertVirtToPhys(out) - gBufferBase), index,
                    static_cast<uint32_t>(batch.mIndices.size()) });
                for (size_t v = 0; v < batch.mVertices.size(); ++v, ++out)
                {
                    const unsigned i = batch.mVertices[v];
                    if (i >= sourceCount)
                        return false;
                    source.fill(*out, i);
                    std::memcpy(out->mSlots, &batch.mSlots[v * 4], 4);
                    std::memcpy(out->mWeights, &batch.mWeights[v * 4], 4);
                }
                std::memcpy(index, batch.mIndices.data(), batch.mIndices.size() * sizeof(uint16_t));
                index += batch.mIndices.size();
            }
            flushToGpu(gpu.mVbo, vertices * sizeof(SkinVertex));
            flushToGpu(gpu.mIbo, indices * sizeof(uint16_t));
            gStats.mUploadedBytes += vertices * sizeof(SkinVertex) + indices * sizeof(uint16_t);
            return true;
        }

        // nullptr: the mesh can't be drawn skinned on the GPU (no memory, unusable arrays).
        GpuSkin* skinFor(const SkinMesh& mesh)
        {
            GpuSkin*& gpu = gSkins[&mesh];
            if (gpu == nullptr)
            {
                gpu = new GpuSkin;
                gpu->mOwner = &mesh;
                mesh.addObserver(gpu);
                gpu->mValid = buildSkin(*gpu, mesh);
                if (!gpu->mValid)
                    gpu->release();
            }
            return gpu->mValid ? gpu : nullptr;
        }

        // ------------------------------------------------------------------ textures

        // Morton (Z-order) offset of (x, y) inside an 8x8 tile.
        inline u32 morton8(u32 x, u32 y)
        {
            static const auto table = [] {
                std::array<uint8_t, 64> t{};
                for (u32 ty = 0; ty < 8; ++ty)
                    for (u32 tx = 0; tx < 8; ++tx)
                    {
                        u32 m = 0;
                        for (u32 i = 0; i < 3; ++i)
                            m |= ((tx >> i) & 1) << (2 * i) | ((ty >> i) & 1) << (2 * i + 1);
                        t[ty * 8 + tx] = uint8_t(m);
                    }
                return t;
            }();
            return table[(y & 7) * 8 + (x & 7)];
        }

        u32 nextPow2(u32 v, u32 minimum, u32 maximum)
        {
            u32 p = minimum;
            while (p < v && p < maximum)
                p *= 2;
            return p;
        }

        // Uncompressed images made at runtime (fonts, generated textures, the warning image): converted to
        // a tiled PICA format (nearest-neighbour resampled to a power of two). One mip level.
        uint32_t gTextureGeneration = 0; // bumped when a runtime texture is (re)converted: the CPU interface redraws
        // M10: which texture changed, and when (a value of gTextureGeneration). The CPU interface used to redraw both
        // screens whole and decode every texture again whenever any runtime texture changed (walking outdoors:
        // the minimap's tiles, 35-86 ms a redraw in Zeste, 20/10). Now only the triangles sampling a changed
        // texture are redrawn, and only that texture is decoded again.
        std::unordered_map<const C3D_Tex*, uint32_t> gTexGenerations;

        void bumpTexture(const C3D_Tex* tex)
        {
            gTexGenerations[tex] = ++gTextureGeneration;
        }

        uint32_t texGeneration(const C3D_Tex* tex)
        {
            const auto it = gTexGenerations.find(tex);
            return it != gTexGenerations.end() ? it->second : 0;
        }

        bool convertImage(GpuTexture& gpu, const osg::Image& image)
        {
            const GLenum format = image.getPixelFormat();
            if (image.getDataType() != GL_UNSIGNED_BYTE || image.data() == nullptr || image.isCompressed())
                return false;
            GPU_TEXCOLOR picaFormat;
            unsigned bytesPerTexel;
            switch (format)
            {
                case GL_RGBA:
                    picaFormat = GPU_RGBA8;
                    bytesPerTexel = 4;
                    break;
                case GL_RGB:
                    picaFormat = GPU_RGB8;
                    bytesPerTexel = 3;
                    break;
                case GL_LUMINANCE_ALPHA:
                    picaFormat = GPU_LA8;
                    bytesPerTexel = 2;
                    break;
                case GL_LUMINANCE:
                    picaFormat = GPU_L8;
                    bytesPerTexel = 1;
                    break;
                case GL_ALPHA:
                    picaFormat = GPU_A8;
                    bytesPerTexel = 1;
                    break;
                default:
                    return false;
            }
            const unsigned srcW = image.s(), srcH = image.t();
            const unsigned srcComponents = osg::Image::computeNumComponents(format);
            const u32 w = nextPow2(srcW, 8, 1024), h = nextPow2(srcH, 8, 1024);
            const u32 size = w * h * bytesPerTexel;

            if (gpu.mOwnedData == nullptr || gpu.mTex.size != size)
            {
                gpuFree(gpu.mOwnedData);
                gpu.mOwnedData = gpuAlloc(size);
                if (gpu.mOwnedData == nullptr)
                    return false;
            }
            uint8_t* out = static_cast<uint8_t*>(gpu.mOwnedData);
            for (u32 y = 0; y < h; ++y)
            {
                // Image row 0 is t = 0, which is also the first row of PICA texture memory.
                const unsigned sy = y * srcH / h;
                const unsigned char* row = image.data(0, sy);
                for (u32 x = 0; x < w; ++x)
                {
                    const unsigned char* src = row + (x * srcW / w) * srcComponents;
                    uint8_t* dst = out + (((y / 8) * (w / 8) + x / 8) * 64 + morton8(x % 8, y % 8)) * bytesPerTexel;
                    // PICA texels are little-endian words: RGBA8 = A,B,G,R bytes. RGB8 = B,G,R. LA8 = A,L.
                    switch (picaFormat)
                    {
                        case GPU_RGBA8:
                            dst[0] = src[3];
                            dst[1] = src[2];
                            dst[2] = src[1];
                            dst[3] = src[0];
                            break;
                        case GPU_RGB8:
                            dst[0] = src[2];
                            dst[1] = src[1];
                            dst[2] = src[0];
                            break;
                        case GPU_LA8:
                            dst[0] = src[1];
                            dst[1] = src[0];
                            break;
                        default:
                            dst[0] = src[0];
                            break;
                    }
                }
            }
            flushToGpu(gpu.mOwnedData, size);

            gpu.mTex = C3D_Tex{};
            gpu.mTex.data = gpu.mOwnedData;
            gpu.mTex.fmt = picaFormat;
            gpu.mTex.size = size;
            gpu.mTex.width = static_cast<u16>(w);
            gpu.mTex.height = static_cast<u16>(h);
            gpu.mTex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
            gpu.mLevels = 1;
            bumpTexture(&gpu.mTex);
            return true;
        }

        void setupCooked(GpuTexture& gpu, const CookedImage& image)
        {
            gpu.mTex = C3D_Tex{};
            gpu.mTex.data = image.gpuData();
            gpu.mTex.fmt = image.gpuFormat();
            gpu.mTex.size = image.getImageSizeInBytes();
            gpu.mTex.width = static_cast<u16>(image.s());
            gpu.mTex.height = static_cast<u16>(image.t());
            gpu.mTex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
            if (image.gpuFormat() == GPU_ETC1)
                gpu.mTex.param |= GPU_TEXTURE_ETC1_PARAM;
            gpu.mLevels = image.levels();
        }

        GPU_TEXTURE_WRAP_PARAM toPicaWrap(osg::Texture::WrapMode mode)
        {
            switch (mode)
            {
                case osg::Texture::REPEAT:
                    return GPU_REPEAT;
                case osg::Texture::MIRROR:
                    return GPU_MIRRORED_REPEAT;
                default:
                    return GPU_CLAMP_TO_EDGE;
            }
        }

        void applySampler(GpuTexture& gpu, const osg::Texture& texture)
        {
            const osg::Texture::FilterMode minFilter = texture.getFilter(osg::Texture::MIN_FILTER);
            const osg::Texture::FilterMode magFilter = texture.getFilter(osg::Texture::MAG_FILTER);
            const bool minLinear = minFilter == osg::Texture::LINEAR || minFilter == osg::Texture::LINEAR_MIPMAP_LINEAR
                || minFilter == osg::Texture::LINEAR_MIPMAP_NEAREST;
            const bool mipmapped = minFilter != osg::Texture::LINEAR && minFilter != osg::Texture::NEAREST;
            C3D_TexSetFilter(&gpu.mTex, magFilter == osg::Texture::NEAREST ? GPU_NEAREST : GPU_LINEAR,
                minLinear ? GPU_LINEAR : GPU_NEAREST);
            const bool mipLinear
                = minFilter == osg::Texture::LINEAR_MIPMAP_LINEAR || minFilter == osg::Texture::NEAREST_MIPMAP_LINEAR;
            C3D_TexSetFilterMipmap(&gpu.mTex, mipLinear ? GPU_LINEAR : GPU_NEAREST);
            C3D_TexSetWrap(&gpu.mTex, toPicaWrap(texture.getWrap(osg::Texture::WRAP_S)),
                toPicaWrap(texture.getWrap(osg::Texture::WRAP_T)));
            gpu.mTex.maxLevel = static_cast<u8>(mipmapped ? gpu.mLevels - 1 : 0);
            gpu.mTex.minLevel = 0;
        }

        C3D_Tex* textureFor(const osg::Texture& texture)
        {
            if (!gRttTargets.empty())
            {
                const auto rtt = gRttTargets.find(&texture);
                if (rtt != gRttTargets.end()) // nullptr without a target or before its first image: left out
                    return rtt->second->mReady ? rtt->second->mTex : nullptr;
            }
            const osg::Image* image = texture.getNumImages() > 0 ? texture.getImage(0) : nullptr;
            if (&texture != gLastTextureKey || gLastTextureSlot == nullptr)
            {
                gLastTextureSlot = &gTextures[&texture];
                gLastTextureKey = &texture;
            }
            GpuTexture*& gpu = *gLastTextureSlot;
            if (gpu == nullptr)
            {
                gpu = new GpuTexture;
                gpu->mOwner = &texture;
                texture.addObserver(gpu);
            }
            else if (gpu->mImage == image && (image == nullptr || gpu->mImageModified == image->getModifiedCount()))
            {
                if (gpu->mValid)
                    applySampler(*gpu, texture); // cheap. Filters/wrap can change at runtime
                return gpu->mValid ? &gpu->mTex : nullptr;
            }

            gpu->mImage = image;
            gpu->mImageModified = image ? image->getModifiedCount() : 0;
            gpu->mValid = false;
            if (image != nullptr)
            {
                if (auto* cooked = dynamic_cast<const CookedImage*>(image))
                {
                    setupCooked(*gpu, *cooked);
                    gpu->mValid = true;
                }
                else
                    gpu->mValid = convertImage(*gpu, *image);
            }
            if (gpu->mValid)
                applySampler(*gpu, texture);
            return gpu->mValid ? &gpu->mTex : nullptr;
        }

        // ------------------------------------------------------------------ draw state

        GPU_BLENDFACTOR toPicaBlend(GLenum factor)
        {
            switch (factor)
            {
                case GL_ZERO:
                    return GPU_ZERO;
                case GL_ONE:
                    return GPU_ONE;
                case GL_SRC_COLOR:
                    return GPU_SRC_COLOR;
                case GL_ONE_MINUS_SRC_COLOR:
                    return GPU_ONE_MINUS_SRC_COLOR;
                case GL_DST_COLOR:
                    return GPU_DST_COLOR;
                case GL_ONE_MINUS_DST_COLOR:
                    return GPU_ONE_MINUS_DST_COLOR;
                case GL_SRC_ALPHA:
                    return GPU_SRC_ALPHA;
                case GL_ONE_MINUS_SRC_ALPHA:
                    return GPU_ONE_MINUS_SRC_ALPHA;
                case GL_DST_ALPHA:
                    return GPU_DST_ALPHA;
                case GL_ONE_MINUS_DST_ALPHA:
                    return GPU_ONE_MINUS_DST_ALPHA;
                case GL_CONSTANT_COLOR:
                    return GPU_CONSTANT_COLOR;
                case GL_ONE_MINUS_CONSTANT_COLOR:
                    return GPU_ONE_MINUS_CONSTANT_COLOR;
                case GL_CONSTANT_ALPHA:
                    return GPU_CONSTANT_ALPHA;
                case GL_ONE_MINUS_CONSTANT_ALPHA:
                    return GPU_ONE_MINUS_CONSTANT_ALPHA;
                case GL_SRC_ALPHA_SATURATE:
                    return GPU_SRC_ALPHA_SATURATE;
                default:
                    return GPU_ONE;
            }
        }

        GPU_TESTFUNC toPicaTest(GLenum function)
        {
            switch (function)
            {
                case GL_NEVER:
                    return GPU_NEVER;
                case GL_LESS:
                    return GPU_LESS;
                case GL_EQUAL:
                    return GPU_EQUAL;
                case GL_LEQUAL:
                    return GPU_LEQUAL;
                case GL_GREATER:
                    return GPU_GREATER;
                case GL_NOTEQUAL:
                    return GPU_NOTEQUAL;
                case GL_GEQUAL:
                    return GPU_GEQUAL;
                default:
                    return GPU_ALWAYS;
            }
        }

        // Fixed-function state as a key, so unchanged state costs one comparison per draw.
        struct FixedState
        {
            uint8_t mBlend = 0;
            uint8_t mBlendSrc = 0, mBlendDst = 0, mBlendSrcAlpha = 0, mBlendDstAlpha = 0;
            uint8_t mAlphaTest = 0, mAlphaFunc = 0, mAlphaRef = 0;
            uint8_t mDepthTest = 0, mDepthFunc = 0, mWriteMask = 0;
            uint8_t mCull = 0;

            bool operator==(const FixedState& other) const { return std::memcmp(this, &other, sizeof(*this)) == 0; }
        };

        FixedState computeFixedState(const ResolvedState& rs)
        {
            FixedState fs;
            if (rs.mBlend.mOn && rs.mBlendFunc.mValue)
            {
                const osg::BlendFunc& bf = *rs.mBlendFunc.mValue;
                fs.mBlend = 1;
                fs.mBlendSrc = toPicaBlend(bf.getSource());
                fs.mBlendDst = toPicaBlend(bf.getDestination());
                fs.mBlendSrcAlpha = toPicaBlend(bf.getSourceAlpha());
                fs.mBlendDstAlpha = toPicaBlend(bf.getDestinationAlpha());
                // Terrain layers (a blend map on unit 1): OpenMW writes the first layer times its weight (src alpha,
                // zero) and adds the others (src alpha, one). The weights sum to 1 only if every pixel is drawn
                // once per layer. The PICA draws some pixels on chunk borders twice, which added the weights twice:
                // white specks where the fog is thick (hardware test #17). Here the first layer is opaque and the
                // others are blended over it, which a second draw of a pixel can't brighten.
                if (blendMapOf(rs) != nullptr && bf.getSource() == osg::BlendFunc::SRC_ALPHA)
                {
                    if (bf.getDestination() == osg::BlendFunc::ZERO)
                        fs.mBlend = 0;
                    else if (bf.getDestination() == osg::BlendFunc::ONE)
                        fs.mBlendDst = fs.mBlendDstAlpha = GPU_ONE_MINUS_SRC_ALPHA;
                }
            }
            if (rs.mAlphaFunc.mValue && rs.mAlphaFunc.mValue->getFunction() != osg::AlphaFunc::ALWAYS)
            {
                // OpenMW alpha-tests in its shaders (the GL alpha test mode is always off): the function is set.
                fs.mAlphaTest = 1;
                fs.mAlphaFunc = toPicaTest(rs.mAlphaFunc.mValue->getFunction());
                fs.mAlphaRef = static_cast<uint8_t>(
                    std::clamp(rs.mAlphaFunc.mValue->getReferenceValue(), 0.f, 1.f) * 255.f + 0.5f);
            }
            uint8_t colorWrite = GPU_WRITE_COLOR;
            if (const osg::ColorMask* mask = rs.mColorMask.mValue)
                colorWrite = (mask->getRedMask() ? GPU_WRITE_RED : 0) | (mask->getGreenMask() ? GPU_WRITE_GREEN : 0)
                    | (mask->getBlueMask() ? GPU_WRITE_BLUE : 0) | (mask->getAlphaMask() ? GPU_WRITE_ALPHA : 0);
            fs.mDepthTest = rs.mDepthTest.mOn;
            const osg::Depth* depth = rs.mDepth.mValue;
            fs.mDepthFunc = toPicaTest(depth ? depth->getFunction() : GL_LESS);
            const bool depthWrite = fs.mDepthTest && (depth == nullptr || depth->getWriteMask());
            fs.mWriteMask = colorWrite | (depthWrite ? GPU_WRITE_DEPTH : 0);
            if (rs.mCullFaceMode.mOn)
            {
                const osg::CullFace::Mode mode = rs.mCullFace.mValue ? rs.mCullFace.mValue->getMode() : osg::CullFace::BACK;
                if (mode == osg::CullFace::BACK)
                    fs.mCull = GPU_CULL_BACK_CCW;
                else if (mode == osg::CullFace::FRONT)
                    fs.mCull = GPU_CULL_FRONT_CCW;
                else
                    fs.mCull = 3; // front and back: nothing visible
            }
            else
                fs.mCull = GPU_CULL_NONE;
            return fs;
        }

        void applyFixedState(const FixedState& fs)
        {
            if (fs.mBlend)
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, static_cast<GPU_BLENDFACTOR>(fs.mBlendSrc),
                    static_cast<GPU_BLENDFACTOR>(fs.mBlendDst), static_cast<GPU_BLENDFACTOR>(fs.mBlendSrcAlpha),
                    static_cast<GPU_BLENDFACTOR>(fs.mBlendDstAlpha));
            else
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            C3D_AlphaTest(fs.mAlphaTest, static_cast<GPU_TESTFUNC>(fs.mAlphaFunc), fs.mAlphaRef);
            C3D_DepthTest(fs.mDepthTest, static_cast<GPU_TESTFUNC>(fs.mDepthFunc),
                static_cast<GPU_WRITEMASK>(fs.mWriteMask));
            C3D_CullFace(fs.mCull == 3 ? GPU_CULL_NONE : static_cast<GPU_CULLMODE>(fs.mCull));
        }

        // What is currently set on the GPU, to skip redundant uniform/state uploads.
        // The scene shader's material uniforms (matDiffuse, matAmbient, matEmission, colorSel, lighting).
        struct MaterialValues
        {
            osg::Vec4f mDiffuse, mAmbient, mEmission, mColorSel, mLighting;
            bool operator==(const MaterialValues&) const = default;
        };

        struct Bound
        {
            const shaderProgram_s* mProgram = nullptr;
            const osg::RefMatrix* mProjection = nullptr;
            const osg::RefMatrix* mModelView = nullptr;
            const osg::Uniform* mLightBuffer = nullptr;
            unsigned mLightBufferModified = 0;
            int mLightCount = -1;
            const osg::Uniform* mSun[3] = {};
            unsigned mSunModified[3] = {};
            const SceneUtil::Material* mMaterial = nullptr;
            float mAlpha = -1;
            MaterialValues mMaterialValues;
            bool mMaterialValid = false;
            const osg::TexMat* mTexMat = nullptr;
            const osg::Uniform* mTexMatU[2] = {};
            unsigned mTexMatUModified[2] = {};
            bool mTexMatSet = false;
            C3D_Tex* mTexture = nullptr;
            C3D_Tex* mTexture1 = nullptr;
            int mTexEnv = -1; // TexEnvMode
            FixedState mFixed;
            bool mFixedValid = false;
            // Perf batch 4: the previous draw's state graph and projection, and its sky pass (drawLeaf).
            const osgUtil::StateGraph* mStateGraph = nullptr;
            const osg::RefMatrix* mStateProjection = nullptr;
            int mSky = 0;
        } gBound;

        enum TexEnvMode
        {
            TexEnv_VertexColor = 0, // lit vertex color
            TexEnv_Textured = 1, // diffuse map x lit vertex color
            TexEnv_BlendMapped = 2, // terrain layer: diffuse map x lit color. Alpha = blend map (unit 1) x lit alpha
        };

        void setTexEnv(int mode)
        {
            if (gBound.mTexEnv == mode)
                return;
            gBound.mTexEnv = mode;
            C3D_TexEnv* env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            switch (mode)
            {
                case TexEnv_Textured:
                    C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
                    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
                    break;
                case TexEnv_BlendMapped:
                    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
                    C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
                    C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
                    break;
                default:
                    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
                    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
                    break;
            }
        }

        // Vertex = position, normal, color (4 unsigned bytes), texcoord, bone slots, bone weights (unsigned bytes):
        // input registers v0..v5, one buffer, for every draw of the frame (the skinned shader reads v4/v5).
        void setupAttributes()
        {
            C3D_AttrInfo* attr = C3D_GetAttrInfo();
            AttrInfo_Init(attr);
            AttrInfo_AddLoader(attr, Attr_Position, GPU_FLOAT, 3);
            AttrInfo_AddLoader(attr, Attr_Normal, GPU_FLOAT, 3);
            AttrInfo_AddLoader(attr, Attr_Color, GPU_UNSIGNED_BYTE, 4);
            AttrInfo_AddLoader(attr, Attr_TexCoord, GPU_FLOAT, 2);
            AttrInfo_AddLoader(attr, 4, GPU_UNSIGNED_BYTE, 4);
            AttrInfo_AddLoader(attr, 5, GPU_UNSIGNED_BYTE, 4);
        }

        void bindVertexBuffer(const void* vbo)
        {
            C3D_BufInfo* buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            buf->base_paddr = gBufferBase;
            BufInfo_Add(buf, vbo, sizeof(Vertex), 6, 0x543210);
        }

        // Scene draws: citro3d re-sends all 12 attribute buffer configurations (~37 command words) whenever the
        // buffer info changes, i.e. for every draw with its own vertex buffer. The configuration is set once per
        // frame (sceneBufferInfo) and each draw only rewrites buffer 0's offset register. citro3d's copy of the
        // offset is kept in step, so a re-send for any reason (it marks the info dirty itself) is still right.
        C3D_BufCfg* gSceneBuffer = nullptr;
        u32 gSceneBufferOffset = ~0u;
        void sceneBufferInfo(const void* anyVbo)
        {
            bindVertexBuffer(anyVbo);
            gSceneBuffer = &C3D_GetBufInfo()->buffers[0];
            gSceneBufferOffset = gSceneBuffer->offset;
        }

        void setSceneBufferOffset(u32 offset)
        {
            if (offset == gSceneBufferOffset)
                return;
            gSceneBufferOffset = offset;
            gSceneBuffer->offset = offset;
            GPUCMD_AddWrite(GPUREG_ATTRIBBUFFER0_OFFSET, offset);
        }

        float uniformFloat(const Slot<osg::Uniform>& slot, float fallback)
        {
            float value = fallback;
            if (slot.mValue)
                slot.mValue->get(value);
            return value;
        }

        osg::Vec4f uniformVec4(const Slot<osg::Uniform>& slot, const osg::Vec4f& fallback)
        {
            osg::Vec4f value = fallback;
            if (slot.mValue)
                slot.mValue->get(value);
            return value;
        }

        // OpenMW's sky shader passes (files/shaders/lib/sky/passes.glsl).
        enum SkyPass
        {
            Sky_None = -1,
            Sky_Atmosphere = 0,
            Sky_AtmosphereNight = 1,
            Sky_Clouds = 2,
            Sky_Moon = 3,
            Sky_Sun = 4,
            Sky_SunflashQuery = 5,
            Sky_Sunglare = 6,
        };

        int skyPass(const ResolvedState& rs)
        {
            int pass = Sky_None;
            if (rs.mSkyPass.mValue)
                rs.mSkyPass.mValue->get(pass);
            return pass;
        }

        void setVec4(int location, const osg::Vec4f& v)
        {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, location, v.x(), v.y(), v.z(), v.w());
        }

        // The shader's lights are unrolled (scene.v.pica): every slot is evaluated and the sum doesn't depend on
        // the order, so lights stay in their slot from one draw to the next and only differences are uploaded.
        // Draws with one light use the variant that only evaluates slot 0, draws without any the one that
        // evaluates none (nothing to upload: the slots keep what they hold).
        // Objects near each other share most lights (in different orders, OpenMW sorts per object), and a full
        // 8-light upload per draw was ~20% of the GPU command buffer on hardware (test #13). A light is recognized
        // by its data, which is identical within a frame for every list containing it (same light, same view).
        struct LightSlot
        {
            float mData[16];
            bool mLit = false;
            bool mKnown = false; // false: whatever is in the GPU's registers is unknown (frame start, self-test)
        };
        LightSlot gLightSlots[kMaxLights];

        void forgetLightSlots()
        {
            for (LightSlot& slot : gLightSlots)
                slot.mKnown = false;
        }

        // data: OpenMW's LightBuffer floats, 16 per light (lightmanager.cpp configure*(): row 0 position +
        // constant, row 1 ambient + linear, row 2 diffuse + quadratic, row 3 specular + radius).
        void writeSlot(int slot, const float* m)
        {
            ++gStats.mLightSlotWrites;
            C3D_FVec* out = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mLights + slot * 4, 4);
            if (m)
            {
                const float radius = std::max(m[15], 1.f);
                out[0] = FVec4_New(m[0], m[1], m[2], 4.f / radius);
                out[1] = FVec4_New(m[8], m[9], m[10], 0.f);
                out[2] = FVec4_New(m[4], m[5], m[6], 0.f);
                out[3] = FVec4_New(m[3], m[7], m[11], 0.f);
            }
            else
            {
                // Off: black and far away (no 1/0 in the shader).
                out[0] = FVec4_New(0.f, 0.f, 10000.f, 0.f);
                out[1] = FVec4_New(0.f, 0.f, 0.f, 0.f);
                out[2] = FVec4_New(0.f, 0.f, 0.f, 0.f);
                out[3] = FVec4_New(1.f, 0.f, 0.f, 0.f);
            }
        }

        // The one light of a draw drawn with gProgram1 goes to slot 0. false: slot 0 already held it.
        bool writeSlot0(const float* m)
        {
            LightSlot& slot = gLightSlots[0];
            if (slot.mKnown && slot.mLit && std::memcmp(slot.mData, m, sizeof(LightSlot::mData)) == 0)
                return false;
            writeSlot(0, m);
            std::memcpy(slot.mData, m, sizeof(LightSlot::mData));
            slot.mLit = true;
            slot.mKnown = true;
            return true;
        }

        // evaluated: the slots the bound program reads (4 for gProgram, 2 for gProgram2). Test #107: the cache placed
        // lights in any of the 8 slots, as the 8-light program of before perf batch 2b read them all. A light kept in
        // slot 4-7 from an earlier draw was then ignored, and which one depended on the draw order, so on the
        // camera: objects' lighting changed as the view turned. Lights now go to the slots read, the rest of those
        // off. The slots above keep what they hold (not read).
        void writeLights(const float* data, int count, int evaluated)
        {
            bool used[kMaxLights] = {};
            int pending[kMaxLights];
            int pendingCount = 0;
            // Lights already in a slot read stay there.
            for (int i = 0; i < count; ++i)
            {
                const float* m = data + i * 16;
                int found = -1;
                for (int slot = 0; slot < evaluated && found < 0; ++slot)
                    if (!used[slot] && gLightSlots[slot].mKnown && gLightSlots[slot].mLit
                        && std::memcmp(gLightSlots[slot].mData, m, sizeof(LightSlot::mData)) == 0)
                        found = slot;
                if (found >= 0)
                    used[found] = true;
                else
                    pending[pendingCount++] = i;
            }
            // New lights go to free slots, preferably ones that are off already.
            for (int k = 0; k < pendingCount; ++k)
            {
                int target = -1;
                for (int slot = 0; slot < evaluated && target < 0; ++slot)
                    if (!used[slot] && gLightSlots[slot].mKnown && !gLightSlots[slot].mLit)
                        target = slot;
                for (int slot = 0; slot < evaluated && target < 0; ++slot)
                    if (!used[slot])
                        target = slot;
                const float* m = data + pending[k] * 16;
                writeSlot(target, m);
                used[target] = true;
                std::memcpy(gLightSlots[target].mData, m, sizeof(LightSlot::mData));
                gLightSlots[target].mLit = true;
                gLightSlots[target].mKnown = true;
            }
            // The other slots read off.
            for (int slot = 0; slot < evaluated; ++slot)
                if (!used[slot] && (!gLightSlots[slot].mKnown || gLightSlots[slot].mLit))
                {
                    writeSlot(slot, nullptr);
                    gLightSlots[slot].mLit = false;
                    gLightSlots[slot].mKnown = true;
                }
        }

        int gDrawLights = 0; // point lights of the current draw (stats)

        // The fireball corruption (docs/history/FIREBALL_CORRUPTION.md): on hardware, from a big explosion on, the shader
        // outputs came out shifted by one (positions taken from texture coordinates, colours from positions) for
        // the rest of the run. Most likely trigger: the vertex shader reconfigured while the GPU is still busy with
        // earlier draws. citro3d's C3D_BindProgram re-sends a program's whole configuration (output map, output
        // counts, geometry stage) at the next draw. We switched programs ~45 times a frame (the light variants),
        // and to a shader with 3 outputs instead of 4 for the GUI at the end of every frame, each queued behind the
        // frame's earlier draws. Now every program a frame uses (the scene variants, the GUI's flat shader) has the
        // same outputs and constants: citro3d keeps gProgram bound (its configuration is sent again only after the
        // GPU self-test) and a frame switches programs with the entry point register alone.
        void useProgram(const shaderProgram_s* program)
        {
            if (program == gBound.mProgram)
                return;
            gBound.mProgram = program;
            ++gStats.mProgramSwitches;
            GPUCMD_AddWrite(GPUREG_VSH_ENTRYPOINT, 0x7FFF0000u | (program->vertexShader->dvle->mainOffset & 0xFFFF));
        }

        // pointLights false: the draw ignores point lights (sky passes, OpenMW's "simpleLighting" weather
        // particles: lighting_vertex_impl.glsl), whatever the light manager attached.
        // skinned: the skinned variant, which evaluates slot 0 only (the draw's first light, OpenMW's nearest and
        // brightest, the others are left out).
        void uploadLighting(const ResolvedState& rs, bool pointLights, bool skinned = false)
        {
            // Sun: shared by everything under the light manager. Its values change once per frame.
            const Slot<osg::Uniform>* sun[3] = { &rs.mSunPosition, &rs.mSunDiffuse, &rs.mSunAmbient };
            bool sunChanged = false;
            for (int i = 0; i < 3; ++i)
            {
                const osg::Uniform* u = sun[i]->mValue;
                const unsigned modified = u ? u->getModifiedCount() : 0;
                if (u != gBound.mSun[i] || modified != gBound.mSunModified[i])
                {
                    gBound.mSun[i] = u;
                    gBound.mSunModified[i] = modified;
                    sunChanged = true;
                }
            }
            if (sunChanged)
            {
                osg::Vec4f position(0, 0, 1, 0), diffuse(0, 0, 0, 0), ambient(0, 0, 0, 0);
                if (rs.mSunPosition.mValue)
                    rs.mSunPosition.mValue->get(position);
                if (rs.mSunDiffuse.mValue)
                    rs.mSunDiffuse.mValue->get(diffuse);
                if (rs.mSunAmbient.mValue)
                    rs.mSunAmbient.mValue->get(ambient);
                osg::Vec3f dir(position.x(), position.y(), position.z());
                if (dir.length2() > 0)
                    dir.normalize();
                setVec4(gLoc.mSunDir, osg::Vec4f(dir, 0));
                setVec4(gLoc.mSunDiffuse, diffuse);
                setVec4(gLoc.mSunAmbient, ambient);
            }

            // Point lights: one LightBuffer uniform per light list (OpenMW's per-object lighting).
            const osg::Uniform* buffer = rs.mLightBuffer.mValue;
            int count = 0;
            if (pointLights && rs.mPointLightCount.mValue)
                rs.mPointLightCount.mValue->get(count);
            // Perf batch 2: at most 4 lights a draw (OpenMW's light list is nearest first), and a variant that
            // evaluates just the ones there are: every draw with 2+ lights ran all 8 light blocks (test #100).
            count = std::clamp(count, 0, kDrawLights);
            if (buffer == nullptr || buffer->getFloatArray() == nullptr)
                count = 0;
            if (count > 0)
                count = std::min<int>(count, static_cast<int>(buffer->getFloatArray()->size() / 16));
            gDrawLights = count;

            const shaderProgram_s* program = skinned ? &gProgramSkin
                : !gVariants || count >= 3               ? &gProgram
                : count == 2                             ? (gVariant2 ? &gProgram2 : &gProgram)
                : count == 1                             ? &gProgram1
                                                         : &gProgram0;
            useProgram(program);
            if (program == &gProgram0)
                return;
            if (program == &gProgramSkin)
            {
                if (count > 0)
                {
                    if (writeSlot0(&(*buffer->getFloatArray())[0]))
                        gBound.mLightCount = -1;
                    gDrawLights = 1;
                }
                else if (!gLightSlots[0].mKnown || gLightSlots[0].mLit)
                {
                    writeSlot(0, nullptr);
                    gLightSlots[0].mLit = false;
                    gLightSlots[0].mKnown = true;
                    gBound.mLightCount = -1;
                }
                return;
            }
            if (program == &gProgram1)
            {
                if (writeSlot0(&(*buffer->getFloatArray())[0]))
                    gBound.mLightCount = -1; // slot 0 changed under the 8-light cache below
                return;
            }

            const unsigned modified = buffer ? buffer->getModifiedCount() : 0;
            if (buffer == gBound.mLightBuffer && modified == gBound.mLightBufferModified && count == gBound.mLightCount)
                return;
            gBound.mLightBuffer = buffer;
            gBound.mLightBufferModified = modified;
            gBound.mLightCount = count;
            writeLights(count > 0 ? &(*buffer->getFloatArray())[0] : nullptr, count,
                program == &gProgram2 ? 2 : kDrawLights);
        }

        // Sky passes as unlit draws (lighting = 0, color = E): the colors of files/shaders/compatibility/sky.frag.
        // The moon's shadow mask is left out. The clouds' fade to the fog color at the horizon is applied to the
        // vertex color (lighting.y), before the texture, not after.
        MaterialValues skyMaterial(const ResolvedState& rs, int pass)
        {
            const osg::Vec4f diffuseColor = uniformVec4(rs.mDiffuseColor, osg::Vec4f(1, 1, 1, 1));
            const float opacity = uniformFloat(rs.mOpacity, 1.f);
            MaterialValues v;
            v.mDiffuse = osg::Vec4f(1, 1, 1, 1);
            v.mEmission = osg::Vec4f(1, 1, 1, 1);
            // colorSel.x = 1: alpha from the vertex color (the dome's fade toward the horizon)
            switch (pass)
            {
                case Sky_Atmosphere:
                    v.mEmission = diffuseColor;
                    v.mColorSel = osg::Vec4f(1, 0, 0, diffuseColor.a());
                    break;
                case Sky_AtmosphereNight:
                    v.mColorSel = osg::Vec4f(1, 0, 0, opacity);
                    break;
                case Sky_Clouds:
                {
                    v.mEmission = diffuseColor;
                    v.mColorSel = osg::Vec4f(1, 0, 0, opacity);
                    osg::Vec4f fog(0, 0, 0, 1);
                    if (rs.mFogColor.mValue)
                        rs.mFogColor.mValue->get(fog);
                    v.mAmbient = fog; // where the clouds fade to near the horizon (white wisps without it)
                    v.mLighting = osg::Vec4f(0, 1, 0, 0);
                    break;
                }
                case Sky_Moon:
                    v.mEmission = uniformVec4(rs.mMoonBlend, osg::Vec4f(1, 1, 1, 1));
                    v.mColorSel = osg::Vec4f(0, 0, 0, uniformVec4(rs.mAtmosphereFade, osg::Vec4f(1, 1, 1, 1)).a());
                    break;
                case Sky_Sunglare:
                    v.mEmission = diffuseColor;
                    v.mColorSel = osg::Vec4f(0, 0, 0, diffuseColor.a());
                    break;
                default: // Sky_Sun
                    v.mColorSel = osg::Vec4f(0, 0, 0, opacity);
                    break;
            }
            return v;
        }

        void uploadMaterial(const ResolvedState& rs, int sky)
        {
            MaterialValues v;
            if (sky == Sky_None)
            {
                const float alpha = uniformFloat(rs.mAlpha, 1.f) * uniformFloat(rs.mActorFade, 1.f);
                const SceneUtil::Material* material = rs.mMaterial.mValue;
                if (gBound.mMaterialValid && material == gBound.mMaterial && alpha == gBound.mAlpha)
                    return;
                gBound.mMaterial = material;
                gBound.mAlpha = alpha;

                static const SceneUtil::Material sDefault;
                const SceneUtil::Material& m = material ? *material : sDefault;
                v.mDiffuse = m.getDiffuse();
                v.mAmbient = m.getAmbient();
                v.mEmission = m.getEmission() * m.getEmissiveMultiplier();
                v.mEmission.a() = m.getEmission().a();
                float d = 0, a = 0, e = 0;
                switch (m.getVertexColorMode())
                {
                    case SceneUtil::VertexColorModes::Emission:
                        e = 1;
                        break;
                    case SceneUtil::VertexColorModes::AmbientAndDiffuse:
                        d = a = 1;
                        break;
                    case SceneUtil::VertexColorModes::Ambient:
                        a = 1;
                        break;
                    case SceneUtil::VertexColorModes::Diffuse:
                        d = 1;
                        break;
                    default:
                        break;
                }
                v.mColorSel = osg::Vec4f(d, a, e, alpha);
                v.mLighting = osg::Vec4f(1, 0, 0, 0);
            }
            else
            {
                gBound.mMaterial = nullptr;
                gBound.mAlpha = -1; // the next lit draw recomputes
                v = skyMaterial(rs, sky);
            }
            if (gBound.mMaterialValid && v == gBound.mMaterialValues)
                return;
            setVec4(gLoc.mMatDiffuse, v.mDiffuse);
            setVec4(gLoc.mMatAmbient, v.mAmbient);
            setVec4(gLoc.mMatEmission, v.mEmission);
            setVec4(gLoc.mColorSel, v.mColorSel);
            setVec4(gLoc.mLighting, v.mLighting);
            gBound.mMaterialValues = v;
            gBound.mMaterialValid = true;
        }

        void writeTexMat(int location, const osg::Matrixf& m)
        {
            C3D_FVec rows[4];
            setRows(rows, m, 4);
            C3D_FVec* out = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, location, 2);
            out[0] = rows[0];
            out[1] = rows[1];
        }

        // Texture matrices: the diffuse map's (an osg::TexMat for UV animation, or the terrain's "texMat0" uniform
        // for layer tiling) and the terrain blend map's ("texMat1").
        void uploadTexMat(const ResolvedState& rs, int sky)
        {
            const osg::TexMat* texMat = rs.mTexMat.mValue;
            // The sky shader applies texMat0 to the clouds only.
            const bool useTexMat0 = sky == Sky_None || sky == Sky_Clouds;
            const osg::Uniform* u[2] = { useTexMat0 ? rs.mTexMat0.mValue : nullptr, rs.mTexMat1.mValue };
            unsigned modified[2];
            for (int i = 0; i < 2; ++i)
                modified[i] = u[i] ? u[i]->getModifiedCount() : 0;
            const bool all = !gBound.mTexMatSet;
            if (all || texMat != gBound.mTexMat || u[0] != gBound.mTexMatU[0]
                || modified[0] != gBound.mTexMatUModified[0])
            {
                osg::Matrixf m;
                if (u[0])
                    u[0]->get(m);
                else if (texMat)
                    m = texMat->getMatrix();
                writeTexMat(gLoc.mTexMat, m);
            }
            if (all || u[1] != gBound.mTexMatU[1] || modified[1] != gBound.mTexMatUModified[1])
            {
                osg::Matrixf m;
                if (u[1])
                    u[1]->get(m);
                writeTexMat(gLoc.mTexMat1, m);
            }
            gBound.mTexMatSet = true;
            gBound.mTexMat = texMat;
            for (int i = 0; i < 2; ++i)
            {
                gBound.mTexMatU[i] = u[i];
                gBound.mTexMatUModified[i] = modified[i];
            }
        }

        // OpenMW's linear depth fog (lib/fog.glsl: mix(color, fog.color, clamp((z - start) / (end - start)))) on the
        // PICA fog unit: a 128-entry LUT of visibility indexed by the depth value (distance / far, see setProjection).
        struct FogState
        {
            float mNear = -1, mFar = -1, mStart = -1, mEnd = -1;
            u32 mColor = 0xFFFFFFFF;
            bool mEnabled = false;
            bool mValid = false;
        } gFog;
        C3D_FogLut gFogLut;

        // OpenMW's ADDITIVE_BLENDING (shadervisitor.cpp): blending (source alpha, one) adds the object to what is
        // behind it, so fog must fade it to nothing (black), not to the fog color: the fog color added on top of a
        // fogged background saturates to white (hardware test #17: white lantern glows in the distance).
        bool isAdditive(const ResolvedState& rs)
        {
            const osg::BlendFunc* bf = rs.mBlendFunc.mValue;
            return rs.mBlend.mOn && bf != nullptr && bf->getSource() == osg::BlendFunc::SRC_ALPHA
                && bf->getDestination() == osg::BlendFunc::ONE
                && blendMapOf(rs) == nullptr; // terrain layers are blended over, not added (computeFixedState)
        }

        void uploadFog(const ResolvedState& rs, int sky)
        {
            const float start = uniformFloat(rs.mFogStart, 0.f);
            const float end = uniformFloat(rs.mFogEnd, 0.f);
            osg::Vec4f color(0, 0, 0, 1);
            if (rs.mFogColor.mValue && !isAdditive(rs))
                rs.mFogColor.mValue->get(color);
            auto channel = [](float v) { return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
            const u32 packed = channel(color.r()) | channel(color.g()) << 8 | channel(color.b()) << 16;
            const bool enabled = end > start && end > 0 && gPerspective && sky == Sky_None; // no fog on the sky

            if (gFog.mValid && gFog.mEnabled == enabled && gFog.mColor == packed && gFog.mStart == start
                && gFog.mEnd == end && gFog.mNear == gNear && gFog.mFar == gFar)
                return;
            const bool lutChanged = !gFog.mValid || gFog.mStart != start || gFog.mEnd != end || gFog.mNear != gNear
                || gFog.mFar != gFar;
            // Weather transitions move the fog a little every frame: log only real changes (an SD write each).
            static float loggedStart = -1, loggedEnd = -1;
            if (lutChanged
                && (std::abs(start - loggedStart) > 0.05f * end || std::abs(end - loggedEnd) > 0.05f * end))
            {
                loggedStart = start;
                loggedEnd = end;
                Log(Debug::Info) << "render3ds: fog " << start << ".." << end << " color " << std::hex << packed
                                 << std::dec << " near " << gNear << " far " << gFar;
            }
            gFog = FogState{ gNear, gFar, start, end, packed, enabled, true };
            if (!enabled)
            {
                C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
                return;
            }
            C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
            C3D_FogColor(packed);
            if (lutChanged)
            {
                float data[256];
                for (int i = 0; i <= 128; ++i)
                {
                    const float distance = gNear + i / 128.f * (gFar - gNear); // depth = (distance - near) / (far - near)
                    const float visibility = 1.f - std::clamp((distance - start) / (end - start), 0.f, 1.f);
                    if (i < 128)
                        data[i] = visibility;
                    if (i > 0)
                        data[i + 127] = visibility - data[i - 1];
                }
                FogLut_FromArray(&gFogLut, data);
            }
            C3D_FogLutBind(&gFogLut);
        }

        // What is not drawn, once per kind of drawable (class and name): the list of missing features.
        void logSkipped(const osg::Drawable* drawable, const char* why)
        {
            static std::set<std::string> logged;
            if (drawable == nullptr || logged.size() >= 64)
                return;
            std::string key = std::string(drawable->className()) + " '" + drawable->getName() + "'";
            if (!logged.insert(key).second)
                return;
            P3DS_LOGI("render3ds: not drawn: %s (%s)", key.c_str(), why);
        }

        // ------------------------------------------------------------------ GPU hang diagnostics

        u32 physical(const void* ptr)
        {
            return ptr ? osConvertVirtToPhys(ptr) : 0;
        }

        void logDraw(bool error, const char* what, const GpuGeometry& gpu, const GpuPrimitive& prim,
            const osg::Geometry& geometry)
        {
            const C3D_Tex* tex = gBound.mTexture;
            const osg::Node* parent = geometry.getNumParents() > 0 ? geometry.getParent(0) : nullptr;
            p3ds::log::writef(error ? p3ds::log::Level::Error : p3ds::log::Level::Info, "render3ds: %s: draw #%lu mode %d count %lu first/offset %lu index type %d | vbo pa %08lx size %lu "
                      "verts %lu | ibo pa %08lx size %lu | tex pa %08lx fmt %d "
                      "%ux%u levels %u param %08lx | base %08lx | lights %d | '%s' in '%s'",
                what, (unsigned long)gVerifiedDraws, int(prim.mMode), (unsigned long)prim.mCount,
                (unsigned long)prim.mFirst, prim.mIndexType, (unsigned long)physical(gpu.mVbo),
                (unsigned long)gpu.mVboSize, (unsigned long)gpu.mVertexCount, (unsigned long)physical(gpu.mIbo),
                (unsigned long)gpu.mIboSize, (unsigned long)(tex ? physical(tex->data) : 0), tex ? int(tex->fmt) : -1,
                tex ? unsigned(tex->width) : 0u, tex ? unsigned(tex->height) : 0u, tex ? unsigned(tex->maxLevel) + 1 : 0u,
                (unsigned long)(tex ? tex->param : 0), (unsigned long)gBufferBase, gDrawLights,
                geometry.getName().c_str(),
                parent ? parent->getName().c_str() : "");
        }

        // A hung GPU: log, reset the GPU core (GSP), stop rendering. Before this, a hang froze the console until a
        // forced power-off (a hung GPU also blocks the process exit).
        void gpuHang(const char* what)
        {
            P3DS_LOGE("render3ds: GPU HANG: %s", what);
            const Result reset = GSPGPU_ResetGpuCore();
            P3DS_LOGE("render3ds: GPU core reset (result %08lx); rendering is off for this run, the game keeps running "
                      "headless - HOME closes it",
                (unsigned long)reset);
            gFailure = "GPU hang - rendering off (see log)";
            gInFrame = false;
        }

        // Waits for the GPU to go idle (C3D_FrameBegin succeeds) for up to timeoutMs. On success a new citro3d
        // frame has begun.
        bool waitGpuAndBegin(unsigned timeoutMs)
        {
            // Only the time this thread spent waiting counts (M19, test #104): with the Rosalina menu open the whole
            // game is paused but the clock runs on, and a wait that began before the menu found its 3 s gone on the
            // way back: a GPU "hang" that wasn't, the GPU reset, a black screen for the rest of the run. A gap of
            // more than 100 ms between two 1 ms sleeps is such a pause, not GPU time.
            const u64 tick = CPU_TICKS_PER_MSEC;
            u64 waited = 0;
            u64 last = svcGetSystemTick();
            while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK))
            {
                const u64 now = svcGetSystemTick();
                const u64 step = now - last;
                last = now;
                if (step < 100 * tick)
                    waited += step;
                if (waited > u64(timeoutMs) * tick)
                    return false;
                svcSleepThread(1000000); // 1 ms
            }
            return true;
        }

        // Verification mode: submit what was just recorded (one draw) and wait for the GPU to finish it.
        void verifyDraw(const GpuGeometry& gpu, const GpuPrimitive& prim, const osg::Geometry& geometry)
        {
            // Not shown: the intermediate "frame" is the clear color plus a few objects, which flashed the fog
            // color on screen whenever new objects came into view (test #13).
            C3D_RenderTargetDetachOutput(gTarget);
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
            if (!waitGpuAndBegin(2000))
            {
                logDraw(true, "GPU HANG on this draw", gpu, prim, geometry);
                gpuHang("a scene draw never finished (draw-by-draw check)");
                return;
            }
            C3D_FrameDrawOn(gTarget);
            ++gVerifiedDraws;
            p3ds::crash::kick("render3ds draw check");
        }

        // ------------------------------------------------------------------ GPU self-test

        // Hardware tests #9 and #10 hung the GPU on the first real draw while every emulator run was fine. Before
        // the first frame, this draws one small quad per step, each submitted alone and waited for, starting from
        // the setup the M0 test app ran on hardware and adding one feature per step. A hang names the step
        // (synced log) before the crash report. On success every feature the renderer uses has run once.
        bool gSelfTestDone = false;

        struct ProbeVertex
        {
            float mPosition[3];
            float mTexCoord[2];
            float mNormal[3];
        };

        // Submits what was recorded and waits for it. false = the GPU hung (it was reset, rendering is off).
        bool gSimulateHang = false; // autorun "fakehang": exercise the recovery path in the emulator

        bool selfTestStep(const char* name)
        {
            const u64 start = svcGetSystemTick();
            C3D_RenderTargetDetachOutput(gTarget);
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
            const bool fake = gSimulateHang && std::strncmp(name, "L2 ", 3) == 0;
            if (fake || !waitGpuAndBegin(2000))
            {
                P3DS_LOGE("render3ds: GPU self-test: HANG at step %s", name);
                gpuHang("GPU self-test");
                return false;
            }
            C3D_FrameDrawOn(gTarget);
            if (p3ds::debugMode()) // each step's line: the start menu's Debug choices only (M15)
                P3DS_LOGI("render3ds: GPU self-test %s: ok (%.1f ms)", name,
                double(svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
            return true;
        }

        void setTestMatrices(int projectionLoc, int modelViewLoc)
        {
            // Quad positions are already in clip space. z = -0.5 is inside the PICA's [-1, 0] for both depth modes.
            C3D_FVec* p = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, projectionLoc, 4);
            p[0] = FVec4_New(1, 0, 0, 0);
            p[1] = FVec4_New(0, 1, 0, 0);
            p[2] = FVec4_New(0, 0, 0, -0.5f);
            p[3] = FVec4_New(0, 0, 0, 1);
            C3D_FVec* mv = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, modelViewLoc, 4);
            mv[0] = FVec4_New(1, 0, 0, 0);
            mv[1] = FVec4_New(0, 1, 0, 0);
            mv[2] = FVec4_New(0, 0, 1, 0);
            mv[3] = FVec4_New(0, 0, 0, 1);
        }

        void setTestLights(int count)
        {
            float data[kMaxLights * 16];
            for (int i = 0; i < kMaxLights; ++i)
            {
                float* m = data + i * 16;
                std::fill(m, m + 16, 0.f);
                m[0] = 0.1f * i; // position
                m[2] = 1.f;
                m[3] = 1.f; // constant attenuation
                m[4] = m[5] = m[6] = 0.2f; // ambient
                m[11] = 0.01f; // quadratic attenuation
                m[8] = m[9] = m[10] = 1.f; // diffuse
                m[15] = 100.f; // radius
            }
            writeLights(data, count, kDrawLights);
        }

        void setupTestTexture(C3D_Tex& tex, const CookedImage& image)
        {
            tex = C3D_Tex{};
            tex.data = image.gpuData();
            tex.fmt = image.gpuFormat();
            tex.size = image.getImageSizeInBytes();
            tex.width = static_cast<u16>(image.s());
            tex.height = static_cast<u16>(image.t());
            tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D) | (image.gpuFormat() == GPU_ETC1 ? GPU_TEXTURE_ETC1_PARAM : 0);
            C3D_TexSetFilter(&tex, GPU_LINEAR, GPU_LINEAR);
            C3D_TexSetFilterMipmap(&tex, GPU_LINEAR);
            C3D_TexSetWrap(&tex, GPU_REPEAT, GPU_REPEAT);
            tex.maxLevel = static_cast<u8>(image.levels() - 1);
        }

        void runGpuSelfTest()
        {
            gSelfTestDone = true;
            P3DS_LOGI("render3ds: GPU self-test: one quad per step, each waited for");

            // Geometry: one quad (scene layout and M0 layout), 16- and 8-bit indices, strip order.
            const Vertex quad[4] = {
                { { -0.5f, -0.5f, 0 }, { 0, 0, 1 }, { 255, 160, 64, 255 }, { 0, 0 } },
                { { 0.5f, -0.5f, 0 }, { 0, 0, 1 }, { 255, 160, 64, 255 }, { 1, 0 } },
                { { 0.5f, 0.5f, 0 }, { 0, 0, 1 }, { 255, 160, 64, 160 }, { 1, 1 } },
                { { -0.5f, 0.5f, 0 }, { 0, 0, 1 }, { 255, 160, 64, 160 }, { 0, 1 } },
            };
            const ProbeVertex probeQuad[4] = {
                { { -0.5f, -0.5f, 0 }, { 0, 0 }, { 0, 0, 1 } },
                { { 0.5f, -0.5f, 0 }, { 1, 0 }, { 0, 0, 1 } },
                { { 0.5f, 0.5f, 0 }, { 1, 1 }, { 0, 0, 1 } },
                { { -0.5f, 0.5f, 0 }, { 0, 1 }, { 0, 0, 1 } },
            };
            const uint16_t indices16[6] = { 0, 1, 2, 0, 2, 3 };
            const uint8_t indices8[8] = { 0, 1, 2, 0, 2, 3, 0, 0 };
            const uint16_t strip16[4] = { 0, 1, 3, 2 };
            auto* vbo = static_cast<Vertex*>(gpuAlloc(sizeof(quad)));
            auto* probeVbo = static_cast<ProbeVertex*>(gpuAlloc(sizeof(probeQuad)));
            auto* ibo16 = static_cast<uint16_t*>(gpuAlloc(sizeof(indices16)));
            auto* ibo8 = static_cast<uint8_t*>(gpuAlloc(sizeof(indices8)));
            auto* strip = static_cast<uint16_t*>(gpuAlloc(sizeof(strip16)));
            auto* white = static_cast<u32*>(gpuAlloc(8 * 8 * 4));
            auto* skinVbo = gSkinVariant ? static_cast<SkinVertex*>(gpuAlloc(sizeof(SkinVertex) * 4)) : nullptr;
            if (skinVbo != nullptr)
            {
                for (int i = 0; i < 4; ++i)
                {
                    skinVbo[i] = quad[i];
                    skinVbo[i].mWeights[0] = 255; // bone 0 (slots all 0), the other weights 0
                }
                GSPGPU_FlushDataCache(skinVbo, sizeof(SkinVertex) * 4);
            }
            if (!vbo || !probeVbo || !ibo16 || !ibo8 || !strip || !white)
            {
                P3DS_LOGE("render3ds: GPU self-test: no linear memory, skipped");
                return;
            }
            std::memcpy(vbo, quad, sizeof(quad));
            std::memcpy(probeVbo, probeQuad, sizeof(probeQuad));
            std::memcpy(ibo16, indices16, sizeof(indices16));
            std::memcpy(ibo8, indices8, sizeof(indices8));
            std::memcpy(strip, strip16, sizeof(strip16));
            for (int i = 0; i < 64; ++i)
                white[i] = 0xFFFFFFFF;
            for (const void* p : { (const void*)vbo, (const void*)probeVbo, (const void*)ibo16, (const void*)ibo8,
                     (const void*)strip, (const void*)white })
                GSPGPU_FlushDataCache(p, 256);

            C3D_Tex rgba{};
            rgba.data = white;
            rgba.fmt = GPU_RGBA8;
            rgba.size = 8 * 8 * 4;
            rgba.width = rgba.height = 8;
            rgba.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
            C3D_TexSetFilter(&rgba, GPU_LINEAR, GPU_NEAREST);
            osg::ref_ptr<osg::Image> etc1Image = loadCookedImageWithFormat(GPU_ETC1);
            osg::ref_ptr<osg::Image> etc1a4Image = loadCookedImageWithFormat(GPU_ETC1A4);
            C3D_Tex etc1{}, etc1a4{};
            if (etc1Image)
                setupTestTexture(etc1, static_cast<CookedImage&>(*etc1Image));
            if (etc1a4Image)
                setupTestTexture(etc1a4, static_cast<CookedImage&>(*etc1a4Image));

            // Common fixed-function state: no depth test effect, no blending, no alpha test, no fog, no culling.
            C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_CullFace(GPU_CULL_NONE);
            C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
            C3D_DepthMap(true, -1.f, 0.f);

            // Each step returns false if the GPU hung (it was reset and rendering is off): stop there.
            auto run = [&]() -> bool {
                // L0: the M0 test app, as it ran on hardware: its shader, citro3d's default buffer base, z-buffer,
                // RGBA8 texture x vertex color.
                {
                    C3D_BindProgram(&gProbeProgram);
                    C3D_AttrInfo* attr = C3D_GetAttrInfo();
                    AttrInfo_Init(attr);
                    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);
                    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);
                    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 3);
                    C3D_BufInfo* buf = C3D_GetBufInfo();
                    BufInfo_Init(buf);
                    BufInfo_Add(buf, probeVbo, sizeof(ProbeVertex), 3, 0x210);
                    const shaderInstance_s* vs = gProbeProgram.vertexShader;
                    setTestMatrices(shaderInstanceGetUniformLocation(const_cast<shaderInstance_s*>(vs), "projection"),
                        shaderInstanceGetUniformLocation(const_cast<shaderInstance_s*>(vs), "modelView"));
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, shaderInstanceGetUniformLocation(const_cast<shaderInstance_s*>(vs), "lightDir"), 0, 0, -1, 0);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, shaderInstanceGetUniformLocation(const_cast<shaderInstance_s*>(vs), "lightColor"), 1, 1, 1, 1);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER, shaderInstanceGetUniformLocation(const_cast<shaderInstance_s*>(vs), "ambient"), 0.3f, 0.3f, 0.3f, 1);
                    C3D_TexBind(0, &rgba);
                    gBound.mTexEnv = -1;
                    setTexEnv(TexEnv_Textured);
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                    if (!selfTestStep("L0 M0 test app setup"))
                        return false;
                }

                // L1: the scene vertex layout (4 attributes, color as unsigned bytes) through a minimal shader.
                {
                    C3D_BindProgram(&gFlatProgram);
                    setupAttributes();
                    bindVertexBuffer(vbo);
                    const shaderInstance_s* vs = gFlatProgram.vertexShader;
                    setTestMatrices(shaderInstanceGetUniformLocation(const_cast<shaderInstance_s*>(vs), "projection"),
                        shaderInstanceGetUniformLocation(const_cast<shaderInstance_s*>(vs), "modelView"));
                    setTexEnv(TexEnv_VertexColor);
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                    if (!selfTestStep("L1 scene vertex layout, minimal shader"))
                        return false;
                }

                // L2..: the scene shader (straight-line, lights unrolled), one feature at a time.
                C3D_BindProgram(&gProgram);
                setupAttributes();
                bindVertexBuffer(vbo);
                setTestMatrices(gLoc.mProjection, gLoc.mModelView);
                writeTexMat(gLoc.mTexMat, osg::Matrixf());
                writeTexMat(gLoc.mTexMat1, osg::Matrixf());
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mLighting, 1, 0, 0, 0);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mSunDir, 0, 0, 1, 0);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mSunDiffuse, 0.5f, 0.5f, 0.5f, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mSunAmbient, 0.3f, 0.3f, 0.3f, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mMatDiffuse, 1, 1, 1, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mMatAmbient, 1, 1, 1, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mMatEmission, 0, 0, 0, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mColorSel, 1, 1, 0, 1);
                forgetLightSlots();
                setTestLights(0);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L2 scene shader (lights unrolled, all off), untextured, z-buffer"))
                    return false;

                C3D_DepthMap(false, -1.f, 0.f);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L3 + W-buffer depth"))
                    return false;

                {
                    float data[256];
                    for (int i = 0; i <= 128; ++i)
                    {
                        const float visibility = 1.f - i / 128.f;
                        if (i < 128)
                            data[i] = visibility;
                        if (i > 0)
                            data[i + 127] = visibility - data[i - 1];
                    }
                    FogLut_FromArray(&gFogLut, data);
                    C3D_FogGasMode(GPU_FOG, GPU_PLAIN_DENSITY, false);
                    C3D_FogColor(0x406080);
                    C3D_FogLutBind(&gFogLut);
                }
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L4 + fog LUT"))
                    return false;

                C3D_TexBind(0, &rgba);
                setTexEnv(TexEnv_Textured);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L5 + RGBA8 texture"))
                    return false;

                if (etc1Image)
                {
                    C3D_TexBind(0, &etc1);
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                    if (!selfTestStep("L6 + ETC1 mipmapped texture (cooked)"))
                        return false;
                }
                if (etc1a4Image)
                {
                    C3D_TexBind(0, &etc1a4);
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                    if (!selfTestStep("L7 + ETC1A4 mipmapped texture (cooked)"))
                        return false;
                }

                setTestLights(1);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L8 + 1 point light"))
                    return false;

                setTestLights(kDrawLights);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L9 + 4 point lights"))
                    return false;

                C3D_AlphaTest(true, GPU_GREATER, 128);
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
                    GPU_ONE_MINUS_SRC_ALPHA);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L10 + alpha test and blending"))
                    return false;

                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_BYTE, ibo8);
                if (!selfTestStep("L11 + 8-bit indices"))
                    return false;

                C3D_DrawElements(GPU_TRIANGLE_STRIP, 4, C3D_UNSIGNED_SHORT, strip);
                if (!selfTestStep("L12 + triangle strip (indexed)"))
                    return false;

                C3D_DrawArrays(GPU_TRIANGLE_FAN, 0, 4);
                if (!selfTestStep("L13 + triangle fan (arrays)"))
                    return false;

                C3D_DepthTest(true, GPU_LESS, GPU_WRITE_ALL);
                C3D_CullFace(GPU_CULL_BACK_CCW);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L14 + depth test and back-face culling"))
                    return false;
                if (!gVariants)
                    return true;

                C3D_BindProgram(&gProgram1);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L15 + 1-light shader variant"))
                    return false;

                C3D_BindProgram(&gProgram0);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L16 + sun-only shader variant"))
                    return false;
                if (gVariant2)
                {
                    C3D_BindProgram(&gProgram2);
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                    if (!selfTestStep("L16b + 2-light shader variant"))
                        return false;
                }
                if (gSkinVariant && skinVbo != nullptr)
                {
                    // L17: GPU skinning: 6 attributes, bones through the address register. Bone 0 = identity
                    // (the quad's own place), weight 255. The other slots point at it with weight 0.
                    C3D_BindProgram(&gProgramSkin);
                    setupAttributes();
                    bindVertexBuffer(skinVbo);
                    C3D_FVec* rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mBones, 3);
                    rows[0] = FVec4_New(1, 0, 0, 0);
                    rows[1] = FVec4_New(0, 1, 0, 0);
                    rows[2] = FVec4_New(0, 0, 1, 0);
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                    if (!selfTestStep("L17 + skinned variant (6 attributes, relative addressing)"))
                        return false;
                    bindVertexBuffer(vbo);
                    gSceneBuffer = nullptr;
                }
                C3D_BindProgram(&gProgram);
                return true;
            };
            if (!run())
                return; // GPU reset, rendering off: leave the buffers (the GPU state is unknown)

            P3DS_LOGI("render3ds: GPU self-test passed");

            // Leave nothing behind: the scene re-applies all of its state. The test quads too: the first frame is
            // drawn while the game loads, the top screen then held (no clear), so the last quad (a textured one,
            // the middle of the screen) showed through the whole first load (the tester, since M8).
            C3D_RenderTargetClear(gTarget, C3D_CLEAR_ALL, 0x000000FF, 0xFFFFFF);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
            C3D_TexBind(0, nullptr);
            gFog = FogState{};
            gBound = Bound{};
            forgetLightSlots();
            gSceneBuffer = nullptr;
            gpuFree(vbo);
            gpuFree(probeVbo);
            gpuFree(ibo16);
            gpuFree(ibo8);
            gpuFree(strip);
            gpuFree(white); // the GPU is idle: selfTestStep waited for it
            gpuFree(skinVbo);
        }

        // ------------------------------------------------------------------ leaves, bins, stages

        // Where the CPU emit time goes, sampled on one frame in 8 (each lap is a system call): the perf log's
        // "emit.*" phases, scaled back to a per-frame average.
        bool gSampleEmit = false;
        uint64_t gEmitLap = 0;
        struct EmitProfile
        {
            uint64_t mGeometry = 0, mState = 0, mTexture = 0, mUniforms = 0, mDraw = 0;
        } gEmitProfile;

        inline void lap(uint64_t& bucket)
        {
            if (!gSampleEmit)
                return;
            const uint64_t now = p3ds::memprobe::ticks();
            bucket += now - gEmitLap;
            gEmitLap = now;
        }

        // Vertices built on the CPU every frame (particle quads). When beginFrame returns, the GPU has finished the
        // previous frame, so the buffer is refilled from the start each frame.
        constexpr uint32_t kTransientVertices = 16384; // 2730 particles, 576 KB (rain alone: ~1400-1900 drops)
        Vertex* gTransient = nullptr; // linear memory (init)
        uint32_t gTransientUsed = 0;
        // M10: particles are 4 vertices each (was 6, a triangle list), drawn through this index buffer: two triangles
        // per quad, (0 1 2, 2 3 0) + 4 * quad. A third fewer vertices built and uploaded on the CPU (rain: ~2,600 a
        // frame, 5 ms of a 37 ms frame in Balmora, test #92). Still an indexed triangle list (see gIdentityIbo).
        uint16_t* gQuadIbo = nullptr; // linear memory (init). Null: 6 vertices a particle, as before
        bool gWarnedTransient = false;

        // Hardware test #33 (docs/history/FIREBALL_CORRUPTION.md): the particles and the GUI were the frame's only
        // non-indexed draws. citro3d draws indexed triangles in the PICA's "geometry primitive" mode
        // (GPUREG_PRIMITIVE_CONFIG mode 3, GPUREG_GEOSTAGE_CONFIG/CONFIG2 bit 8) and arrays in plain triangle mode
        // (mode 0, GEOSTAGE_CONFIG2 bit 0), and those mode registers are written by the command processor as soon
        // as it reaches them, whether or not the previous draw's vertices are still in the pipeline. Behind a blast
        // mesh scaled x60,000 (hundreds of screen-sized triangles) they are, and flipping the mode under them left
        // the vertex stream one output out of step for the rest of the run (test #33: no corruption over five
        // blasts with this change, corruption within three with the array-draws switch, same build and console).
        // So every triangle list in the frame is an indexed draw through this identity index buffer (index i =
        // vertex i of the bound buffer): one primitive mode for the whole frame. Never add a C3D_DrawArrays or an
        // immediate-mode draw to a frame with scene geometry. The self-test's TRIANGLE_FAN draw stays (startup,
        // GPU idle).
        constexpr uint32_t kIdentityIndices = 65536; // the particle buffer (16384) and the GUI's at most (65536)
        uint16_t* gIdentityIbo = nullptr; // linear memory (init). Null: back to C3D_DrawArrays
        static_assert(kTransientVertices <= kIdentityIndices); // and kGuiMaxVertices, checked where it is defined

        // Draws vertices [first, first + count) of the bound buffer as a triangle list.
        void drawTriangleList(uint32_t first, uint32_t count)
        {
            if (gIdentityIbo == nullptr || first + count > kIdentityIndices)
                C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(first), static_cast<int>(count));
            else
                C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(count), C3D_UNSIGNED_SHORT, gIdentityIbo + first);
        }

        // Perf batch 2 (after MW3DS's flipbooks): the flames and smoke of still objects (candles, lanterns,
        // braziers: Objects::insertModel marks their particle systems "still3ds") are simulated once per kind and
        // replayed. The first one drawn records 16 frames at 12 a second (after 3 s, once steady), in the frame of
        // its object. Then every system of that kind (same material, life time and size) stops simulating (frozen)
        // and draws the loop at its own place, each at its own point of the loop. Systems in world space (NIF
        // particles without the local-space flag) sit under a transform holding the inverse of their object's world
        // matrix (NifOsg::InverseWorldMatrix): that matrix takes them to the object's frame and back.
        struct FlameParticle
        {
            osg::Vec3 mPosition; // in the object's frame
            float mSize;
            osg::Vec4 mColor; // alpha included
            osg::Vec3 mAngle;
            float mS0, mT0, mS1, mT1;
        };
        struct FlameLoop
        {
            osg::observer_ptr<const osgParticle::ParticleSystem> mRecorder;
            double mNextFrame = 0;
            std::vector<std::vector<FlameParticle>> mFrames;
            bool mReady = false;
        };
        constexpr int kFlameFrames = 16;
        constexpr double kFlameRate = 12.0; // frames a second
        constexpr double kFlameWarmup = 3.0; // seconds before recording
        std::map<std::tuple<const osg::StateSet*, float, float>, FlameLoop> gFlameLoops;

        double nowSeconds()
        {
            return static_cast<double>(svcGetSystemTick()) / SYSCLOCK_ARM11;
        }

        // ps's coordinates to its object's frame (identity unless the system is in world space).
        osg::Matrix particlesToObject(const osgParticle::ParticleSystem& ps)
        {
            const osg::UserDataContainer* udc = ps.getUserDataContainer();
            if (udc == nullptr || ps.getNumParents() == 0)
                return osg::Matrix::identity();
            const osg::UserDataContainer::DescriptionList& descriptions = udc->getDescriptions();
            if (std::find(descriptions.begin(), descriptions.end(), "worldspace") == descriptions.end())
                return osg::Matrix::identity();
            const osg::MatrixTransform* transform = dynamic_cast<const osg::MatrixTransform*>(ps.getParent(0));
            return transform != nullptr ? transform->getMatrix() : osg::Matrix::identity();
        }

        // The loop frame to draw instead of ps's own particles (and objectToParticles set), or nullptr: ps draws
        // its live particles (not a still system, or its kind's loop isn't recorded yet).
        const std::vector<FlameParticle>* flameLoop(const osgParticle::ParticleSystem& ps, osg::Matrix& objectToParticles)
        {
            bool still = false;
            if (!flameLoops() || !ps.getUserValue("still3ds", still) || !still)
                return nullptr;
            const osgParticle::Particle& model = ps.getDefaultParticleTemplate();
            FlameLoop& loop = gFlameLoops[{ ps.getStateSet(), model.getLifeTime(), model.getSizeRange().maximum }];
            const double now = nowSeconds();
            if (loop.mReady)
            {
                if (!ps.isFrozen())
                    const_cast<osgParticle::ParticleSystem&>(ps).setFrozen(true);
                const osg::Matrix toObject = particlesToObject(ps);
                objectToParticles = osg::Matrix::inverse(toObject);
                const unsigned phase = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&ps) >> 5);
                const unsigned frame = (static_cast<unsigned>(now * kFlameRate) + phase) % kFlameFrames;
                ++gStats.mFlameReplays;
                return &loop.mFrames[frame];
            }
            if (!loop.mRecorder.valid())
            {
                loop.mRecorder = &ps;
                loop.mFrames.clear();
                loop.mNextFrame = now + kFlameWarmup;
            }
            if (loop.mRecorder.get() != &ps || now < loop.mNextFrame)
                return nullptr;
            if (now - loop.mNextFrame > 0.5) // not drawn for a while: frames too far apart, start again
            {
                loop.mFrames.clear();
                loop.mNextFrame = now + 1.0 / kFlameRate;
                return nullptr;
            }
            const osg::Matrix toObject = particlesToObject(ps);
            std::vector<FlameParticle>& frame = loop.mFrames.emplace_back();
            for (int i = 0; i < ps.numParticles(); ++i)
            {
                const osgParticle::Particle* particle = ps.getParticle(i);
                if (!particle->isAlive())
                    continue;
                FlameParticle& p = frame.emplace_back();
                p.mPosition = particle->getPosition() * toObject;
                p.mSize = particle->getCurrentSize();
                p.mColor = particle->getCurrentColor();
                p.mColor.a() *= particle->getCurrentAlpha();
                p.mAngle = particle->getAngle();
                p.mS0 = particle->getSTexCoord();
                p.mT0 = particle->getTTexCoord();
                p.mS1 = p.mS0 + particle->getSTexTile();
                p.mT1 = p.mT0 + particle->getTTexTile();
            }
            loop.mNextFrame += 1.0 / kFlameRate;
            if (loop.mFrames.size() == kFlameFrames)
            {
                loop.mReady = true;
                loop.mRecorder = nullptr;
            }
            return nullptr;
        }

        // osgParticle::ParticleSystem::drawImplementation's quads (OSG ParticleSystem.cpp), on the CPU: OpenMW's
        // fires, flames, smoke and spell effects (they draw through immediate GL that never reaches us). Normal
        // (0, 0, 1) like NifOsg::ParticleSystem's normal array.
        bool buildParticles(
            const osgParticle::ParticleSystem& ps, const osg::Matrix& modelview, uint32_t& first, uint32_t& count)
        {
            first = gTransientUsed;
            count = 0;
            osg::Matrix objectToParticles;
            const std::vector<FlameParticle>* replay = flameLoop(ps, objectToParticles);
            if (gTransient == nullptr || (replay == nullptr && ps.numParticles() <= 0))
                return false;
            const int detail = std::max(1, ps.getLevelOfDetail());
            const float scale = std::sqrt(static_cast<float>(detail));
            osg::Vec3 xAxis = ps.getAlignVectorX();
            osg::Vec3 yAxis = ps.getAlignVectorY();
            osg::Vec3 alignedX = xAxis;
            osg::Vec3 alignedY = yAxis;
            const bool billboard = ps.getParticleAlignment() == osgParticle::ParticleSystem::BILLBOARD;
            if (billboard)
            {
                xAxis = osg::Matrix::transform3x3(modelview, xAxis);
                yAxis = osg::Matrix::transform3x3(modelview, yAxis);
                const float lengthX2 = xAxis.length2(), lengthY2 = yAxis.length2();
                // A spell's hit or area effect starts at scale 0 and grows: 1/0 below would make every corner
                // NaN. The PICA has no defined behaviour for those (hardware test #28: both screens and textures
                // corrupted as a fireball exploded, Azahar just drops them). Nothing shows at scale 0 anyway.
                if (!(lengthX2 > 1e-12f && lengthY2 > 1e-12f))
                {
                    gStats.mBadParticles += static_cast<uint32_t>(ps.numParticles());
                    return false;
                }
                float xScale, yScale;
                if (ps.getParticleScaleReferenceFrame() == osgParticle::ParticleSystem::LOCAL_COORDINATES)
                {
                    xScale = 1.f / std::sqrt(lengthX2);
                    yScale = 1.f / std::sqrt(lengthY2);
                }
                else
                {
                    xScale = 1.f / lengthX2;
                    yScale = 1.f / lengthY2;
                }
                alignedX *= xScale;
                alignedY *= yScale;
                xAxis *= xScale;
                yAxis *= yScale;
            }
            // Weather (perf batch 1, after MW3DS's precipitation): rain drops about a pixel wide whatever their
            // distance (never wider than OpenMW's), and the drops and storm clouds nearest the eye faded out, as
            // those few cover most of the blended pixels (sky.cpp names the systems).
            enum class Weather
            {
                None,
                Rain,
                Storm,
            };
            const std::string& psName = ps.getName();
            const Weather weather = !thinWeather() ? Weather::None
                : psName == "Rain3DS"             ? Weather::Rain
                : psName == "Storm3DS"            ? Weather::Storm
                                                  : Weather::None;
            const float fadeStart = weather == Weather::Rain ? 40.f : 150.f; // nothing nearer
            const float fadeEnd = weather == Weather::Rain ? 100.f : 400.f; // full alpha from here
            const uint32_t room = kTransientVertices - gTransientUsed;
            Vertex* out = gTransient + first;
            const int total = replay != nullptr ? static_cast<int>(replay->size()) : ps.numParticles();
            const int step = replay != nullptr ? 1 : detail;
            for (int i = 0; i < total; i += step)
            {
                FlameParticle source;
                if (replay != nullptr)
                {
                    source = (*replay)[i];
                    source.mPosition = source.mPosition * objectToParticles;
                }
                else
                {
                    const osgParticle::Particle* particle = ps.getParticle(i);
                    if (!particle->isAlive())
                        continue;
                    switch (particle->getShape())
                    {
                        case osgParticle::Particle::QUAD:
                        case osgParticle::Particle::QUAD_TRIANGLESTRIP:
                        case osgParticle::Particle::HEXAGON:
                        case osgParticle::Particle::USER:
                            break;
                        default:
                            continue; // points and lines
                    }
                    source.mPosition = particle->getPosition();
                    source.mSize = particle->getCurrentSize();
                    source.mColor = particle->getCurrentColor();
                    source.mColor.a() *= particle->getCurrentAlpha();
                    source.mAngle = particle->getAngle();
                    source.mS0 = particle->getSTexCoord();
                    source.mT0 = particle->getTTexCoord();
                    source.mS1 = source.mS0 + particle->getSTexTile();
                    source.mT1 = source.mT0 + particle->getTTexTile();
                }
                const uint32_t perParticle = gQuadIbo != nullptr ? 4 : 6;
                if (count + perParticle > room)
                {
                    if (!gWarnedTransient)
                        Log(Debug::Warning) << "render3ds: more particles than the per-frame buffer holds";
                    gWarnedTransient = true;
                    break;
                }
                osg::Vec3 x = xAxis, y = yAxis;
                const osg::Vec3& angle = source.mAngle;
                if (angle.x() != 0.f || angle.y() != 0.f || angle.z() != 0.f)
                {
                    osg::Matrix rotation;
                    rotation.makeRotate(angle.x(), osg::Vec3(1, 0, 0), angle.y(), osg::Vec3(0, 1, 0), angle.z(),
                        osg::Vec3(0, 0, 1));
                    x = osg::Matrix::transform3x3(alignedX, rotation);
                    y = osg::Matrix::transform3x3(alignedY, rotation);
                    if (billboard)
                    {
                        x = osg::Matrix::transform3x3(modelview, x);
                        y = osg::Matrix::transform3x3(modelview, y);
                    }
                }
                osg::Vec4 color = source.mColor;
                const float size = source.mSize * (replay != nullptr ? 1.f : scale);
                osg::Vec3 p1 = x * size;
                const osg::Vec3 p2 = y * size;
                const osg::Vec3& pos = source.mPosition;
                if (weather != Weather::None)
                {
                    const float distance = -(pos * modelview).z();
                    if (distance < fadeStart)
                    {
                        ++gStats.mWeatherSkipped;
                        continue;
                    }
                    if (distance < fadeEnd)
                        color.a() *= (distance - fadeStart) / (fadeEnd - fadeStart);
                    if (weather == Weather::Rain)
                    {
                        // Half a pixel either side: the 240-row screen spans ~1.15 x distance at OpenMW's 60 degrees.
                        const float halfWidth = std::max(0.02f, 0.0024f * distance);
                        const float length = p1.length();
                        if (length > halfWidth)
                            p1 *= halfWidth / length;
                    }
                }
                const float s0 = source.mS0, t0 = source.mT0, s1 = source.mS1, t1 = source.mT1;
                const osg::Vec3 corners[4] = { pos - p1 - p2, pos + p1 - p2, pos + p1 + p2, pos - p1 + p2 };
                const auto finite = [](const osg::Vec3& v) {
                    return std::isfinite(v.x()) && std::isfinite(v.y()) && std::isfinite(v.z());
                };
                if (!finite(corners[0]) || !finite(corners[2]))
                {
                    ++gStats.mBadParticles;
                    continue;
                }
                const float uv[4][2] = { { s0, t0 }, { s1, t0 }, { s1, t1 }, { s0, t1 } };
                uint8_t rgba[4];
                for (int k = 0; k < 4; ++k)
                    rgba[k] = static_cast<uint8_t>(std::clamp(color[k], 0.f, 1.f) * 255.f + 0.5f);
                static const int order[6] = { 0, 1, 2, 2, 3, 0 };
                for (uint32_t k = 0; k < perParticle; ++k)
                {
                    const int corner = perParticle == 4 ? static_cast<int>(k) : order[k];
                    Vertex& v = out[count++];
                    std::memcpy(v.mPosition, corners[corner].ptr(), 12);
                    v.mNormal[0] = v.mNormal[1] = 0.f;
                    v.mNormal[2] = 1.f;
                    std::memcpy(v.mColor, rgba, 4);
                    v.mTexCoord[0] = uv[corner][0];
                    v.mTexCoord[1] = uv[corner][1];
                }
            }
            if (count == 0)
                return false;
            flushToGpu(out, count * sizeof(Vertex));
            gTransientUsed += count;
            return true;
        }

#ifdef OPENMW3DS_DEBUG
        // Zeste (the Azahar fork used for testing) labels its strict-PICA findings with the draw that caused them: a
        // write to GPUREG_FRAGOP_SHADOW with an empty byte mask (no effect on hardware) before each draw, and the
        // label's text once through svcOutputDebugString. Only with sdmc:/openmw/zeste-tags.
        void zesteTag(const osgUtil::RenderLeaf& leaf, bool particles)
        {
            static const bool enabled = [] {
                struct stat st;
                return stat("sdmc:/openmw/zeste-tags", &st) == 0;
            }();
            if (!enabled)
                return;
            const osg::Drawable* drawable = leaf._drawable.get();
            std::string name = drawable->getName();
            if (name.empty() && drawable->getNumParents() > 0)
                name = drawable->getParent(0)->getName();
            if (particles)
                name = "particles " + name;
            static std::unordered_map<std::string, u32> ids;
            auto [it, added] = ids.emplace(name, static_cast<u32>(ids.size() + 1));
            if (added)
            {
                const std::string text = "ZESTE:tag " + std::to_string(it->second) + " " + name;
                svcOutputDebugString(text.c_str(), static_cast<s32>(text.size()));
            }
            GPUCMD_AddMaskedWrite(0x130, 0x0, it->second);
        }
#endif

#ifdef OPENMW3DS_DEBUG
        // Hardware test #29: a fireball exploding at the player's feet breaks the GPU (Azahar draws it fine). What
        // reaches the GPU then, logged once per drawable: draws at (near) zero scale (an effect's first frames:
        // normals of length 0, NaN once normalized in the shader) and draws whose bounds enclose the camera.
        // sdmc:/openmw/draw-census: the draws (sky and terrain left out) of one busy frame are logged once, with
        // their node path, size and distance (M10: what the 3DS's many small draws are).
        bool gCensusFrame = false;
        void censusDraw(const osgUtil::RenderLeaf& leaf, uint32_t triangles, const ResolvedState& rs)
        {
            const osg::Drawable* drawable = leaf._drawable.get();
            std::string path = drawable->getName();
            const osg::Node* node = drawable->getNumParents() > 0 ? drawable->getParent(0) : nullptr;
            for (int depth = 0; node != nullptr && depth < 8; ++depth)
            {
                if (!node->getName().empty())
                    path = node->getName() + "/" + path;
                node = node->getNumParents() > 0 ? node->getParent(0) : nullptr;
            }
            const osg::Matrix& mv = *leaf._modelview;
            const float scale = osg::Vec3(mv(0, 0), mv(0, 1), mv(0, 2)).length();
            const osg::BoundingSphere& bound = drawable->getBound();
            const osg::Image* image = rs.mTexture.mValue ? rs.mTexture.mValue->getImage(0) : nullptr;
            P3DS_LOGI("census: %u tris, radius %.0f, distance %.0f, texture %s, %s", unsigned(triangles),
                double(bound.radius() * scale), double((bound.center() * mv).length()),
                image ? image->getFileName().c_str() : "-", path.c_str());
        }

        void noteUnusualDraw(const osgUtil::RenderLeaf& leaf)
        {
            static std::unordered_set<const osg::Drawable*> noted;
            if (noted.size() >= 150)
                return;
            const osg::Drawable* drawable = leaf._drawable.get();
            const osg::Matrix& mv = *leaf._modelview;
            // Squared lengths: this runs for every draw of a debug build (perf batch 4: 4 square roots a draw).
            const float sx2 = osg::Vec3(mv(0, 0), mv(0, 1), mv(0, 2)).length2();
            const float sy2 = osg::Vec3(mv(1, 0), mv(1, 1), mv(1, 2)).length2();
            const float sz2 = osg::Vec3(mv(2, 0), mv(2, 1), mv(2, 2)).length2();
            const float scale2 = std::max(sx2, std::max(sy2, sz2));
            const osg::BoundingSphere& bound = drawable->getBound();
            const float distance2 = (bound.center() * mv).length2();
            const bool zeroScale = std::min(sx2, std::min(sy2, sz2)) < 1e-8f;
            const bool enclosing = bound.valid() && distance2 < bound.radius2() * scale2;
            if (!zeroScale && !enclosing)
                return;
            const float sx = std::sqrt(sx2), sy = std::sqrt(sy2), sz = std::sqrt(sz2);
            const float scale = std::sqrt(scale2);
            const float distance = std::sqrt(distance2);
            if (!noted.insert(drawable).second)
                return;
            const osg::Node* parent = drawable->getNumParents() > 0 ? drawable->getParent(0) : nullptr;
            const osg::Geometry* geometry = drawable->asGeometry();
            P3DS_LOGI("render3ds: %s draw '%s' (parent '%s'): scale %.3g %.3g %.3g, radius %.0f, camera %.0f from "
                      "its centre, %u vertices, %s",
                zeroScale ? "zero-scale" : "camera-enclosing", drawable->getName().c_str(),
                parent ? parent->getName().c_str() : "-", double(sx), double(sy), double(sz),
                double(bound.radius() * scale), double(distance),
                geometry && geometry->getVertexArray() ? geometry->getVertexArray()->getNumElements() : 0u,
                geometry ? "geometry" : "particles");
        }
#endif

        void drawLeaf(osgUtil::RenderLeaf* leaf)
        {
            if (gHoldTop)
                return;
            if (gFailure)
                return;
            if (gSampleEmit)
                gEmitLap = p3ds::memprobe::ticks();
            ++gStats.mLeaves;
            const osg::Geometry* geometry = leaf->_drawable ? leaf->_drawable->asGeometry() : nullptr;
            const osgParticle::ParticleSystem* particles = geometry == nullptr && leaf->_drawable
                ? dynamic_cast<const osgParticle::ParticleSystem*>(leaf->_drawable.get())
                : nullptr;
            if ((geometry == nullptr && particles == nullptr) || leaf->_modelview == nullptr
                || leaf->_projection == nullptr || leaf->_parent == nullptr)
            {
                ++gStats.mSkipped; // MyGUI, debug drawables, ... (not drawn)
                logSkipped(leaf->_drawable.get(), "not a geometry");
                return;
            }
            GpuGeometry* gpu = nullptr;
            uint32_t particleFirst = 0, particleCount = 0;
            // GPU skinning: RigGeometry's drawable carries this frame's pose. Its mesh is drawn instead of its
            // vertices (which hold the first frame's pose, for ray casts).
            const SkinPose* pose = nullptr;
            GpuSkin* skin = nullptr;
            if (geometry != nullptr && gSkinVariant)
                if (const osg::Referenced* data = geometry->getUserData())
                    if ((pose = dynamic_cast<const SkinPose*>(data)) != nullptr && pose->mMesh != nullptr)
                        skin = skinFor(*pose->mMesh);
            if (particles)
            {
                if (!buildParticles(*particles, *leaf->_modelview, particleFirst, particleCount))
                    return; // no live particles
            }
            else if (skin == nullptr)
            {
                gpu = geometryFor(*geometry);
                if (gpu == nullptr || gpu->mPrimitives.empty())
                {
                    ++gStats.mSkipped;
                    logSkipped(geometry, "no usable vertices/primitives");
                    return;
                }
            }
            lap(gEmitProfile.mGeometry);
#ifdef OPENMW3DS_DEBUG
            noteUnusualDraw(*leaf);
            zesteTag(*leaf, particles != nullptr);
#endif
            const ResolvedState& rs = resolve(leaf->_parent);
            // Perf batch 4: a draw under the same state graph as the previous one (OSG sorts the opaque bin by
            // state: the copies of a model come one after the other) has the same sky pass, fixed state, textures,
            // material, texture matrices and fog, all set already. Only its model-view and lights change.
            const bool sameState = leaf->_parent == gBound.mStateGraph
                && leaf->_projection.get() == gBound.mStateProjection && gFog.mValid && sameStateSkip();
            const int sky = sameState ? gBound.mSky : skyPass(rs);
            if (sky == Sky_SunflashQuery)
                return; // an occlusion query for the sun glare: draws nothing visible
            // M10: small feature culling for each part of a model. OSG culls whole objects smaller than [Camera]
            // small feature culling pixel size, not their parts: a door far enough to show 5 pixels of keyhole
            // still drew it (outside Seyda Neen, 169 of 304 draws had under 64 triangles). Never an actor's skinned
            // parts: hands and forearms are separate small meshes, and NPCs a few metres away lost their arms (test
            // #92).
            const bool skinnedPart = skin != nullptr
                || dynamic_cast<const SceneUtil::RigGeometry::CopyBoundingSphereCallback*>(
                       geometry != nullptr ? geometry->getComputeBoundingSphereCallback() : nullptr)
                    != nullptr;
            if (gSmallFeaturePixels > 0.f && particles == nullptr && sky == Sky_None && !skinnedPart)
            {
                const osg::Matrix& projection = *leaf->_projection;
                const osg::BoundingSphere& bound = geometry->getBound();
                if (projection(3, 3) == 0.f && bound.valid())
                {
                    const osg::Matrix& mv = *leaf->_modelview;
                    // Squared, no square roots (perf batch 4): radius * p11 * height < pixels * distance.
                    const float radius2 = bound.radius2() * osg::Vec3(mv(0, 0), mv(0, 1), mv(0, 2)).length2();
                    const float distance2 = (bound.center() * mv).length2();
                    const float height = gRotatedTarget ? 240.f : 256.f; // pixels across the vertical field of view
                    const float onScreen = float(projection(1, 1)) * height;
                    if (distance2 > radius2
                        && radius2 * onScreen * onScreen < gSmallFeaturePixels * gSmallFeaturePixels * distance2)
                    {
                        ++gStats.mSmallCulled;
                        return;
                    }
                }
            }

            if (!sameState)
            {
                const FixedState fixed = computeFixedState(rs);
                if (fixed.mCull == 3)
                    return;
                if (!gBound.mFixedValid || !(fixed == gBound.mFixed))
                {
                    applyFixedState(fixed);
                    gBound.mFixed = fixed;
                    gBound.mFixedValid = true;
                }
            }

            lap(gEmitProfile.mState);
            // The terrain's blend map, as the previous draw bound it. Recomputed on a new state.
            C3D_Tex* blendMap = gBound.mTexture1 != &gUnusedTex1 ? gBound.mTexture1 : nullptr;
            if (!sameState)
            {
                // The sky's atmosphere and sun glare passes don't sample their texture.
                const bool skyUntextured = sky == Sky_Atmosphere || sky == Sky_Sunglare;
                C3D_Tex* texture = rs.mTexture.mValue && !skyUntextured ? textureFor(*rs.mTexture.mValue) : nullptr;
                if (texture != gBound.mTexture)
                {
                    gBound.mTexture = texture;
                    if (texture)
                        C3D_TexBind(0, texture);
                }
                const osg::Texture* blendMapTexture = blendMapOf(rs);
                blendMap = texture && blendMapTexture ? textureFor(*blendMapTexture) : nullptr;
                C3D_Tex* unit1 = blendMap ? blendMap : &gUnusedTex1;
                if (unit1 != gBound.mTexture1)
                {
                    gBound.mTexture1 = unit1;
                    C3D_TexBind(1, unit1);
                }
                setTexEnv(blendMap ? TexEnv_BlendMapped : texture ? TexEnv_Textured : TexEnv_VertexColor);
            }
            lap(gEmitProfile.mTexture);

            if (leaf->_projection.get() != gBound.mProjection)
            {
                gBound.mProjection = leaf->_projection.get();
                setProjection(*leaf->_projection);
            }
            if (leaf->_modelview.get() != gBound.mModelView)
            {
                gBound.mModelView = leaf->_modelview.get();
                C3D_FVec* rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mModelView, 3);
                setRows(rows, *leaf->_modelview, 3);
            }
            bool simpleLighting = false;
            if (particles)
                particles->getUserValue("simpleLighting", simpleLighting);
            uploadLighting(rs, sky == Sky_None && !simpleLighting, skin != nullptr);
            if (!sameState)
            {
                uploadMaterial(rs, sky);
                uploadTexMat(rs, sky);
                uploadFog(rs, sky);
                gBound.mStateGraph = leaf->_parent;
                gBound.mStateProjection = leaf->_projection.get();
                gBound.mSky = sky;
            }
            else
                ++gStats.mSameState;
            lap(gEmitProfile.mUniforms);

            if (skin)
            {
                // Each batch: its bones' matrices times the model-view (3 rows each), then its triangles.
                const osg::Matrixf modelView(*leaf->_modelview);
                if (gSceneBuffer == nullptr)
                    sceneBufferInfo(skin->mVbo);
                const std::vector<SkinBatch>& batches = pose->mMesh->mBatches;
                for (size_t b = 0; b < batches.size() && b < skin->mBatches.size(); ++b)
                {
                    const SkinBatch& batch = batches[b];
                    C3D_FVec* rows = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mBones, batch.mBones.size() * 3);
                    for (size_t k = 0; k < batch.mBones.size(); ++k)
                    {
                        const uint16_t bone = batch.mBones[k];
                        if (bone < pose->mBones.size())
                            setRows(rows + k * 3, pose->mBones[bone] * modelView, 3);
                        else
                            for (int r = 0; r < 3; ++r)
                                rows[k * 3 + r] = FVec4_New(0.f, 0.f, 0.f, 0.f);
                    }
                    const GpuSkin::Batch& gpuBatch = skin->mBatches[b];
                    setSceneBufferOffset(gpuBatch.mVboOffset);
                    C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(gpuBatch.mCount), C3D_UNSIGNED_SHORT,
                        gpuBatch.mIndices);
                    ++gStats.mDraws;
                    ++gStats.mSkinnedDraws;
                    gStats.mTriangles += gpuBatch.mCount / 3;
                    gStats.mShaderRuns[gDrawLights] += gpuBatch.mCount;
                }
                lap(gEmitProfile.mDraw);
                return;
            }

            if (particles)
            {
                if (gSceneBuffer == nullptr)
                    sceneBufferInfo(gTransient);
                setSceneBufferOffset(osConvertVirtToPhys(gTransient) - gBufferBase);
                uint32_t indices = particleCount;
                if (gQuadIbo != nullptr)
                {
                    // particleFirst is a multiple of 4: every particle system writes whole quads
                    indices = particleCount / 4 * 6;
                    C3D_DrawElements(GPU_TRIANGLES, static_cast<int>(indices), C3D_UNSIGNED_SHORT,
                        gQuadIbo + particleFirst / 4 * 6);
                }
                else
                    drawTriangleList(particleFirst, particleCount);
                ++gStats.mDraws;
                ++gStats.mParticleDraws;
                gStats.mTriangles += indices / 3;
                gStats.mShaderRuns[gDrawLights] += indices;
                gStats.mParticleVertices += particleCount;
                lap(gEmitProfile.mDraw);
                return;
            }
            if (gSceneBuffer == nullptr)
                sceneBufferInfo(gpu->mVbo);
            setSceneBufferOffset(gpu->mVboOffset);
            const bool verify = gpuVerify() && (gVerifyThisFrame || !gpu->mVerified);
            for (const GpuPrimitive& prim : gpu->mPrimitives)
            {
                if (gVerifyThisFrame && gVerifiedDraws < 3)
                    logDraw(false, "draw check", *gpu, prim, *geometry); // a few samples of what a draw looks like
                if (prim.mIndexType < 0)
                    C3D_DrawArrays(prim.mMode, static_cast<int>(prim.mFirst), static_cast<int>(prim.mCount));
                else
                    C3D_DrawElements(prim.mMode, static_cast<int>(prim.mCount), prim.mIndexType,
                        static_cast<uint8_t*>(gpu->mIbo) + prim.mFirst);
                ++gStats.mDraws;
#ifdef OPENMW3DS_DEBUG
                // Every draw of the census frame but the sky and terrain (batch 3: which draws share a texture).
                if (gCensusFrame && sky == Sky_None && !blendMap)
                    censusDraw(*leaf, prim.mTriangles, rs);
#endif
                ++(sky != Sky_None ? gStats.mSkyDraws
                        : blendMap      ? gStats.mTerrainDraws
                        : prim.mTriangles < 64 ? gStats.mSmallDraws
                                               : gStats.mOtherDraws);
                gStats.mTriangles += prim.mTriangles;
                gStats.mShaderRuns[gDrawLights] += prim.mCount;
                if (verify)
                {
                    verifyDraw(*gpu, prim, *geometry);
                    if (gFailure)
                        return;
                }
            }
            lap(gEmitProfile.mDraw);
            if (verify && !gpu->mVerified)
            {
                gpu->mVerified = true;
                ++gStats.mVerified;
            }
        }

        void drawBin(osgUtil::RenderBin* bin)
        {
            osgUtil::RenderBin::RenderBinList& bins = bin->getRenderBinList();
            auto it = bins.begin();
            for (; it != bins.end() && it->first < 0; ++it)
                drawBin(it->second.get());
            for (osgUtil::RenderLeaf* leaf : bin->getRenderLeafList())
                drawLeaf(leaf);
            for (osgUtil::StateGraph* sg : bin->getStateGraphList())
                for (auto& leaf : sg->_leaves)
                    drawLeaf(leaf.get());
            for (; it != bins.end(); ++it)
                drawBin(it->second.get());
        }

        u32 toRgba8(const osg::Vec4& c)
        {
            auto channel = [](float v) { return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
            return channel(c.r()) << 24 | channel(c.g()) << 16 | channel(c.b()) << 8 | channel(c.a());
        }

        bool isMainCamera(const osg::Camera* camera)
        {
            return camera != nullptr && camera->getView() != nullptr && camera->getView()->getCamera() == camera;
        }

        std::string gCapturePath;

        // The render target still holds the previous frame once C3D_FrameBegin has waited for the GPU. It is
        // read straight from VRAM and detiled on the CPU (a GX display transfer here can return before it ran:
        // its completion event may already be signalled by the previous frame's screen transfer).
        bool captureTarget(const C3D_RenderTarget& target, const std::string& path)
        {
            const unsigned kW = target.frameBuf.width, kH = target.frameBuf.height; // portrait, as the LCD
            const auto* src = static_cast<const u32*>(target.frameBuf.colorBuf);
            GSPGPU_InvalidateDataCache(src, kW * kH * 4);
            std::vector<uint8_t> out(kW * kH * 3);
            for (unsigned y = 0; y < kH; ++y)
                for (unsigned x = 0; x < kW; ++x)
                {
                    const u32 texel = src[((y / 8) * (kW / 8) + x / 8) * 64 + morton8(x % 8, y % 8)]; // 0xRRGGBBAA
                    uint8_t* dst = &out[(y * kW + x) * 3]; // BGR8, as the LCD framebuffer
                    dst[0] = static_cast<uint8_t>(texel >> 8);
                    dst[1] = static_cast<uint8_t>(texel >> 16);
                    dst[2] = static_cast<uint8_t>(texel >> 24);
                }
            return p3ds::screenshot::saveRotatedFramebuffer(
                path.c_str(), out.data(), kH, kW, p3ds::screenshot::Format::BGR8);
        }

        void checkDisplayState(); // with the MyGUI section (it reads the bottom target)
        void captureAll(const std::string& path); // likewise

        // The player's screenshot (the Home tab's camera button): the top screen's finished frame, with the bottom
        // screen as it shows, in one file written in the background (p3ds::screenshot::saveBothScreensAsync).
        std::string gPlayerCapturePath;
        bool gPlayerCaptureStarted = false;
        void capturePlayer(const std::string& path)
        {
            const unsigned kW = gTarget->frameBuf.width, kH = gTarget->frameBuf.height; // portrait, as the LCD
            const auto* src = static_cast<const u32*>(gTarget->frameBuf.colorBuf);
            GSPGPU_InvalidateDataCache(src, kW * kH * 4);
            std::vector<uint8_t> out(kW * kH * 3);
            for (unsigned y = 0; y < kH; ++y)
                for (unsigned x = 0; x < kW; ++x)
                {
                    const u32 texel = src[((y / 8) * (kW / 8) + x / 8) * 64 + morton8(x % 8, y % 8)]; // 0xRRGGBBAA
                    uint8_t* dst = &out[(y * kW + x) * 3]; // BGR8, as the LCD framebuffer
                    dst[0] = static_cast<uint8_t>(texel >> 8);
                    dst[1] = static_cast<uint8_t>(texel >> 16);
                    dst[2] = static_cast<uint8_t>(texel >> 24);
                }
            gPlayerCaptureStarted = p3ds::screenshot::saveBothScreensAsync(path.c_str(), out.data());
        }

        bool beginFrame()
        {
            if (gInFrame)
                return true;
            // Waits for the GPU to finish the previous frame (not for vblank: C3D_FRAME_SYNCDRAW would). A GPU that
            // doesn't finish within 3 s is hung: report it instead of waiting forever.
            if (!waitGpuAndBegin(3000))
            {
                P3DS_LOGE("render3ds: frame %lu (%lu draws, %lu triangles) never finished", (unsigned long)gFrame,
                    (unsigned long)gLastStats.mDraws, (unsigned long)gLastStats.mTriangles);
                gpuHang("the previous frame never finished");
                return false;
            }
            gInFrame = true;
            // Perf batch 1: scenery parts go at 10 pixels when "Small details far away" is Low (MW3DS drops theirs
            // at ~25 pixels across). Actors' parts are never culled this way.
            const float smallPixels = Settings::camera().smallFeaturePixels3DS();
            gSmallFeaturePixels = Settings::camera().mSmallFeatureCulling
                ? std::max(0.f, smallCullBoost() && Settings::camera().mSmallDetails3DS == 0 ? std::max(smallPixels, 10.f)
                                                                                           : smallPixels)
                : 0.f;
#ifdef OPENMW3DS_DEBUG
            {
                static bool censusDone = false;
                gCensusFrame = false;
                if (!censusDone && gLastStats.mDraws > 150 && gFrame % 300 == 0)
                    if (std::FILE* f = std::fopen("sdmc:/openmw/draw-census", "r"))
                    {
                        std::fclose(f);
                        gCensusFrame = censusDone = true;
                    }
            }
#endif
            // Textures the GPU drew into last frame (render-to-texture): finished now, the CPU interface reads them.
            for (auto& [owner, rtt] : gRttTargets)
                if (rtt->mDrawn && rtt->mMapTile)
                {
                    copyMapTile(*rtt);
                    rtt->mDrawn = false;
                }
                else if (rtt->mDrawn)
                {
                    // The CPU's cache may still hold the texture's previous pixels (the CPU interface read it last
                    // frame): the head turning left stale specks (test #87), as captureTarget's reads would.
                    if (rtt->mTex != nullptr)
                        GSPGPU_InvalidateDataCache(rtt->mTex->data, rtt->mTex->size);
                    bumpTexture(rtt->mTex);
                    rtt->mDrawn = false;
                    rtt->mReady = rtt->mTex != nullptr;
                }
            if (gFrame % 60 == 0)
                checkDisplayState();
            if (!gCapturePath.empty())
            {
                captureAll(gCapturePath);
                gCapturePath.clear();
            }
            if (!gPlayerCapturePath.empty())
            {
                capturePlayer(gPlayerCapturePath);
                gPlayerCapturePath.clear();
            }
            ++gFrame;
            gResolvedCount = 0;
            gStats = FrameStats{};
            gBound = Bound{};
            forgetLightSlots();
            gSceneBuffer = nullptr;
            gTransientUsed = 0;
            processPendingDeletes();
            C3D_TexBind(0, nullptr); // the bound C3D_Tex may have been freed with its texture
            if (gSelfTestDone) // the self-test's first steps keep the exact M0 setup: unit 1 never bound
            {
                C3D_TexBind(1, &gUnusedTex1); // never nullptr (see gUnusedTex1)
                gBound.mTexture1 = &gUnusedTex1;
            }
            return true;
        }

        // ------------------------------------------------------------------ MyGUI (M8)

        constexpr uint32_t kGuiMaxVertices = 65536; // 2.3 MB of linear memory at most
        static_assert(kGuiMaxVertices <= kIdentityIndices); // drawTriangleList

        bool gDrawsGui = false;
        GuiBatchSource gGuiSource = nullptr;
        bool gGuiOwnsBottom = false;
        uint64_t gBottomHash = 0; // what the bottom screen shows (0: redraw)
        int gFlatProjection = -1;
        int gFlatModelView = -1;

        struct GuiVertex
        {
            float mPosition[3];
            uint8_t mColor[4];
            float mTexCoord[2];
        };
        static_assert(sizeof(GuiVertex) == 24);
        // In the scene's layout: the GPU's vertex input configuration stays the same all frame (useProgram).
        Vertex* gGuiBuffer = nullptr; // linear memory, grows on demand
        uint32_t gGuiCapacity = 0;

        struct GuiDraw
        {
            uint32_t mFirst;
            uint32_t mCount;
            C3D_Tex* mTexture;
            GuiBlend mBlend;
            uint8_t mScreen; // 0: the top screen's overlay pass (the CPU draws the rest)
        };

        // The interface's triangles clipped to a screen on the CPU (hardware tests #47-#50: quads leaving the
        // render target they are drawn into, the plain fills spanning the canvas, came with flashing meshes and a
        // scrambled top screen on loading, the scissor test alone did not keep the GPU inside the target). A
        // Sutherland-Hodgman pass against the screen's rectangle in canvas clip space. A triangle gives up to 7
        // vertices, drawn as a fan.
        struct ClipVertex
        {
            float mX, mY, mU, mV;
            float mColor[4];
        };
        uint32_t clipTriangle(const ClipVertex in[3], float x0, float y0, float x1, float y1, ClipVertex out[8])
        {
            ClipVertex buffer[8];
            uint32_t n = 3;
            std::copy(in, in + 3, out);
            auto lerp = [](const ClipVertex& a, const ClipVertex& b, float t) {
                ClipVertex r;
                r.mX = a.mX + (b.mX - a.mX) * t;
                r.mY = a.mY + (b.mY - a.mY) * t;
                r.mU = a.mU + (b.mU - a.mU) * t;
                r.mV = a.mV + (b.mV - a.mV) * t;
                for (int k = 0; k < 4; ++k)
                    r.mColor[k] = a.mColor[k] + (b.mColor[k] - a.mColor[k]) * t;
                return r;
            };
            auto pass = [&](auto inside, auto cross) {
                uint32_t m = 0;
                for (uint32_t i = 0; i < n; ++i)
                {
                    const ClipVertex& a = out[i];
                    const ClipVertex& b = out[(i + 1) % n];
                    const bool ia = inside(a), ib = inside(b);
                    if (ia)
                        buffer[m++] = a;
                    if (ia != ib)
                        buffer[m++] = lerp(a, b, cross(a, b));
                }
                std::copy(buffer, buffer + m, out);
                n = m;
            };
            pass([&](const ClipVertex& p) { return p.mX >= x0; },
                [&](const ClipVertex& a, const ClipVertex& b) { return (x0 - a.mX) / (b.mX - a.mX); });
            if (n == 0)
                return 0;
            pass([&](const ClipVertex& p) { return p.mX <= x1; },
                [&](const ClipVertex& a, const ClipVertex& b) { return (x1 - a.mX) / (b.mX - a.mX); });
            if (n == 0)
                return 0;
            pass([&](const ClipVertex& p) { return p.mY >= y0; },
                [&](const ClipVertex& a, const ClipVertex& b) { return (y0 - a.mY) / (b.mY - a.mY); });
            if (n == 0)
                return 0;
            pass([&](const ClipVertex& p) { return p.mY <= y1; },
                [&](const ClipVertex& a, const ClipVertex& b) { return (y1 - a.mY) / (b.mY - a.mY); });
            return n < 3 ? 0 : n;
        }

        std::vector<GuiBatch> gGuiBatches;
        std::vector<GuiDraw> gGuiDraws;

        // Hardware test #26: both screens showed the top target's depth buffer at half size, then garbage. What the
        // render targets and the LCDs are set to, checked about once a second and logged when it changes: a target
        // whose fields change means something overwrote citro3d's memory. An LCD format or stride other than
        // libctru's means the display itself was reconfigured.
        struct TargetState
        {
            const void* mColor = nullptr;
            const void* mDepth = nullptr;
            uint32_t mSize = 0; // width << 16 | height
            uint32_t mFormats = 0; // colour << 8 | depth
            uint32_t mFlags = 0; // transfer flags
            bool mLinked = false;

            bool operator!=(const TargetState& other) const
            {
                return mColor != other.mColor || mDepth != other.mDepth || mSize != other.mSize
                    || mFormats != other.mFormats || mFlags != other.mFlags || mLinked != other.mLinked;
            }
        };

        TargetState stateOf(const C3D_RenderTarget& target)
        {
            TargetState state;
            state.mColor = target.frameBuf.colorBuf;
            state.mDepth = target.frameBuf.depthBuf;
            state.mSize = uint32_t(target.frameBuf.width) << 16 | target.frameBuf.height;
            state.mFormats = uint32_t(target.frameBuf.colorFmt) << 8 | uint32_t(target.frameBuf.depthFmt);
            state.mFlags = target.transferFlags;
            state.mLinked = target.linked;
            return state;
        }

        void checkTarget(const char* name, const C3D_RenderTarget* target, TargetState& known)
        {
            if (target == nullptr)
                return;
            const TargetState now = stateOf(*target);
            if (known.mColor != nullptr && now != known)
                P3DS_LOGE("render3ds: the %s render target changed: colour %p -> %p, depth %p -> %p, size %08lx -> "
                          "%08lx, formats %04lx -> %04lx, transfer %08lx -> %08lx, linked %d -> %d",
                    name, known.mColor, now.mColor, known.mDepth, now.mDepth, (unsigned long)known.mSize,
                    (unsigned long)now.mSize, (unsigned long)known.mFormats, (unsigned long)now.mFormats,
                    (unsigned long)known.mFlags, (unsigned long)now.mFlags, int(known.mLinked), int(now.mLinked));
            known = now;
        }

        // The LCD framebuffer registers (3dbrew "GPU/External Registers": 0x1EF00468 top, 0x1EF00568 bottom):
        // address A, ..., format, ..., select, ..., stride, address B.
        void checkLcds()
        {
            static uint32_t known[2][2] = {}; // format, stride
            static bool logged = false;
            uint32_t regs[2][13] = {};
            for (int screen = 0; screen < 2; ++screen)
                if (R_FAILED(GSPGPU_ReadHWRegs(screen == 0 ? 0x400468 : 0x400568, regs[screen], sizeof(regs[screen]))))
                    return;
            const bool same = known[0][0] == regs[0][2] && known[0][1] == regs[0][10] && known[1][0] == regs[1][2]
                && known[1][1] == regs[1][10];
            if (logged && same)
                return;
            p3ds::log::writef(logged ? p3ds::log::Level::Error : p3ds::log::Level::Info,
                "lcd%s: top format %08lx stride %lu (A %08lx B %08lx select %lu), bottom format %08lx stride %lu "
                "(A %08lx B %08lx select %lu)",
                logged ? " CHANGED" : "", (unsigned long)regs[0][2], (unsigned long)regs[0][10],
                (unsigned long)regs[0][0], (unsigned long)regs[0][11], (unsigned long)regs[0][4],
                (unsigned long)regs[1][2], (unsigned long)regs[1][10], (unsigned long)regs[1][0],
                (unsigned long)regs[1][11], (unsigned long)regs[1][4]);
            for (int screen = 0; screen < 2; ++screen)
            {
                known[screen][0] = regs[screen][2];
                known[screen][1] = regs[screen][10];
            }
            logged = true;
        }

        // The GPU's own framebuffer registers (GPUREG_DEPTHBUFFER_LOC, COLORBUFFER_LOC, FRAMEBUFFER_DIM: 0x11C-0x11E)
        // against the target the last frame ended on: different values mean the GPU's state isn't what citro3d set
        // (reset, or commands it never sent), which would draw into the wrong place or at the wrong size.
        const C3D_RenderTarget* gLastTarget = nullptr;

        void checkGpuFramebuffer()
        {
            if (gLastTarget == nullptr)
                return;
            uint32_t regs[3] = {};
            if (R_FAILED(GSPGPU_ReadHWRegs(0x401000 + 0x11C * 4, regs, sizeof(regs))))
                return;
            const C3D_FrameBuf& fb = gLastTarget->frameBuf;
            const uint32_t expected[3] = { fb.depthBuf ? osConvertVirtToPhys(fb.depthBuf) >> 3 : 0,
                osConvertVirtToPhys(fb.colorBuf) >> 3,
                0x01000000u | ((uint32_t(fb.height) - 1) & 0xFFF) << 12 | (fb.width & 0xFFF) };
            static uint32_t reported[3] = {};
            const bool same = regs[1] == expected[1] && regs[2] == expected[2]
                && (fb.depthBuf == nullptr || regs[0] == expected[0]);
            if (same || std::memcmp(regs, reported, sizeof(regs)) == 0)
                return;
            std::memcpy(reported, regs, sizeof(regs));
            P3DS_LOGE("render3ds: GPU framebuffer registers aren't citro3d's (%s target): depth %08lx (expected %08lx), "
                      "colour %08lx (%08lx), dimensions %08lx (%08lx)",
                gLastTarget == gTarget ? "top" : "bottom", (unsigned long)regs[0], (unsigned long)expected[0],
                (unsigned long)regs[1], (unsigned long)expected[1], (unsigned long)regs[2], (unsigned long)expected[2]);
        }

        // Every draw goes to `target`, and only inside it: the scissor test bounds what the GPU writes to the target's
        // own memory, whatever a draw's vertices are (the render targets lie next to each other in VRAM).
        void drawOn(C3D_RenderTarget* target)
        {
            C3D_FrameDrawOn(target);
            C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, target->frameBuf.width, target->frameBuf.height);
            gLastTarget = target;
        }

        // A debug screenshot (autorun shot:, the no-m8ui screen's button): the top render target (the bottom screen
        // as shown is saved separately, frame3ds.cpp), and the GPU's rasterizer, texturing and framebuffer
        // registers, to tell drawing problems from display ones.
        void captureAll(const std::string& path)
        {
            if (captureTarget(*gTarget, path))
                P3DS_LOGI("render3ds: top screen saved to %s", path.c_str());
            const struct
            {
                uint32_t mFirst, mCount;
            } ranges[] = { { 0x40, 0x30 }, { 0x80, 0x20 }, { 0x100, 0x30 } };
            for (const auto& range : ranges)
                for (uint32_t reg = range.mFirst; reg < range.mFirst + range.mCount; reg += 8)
                {
                    uint32_t v[8] = {};
                    if (R_FAILED(GSPGPU_ReadHWRegs(0x401000 + reg * 4, v, sizeof(v))))
                        return;
                    P3DS_LOGI("gpu regs %03lx: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", (unsigned long)reg,
                        (unsigned long)v[0], (unsigned long)v[1], (unsigned long)v[2], (unsigned long)v[3],
                        (unsigned long)v[4], (unsigned long)v[5], (unsigned long)v[6], (unsigned long)v[7]);
                }
        }

        void checkDisplayState()
        {
            static TargetState top;
            checkTarget("top", gTarget, top);
            checkLcds();
            checkGpuFramebuffer();
        }

        bool ensureGuiCapacity(uint32_t vertices)
        {
            if (vertices <= gGuiCapacity)
                return true;
            uint32_t capacity = std::max<uint32_t>(4096, gGuiCapacity);
            while (capacity < vertices)
                capacity *= 2;
            if (capacity > kGuiMaxVertices)
                return false;
            // The GPU finished the previous frame (beginFrame) and nothing drew from the old buffer in this one.
            gpuFree(gGuiBuffer);
            gGuiBuffer = static_cast<Vertex*>(gpuAlloc(capacity * sizeof(Vertex)));
            gGuiCapacity = gGuiBuffer != nullptr ? capacity : 0;
            if (gGuiBuffer == nullptr)
                P3DS_LOGW("render3ds: no linear memory for %lu GUI vertices", (unsigned long)capacity);
            return gGuiBuffer != nullptr;
        }

        // Copies this frame's batches into the GUI buffer (0: nothing to draw).
        //
        // Texture coordinates: a padded UI texture's are scaled to the image, an image in the UI atlas has them
        // mapped into its atlas rectangle (the CPU interface decodes the atlas once). A batch whose coordinates
        // leave [0, 1] (a tiling skin) keeps the image's own texture, logged once per image.
        // `reusable`: the CPU interface's screens still hold what the last frame's batches drew. Then, if MyGUI's
        // batches are the same as last frame's (textures, blends, vertices), `unchanged` is set and nothing is
        // redone: gGuiDraws and gGuiBuffer still hold last frame's result (M10: most frames change nothing, and
        // converting, comparing and fingerprinting every triangle cost ~7 ms a frame on the console, test #71).
        // M10: set when MyGUI collects its batches again (noteGuiCollected). The fingerprint below folds every
        // vertex word (64-bit arithmetic, ~2% of the main thread in the Zeste profile of 20/10), useless when MyGUI
        // skipped its layout and the batches are last frame's.
        bool gGuiCollected = true;

        uint64_t prepareGuiDraws(bool reusable, bool& unchanged)
        {
            unchanged = false;
            if (reusable && !gGuiCollected)
            {
                unchanged = true;
                return 1;
            }
            gGuiCollected = false;
            gGuiBatches.clear();
            gGuiSource([](const GuiBatch& batch) { gGuiBatches.push_back(batch); });
            {
                // 32-bit arithmetic (M10: 64-bit shifts and adds per vertex word were slow on this CPU).
                static uint32_t lastPrint = 0;
                uint32_t print = 0x9E3779B9u;
                auto fold = [&print](uint32_t value) {
                    print ^= value + 0x9E3779B9u + (print << 6) + (print >> 2);
                };
                for (const GuiBatch& batch : gGuiBatches)
                {
                    fold(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(batch.mTexture)));
                    fold(static_cast<uint32_t>(batch.mBlend) << 24 ^ batch.mCount);
                    const auto* words = static_cast<const uint32_t*>(batch.mVertices);
                    const size_t count = size_t(batch.mCount) * sizeof(GuiVertex) / 4;
                    for (size_t i = 0; i < count; ++i)
                        fold(words[i]);
                }
                if (reusable && print == lastPrint)
                {
                    unchanged = true;
                    return 1;
                }
                lastPrint = print;
            }
            gGuiDraws.clear();
            // Every batch from its own texture (or its UI atlas rectangle), in order, in one list over the whole
            // canvas: the CPU rasterizer clips to each screen itself (rasterizeGui).
            uint32_t total = 0;
            for (const GuiBatch& batch : gGuiBatches)
                total += batch.mCount;
            if (total == 0 || !ensureGuiCapacity(total * 3))
                return 0;

            struct Record
            {
                const GuiVertex* mIn;
                uint32_t mCount;
                C3D_Tex* mTexture;
                GuiBlend mBlend;
                float mScaleU, mScaleV, mOffsetU, mOffsetV;
                bool mTransposed, mFlipV;
            };
            static std::vector<Record> records;
            records.clear();
            static std::set<std::string> loggedTiled;
            constexpr float kUvSlack = 1.f / 1024.f; // MyGUI's coordinates are pixel / size: 0 and 1 exactly
            for (const GuiBatch& batch : gGuiBatches)
            {
                const osg::Image* image
                    = batch.mTexture != nullptr && batch.mTexture->getNumImages() > 0 ? batch.mTexture->getImage(0) : nullptr;
                const auto* cooked = dynamic_cast<const CookedImage*>(image);
                const auto* in = static_cast<const GuiVertex*>(batch.mVertices);
                C3D_Tex* texture = nullptr;
                bool atlased = false;
                bool inside = cooked != nullptr;
                for (uint32_t i = 0; i < batch.mCount && inside; ++i)
                    inside = in[i].mTexCoord[0] >= -kUvSlack && in[i].mTexCoord[0] <= 1.f + kUvSlack
                        && in[i].mTexCoord[1] >= -kUvSlack && in[i].mTexCoord[1] <= 1.f + kUvSlack;
                if (cooked != nullptr && cooked->inAtlas())
                {
                    if (inside)
                    {
                        if (osg::Texture* atlas = uiAtlasTexture(cooked->atlasIndex()))
                            texture = textureFor(*atlas);
                        atlased = texture != nullptr;
                    }
                    else if (loggedTiled.insert(image->getFileName()).second)
                        P3DS_LOGI("render3ds: GUI: %s is drawn tiled (texture coordinates outside [0, 1]): its own "
                                  "texture instead of the UI atlas",
                            image->getFileName().c_str());
                }
                if (!atlased)
                {
                    texture = batch.mTexture != nullptr ? textureFor(*batch.mTexture) : nullptr;
#ifdef OPENMW3DS_DEBUG
                    // What binds a texture of its own in the interface pass (logged once per image).
                    static std::set<const osg::Image*> loggedOwn;
                    if (texture != nullptr && image != nullptr && loggedOwn.insert(image).second)
                        P3DS_LOGI("render3ds: GUI: %s drawn from its own texture (%dx%d%s%s, format %x type %x, first "
                                  "texel %02x%02x%02x%02x)",
                            image->getFileName().empty() ? "(runtime image)" : image->getFileName().c_str(),
                            image->s(), image->t(), cooked != nullptr ? ", cooked" : "",
                            !inside ? ", tiled coordinates" : "", (unsigned)image->getPixelFormat(),
                            (unsigned)image->getDataType(), image->data() ? image->data()[0] : 0,
                            image->data() ? image->data()[1] : 0, image->data() ? image->data()[2] : 0,
                            image->data() ? image->data()[3] : 0);
#endif
                    // A texture the GPU can't show: left out rather than drawn as a plain white quad.
                    if (batch.mTexture != nullptr && texture == nullptr)
                        continue;
                }
                Record record{ in, batch.mCount, texture, batch.mBlend, 1.f, 1.f, 0.f, 0.f, false, false };
                if (atlased)
                {
                    record.mTransposed = cooked->atlasTransposed();
                    const float atlasWidth = float(texture->width), atlasHeight = float(texture->height);
                    record.mOffsetU = float(cooked->atlasX()) / atlasWidth;
                    record.mOffsetV = float(cooked->atlasY()) / atlasHeight;
                    // The image's extent in texels: its source size, or its whole texture when the cooker resampled
                    // it (a side above 256, test #40, then it isn't padded).
                    const float halved = cooked->atlasHalved() ? 0.5f : 1.f;
                    const float extentW = float(cooked->padded() ? cooked->sourceWidth() : cooked->s()) * halved;
                    const float extentH = float(cooked->padded() ? cooked->sourceHeight() : cooked->t()) * halved;
                    record.mScaleU = (record.mTransposed ? extentH : extentW) / atlasWidth;
                    record.mScaleV = (record.mTransposed ? extentW : extentH) / atlasHeight;
                }
                else if (cooked != nullptr && cooked->padded())
                {
                    record.mScaleU = float(cooked->sourceWidth()) / float(cooked->s());
                    record.mScaleV = float(cooked->sourceHeight()) / float(cooked->t());
                }
                // Images made at runtime (MyGUI's font atlases) are uploaded with their first row at t = 0, the
                // cooked ones (which MyGUI's v = 0 at the top expects) flipped: mirror v for the runtime ones.
                record.mFlipV = image != nullptr && cooked == nullptr;
                records.push_back(record);
            }

            // The vertices, batch by batch, in the scene's vertex layout, each triangle clipped to the canvas (clip
            // space [-1, 1]).
            uint32_t used = 0;
            static bool loggedFull = false;
            for (const Record& record : records)
            {
                const uint32_t first = used;
                const GuiVertex* in = record.mIn;
                for (uint32_t t = 0; t + 2 < record.mCount; t += 3)
                {
                    ClipVertex tri[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        const GuiVertex& g = in[t + k];
                        tri[k].mX = g.mPosition[0];
                        tri[k].mY = g.mPosition[1];
                        const float u = record.mTransposed ? g.mTexCoord[1] : g.mTexCoord[0];
                        const float v = record.mTransposed ? g.mTexCoord[0] : g.mTexCoord[1];
                        tri[k].mU = record.mOffsetU + u * record.mScaleU;
                        tri[k].mV = record.mFlipV ? 1.f - g.mTexCoord[1] : record.mOffsetV + v * record.mScaleV;
                        for (int c = 0; c < 4; ++c)
                            tri[k].mColor[c] = float(g.mColor[c]);
                    }
                    ClipVertex poly[8];
                    uint32_t n;
                    const bool inside = tri[0].mX >= -1.f && tri[0].mX <= 1.f && tri[0].mY >= -1.f && tri[0].mY <= 1.f
                        && tri[1].mX >= -1.f && tri[1].mX <= 1.f && tri[1].mY >= -1.f && tri[1].mY <= 1.f
                        && tri[2].mX >= -1.f && tri[2].mX <= 1.f && tri[2].mY >= -1.f && tri[2].mY <= 1.f;
                    if (inside)
                    {
                        std::copy(tri, tri + 3, poly);
                        n = 3;
                    }
                    else
                        n = clipTriangle(tri, -1.f, -1.f, 1.f, 1.f, poly);
                    if (n < 3)
                        continue;
                    const uint32_t triangles = n - 2;
                    if (used + triangles * 3 > gGuiCapacity)
                    {
                        if (!loggedFull)
                            P3DS_LOGW("render3ds: GUI vertex buffer full while clipping: some quads left out");
                        loggedFull = true;
                        break;
                    }
                    const float z = in[t].mPosition[2];
                    for (uint32_t f = 0; f < triangles; ++f)
                    {
                        const ClipVertex* src[3] = { &poly[0], &poly[f + 1], &poly[f + 2] };
                        for (int k = 0; k < 3; ++k)
                        {
                            Vertex& out = gGuiBuffer[used++];
                            out.mPosition[0] = src[k]->mX;
                            out.mPosition[1] = src[k]->mY;
                            out.mPosition[2] = z;
                            out.mNormal[0] = out.mNormal[1] = 0.f;
                            out.mNormal[2] = 1.f;
                            for (int c = 0; c < 4; ++c)
                                out.mColor[c] = static_cast<uint8_t>(std::clamp(src[k]->mColor[c] + 0.5f, 0.f, 255.f));
                            out.mTexCoord[0] = src[k]->mU;
                            out.mTexCoord[1] = src[k]->mV;
                        }
                    }
                }
                if (used > first)
                    gGuiDraws.push_back(GuiDraw{ first, used - first, record.mTexture, record.mBlend, 0 });
            }
            flushToGpu(gGuiBuffer, used * sizeof(Vertex));
            return 1; // never 0 ("redraw")
        }

        // Draws the GUI's draws for one screen. The canvas's clip space (x right, y up) maps to the screen's by
        // x_s = scaleX * x, y_s = 2 * y + offsetY, then the screens' 90-degree rotation (x' = y_s, y' = -x_s, as
        // setProjection) and the fixed depth z' = -0.5 (no depth test).
        void drawGuiOn(C3D_RenderTarget* target, float scaleX, float offsetY, int screen)
        {
            drawOn(target);
            C3D_FVec* projection = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gFlatProjection, 4);
            projection[0] = FVec4_New(0.f, 2.f, 0.f, offsetY);
            projection[1] = FVec4_New(-scaleX, 0.f, 0.f, 0.f);
            projection[2] = FVec4_New(0.f, 0.f, 0.f, -0.5f);
            projection[3] = FVec4_New(0.f, 0.f, 0.f, 1.f);
            C3D_FVec* modelView = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gFlatModelView, 4);
            modelView[0] = FVec4_New(1.f, 0.f, 0.f, 0.f);
            modelView[1] = FVec4_New(0.f, 1.f, 0.f, 0.f);
            modelView[2] = FVec4_New(0.f, 0.f, 1.f, 0.f);
            modelView[3] = FVec4_New(0.f, 0.f, 0.f, 1.f);

            C3D_Tex* bound = reinterpret_cast<C3D_Tex*>(1);
            int blend = -1;
            for (const GuiDraw& draw : gGuiDraws)
            {
                if (draw.mScreen != screen)
                    continue;
                if (draw.mTexture != bound)
                {
                    bound = draw.mTexture;
                    if (bound != nullptr)
                        C3D_TexBind(0, bound);
                    gBound.mTexEnv = -1;
                    setTexEnv(bound != nullptr ? TexEnv_Textured : TexEnv_VertexColor);
                }
                if (static_cast<int>(draw.mBlend) != blend)
                {
                    blend = static_cast<int>(draw.mBlend);
                    switch (draw.mBlend)
                    {
                        case GuiBlend::Additive:
                            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_SRC_ALPHA, GPU_ONE);
                            break;
                        case GuiBlend::Premultiplied:
                            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
                                GPU_ONE_MINUS_SRC_ALPHA);
                            break;
                        default:
                            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                                GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
                            break;
                    }
                }
                drawTriangleList(draw.mFirst, draw.mCount);
                ++gStats.mGuiDraws;
            }
        }

        // ------------------------------------------------------------------ the interface drawn by the CPU

        // The flashing meshes (tests #34-#41): with the interface drawn by the GPU, a frame's run of nearby meshes
        // was drawn wrong at random. So the CPU draws the interface (MyGUI's batches, the same layouts and
        // textures): straight into the bottom screen's framebuffer, and into one overlay texture for the top screen
        // that the GPU draws as a single quad. Each screen is redrawn only when what it shows changes. The GPU path
        // (tests #47-#55) was deprecated on 18/10/2026 and removed in M20.
        struct Texel
        {
            int r, g, b, a;
        };

        // One texel of a PICA texture (level 0), (x, y) in memory order. The GPU reads texture coordinate v at row
        // height - 1 - v * height (rasterizeGui).
        Texel fetchTexel(const C3D_Tex& tex, unsigned x, unsigned y)
        {
            const auto* base = static_cast<const uint8_t*>(tex.data);
            const unsigned w = tex.width;
            const GPU_TEXCOLOR fmt = static_cast<GPU_TEXCOLOR>(tex.fmt);
            if (fmt == GPU_ETC1 || fmt == GPU_ETC1A4)
            {
                // 8x8 tiles of four 4x4 blocks (Z order), each a little-endian 64-bit ETC1 word, after a 64-bit
                // block of 4-bit alphas for ETC1A4. Pixel i = column * 4 + row inside the block.
                const bool alpha = fmt == GPU_ETC1A4;
                const unsigned blockBytes = alpha ? 16 : 8;
                const unsigned tile = (y / 8) * (w / 8) + x / 8;
                const unsigned sub = ((y % 8) / 4) * 2 + (x % 8) / 4;
                const uint8_t* block = base + (tile * 4 + sub) * blockBytes;
                const unsigned px = x % 4, py = y % 4;
                const unsigned i = px * 4 + py;
                int a = 255;
                if (alpha)
                {
                    uint64_t alphas;
                    std::memcpy(&alphas, block, 8);
                    a = int((alphas >> (4 * i)) & 0xF) * 17;
                    block += 8;
                }
                uint64_t bits;
                std::memcpy(&bits, block, 8);
                const uint32_t hi = uint32_t(bits >> 32), lo = uint32_t(bits);
                const bool flip = (hi & 1) != 0, diff = (hi & 2) != 0;
                const bool second = flip ? py >= 2 : px >= 2;
                int c[3];
                for (int k = 0; k < 3; ++k)
                {
                    const int shift = 27 - 8 * k; // R at 27, G at 19, B at 11 (5-bit base, 3-bit delta below)
                    if (diff)
                    {
                        int v = int((hi >> shift) & 31);
                        if (second)
                        {
                            int d = int((hi >> (shift - 3)) & 7);
                            if (d >= 4)
                                d -= 8;
                            v += d;
                        }
                        v &= 31;
                        c[k] = (v << 3) | (v >> 2);
                    }
                    else
                    {
                        const int v = int((hi >> (second ? shift - 3 : shift + 1)) & 15);
                        c[k] = v * 17;
                    }
                }
                static const int kTable[8][2] = { { 2, 8 }, { 5, 17 }, { 9, 29 }, { 13, 42 }, { 18, 60 }, { 24, 80 },
                    { 33, 106 }, { 47, 183 } };
                const unsigned table = second ? (hi >> 2) & 7 : (hi >> 5) & 7;
                const unsigned msb = (lo >> (16 + i)) & 1, lsb = (lo >> i) & 1;
                const int step = kTable[table][lsb];
                const int mod = msb ? -step : step;
                return { std::clamp(c[0] + mod, 0, 255), std::clamp(c[1] + mod, 0, 255), std::clamp(c[2] + mod, 0, 255),
                    a };
            }
            const unsigned texel = ((y / 8) * (w / 8) + x / 8) * 64 + morton8(x % 8, y % 8);
            switch (fmt)
            {
                case GPU_RGBA8:
                {
                    const uint8_t* p = base + texel * 4; // A, B, G, R
                    return { p[3], p[2], p[1], p[0] };
                }
                case GPU_RGB8:
                {
                    const uint8_t* p = base + texel * 3; // B, G, R
                    return { p[2], p[1], p[0], 255 };
                }
                case GPU_RGBA5551:
                case GPU_RGB565:
                case GPU_RGBA4:
                case GPU_LA8:
                {
                    uint16_t v;
                    std::memcpy(&v, base + texel * 2, 2);
                    if (fmt == GPU_RGBA5551)
                        return { int((v >> 11) & 31) * 255 / 31, int((v >> 6) & 31) * 255 / 31,
                            int((v >> 1) & 31) * 255 / 31, (v & 1) ? 255 : 0 };
                    if (fmt == GPU_RGB565)
                        return { int((v >> 11) & 31) * 255 / 31, int((v >> 5) & 63) * 255 / 63, int(v & 31) * 255 / 31,
                            255 };
                    if (fmt == GPU_RGBA4)
                        return { int((v >> 12) & 15) * 17, int((v >> 8) & 15) * 17, int((v >> 4) & 15) * 17,
                            int(v & 15) * 17 };
                    const int l = v >> 8; // LA8: A, L
                    return { l, l, l, int(v & 0xFF) };
                }
                case GPU_L8:
                {
                    const int l = base[texel];
                    return { l, l, l, 255 };
                }
                case GPU_A8:
                    return { 0, 0, 0, base[texel] };
                case GPU_LA4:
                {
                    const int v = base[texel];
                    return { (v >> 4) * 17, (v >> 4) * 17, (v >> 4) * 17, (v & 15) * 17 };
                }
                default:
                    return { 255, 0, 255, 255 }; // a format the interface doesn't use
            }
        }

        // Interface textures unpacked once for the CPU: RGBA8 words (r, g, b, a from the low byte), row r being
        // what the GPU samples for v in [r / height, (r + 1) / height). Dropped when a runtime texture changes
        // (gTextureGeneration), when they pass 3 MB, or at a loading screen.
        struct DecodedTexture
        {
            const void* mData = nullptr;
            uint16_t mWidth = 0, mHeight = 0;
            uint32_t mGeneration = 0; // texGeneration() when decoded
            std::vector<uint32_t> mPixels;
        };
        std::unordered_map<const C3D_Tex*, DecodedTexture> gDecoded;
        size_t gDecodedBytes = 0;

        const DecodedTexture& decodedTexture(const C3D_Tex& tex)
        {
            if (gDecodedBytes > (3u << 20))
            {
                gDecoded.clear();
                gDecodedBytes = 0;
            }
            DecodedTexture& decoded = gDecoded[&tex];
            const uint32_t generation = texGeneration(&tex);
            if (decoded.mData == tex.data && decoded.mWidth == tex.width && decoded.mHeight == tex.height
                && decoded.mGeneration == generation)
                return decoded;
            decoded.mGeneration = generation;
            decoded.mData = tex.data;
            decoded.mWidth = tex.width;
            decoded.mHeight = tex.height;
            decoded.mPixels.resize(size_t(tex.width) * tex.height);
            for (unsigned y = 0; y < tex.height; ++y)
                for (unsigned x = 0; x < tex.width; ++x)
                {
                    const Texel t = fetchTexel(tex, x, tex.height - 1 - y);
                    decoded.mPixels[size_t(y) * tex.width + x]
                        = uint32_t(t.r) | uint32_t(t.g) << 8 | uint32_t(t.b) << 16 | uint32_t(t.a) << 24;
                }
            gDecodedBytes += decoded.mPixels.size() * 4;
            return decoded;
        }

        struct PixelRect
        {
            int mX0 = 0, mY0 = 0, mX1 = 0, mY1 = 0; // [x0, x1) x [y0, y1)

            bool empty() const { return mX0 >= mX1 || mY0 >= mY1; }
            void add(const PixelRect& other)
            {
                if (other.empty())
                    return;
                if (empty())
                {
                    *this = other;
                    return;
                }
                mX0 = std::min(mX0, other.mX0);
                mY0 = std::min(mY0, other.mY0);
                mX1 = std::max(mX1, other.mX1);
                mY1 = std::max(mY1, other.mY1);
            }
            PixelRect clipped(const PixelRect& clip) const
            {
                return { std::max(mX0, clip.mX0), std::max(mY0, clip.mY0), std::min(mX1, clip.mX1),
                    std::min(mY1, clip.mY1) };
            }
        };

        // a * b / 255 without a division (the ARM11 has no divide instruction).
        inline uint32_t mul8(uint32_t a, uint32_t b)
        {
            const uint32_t t = a * b + 128;
            return (t + (t >> 8)) >> 8;
        }

        // Blends a straight-alpha colour into a premultiplied pixel (r, g, b, a from the low byte), as the GPU
        // would with MyGUI's blend modes (see rasterizeGui).
        inline void blendPixel(uint32_t& dst, uint32_t r, uint32_t g, uint32_t b, uint32_t a, GuiBlend mode)
        {
            uint32_t dr = dst & 0xFF, dg = (dst >> 8) & 0xFF, db = (dst >> 16) & 0xFF, da = dst >> 24;
            switch (mode)
            {
                case GuiBlend::Additive:
                    dr = std::min(255u, dr + mul8(r, a));
                    dg = std::min(255u, dg + mul8(g, a));
                    db = std::min(255u, db + mul8(b, a));
                    break;
                case GuiBlend::Premultiplied:
                {
                    const uint32_t keep = 255 - a;
                    dr = std::min(255u, r + mul8(dr, keep));
                    dg = std::min(255u, g + mul8(dg, keep));
                    db = std::min(255u, b + mul8(db, keep));
                    da = a + mul8(da, keep);
                    break;
                }
                default:
                {
                    if (a == 255)
                    {
                        dst = r | g << 8 | b << 16 | 0xFF000000u;
                        return;
                    }
                    const uint32_t keep = 255 - a;
                    dr = mul8(r, a) + mul8(dr, keep);
                    dg = mul8(g, a) + mul8(dg, keep);
                    db = mul8(b, a) + mul8(db, keep);
                    da = a + mul8(da, keep);
                    break;
                }
            }
            dst = dr | dg << 8 | db << 16 | da << 24;
        }

        // A premultiplied RGBA canvas of one screen (origin: its top left in GUI canvas pixels), kept between frames
        // so that only what changed is drawn again.
        struct SoftTarget
        {
            int mWidth, mHeight, mOriginX, mOriginY;
            std::vector<uint32_t> mPixels;
            // What was drawn last time (to find what changed): each triangle touching this screen, as a signature
            // (vertices, texture, blend) and its box.
            std::vector<std::pair<uint64_t, PixelRect>> mPrevTriangles;
            uint32_t mPrevGeneration = 0;
            bool mValid = false;
            // Stats for the log: redraws and their time since the last log line.
            unsigned mRedraws = 0;
            double mRedrawMs = 0, mWorstMs = 0;
            PixelRect mLastRect;

            PixelRect bounds() const { return { 0, 0, mWidth, mHeight }; }
        };

        constexpr float kHalfCanvasW = kGuiCanvasWidth * 0.5f, kHalfCanvasH = kGuiCanvasHeight * 0.5f;

        inline float screenX(const Vertex& v, const SoftTarget& target)
        {
            return (v.mPosition[0] + 1.f) * kHalfCanvasW - float(target.mOriginX);
        }

        inline float screenY(const Vertex& v, const SoftTarget& target)
        {
            return (1.f - v.mPosition[1]) * kHalfCanvasH - float(target.mOriginY);
        }

        // The pixels a triangle can touch on `target`, rounded out by one.
        PixelRect triangleRect(const Vertex* v, const SoftTarget& target)
        {
            float x0 = screenX(v[0], target), x1 = x0, y0 = screenY(v[0], target), y1 = y0;
            for (int k = 1; k < 3; ++k)
            {
                const float x = screenX(v[k], target), y = screenY(v[k], target);
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
            return { int(std::floor(x0)) - 1, int(std::floor(y0)) - 1, int(std::ceil(x1)) + 1, int(std::ceil(y1)) + 1 };
        }

        // Pixel centres of [a, b): the first and one past the last index.
        inline void pixelSpan(float a, float b, int& first, int& end)
        {
            first = int(std::ceil(a - 0.5f));
            end = int(std::ceil(b - 0.5f));
        }

        // Two triangles (6 vertices) that make an axis-aligned rectangle whose texture coordinates follow the axes
        // and whose vertex colours are all the same: nearly every MyGUI quad. Drawn row by row.
        bool drawRectQuad(const Vertex* v, const C3D_Tex* tex, GuiBlend blend, SoftTarget& target, const PixelRect& clip)
        {
            float xs[2], ys[2], us[2], vs[2];
            int nx = 0, ny = 0;
            for (int i = 0; i < 6; ++i)
            {
                if (std::memcmp(v[i].mColor, v[0].mColor, 4) != 0)
                    return false;
                const float x = screenX(v[i], target), y = screenY(v[i], target);
                int ix = 0;
                while (ix < nx && std::fabs(xs[ix] - x) > 1e-3f)
                    ++ix;
                if (ix == nx)
                {
                    if (nx == 2)
                        return false;
                    xs[nx] = x;
                    us[nx++] = v[i].mTexCoord[0];
                }
                else if (std::fabs(us[ix] - v[i].mTexCoord[0]) > 1e-5f)
                    return false;
                int iy = 0;
                while (iy < ny && std::fabs(ys[iy] - y) > 1e-3f)
                    ++iy;
                if (iy == ny)
                {
                    if (ny == 2)
                        return false;
                    ys[ny] = y;
                    vs[ny++] = v[i].mTexCoord[1];
                }
                else if (std::fabs(vs[iy] - v[i].mTexCoord[1]) > 1e-5f)
                    return false;
            }
            if (nx != 2 || ny != 2)
                return false;
            if (xs[0] > xs[1])
            {
                std::swap(xs[0], xs[1]);
                std::swap(us[0], us[1]);
            }
            if (ys[0] > ys[1])
            {
                std::swap(ys[0], ys[1]);
                std::swap(vs[0], vs[1]);
            }
            int px0, px1, py0, py1;
            pixelSpan(xs[0], xs[1], px0, px1);
            pixelSpan(ys[0], ys[1], py0, py1);
            px0 = std::max(px0, clip.mX0);
            px1 = std::min(px1, clip.mX1);
            py0 = std::max(py0, clip.mY0);
            py1 = std::min(py1, clip.mY1);
            if (px0 >= px1 || py0 >= py1)
                return true;
            const uint32_t cr = v[0].mColor[0], cg = v[0].mColor[1], cb = v[0].mColor[2], ca = v[0].mColor[3];
            if (tex == nullptr)
            {
                if (ca == 0 && blend != GuiBlend::Premultiplied)
                    return true;
                for (int py = py0; py < py1; ++py)
                {
                    uint32_t* row = target.mPixels.data() + size_t(py) * target.mWidth;
                    for (int px = px0; px < px1; ++px)
                        blendPixel(row[px], cr, cg, cb, ca, blend);
                }
                return true;
            }
            const DecodedTexture& decoded = decodedTexture(*tex);
            const int w = decoded.mWidth, h = decoded.mHeight;
            // Texel column of each pixel column (nearest, as for the triangles).
            static std::vector<int> columns;
            columns.resize(size_t(px1 - px0));
            const float du = (us[1] - us[0]) / (xs[1] - xs[0]);
            for (int px = px0; px < px1; ++px)
                columns[size_t(px - px0)]
                    = std::clamp(int(std::floor((us[0] + (px + 0.5f - xs[0]) * du) * w)), 0, w - 1);
            const float dv = (vs[1] - vs[0]) / (ys[1] - ys[0]);
            const bool white = cr == 255 && cg == 255 && cb == 255 && ca == 255;
            for (int py = py0; py < py1; ++py)
            {
                const int ty = std::clamp(int(std::floor((vs[0] + (py + 0.5f - ys[0]) * dv) * h)), 0, h - 1);
                const uint32_t* texRow = decoded.mPixels.data() + size_t(ty) * w;
                uint32_t* row = target.mPixels.data() + size_t(py) * target.mWidth;
                for (int px = px0; px < px1; ++px)
                {
                    const uint32_t t = texRow[columns[size_t(px - px0)]];
                    uint32_t r = t & 0xFF, g = (t >> 8) & 0xFF, b = (t >> 16) & 0xFF, a = t >> 24;
                    if (!white)
                    {
                        r = mul8(r, cr);
                        g = mul8(g, cg);
                        b = mul8(b, cb);
                        a = mul8(a, ca);
                    }
                    if (a == 0 && blend != GuiBlend::Premultiplied)
                        continue;
                    blendPixel(row[px], r, g, b, a, blend);
                }
            }
            return true;
        }

        // Any triangle (the rotated ones: the compass needle), clipped to `clip`: the modulate texenv (texel x
        // vertex colour), nearest texel, pixel centres, the top-left fill rule (a quad's two triangles never both
        // cover a pixel).
        void drawTriangle(const Vertex* tri, const C3D_Tex* tex, GuiBlend blend, SoftTarget& target, const PixelRect& clip)
        {
            float sx[3], sy[3];
            for (int k = 0; k < 3; ++k)
            {
                sx[k] = screenX(tri[k], target);
                sy[k] = screenY(tri[k], target);
            }
            float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sy[1] - sy[0]) * (sx[2] - sx[0]);
            if (std::fabs(area) < 1e-6f)
                return;
            int order[3] = { 0, 1, 2 };
            if (area < 0)
            {
                std::swap(order[1], order[2]);
                area = -area;
            }
            const float x0 = sx[order[0]], y0 = sy[order[0]], x1 = sx[order[1]], y1 = sy[order[1]], x2 = sx[order[2]],
                        y2 = sy[order[2]];
            const int minX = std::max(clip.mX0, int(std::floor(std::min({ x0, x1, x2 }))));
            const int maxX = std::min(clip.mX1 - 1, int(std::ceil(std::max({ x0, x1, x2 }))));
            const int minY = std::max(clip.mY0, int(std::floor(std::min({ y0, y1, y2 }))));
            const int maxY = std::min(clip.mY1 - 1, int(std::ceil(std::max({ y0, y1, y2 }))));
            if (minX > maxX || minY > maxY)
                return;
            auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
                return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
            };
            auto topLeft = [](float ax, float ay, float bx, float by) { return (ay == by && bx < ax) || by > ay; };
            const bool tl0 = topLeft(x1, y1, x2, y2), tl1 = topLeft(x2, y2, x0, y0), tl2 = topLeft(x0, y0, x1, y1);
            const Vertex& a = tri[order[0]];
            const Vertex& b = tri[order[1]];
            const Vertex& c = tri[order[2]];
            const DecodedTexture* decoded = tex != nullptr ? &decodedTexture(*tex) : nullptr;
            const float inv = 1.f / area;
            for (int py = minY; py <= maxY; ++py)
            {
                uint32_t* row = target.mPixels.data() + size_t(py) * target.mWidth;
                const float cy = py + 0.5f;
                for (int px = minX; px <= maxX; ++px)
                {
                    const float cx = px + 0.5f;
                    const float w0 = edge(x1, y1, x2, y2, cx, cy);
                    const float w1 = edge(x2, y2, x0, y0, cx, cy);
                    const float w2 = edge(x0, y0, x1, y1, cx, cy);
                    if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !tl0) || (w1 == 0 && !tl1) || (w2 == 0 && !tl2))
                        continue;
                    const float l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
                    uint32_t col[4];
                    for (int k = 0; k < 4; ++k)
                        col[k] = uint32_t(l0 * a.mColor[k] + l1 * b.mColor[k] + l2 * c.mColor[k] + 0.5f);
                    if (decoded != nullptr)
                    {
                        const float u = l0 * a.mTexCoord[0] + l1 * b.mTexCoord[0] + l2 * c.mTexCoord[0];
                        const float vv = l0 * a.mTexCoord[1] + l1 * b.mTexCoord[1] + l2 * c.mTexCoord[1];
                        const int tx = std::clamp(int(std::floor(u * decoded->mWidth)), 0, decoded->mWidth - 1);
                        const int ty = std::clamp(int(std::floor(vv * decoded->mHeight)), 0, decoded->mHeight - 1);
                        const uint32_t t = decoded->mPixels[size_t(ty) * decoded->mWidth + tx];
                        col[0] = mul8(col[0], t & 0xFF);
                        col[1] = mul8(col[1], (t >> 8) & 0xFF);
                        col[2] = mul8(col[2], (t >> 16) & 0xFF);
                        col[3] = mul8(col[3], t >> 24);
                    }
                    if (col[3] == 0 && blend != GuiBlend::Premultiplied)
                        continue;
                    blendPixel(row[px], col[0], col[1], col[2], col[3], blend);
                }
            }
        }

        // Draws this frame's GUI draws (gGuiDraws over gGuiBuffer, as prepareGuiDraws left them) into `target`,
        // only inside `clip` (cleared first).
        void rasterizeGui(SoftTarget& target, const PixelRect& clip)
        {
            for (int y = clip.mY0; y < clip.mY1; ++y)
                std::fill_n(target.mPixels.data() + size_t(y) * target.mWidth + clip.mX0, clip.mX1 - clip.mX0, 0u);
            for (const GuiDraw& draw : gGuiDraws)
            {
                const Vertex* v = gGuiBuffer + draw.mFirst;
                uint32_t t = 0;
                while (t + 2 < draw.mCount)
                {
                    if (t + 5 < draw.mCount)
                    {
                        // Quick reject before the quad test: both triangles' boxes outside the clip.
                        PixelRect box = triangleRect(v + t, target);
                        box.add(triangleRect(v + t + 3, target));
                        if (box.clipped(clip).empty())
                        {
                            t += 6;
                            continue;
                        }
                        if (drawRectQuad(v + t, draw.mTexture, draw.mBlend, target, clip))
                        {
                            t += 6;
                            continue;
                        }
                    }
                    if (!triangleRect(v + t, target).clipped(clip).empty())
                        drawTriangle(v + t, draw.mTexture, draw.mBlend, target, clip);
                    t += 3;
                }
            }
        }

        // This frame's triangles touching `target`: signature (vertices, texture, blend) and box.
        void collectTriangles(const SoftTarget& target, std::vector<std::pair<uint64_t, PixelRect>>& out)
        {
            out.clear();
            const PixelRect all = target.bounds();
            for (const GuiDraw& draw : gGuiDraws)
            {
                const Vertex* v = gGuiBuffer + draw.mFirst;
                // 32-bit FNV-1a (M10: 64-bit multiplies are slow on this CPU, a collision only risks a missed redraw).
                const uint32_t textureState
                    = (uint32_t(reinterpret_cast<uintptr_t>(draw.mTexture)) * 2654435761u) ^ texGeneration(draw.mTexture);
                for (uint32_t t = 0; t + 2 < draw.mCount; t += 3)
                {
                    const PixelRect rect = triangleRect(v + t, target).clipped(all);
                    if (rect.empty())
                        continue;
                    uint32_t hash = 2166136261u;
                    const auto* words = reinterpret_cast<const uint32_t*>(v + t);
                    for (size_t i = 0; i < 3 * sizeof(Vertex) / 4; ++i)
                        hash = (hash ^ words[i]) * 16777619u;
                    hash = (hash ^ textureState) * 16777619u;
                    hash = (hash ^ static_cast<uint32_t>(draw.mBlend)) * 16777619u;
                    out.emplace_back(hash, rect);
                }
            }
        }

        // What changed on `target` since its last redraw: the boxes of the triangles that appeared or went away,
        // whatever the draws they're grouped in (prepareGuiDraws regroups batches as the interface changes, the
        // order of triangles that don't overlap doesn't change the picture). Everything after a runtime texture
        // changed. Leaves this frame's triangles as the reference for the next one.
        PixelRect changedRect(SoftTarget& target)
        {
            static std::vector<std::pair<uint64_t, PixelRect>> now;
            collectTriangles(target, now);
            // Sorted by hash (mPrevTriangles was sorted the same way): a merge walk pairs equal hashes and leaves the
            // triangles that appeared or went away (M10: an unordered_map divided by its bucket count per triangle).
            std::sort(now.begin(), now.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            PixelRect dirty;
            if (!target.mValid) // a changed texture shows in its triangles' hashes (collectTriangles)
                dirty = target.bounds();
            else
            {
                const auto& prev = target.mPrevTriangles;
                size_t i = 0, j = 0;
                while (i < prev.size() && j < now.size())
                {
                    if (prev[i].first == now[j].first)
                    {
                        ++i;
                        ++j;
                    }
                    else if (prev[i].first < now[j].first)
                        dirty.add(prev[i++].second);
                    else
                        dirty.add(now[j++].second);
                }
                for (; i < prev.size(); ++i)
                    dirty.add(prev[i].second);
                for (; j < now.size(); ++j)
                    dirty.add(now[j].second);
            }
            target.mPrevTriangles.swap(now);
            target.mPrevGeneration = gTextureGeneration;
            target.mValid = true;
            return dirty;
        }

        SoftTarget gSoftTop{ kGuiCanvasWidth, kGuiCanvasHeight / 2, 0, 0, {} };
        SoftTarget gSoftBottom{ kGuiCanvasWidth - 2 * kGuiBottomX, kGuiCanvasHeight / 2, kGuiBottomX, kGuiCanvasHeight / 2, {} };
        bool gSoftTopEmpty = true;
        C3D_Tex gOverlay{}; // the top screen's interface: 512x256 RGBA8, premultiplied (gSoftTop at its top left)
        bool gOverlayReady = false;

        // How long the screens' redraws take on the CPU: one log line per screen at most every 5 s.
        void noteSoftRedraw(const char* screen, SoftTarget& target, const PixelRect& rect, uint64_t start)
        {
            const double ms = double(svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC;
            ++target.mRedraws;
            target.mRedrawMs += ms;
            target.mWorstMs = std::max(target.mWorstMs, ms);
            target.mLastRect = rect;
            static uint64_t lastTop = 0, lastBottom = 0;
            uint64_t& last = screen[0] == 't' ? lastTop : lastBottom;
            const uint64_t now = svcGetSystemTick();
            if (now - last < 5 * uint64_t(SYSCLOCK_ARM11))
                return;
            last = now;
            P3DS_LOGI("render3ds: CPU interface, %s screen: %u redraws, avg %.1f ms, worst %.1f ms; last one %dx%d at "
                      "(%d, %d)",
                screen, target.mRedraws, target.mRedrawMs / target.mRedraws, target.mWorstMs,
                rect.mX1 - rect.mX0, rect.mY1 - rect.mY0, rect.mX0, rect.mY0);
            target.mRedraws = 0;
            target.mRedrawMs = 0;
            target.mWorstMs = 0;
        }

        // The bottom screen: `rect` of the canvas over black, into the LCD framebuffer (RGB565, column-major).
        void presentSoftBottom(const PixelRect& rect)
        {
            uint16_t* fb = p3ds::textfb::framebuffer();
            const int w = gSoftBottom.mWidth, h = gSoftBottom.mHeight;
            for (int x = rect.mX0; x < rect.mX1; ++x)
            {
                uint16_t* col = fb + x * h;
                for (int y = rect.mY0; y < rect.mY1; ++y)
                {
                    const uint32_t p = gSoftBottom.mPixels[size_t(y) * w + x];
                    col[h - 1 - y] = uint16_t((p & 0xF8) << 8 | (p >> 5 & 0x7E0) | (p >> 19 & 0x1F));
                }
            }
            GSPGPU_FlushDataCache(fb, w * h * sizeof(uint16_t));
        }

        // `rect` of gSoftTop into the overlay texture (v = 0 at the screen's top row, stored as the GPU reads it).
        bool uploadSoftTop(const PixelRect& rect)
        {
            if (!gOverlayReady)
            {
                void* data = gpuAlloc(512 * 256 * 4);
                if (data == nullptr)
                    return false;
                gOverlay = C3D_Tex{};
                gOverlay.data = data;
                gOverlay.fmt = GPU_RGBA8;
                gOverlay.size = 512 * 256 * 4;
                gOverlay.width = 512;
                gOverlay.height = 256;
                gOverlay.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
                C3D_TexSetFilter(&gOverlay, GPU_NEAREST, GPU_NEAREST);
                C3D_TexSetWrap(&gOverlay, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
                std::memset(data, 0, gOverlay.size);
                gOverlayReady = true;
            }
            auto* out = static_cast<uint32_t*>(gOverlay.data);
            const int w = gSoftTop.mWidth;
            for (int y = rect.mY0; y < rect.mY1; ++y)
            {
                const int ty = 255 - y; // the GPU reads texture rows bottom up (fetchTexel)
                for (int x = rect.mX0; x < rect.mX1; ++x)
                {
                    const uint32_t p = gSoftTop.mPixels[size_t(y) * w + x]; // r, g, b, a from the low byte
                    // PICA RGBA8 texel: the word 0xRRGGBBAA.
                    out[((ty / 8) * (512 / 8) + x / 8) * 64 + morton8(x % 8, ty % 8)]
                        = (p & 0xFF) << 24 | (p >> 8 & 0xFF) << 16 | (p >> 16 & 0xFF) << 8 | p >> 24;
                }
            }
            flushToGpu(gOverlay.data, gOverlay.size);
            return true;
        }

        // CPU interface for this frame (gGuiDraws prepared). Leaves gGuiDraws as the GPU's work: nothing, or the
        // top overlay as one quad (6 vertices at the start of gGuiBuffer, premultiplied blend).
        void drawSoftGui()
        {
            const PixelRect topRect = changedRect(gSoftTop);
            if (!topRect.empty())
            {
                const uint64_t start = svcGetSystemTick();
                if (gSoftTop.mPixels.empty())
                    gSoftTop.mPixels.resize(size_t(gSoftTop.mWidth) * gSoftTop.mHeight);
                rasterizeGui(gSoftTop, topRect);
                gSoftTopEmpty = std::all_of(
                    gSoftTop.mPixels.begin(), gSoftTop.mPixels.end(), [](uint32_t value) { return value == 0; });
                // Uploaded even when the screen just emptied: the texture must lose the closed window too, or
                // the next draw on the top screen shows the window's remains around it (test #63).
                if (!uploadSoftTop(topRect))
                    gSoftTopEmpty = true;
                noteSoftRedraw("top", gSoftTop, topRect, start);
            }

            if (gGuiOwnsBottom)
            {
                if (gBottomHash == 0)
                    gSoftBottom.mValid = false; // the bottom screen was someone else's: everything again
                const PixelRect bottomRect = changedRect(gSoftBottom);
                if (!bottomRect.empty())
                {
                    const uint64_t start = svcGetSystemTick();
                    if (gSoftBottom.mPixels.empty())
                        gSoftBottom.mPixels.resize(size_t(gSoftBottom.mWidth) * gSoftBottom.mHeight);
                    rasterizeGui(gSoftBottom, bottomRect);
                    presentSoftBottom(bottomRect);
                    noteSoftRedraw("bottom", gSoftBottom, bottomRect, start);
                    ++gStats.mGuiBottomFrames;
                }
                gBottomHash = 1;
            }

            gGuiDraws.clear();
            if (gSoftTopEmpty)
                return;
            // The overlay quad over the whole top screen: canvas x in [-1, 1], y in [0, 1]. Texels 1:1.
            const float u1 = float(gSoftTop.mWidth) / 512.f, v1 = float(gSoftTop.mHeight) / 256.f;
            const float corners[4][4] = { { -1.f, 1.f, 0.f, 0.f }, { 1.f, 1.f, u1, 0.f }, { 1.f, 0.f, u1, v1 },
                { -1.f, 0.f, 0.f, v1 } };
            static const int kOrder[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 6; ++i)
            {
                Vertex& out = gGuiBuffer[i];
                const float* c = corners[kOrder[i]];
                out.mPosition[0] = c[0];
                out.mPosition[1] = c[1];
                out.mPosition[2] = 0.f;
                out.mNormal[0] = out.mNormal[1] = 0.f;
                out.mNormal[2] = 1.f;
                out.mColor[0] = out.mColor[1] = out.mColor[2] = out.mColor[3] = 255;
                out.mTexCoord[0] = c[2];
                out.mTexCoord[1] = c[3];
            }
            flushToGpu(gGuiBuffer, 6 * sizeof(Vertex));
            gGuiDraws.push_back(GuiDraw{ 0, 6, &gOverlay, GuiBlend::Premultiplied, 0 });
        }

        void drawGui()
        {
            // Only while the GUI owns the bottom screen (with no-m8ui the M6 text menus have it, and the MyGUI windows
            // mustn't show on the top screen meanwhile).
            if (!gDrawsGui || !gGuiOwnsBottom || gGuiSource == nullptr || gFlatProjection < 0)
                return;
            p3ds::perf::Phase phase("guiDraw");
            const bool reusable = gSoftTop.mValid && gSoftTop.mPrevGeneration == gTextureGeneration
                && (!gGuiOwnsBottom
                    || (gBottomHash != 0 && gSoftBottom.mValid && gSoftBottom.mPrevGeneration == gTextureGeneration));
            bool unchanged = false;
            // Test #105: 1.7 s in guiDraw as the rest window opened. Its two parts timed apart for the spike reports.
            {
                p3ds::perf::Phase prepare("guiPrepare");
                prepareGuiDraws(reusable, unchanged);
            }
            if (!unchanged)
            {
                p3ds::perf::Phase cpu("guiSoft");
                drawSoftGui();
            }
            if (gGuiDraws.empty())
                return;

            useProgram(&gFlatProgram);
            if (gSceneBuffer == nullptr)
                sceneBufferInfo(gGuiBuffer);
            setSceneBufferOffset(osConvertVirtToPhys(gGuiBuffer) - gBufferBase);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_CullFace(GPU_CULL_NONE);
            // No fog on the interface (hardware test #78: the scene's fog unit stayed on, and the top screen's
            // full-screen overlay, blended premultiplied, added the fog color to every pixel: a white haze in
            // daylight). The next scene draw sets its fog again.
            C3D_FogGasMode(GPU_NO_FOG, GPU_PLAIN_DENSITY, false);
            gFog.mValid = false;

            // No scene drawn this frame (the main menu before a game): the top screen is black under the interface
            // rather than whatever its target holds.
            if (gStats.mDraws == 0 && !gHoldTop)
                C3D_RenderTargetClear(gTarget, C3D_CLEAR_COLOR, 0x000000FF, 0);
            // The top screen's overlay quad (drawSoftGui made its texture).
            drawGuiOn(gTarget, 1.f, -1.f, 0);

            // The rest of the frame's state is the scene's: rebind it when the next frame starts (beginFrame).
            gBound = Bound{};
            gSceneBuffer = nullptr;
        }

        void drawMainStage(osgUtil::RenderStage* stage)
        {
            p3ds::perf::Phase phase("drawEmit");
            const uint64_t start = p3ds::memprobe::ticks();
            if (!beginFrame())
                return;

            const u32 clearBits = (stage->getClearMask() & GL_COLOR_BUFFER_BIT ? C3D_CLEAR_COLOR : 0)
                | (stage->getClearMask() & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) ? C3D_CLEAR_DEPTH : 0);
            if (clearBits && !gHoldTop)
                C3D_RenderTargetClear(
                    gTarget, static_cast<C3D_ClearBits>(clearBits), toRgba8(stage->getClearColor()), 0xFFFFFF);
            drawOn(gTarget);

            if (!gSelfTestDone)
            {
                runGpuSelfTest();
                if (gFailure)
                    return;
            }
            // A no-op after the first frame (citro3d's program stays gProgram, see useProgram). The first draw sets
            // its variant's entry point (gBound was reset).
            C3D_BindProgram(&gProgram);
            setupAttributes();
            gVerifyThisFrame = gVerifyFramesLeft > 0 && gpuVerify();
            const uint32_t drawsBefore = gStats.mDraws;
#ifdef OPENMW3DS_DEBUG
            gSampleEmit = gFrame % 8 == 0; // the perf log's emit.* phases
#endif
            gEmitProfile = EmitProfile{};
            drawBin(stage);
            if (gSampleEmit)
            {
                p3ds::perf::add("emit.geometry", gEmitProfile.mGeometry * 8);
                p3ds::perf::add("emit.state", gEmitProfile.mState * 8);
                p3ds::perf::add("emit.texture", gEmitProfile.mTexture * 8);
                p3ds::perf::add("emit.uniforms", gEmitProfile.mUniforms * 8);
                p3ds::perf::add("emit.draw", gEmitProfile.mDraw * 8);
                gSampleEmit = false;
            }
            if (gFailure)
                return;
            if (gVerifyThisFrame && gStats.mDraws > drawsBefore)
            {
                --gVerifyFramesLeft;
                P3DS_LOGI("render3ds: draw-by-draw check passed: %lu draws, each completed by the GPU",
                    (unsigned long)(gStats.mDraws - drawsBefore));
                gVerifiedDraws = 0;
            }
            gVerifyThisFrame = false;

            drawGui();

            gStats.mEmitMs += static_cast<float>(p3ds::memprobe::ticks() - start) / CPU_TICKS_PER_MSEC;
        }

        // A render-to-texture stage: only the character preview (the race window's head). Others (the local map's
        // tiles, M8 step 5) stay skipped: each costs VRAM and GPU time of its own.
        void drawRttStage(osgUtil::RenderStage* stage)
        {
            osg::Camera* camera = stage->getCamera();
            const auto& attachments = camera->getBufferAttachmentMap();
            const auto color = attachments.find(osg::Camera::COLOR_BUFFER);
            if (color == attachments.end() || color->second._texture == nullptr)
                return;
            osg::Texture* texture = color->second._texture.get();
            // The frame first: beginFrame frees the GPU objects of deleted OSG objects and drops them from
            // gRttTargets. Looked up before, a new preview texture at a freed one's address found the dying entry,
            // which beginFrame then erased under the reference (test #94, a data abort leaving the Prison Ship).
            if (!beginFrame())
                return;
            RttTarget*& rtt = gRttTargets[texture];
            if (rtt == nullptr)
            {
                const int w = texture->getTextureWidth(), h = texture->getTextureHeight();
                // The race window's preview (256x256 on the 3DS). The inventory's paper doll (512x1024, not shown
                // by the port's pages) gets no target.
                rtt = new RttTarget;
                rtt->mOwner = texture;
                texture->addObserver(rtt);
                // The newest preview takes the reserve: the race window makes its preview twice when it opens (test
                // #86), the first one's texture outliving it a while (OpenMW frees scene objects later, on a queue).
                // Only one race window shows at a time.
                if (w == 256 && h == 256 && gRttReserve.mTarget != nullptr && gRttReserve.mInUse)
                    for (auto& [owner, user] : gRttTargets)
                        if (user != rtt && user->mTex == &gRttReserve.mTex)
                        {
                            user->mTex = nullptr; // its release no longer frees the reserve
                            user->mTarget = nullptr;
                            gRttReserve.mInUse = false;
                            break;
                        }
                if (w == 256 && h == 256 && gRttReserve.mTarget != nullptr && !gRttReserve.mInUse)
                {
                    gRttReserve.mInUse = true;
                    rtt->mTex = &gRttReserve.mTex;
                    rtt->mTarget = gRttReserve.mTarget;
                }
                P3DS_LOGI("render3ds: render-to-texture \"%s\" %dx%d: %s", camera->getName().c_str(), w, h,
                    rtt->mTarget != nullptr ? "the reserved target" : "not drawn (not 256x256, or the target is busy)");
            }
            if (rtt->mTarget == nullptr)
                return;
            C3D_RenderTargetClear(rtt->mTarget, C3D_CLEAR_ALL, toRgba8(stage->getClearColor()), 0xFFFFFF);
            gRotatedTarget = false;
            gBound.mProjection = nullptr;
            drawOn(rtt->mTarget);
            C3D_BindProgram(&gProgram);
            setupAttributes();
            drawBin(stage);
            gRotatedTarget = true;
            gBound.mProjection = nullptr; // the main stage sets its own (rotated) projection
            rtt->mDrawn = true;
        }

        // Test #105: a tile drawn at once prepared every object of the area the view hadn't drawn yet in that one
        // frame (its vertices converted and uploaded, 725 draws on the Imperial Prison Ship): 1.6-3.7 s frozen on the
        // console a few seconds after arriving. The tile's objects and textures are now prepared a few milliseconds
        // per frame. The tile is drawn once nothing is left (false: time ran out, the walk resumes next frame, where
        // what is done is only a lookup). Skinned meshes and particles are left to the draw (none on a map).
        bool prepareMapBin(osgUtil::RenderBin* bin, uint64_t deadline, unsigned& prepared)
        {
            auto prepare = [&](osgUtil::RenderLeaf* leaf) {
                const osg::Geometry* geometry = leaf->_drawable ? leaf->_drawable->asGeometry() : nullptr;
                if (geometry == nullptr || leaf->_parent == nullptr)
                    return true;
                if (geometry->getUserData() == nullptr && gGeometries.find(geometry) == gGeometries.end())
                {
                    geometryFor(*geometry);
                    ++prepared;
                }
                const ResolvedState& rs = resolve(leaf->_parent);
                if (rs.mTexture.mValue != nullptr && gTextures.find(rs.mTexture.mValue) == gTextures.end()
                    && gRttTargets.find(rs.mTexture.mValue) == gRttTargets.end())
                {
                    textureFor(*rs.mTexture.mValue);
                    ++prepared;
                }
                return svcGetSystemTick() < deadline;
            };
            for (auto& child : bin->getRenderBinList())
                if (!prepareMapBin(child.second.get(), deadline, prepared))
                    return false;
            for (osgUtil::RenderLeaf* leaf : bin->getRenderLeafList())
                if (!prepare(leaf))
                    return false;
            for (osgUtil::StateGraph* sg : bin->getStateGraphList())
                for (auto& leaf : sg->_leaves)
                    if (!prepare(leaf.get()))
                        return false;
            return true;
        }

        // A local map tile's camera (LocalMapRenderToTexture, named "LocalMap" on the 3DS): drawn into the scratch
        // target when it is free. copyMapTile takes the image out at the next frame's start.
        void drawMapStage(osgUtil::RenderStage* stage)
        {
            osg::Camera* camera = stage->getCamera();
            const auto& attachments = camera->getBufferAttachmentMap();
            const auto color = attachments.find(osg::Camera::COLOR_BUFFER);
            if (color == attachments.end() || color->second._texture == nullptr)
                return;
            osg::Texture* texture = color->second._texture.get();
            if (!beginFrame()) // the frame first: see drawRttStage
                return;
            RttTarget*& rtt = gRttTargets[texture];
            if (rtt == nullptr)
            {
                rtt = new RttTarget;
                rtt->mOwner = texture;
                rtt->mMapTile = true;
                texture->addObserver(rtt);
                const int w = texture->getTextureWidth(), h = texture->getTextureHeight();
                if (gMapScratch.mTarget == nullptr || w != kMapTileSize || h != kMapTileSize)
                {
                    rtt->mGaveUp = true;
                    P3DS_LOGW("render3ds: local map tile %dx%d not drawn (%s)", w, h,
                        gMapScratch.mTarget == nullptr ? "no VRAM for its target" : "not 128x128");
                }
            }
            if (rtt->mGaveUp || rtt->mReady || rtt->mDrawn || gMapScratch.mBusy != nullptr)
                return;
            // 6 ms of preparing per frame (about a fifth of a frame at 30 fps).
            constexpr uint64_t kPrepareTicks = CPU_TICKS_PER_MSEC * 6;
            ++rtt->mPrepareFrames;
            if (!prepareMapBin(stage, svcGetSystemTick() + kPrepareTicks, rtt->mPrepared))
                return;
            C3D_RenderTargetClear(gMapScratch.mTarget, C3D_CLEAR_ALL, toRgba8(stage->getClearColor()), 0xFFFFFF);
            gRotatedTarget = false;
            gBound.mProjection = nullptr;
            drawOn(gMapScratch.mTarget);
            C3D_BindProgram(&gProgram);
            setupAttributes();
            const uint32_t drawsBefore = gStats.mDraws;
            // Debug mode: what a tile costs (test #105, the stalls after arriving in a cell: its textures load here).
            const size_t texturesBefore = gTextures.size();
            const uint32_t bytesBefore = gStats.mUploadedBytes;
            const uint64_t ticksBefore = svcGetSystemTick();
            drawBin(stage);
            if (p3ds::debugMode())
                P3DS_LOGI("render3ds: local map tile drawn, %lu draws, %.0f ms, %lu new textures, %lu KB uploaded "
                          "(%u objects and textures prepared before, over %d frames)",
                    (unsigned long)(gStats.mDraws - drawsBefore),
                    double(svcGetSystemTick() - ticksBefore) / CPU_TICKS_PER_MSEC,
                    (unsigned long)(gTextures.size() - texturesBefore),
                    (unsigned long)((gStats.mUploadedBytes - bytesBefore) / 1024), rtt->mPrepared, rtt->mPrepareFrames);
            gRotatedTarget = true;
            gBound.mProjection = nullptr; // the main stage sets its own (rotated) projection
            rtt->mDrawn = true;
            gMapScratch.mBusy = rtt;
        }

        bool drawStageHook(osgUtil::RenderStage* stage, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*&)
        {
            if (!gActive)
                return false;
            // Only the main scene camera is drawn for now. Pre-render (RTT) and post-render stages (the
            // post-processing canvas, MyGUI) are skipped: returning true keeps OSG from drawing them. After a
            // GPU failure nothing is drawn any more (and not through the stub GL either).
            if (!isMainCamera(stage->getCamera()))
            {
                if (!gFailure && stage->getCamera() != nullptr && stage->getCamera()->getName() == "CharacterPreview")
                    drawRttStage(stage);
                else if (!gFailure && stage->getCamera() != nullptr && stage->getCamera()->getName() == "LocalMap")
                    drawMapStage(stage);
                return true;
            }
            if (!gFailure)
                drawMainStage(stage);
            // OSG runs the camera's final draw callback once the stage is drawn, and something may be waiting
            // for it: ScreenshotManager (save game thumbnails) blocks until it runs.
            if (osg::Camera::DrawCallback* callback = stage->getCamera()->getFinalDrawCallback())
                callback->run(renderInfo);
            return true;
        }

        void swapHook()
        {
            if (!gInFrame || gFailure)
                return;
            p3ds::perf::Phase phase("frameEnd");
            gStats.mCmdBufUsage = C3D_GetCmdBufUsage();
            // GX_CMDLIST_FLUSH: flush only the command list. We flush every buffer we write ourselves, so
            // citro3d doesn't need to flush the whole linear heap from the data cache every frame.
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            gInFrame = false;
            gStats.mGpuMs = C3D_GetDrawingTime();
            // The GPU's time for one frame (not the CPU's): explosions and big transparent effects can take long.
            // (A Rosalina or HOME Menu pause in between counts too.)
            static uint32_t badParticlesLogged = 0;
            if (gStats.mBadParticles != 0 && gFrame - badParticlesLogged >= 30)
            {
                P3DS_LOGW("render3ds: %lu particles left out in frame %lu (zero-scale effect or non-finite corners)",
                    (unsigned long)gStats.mBadParticles, (unsigned long)gFrame);
                badParticlesLogged = gFrame;
            }
            if (gStats.mGpuMs > 500.f)
                P3DS_LOGW("render3ds: the GPU took %.0f ms for frame %lu (%lu draws, %lu triangles)",
                    double(gStats.mGpuMs), (unsigned long)gFrame, (unsigned long)gStats.mDraws,
                    (unsigned long)gStats.mTriangles);
            gStats.mGeometries = static_cast<uint32_t>(gGeometries.size());
            gStats.mLinearFreeKb = static_cast<uint32_t>(linearSpaceFree() / 1024);
            gLastStats = gStats;
        }
    }

    bool init()
    {
        if (gActive)
            return true;
        // 1 MB of GPU commands per frame (~100-200 bytes per draw). libctru silently drops commands that don't
        // fit, so usage is reported in the perf log.
        if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4))
        {
            P3DS_LOGE("render3ds: C3D_Init failed");
            return false;
        }
        gTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
        if (gTarget == nullptr)
        {
            P3DS_LOGE("render3ds: no render target");
            C3D_Fini();
            return false;
        }
        C3D_RenderTargetSetOutput(gTarget, GFX_TOP, GFX_LEFT, kDisplayTransferFlags);
        // The character preview's target (drawRttStage), reserved before the scene's textures fill the VRAM:
        // 512 KB (color and depth).
        if (C3D_TexInitVRAM(&gRttReserve.mTex, 256, 256, GPU_RGBA8))
            gRttReserve.mTarget
                = C3D_RenderTargetCreateFromTex(&gRttReserve.mTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
        if (gRttReserve.mTarget == nullptr)
            P3DS_LOGW("render3ds: no VRAM for the character preview's target: the race window shows no head");
        // The local map tiles' target (gMapScratch): 128 KB (color and depth).
        if (C3D_TexInitVRAM(&gMapScratch.mTex, kMapTileSize, kMapTileSize, GPU_RGBA8))
            gMapScratch.mTarget
                = C3D_RenderTargetCreateFromTex(&gMapScratch.mTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
        if (gMapScratch.mTarget == nullptr)
            P3DS_LOGW("render3ds: no VRAM for the local map's target: the minimap stays black");
        if (!C3D_TexInit(&gUnusedTex1, 8, 8, GPU_A8))
        {
            P3DS_LOGE("render3ds: no memory for the unit 1 placeholder texture");
            C3D_RenderTargetDelete(gTarget);
            C3D_Fini();
            return false;
        }
        std::memset(gUnusedTex1.data, 0, gUnusedTex1.size);
        GSPGPU_FlushDataCache(gUnusedTex1.data, gUnusedTex1.size);
        gUnusedTex1Ready = true;
        gTransient = static_cast<Vertex*>(gpuAlloc(kTransientVertices * sizeof(Vertex)));
        if (gTransient == nullptr)
            P3DS_LOGW("render3ds: no linear memory for the particle buffer; particles are not drawn");
        gQuadIbo = static_cast<uint16_t*>(gpuAlloc(kTransientVertices / 4 * 6 * sizeof(uint16_t)));
        if (gQuadIbo != nullptr)
        {
            static const uint16_t pattern[6] = { 0, 1, 2, 2, 3, 0 };
            for (uint32_t q = 0; q < kTransientVertices / 4; ++q)
                for (int k = 0; k < 6; ++k)
                    gQuadIbo[q * 6 + k] = static_cast<uint16_t>(q * 4 + pattern[k]);
            flushToGpu(gQuadIbo, kTransientVertices / 4 * 6 * sizeof(uint16_t));
        }
        gIdentityIbo = static_cast<uint16_t*>(gpuAlloc(kIdentityIndices * sizeof(uint16_t)));
        if (gIdentityIbo != nullptr)
        {
            for (uint32_t i = 0; i < kIdentityIndices; ++i)
                gIdentityIbo[i] = static_cast<uint16_t>(i);
            flushToGpu(gIdentityIbo, kIdentityIndices * sizeof(uint16_t));
        }
        else
            P3DS_LOGW("render3ds: no linear memory for the identity index buffer; particles and GUI drawn as arrays");

        gDvlb = DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(scene_shbin)), scene_shbin_size);
        shaderProgramInit(&gProgram);
        shaderProgramSetVsh(&gProgram, &gDvlb->DVLE[0]);
        gProbeDvlb = DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(probe_shbin)), probe_shbin_size);
        shaderProgramInit(&gProbeProgram);
        shaderProgramSetVsh(&gProbeProgram, &gProbeDvlb->DVLE[0]);
        shaderProgramInit(&gFlatProgram);
        shaderProgramSetVsh(&gFlatProgram, &gDvlb->DVLE[1]);
        shaderProgramInit(&gProgram1);
        shaderProgramInit(&gProgram0);
        shaderProgramInit(&gProgramSkin);
        if (gDvlb->numDVLE >= 4)
        {
            shaderProgramSetVsh(&gProgram1, &gDvlb->DVLE[2]);
            shaderProgramSetVsh(&gProgram0, &gDvlb->DVLE[3]);
            gVariants = true;
        }
        if (gVariants && gDvlb->numDVLE >= 5)
        {
            shaderProgramSetVsh(&gProgramSkin, &gDvlb->DVLE[4]);
            gSkinVariant = true;
        }
        shaderProgramInit(&gProgram2);
        if (gVariants && gDvlb->numDVLE >= 6)
        {
            shaderProgramSetVsh(&gProgram2, &gDvlb->DVLE[5]);
            gVariant2 = true;
        }
        C3D_BindProgram(&gProgram);
        auto loc = [](const char* name) {
            const int location = shaderInstanceGetUniformLocation(gProgram.vertexShader, name);
            if (location < 0)
                P3DS_LOGE("render3ds: shader uniform %s not found", name);
            return location;
        };
        gLoc.mProjection = loc("projection");
        gLoc.mModelView = loc("modelView");
        gLoc.mTexMat = loc("texMat");
        gLoc.mTexMat1 = loc("texMat1");
        gLoc.mLighting = loc("lighting");
        gLoc.mSunDir = loc("sunDir");
        gLoc.mSunDiffuse = loc("sunDiffuse");
        gLoc.mSunAmbient = loc("sunAmbient");
        gLoc.mMatDiffuse = loc("matDiffuse");
        gLoc.mMatAmbient = loc("matAmbient");
        gLoc.mMatEmission = loc("matEmission");
        gLoc.mColorSel = loc("colorSel");
        gLoc.mLights = loc("lights");
        gLoc.mBones = gSkinVariant ? loc("bones") : -1;
        gFlatProjection = shaderInstanceGetUniformLocation(gFlatProgram.vertexShader, "projection");
        gFlatModelView = shaderInstanceGetUniformLocation(gFlatProgram.vertexShader, "modelView");
        if (gFlatProjection < 0 || gFlatModelView < 0)
            P3DS_LOGE("render3ds: flat shader uniforms not found: no GUI");
        for (shaderProgram_s* variant : { &gProgram1, &gProgram0, &gProgramSkin, &gProgram2 })
        {
            if (!gVariants || (variant == &gProgramSkin && !gSkinVariant) || (variant == &gProgram2 && !gVariant2))
                continue;
            for (const char* name : { "projection", "modelView", "texMat", "texMat1", "lighting", "sunDir",
                     "sunDiffuse", "sunAmbient", "matDiffuse", "matAmbient", "matEmission", "colorSel", "lights",
                     "bones" })
                if (shaderInstanceGetUniformLocation(variant->vertexShader, name)
                    != shaderInstanceGetUniformLocation(gProgram.vertexShader, name))
                {
                    P3DS_LOGE("render3ds: shader variant uniform %s moved; variants off (8 lights everywhere, CPU "
                              "skinning)", name);
                    gVariants = false;
                    gSkinVariant = false;
                    break;
                }
        }
#ifdef OPENMW3DS_DEBUG
        // sdmc:/openmw/cpu-skinning: skinned meshes on the CPU, as before M10 (comparisons on hardware).
        if (gSkinVariant)
            if (std::FILE* f = std::fopen("sdmc:/openmw/cpu-skinning", "r"))
            {
                std::fclose(f);
                gSkinVariant = false;
            }
#endif
        P3DS_LOGI("render3ds: shaders: %lu instructions in %d programs (max 512), light variants %s, skinning %s",
            (unsigned long)gDvlb->DVLP.codeSize, int(gDvlb->numDVLE), gVariants ? (gVariant2 ? "0/1/2/4" : "0/1/4") : "off",
            gSkinVariant ? "GPU" : "CPU");

        gLinearBasePa = osConvertVirtToPhys(reinterpret_cast<void*>(__ctru_linear_heap)) & ~7u;
        if (gLinearBasePa + __ctru_linear_heap_size > 0x28000000)
            gBufferBase = gLinearBasePa; // the default base wouldn't reach the end of the heap
        P3DS_LOGI("render3ds: linear heap va %08lx pa %08lx-%08lx (%lu KB); vertex/index base %08lx",
            (unsigned long)__ctru_linear_heap, (unsigned long)gLinearBasePa,
            (unsigned long)(gLinearBasePa + __ctru_linear_heap_size), (unsigned long)(__ctru_linear_heap_size / 1024),
            (unsigned long)gBufferBase);

#ifdef OPENMW3DS_DEBUG
        if (std::FILE* f = std::fopen("sdmc:/openmw/autorun.txt", "r"))
        {
            char buf[256] = {};
            std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            gSimulateHang = std::strstr(buf, "fakehang") != nullptr;
        }
#endif

        gResolved.resize(256);
        osgUtil::gRenderStageDrawHook3DS = &drawStageHook;
        SDLUtil::gSwapBuffersHook3DS = &swapHook;
        gActive = true;
        // The top screen's target holds whatever VRAM held until the first scene frame (startup noise, test
        // #60): one black frame now.
        if (C3D_FrameBegin(C3D_FRAME_SYNCDRAW))
        {
            C3D_RenderTargetClear(gTarget, C3D_CLEAR_ALL, 0x000000FF, 0);
            C3D_FrameDrawOn(gTarget);
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
        }
        P3DS_LOGI("render3ds: citro3d ready, %lu KB linear memory free", (unsigned long)(linearSpaceFree() / 1024));
        return true;
    }

    void shutdown()
    {
        if (!gActive)
            return;
        osgUtil::gRenderStageDrawHook3DS = nullptr;
        SDLUtil::gSwapBuffersHook3DS = nullptr;
        gActive = false;
        if (gFailure)
            return; // after a GPU hang or a close from the HOME Menu, citro3d's queue never completes: C3D_Fini
                    // would wait forever
        if (gInFrame)
        {
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            gInFrame = false;
        }
        // GPU objects of still-alive OSG objects stay allocated: the process is about to exit.
        shaderProgramFree(&gProgram);
        shaderProgramFree(&gProbeProgram);
        shaderProgramFree(&gFlatProgram);
        shaderProgramFree(&gProgramSkin);
        shaderProgramFree(&gProgram1);
        shaderProgramFree(&gProgram0);
        shaderProgramFree(&gProgram2);
        DVLB_Free(gDvlb);
        DVLB_Free(gProbeDvlb);
        C3D_RenderTargetDelete(gTarget);
        if (gUnusedTex1Ready)
            C3D_TexDelete(&gUnusedTex1);
        C3D_Fini();
    }

    bool gpuSkinning()
    {
        return gActive && gSkinVariant && gFailure == nullptr;
    }

    namespace
    {
        // The performance batches' features (tests #99-#103) are on. Debug builds only: an empty "no-<feature>"
        // file on the SD card turns one off, for before/after comparisons (M20, the tester: debug purposes only).
        bool switchOn(const char* offFile)
        {
#ifdef OPENMW3DS_DEBUG
            struct stat st;
            const bool on = stat(offFile, &st) != 0;
            Log(Debug::Info) << "render3ds: " << (offFile + std::strlen("sdmc:/openmw/no-")) << (on ? " on" : " off");
            return on;
#else
            (void)offFile;
            return true;
#endif
        }
    }

    bool thinWeather()
    {
        static const bool on = switchOn("sdmc:/openmw/no-thinweather");
        return on;
    }

    bool poseCadence()
    {
        static const bool on = switchOn("sdmc:/openmw/no-posecadence");
        return on;
    }

    bool smallCullBoost()
    {
        static const bool on = switchOn("sdmc:/openmw/no-smallcull");
        return on;
    }

    bool flameLoops()
    {
        static const bool on = switchOn("sdmc:/openmw/no-flameloops");
        return on;
    }

    bool mapTileReady(const osg::Texture* texture)
    {
        if (gMapScratch.mTarget == nullptr)
            return true; // never drawn: don't keep asking
        const auto found = gRttTargets.find(texture);
        return found != gRttTargets.end() && (found->second->mReady || found->second->mGaveUp);
    }

    bool sameStateSkip()
    {
        static const bool on = switchOn("sdmc:/openmw/no-samestate");
        return on;
    }

    bool isActive()
    {
        return gActive && gFailure == nullptr;
    }

    void releaseGpuForExit()
    {
        if (!gActive || gFailure != nullptr)
            return;
        gFailure = "closing (the GPU belongs to the HOME Menu)";
        P3DS_LOGI("render3ds: app closing, no more GPU work");
    }

    const char* failure()
    {
        return gFailure;
    }

    void setDrawsGui(bool draws)
    {
        gDrawsGui = draws;
        P3DS_LOGI("render3ds: MyGUI %s", draws ? "drawn (M8 UI)" : "not drawn (text menus)");
    }

    bool drawsGui()
    {
        return gDrawsGui;
    }

    void setGuiSource(GuiBatchSource source)
    {
        gGuiSource = source;
    }

    void noteGuiCollected()
    {
        gGuiCollected = true;
    }

    namespace
    {
        bool gHudOnly = false;
    }

    void setHudOnly(bool hudOnly)
    {
        gHudOnly = hudOnly;
    }

    bool hudOnly()
    {
        return gHudOnly;
    }

    void invalidateInterface()
    {
        P3DS_LOGI("render3ds: interface redrawn whole (applet)");
        reloadVramTextures(); // the transition's capture lands in VRAM
        gSoftTop.mValid = false;
        gSoftBottom.mValid = false;
        gBottomHash = 0;
    }

    void setHoldTopScreen(bool hold)
    {
        if (hold != gHoldTop)
            P3DS_LOGI("render3ds: top screen %s", hold ? "held (loading)" : "released");
        gHoldTop = hold;
        if (hold && !gDecoded.empty())
        {
            // A loading screen: the interface's decoded textures go (the creation windows' pictures and the
            // like, up to 3 MB on a heap that test #67 ran out of).
            gDecoded.clear();
            gDecodedBytes = 0;
        }
    }

    void setGuiOwnsBottomScreen(bool owns)
    {
        if (owns == gGuiOwnsBottom || !gActive)
            return;
        gGuiOwnsBottom = owns;
        gBottomHash = 0;
        p3ds::textfb::setGpuOwned(owns);
        P3DS_LOGI("render3ds: bottom screen drawn by %s",
            owns ? "the GUI (CPU)" : "the text display (CPU)");
    }

    bool guiOwnsBottomScreen()
    {
        return gGuiOwnsBottom;
    }

    void requestCapture(const char* path)
    {
        gCapturePath = path;
    }

    void requestPlayerCapture(const char* path)
    {
        gPlayerCapturePath = path;
        gPlayerCaptureStarted = false;
    }

    bool playerCaptureStarted()
    {
        return std::exchange(gPlayerCaptureStarted, false);
    }

    const FrameStats& getLastFrameStats()
    {
        return gLastStats;
    }
}
