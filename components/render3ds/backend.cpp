#include "render3ds.hpp"

#include "gpumemory.hpp"

#include <components/debug/debuglog.hpp>
#include <components/sceneutil/material.hpp>

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
#include <platform3ds/screenshot.hpp>
#include <platform3ds/textfb.hpp>

#include <citro3d.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

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

        // Vertex shader input registers; attribute index == register (see setupAttributes).
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
        // gProgram's variants that evaluate fewer point lights (scene1.v.pica: slot 0 only; scene0.v.pica: none).
        // Same code upload and uniform layout as gProgram (one DVLB, gen_scene_shader.py); off if the layouts
        // don't match.
        shaderProgram_s gProgram1;
        shaderProgram_s gProgram0;
        bool gVariants = false;

        // Set when the GPU hung: the GPU core was reset and nothing is drawn for the rest of the run, so the game
        // keeps running (headless) and HOME closes it normally instead of a frozen console.
        const char* gFailure = nullptr;
        bool gInFrame = false;

        // What texture unit 1 holds when no blend map is needed. citro3d's C3D_TexBind dereferences the texture
        // for units 1 and 2 without a null check (it refuses non-2D types there), so unit 1 is never "unbound":
        // it gets this 8x8 texture, which the TexEnv stage ignores and whose fetches always hit the texture cache.
        C3D_Tex gUnusedTex1;
        bool gUnusedTex1Ready = false;

        struct UniformLocations
        {
            int mProjection, mModelView, mTexMat, mTexMat1, mSunDir, mSunDiffuse, mSunAmbient, mMatDiffuse,
                mMatAmbient, mMatEmission, mColorSel, mLighting, mLights;
        } gLoc;

        FrameStats gStats;
        FrameStats gLastStats;
        uint32_t gFrame = 0; // backend frame counter (resolved-state cache stamp)
        float gNear = 1, gFar = 1000; // current perspective projection (fog LUT)

        // Vertex and index buffers are addressed by the PICA as 28-bit offsets from a base address. citro3d's
        // default base 0x18000000 reaches physical 0x27FFFFFF. The linear heap is at 0x20000000 on hardware too
        // (test #10), so the default is kept (exactly what the M0 test app used); the base only moves if the heap
        // ever ends beyond reach.
        u32 gLinearBasePa = 0;
        u32 gBufferBase = 0x18000000;

        // Draw-by-draw verification of the first frames that draw anything: each draw is submitted alone and
        // waited for, so a draw that hangs the GPU is named in the log instead of freezing the console.
        int gVerifyFramesLeft = 2;
        bool gVerifyThisFrame = false;
        uint32_t gVerifiedDraws = 0;

        bool gWarnedPrimitive = false;
        bool gWarnedArray = false;

        // ------------------------------------------------------------------ matrices

        // OSG matrices are row-vector (v * M); PICA uniforms are rows of the column-vector matrix: transpose.
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

        // GL clip space -> PICA clip space. The top screen framebuffer is rotated 90 degrees (as citro3d's
        // Mtx_PerspTilt: x' = y, y' = -x). Depth, with the convention 0 = near, 1 = far (GL's LESS works as is):
        // - perspective: W-buffering, z' = -w / far so depth = eye distance / far. Linear depth gives the fog LUT
        //   (128 entries indexed by depth) uniform resolution; with z/w and a 1-unit near plane everything past
        //   ~120 units would share its first entry.
        // - orthographic: z-buffering, GL's [-w, w] squeezed into the PICA's [-w, 0], depth = z/w + 1.
        void setProjection(const osg::Matrixf& projection)
        {
            C3D_FVec p[4];
            setRows(p, projection, 4);
            C3D_FVec* out = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mProjection, 4);
            out[0] = p[1];
            out[1] = FVec4_Scale(p[0], -1.f);
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
            virtual void release() = 0; // frees GPU memory; called on the main thread
        };

        std::mutex gPendingMutex;
        std::vector<GpuObject*> gPendingDeletes;

        void GpuObject::objectDeleted(void*)
        {
            std::lock_guard<std::mutex> lock(gPendingMutex);
            gPendingDeletes.push_back(this);
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
            void* mOwnedData = nullptr; // converted images; cooked images own their data
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
                else if (auto* texture = dynamic_cast<GpuTexture*>(object))
                {
                    auto it = gTextures.find(static_cast<const osg::Texture*>(object->mOwner));
                    if (it != gTextures.end() && it->second == texture)
                        gTextures.erase(it);
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
            Slot<osg::Uniform> mTexMat0; // terrain layer tiling (uniform "texMat0"; else mTexMat)
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
                        if (name == "LightBuffer")
                            rs.mLightBuffer.merge(uniform, flags);
                        break;
                    case 'P':
                        if (name == "PointLightCount")
                            rs.mPointLightCount.merge(uniform, flags);
                        break;
                    case 's':
                        if (name == "sun.position")
                            rs.mSunPosition.merge(uniform, flags);
                        else if (name == "sun.diffuse")
                            rs.mSunDiffuse.merge(uniform, flags);
                        else if (name == "sun.ambient")
                            rs.mSunAmbient.merge(uniform, flags);
                        break;
                    case 'f':
                        if (name == "fog.start")
                            rs.mFogStart.merge(uniform, flags);
                        else if (name == "fog.end")
                            rs.mFogEnd.merge(uniform, flags);
                        else if (name == "fog.color")
                            rs.mFogColor.merge(uniform, flags);
                        break;
                    case 'a':
                        if (name == "alpha")
                            rs.mAlpha.merge(uniform, flags);
                        else if (name == "actorFade")
                            rs.mActorFade.merge(uniform, flags);
                        else if (name == "atmosphereFade")
                            rs.mAtmosphereFade.merge(uniform, flags);
                        break;
                    case 'p':
                        if (name == "pass")
                            rs.mSkyPass.merge(uniform, flags);
                        break;
                    case 'b':
                        if (name == "blendMap")
                            rs.mBlendMap.merge(uniform, flags);
                        break;
                    case 'd':
                        if (name == "diffuseColor")
                            rs.mDiffuseColor.merge(uniform, flags);
                        break;
                    case 'o':
                        if (name == "opacity")
                            rs.mOpacity.merge(uniform, flags);
                        break;
                    case 'm':
                        if (name == "moonBlend")
                            rs.mMoonBlend.merge(uniform, flags);
                        break;
                    case 't':
                        if (name == "texMat0")
                            rs.mTexMat0.merge(uniform, flags);
                        else if (name == "texMat1")
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
        struct Vertex
        {
            float mPosition[3];
            float mNormal[3];
            uint8_t mColor[4]; // 0..255, the shader scales by 1/255
            float mTexCoord[2];
        };
        static_assert(sizeof(Vertex) == 36);

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

        // OpenMW's water plane repeats its texture 900 times; near the player the texture coordinates are ~450,
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

        bool buildVertices(GpuGeometry& gpu, const osg::Geometry& geometry)
        {
            const auto* positions = dynamic_cast<const osg::Vec3Array*>(geometry.getVertexArray());
            if (positions == nullptr || positions->empty())
            {
                if (!gWarnedArray)
                    Log(Debug::Warning) << "render3ds: geometry without a Vec3Array vertex array (not drawn)";
                gWarnedArray = true;
                return false;
            }
            const unsigned count = positions->size();
            const auto* normals = dynamic_cast<const osg::Vec3Array*>(perVertex(geometry.getNormalArray(), count));
            const osg::Array* colors = perVertex(geometry.getColorArray(), count);
            const auto* colors4f = dynamic_cast<const osg::Vec4Array*>(colors);
            const auto* colors4ub = dynamic_cast<const osg::Vec4ubArray*>(colors);
            const auto* texcoords = dynamic_cast<const osg::Vec2Array*>(perVertex(geometry.getTexCoordArray(0), count));

            // BIND_OVERALL colors (or none: white) for every vertex.
            uint8_t overall[4] = { 255, 255, 255, 255 };
            if (const osg::Array* c = geometry.getColorArray();
                c && c->getBinding() == osg::Array::BIND_OVERALL && c->getNumElements() > 0)
            {
                if (auto* c4f = dynamic_cast<const osg::Vec4Array*>(c))
                    for (int k = 0; k < 4; ++k)
                        overall[k] = toByte((*c4f)[0][k]);
                else if (auto* c4ub = dynamic_cast<const osg::Vec4ubArray*>(c))
                    std::memcpy(overall, &(*c4ub)[0], 4);
            }

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
            {
                std::memcpy(out->mPosition, &(*positions)[i], 12);
                if (normals)
                    std::memcpy(out->mNormal, &(*normals)[i], 12);
                else
                {
                    out->mNormal[0] = out->mNormal[1] = 0.f;
                    out->mNormal[2] = 1.f;
                }
                if (colors4f)
                    for (int k = 0; k < 4; ++k)
                        out->mColor[k] = toByte((*colors4f)[i][k]);
                else if (colors4ub)
                    std::memcpy(out->mColor, &(*colors4ub)[i], 4);
                else
                    std::memcpy(out->mColor, overall, 4);
                if (texcoords)
                    std::memcpy(out->mTexCoord, &(*texcoords)[i], 8);
                else
                    out->mTexCoord[0] = out->mTexCoord[1] = 0.f;
            }
            if (texcoords && onlyArrayQuads(geometry))
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

        // ------------------------------------------------------------------ textures

        // Morton (Z-order) offset of (x, y) inside an 8x8 tile.
        inline u32 morton8(u32 x, u32 y)
        {
            u32 m = 0;
            for (u32 i = 0; i < 3; ++i)
                m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
            return m;
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
                    // PICA texels are little-endian words: RGBA8 = A,B,G,R bytes; RGB8 = B,G,R; LA8 = A,L.
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
            const osg::Image* image = texture.getNumImages() > 0 ? texture.getImage(0) : nullptr;
            GpuTexture*& gpu = gTextures[&texture];
            if (gpu == nullptr)
            {
                gpu = new GpuTexture;
                gpu->mOwner = &texture;
                texture.addObserver(gpu);
            }
            else if (gpu->mImage == image && (image == nullptr || gpu->mImageModified == image->getModifiedCount()))
            {
                if (gpu->mValid)
                    applySampler(*gpu, texture); // cheap; filters/wrap can change at runtime
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
                // zero) and adds the others (src alpha, one); the weights sum to 1 only if every pixel is drawn
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
        } gBound;

        enum TexEnvMode
        {
            TexEnv_VertexColor = 0, // lit vertex color
            TexEnv_Textured = 1, // diffuse map x lit vertex color
            TexEnv_BlendMapped = 2, // terrain layer: diffuse map x lit color; alpha = blend map (unit 1) x lit alpha
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

        // Vertex = position, normal, color (4 unsigned bytes), texcoord: input registers v0..v3, one buffer.
        void setupAttributes()
        {
            C3D_AttrInfo* attr = C3D_GetAttrInfo();
            AttrInfo_Init(attr);
            AttrInfo_AddLoader(attr, Attr_Position, GPU_FLOAT, 3);
            AttrInfo_AddLoader(attr, Attr_Normal, GPU_FLOAT, 3);
            AttrInfo_AddLoader(attr, Attr_Color, GPU_UNSIGNED_BYTE, 4);
            AttrInfo_AddLoader(attr, Attr_TexCoord, GPU_FLOAT, 2);
        }

        void bindVertexBuffer(const void* vbo)
        {
            C3D_BufInfo* buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            buf->base_paddr = gBufferBase;
            BufInfo_Add(buf, vbo, sizeof(Vertex), 4, 0x3210);
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

        void writeLights(const float* data, int count)
        {
            bool used[kMaxLights] = {};
            int pending[kMaxLights];
            int pendingCount = 0;
            // Lights already in a slot stay there.
            for (int i = 0; i < count; ++i)
            {
                const float* m = data + i * 16;
                int found = -1;
                for (int slot = 0; slot < kMaxLights && found < 0; ++slot)
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
                for (int slot = 0; slot < kMaxLights && target < 0; ++slot)
                    if (!used[slot] && gLightSlots[slot].mKnown && !gLightSlots[slot].mLit)
                        target = slot;
                for (int slot = 0; slot < kMaxLights && target < 0; ++slot)
                    if (!used[slot])
                        target = slot;
                const float* m = data + pending[k] * 16;
                writeSlot(target, m);
                used[target] = true;
                std::memcpy(gLightSlots[target].mData, m, sizeof(LightSlot::mData));
                gLightSlots[target].mLit = true;
                gLightSlots[target].mKnown = true;
            }
            // Everything else off.
            for (int slot = 0; slot < kMaxLights; ++slot)
                if (!used[slot] && (!gLightSlots[slot].mKnown || gLightSlots[slot].mLit))
                {
                    writeSlot(slot, nullptr);
                    gLightSlots[slot].mLit = false;
                    gLightSlots[slot].mKnown = true;
                }
        }

        int gDrawLights = 0; // point lights of the current draw (stats)

#ifdef OPENMW3DS_DEBUG
        // Hardware test #32: sdmc:/openmw/full-program-switch switches the scene's shader variants through citro3d
        // again (C3D_BindProgram: the whole shader configuration), as before the fix below.
        bool fullProgramSwitch()
        {
            static const bool on = [] {
                struct stat st;
                const bool exists = stat("sdmc:/openmw/full-program-switch", &st) == 0;
                if (exists)
                    P3DS_LOGW("render3ds: debug switch: full-program-switch");
                return exists;
            }();
            return on;
        }
#endif

        // The fireball corruption (docs/FIREBALL_CORRUPTION.md): on hardware, from a big explosion on, the shader
        // outputs came out shifted by one (positions taken from texture coordinates, colours from positions) for
        // the rest of the run. Most likely trigger: the vertex shader reconfigured while the GPU is still busy with
        // earlier draws. citro3d's C3D_BindProgram re-sends a program's whole configuration (output map, output
        // counts, geometry stage) at the next draw; we switched programs ~45 times a frame (the light variants),
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
#ifdef OPENMW3DS_DEBUG
            if (fullProgramSwitch() && program != &gFlatProgram)
            {
                C3D_BindProgram(const_cast<shaderProgram_s*>(program));
                return;
            }
#endif
            GPUCMD_AddWrite(GPUREG_VSH_ENTRYPOINT, 0x7FFF0000u | (program->vertexShader->dvle->mainOffset & 0xFFFF));
        }

        // pointLights false: the draw ignores point lights (sky passes, OpenMW's "simpleLighting" weather
        // particles: lighting_vertex_impl.glsl), whatever the light manager attached.
        void uploadLighting(const ResolvedState& rs, bool pointLights)
        {
            // Sun: shared by everything under the light manager; its values change once per frame.
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
            count = std::clamp(count, 0, kMaxLights);
            if (buffer == nullptr || buffer->getFloatArray() == nullptr)
                count = 0;
            if (count > 0)
                count = std::min<int>(count, static_cast<int>(buffer->getFloatArray()->size() / 16));
            gDrawLights = count;

            const shaderProgram_s* program = !gVariants || count >= 2 ? &gProgram : count == 1 ? &gProgram1 : &gProgram0;
            useProgram(program);
            if (program == &gProgram0)
                return;
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
            writeLights(count > 0 ? &(*buffer->getFloatArray())[0] : nullptr, count);
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
            const u64 start = svcGetSystemTick();
            while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK))
            {
                if (svcGetSystemTick() - start > u64(timeoutMs) * CPU_TICKS_PER_MSEC)
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
        // (synced log) before the crash report; on success every feature the renderer uses has run once.
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
            P3DS_LOGI("render3ds: GPU self-test %s: ok (%.1f ms)", name,
                double(svcGetSystemTick() - start) / CPU_TICKS_PER_MSEC);
            return true;
        }

        void setTestMatrices(int projectionLoc, int modelViewLoc)
        {
            // Quad positions are already in clip space; z = -0.5 is inside the PICA's [-1, 0] for both depth modes.
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
            writeLights(data, count);
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

                setTestLights(kMaxLights);
                C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                if (!selfTestStep("L9 + 8 point lights"))
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
                C3D_BindProgram(&gProgram);
                return true;
            };
            if (!run())
                return; // GPU reset, rendering off: leave the buffers (the GPU state is unknown)

            P3DS_LOGI("render3ds: GPU self-test passed");

            // Leave nothing behind: the scene re-applies all of its state.
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

#ifdef OPENMW3DS_DEBUG
        // Hardware test #29: a fireball exploding at the player's feet breaks the GPU, with or without M8's UI, and
        // not in Azahar. What reaches the GPU then (noteUnusualDraw, Azahar): the hit's particles around the camera,
        // particles left at the world origin 71000 units away (on every cast), and the blast meshes, tiny models
        // scaled x1500 to x44000 by their animation, enclosing the camera. Empty files in sdmc:/openmw/ leave one
        // kind out, to find which one the hardware can't take.
        struct DebugSkips
        {
            bool mParticles = false; // skip-particles: no particles at all
            bool mFarParticles = false; // skip-far-particles: particles past the far plane
            bool mBigEffects = false; // skip-big-effects: draws scaled more than x100
        };

        const DebugSkips& debugSkips()
        {
            static const DebugSkips skips = [] {
                DebugSkips s;
                struct stat st;
                s.mParticles = stat("sdmc:/openmw/skip-particles", &st) == 0;
                s.mFarParticles = stat("sdmc:/openmw/skip-far-particles", &st) == 0;
                s.mBigEffects = stat("sdmc:/openmw/skip-big-effects", &st) == 0;
                if (s.mParticles || s.mFarParticles || s.mBigEffects)
                    P3DS_LOGW("render3ds: debug switches:%s%s%s", s.mParticles ? " skip-particles" : "",
                        s.mFarParticles ? " skip-far-particles" : "", s.mBigEffects ? " skip-big-effects" : "");
                return s;
            }();
            return skips;
        }
#endif

        // Vertices built on the CPU every frame (particle quads). When beginFrame returns, the GPU has finished the
        // previous frame, so the buffer is refilled from the start each frame.
        constexpr uint32_t kTransientVertices = 16384; // 2730 particles, 576 KB (rain alone: ~1400-1900 drops)
        Vertex* gTransient = nullptr; // linear memory (init)
        uint32_t gTransientUsed = 0;
        bool gWarnedTransient = false;

        // osgParticle::ParticleSystem::drawImplementation's quads (OSG ParticleSystem.cpp), on the CPU: OpenMW's
        // fires, flames, smoke and spell effects (they draw through immediate GL that never reaches us). Normal
        // (0, 0, 1) like NifOsg::ParticleSystem's normal array.
        bool buildParticles(
            const osgParticle::ParticleSystem& ps, const osg::Matrix& modelview, uint32_t& first, uint32_t& count)
        {
            first = gTransientUsed;
            count = 0;
            if (gTransient == nullptr || ps.numParticles() <= 0)
                return false;
#ifdef OPENMW3DS_DEBUG
            if (debugSkips().mParticles)
            {
                gStats.mDebugSkipped += static_cast<uint32_t>(ps.numParticles());
                return false;
            }
#endif
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
                // corrupted as a fireball exploded; Azahar just drops them). Nothing shows at scale 0 anyway.
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
            const uint32_t room = kTransientVertices - gTransientUsed;
            Vertex* out = gTransient + first;
            for (int i = 0; i < ps.numParticles(); i += detail)
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
                if (count + 6 > room)
                {
                    if (!gWarnedTransient)
                        Log(Debug::Warning) << "render3ds: more particles than the per-frame buffer holds";
                    gWarnedTransient = true;
                    break;
                }
                osg::Vec3 x = xAxis, y = yAxis;
                const osg::Vec3& angle = particle->getAngle();
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
                osg::Vec4 color = particle->getCurrentColor();
                color.a() *= particle->getCurrentAlpha();
                const float size = particle->getCurrentSize() * scale;
                const osg::Vec3 p1 = x * size, p2 = y * size;
                const osg::Vec3& pos = particle->getPosition();
#ifdef OPENMW3DS_DEBUG
                if (debugSkips().mFarParticles && -(pos * modelview).z() > gFar)
                {
                    ++gStats.mDebugSkipped;
                    continue;
                }
#endif
                const float s0 = particle->getSTexCoord(), t0 = particle->getTTexCoord();
                const float s1 = s0 + particle->getSTexTile(), t1 = t0 + particle->getTTexTile();
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
                for (int corner : order)
                {
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
        void noteUnusualDraw(const osgUtil::RenderLeaf& leaf)
        {
            static std::unordered_set<const osg::Drawable*> noted;
            if (noted.size() >= 150)
                return;
            const osg::Drawable* drawable = leaf._drawable.get();
            const osg::Matrix& mv = *leaf._modelview;
            const float sx = osg::Vec3(mv(0, 0), mv(0, 1), mv(0, 2)).length();
            const float sy = osg::Vec3(mv(1, 0), mv(1, 1), mv(1, 2)).length();
            const float sz = osg::Vec3(mv(2, 0), mv(2, 1), mv(2, 2)).length();
            const float scale = std::max(sx, std::max(sy, sz));
            const osg::BoundingSphere& bound = drawable->getBound();
            const float distance = (bound.center() * mv).length();
            const bool zeroScale = std::min(sx, std::min(sy, sz)) < 1e-4f;
            const bool enclosing = bound.valid() && distance < bound.radius() * scale;
            if (!zeroScale && !enclosing)
                return;
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
            if (particles)
            {
                if (!buildParticles(*particles, *leaf->_modelview, particleFirst, particleCount))
                    return; // no live particles
            }
            else
            {
#ifdef OPENMW3DS_DEBUG
                if (debugSkips().mBigEffects)
                {
                    const osg::Matrix& mv = *leaf->_modelview;
                    if (osg::Vec3(mv(0, 0), mv(0, 1), mv(0, 2)).length2() > 100.f * 100.f)
                    {
                        ++gStats.mDebugSkipped;
                        return;
                    }
                }
#endif
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
            const int sky = skyPass(rs);
            if (sky == Sky_SunflashQuery)
                return; // an occlusion query for the sun glare: draws nothing visible

            const FixedState fixed = computeFixedState(rs);
            if (fixed.mCull == 3)
                return;
            if (!gBound.mFixedValid || !(fixed == gBound.mFixed))
            {
                applyFixedState(fixed);
                gBound.mFixed = fixed;
                gBound.mFixedValid = true;
            }

            lap(gEmitProfile.mState);
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
            C3D_Tex* blendMap = texture && blendMapTexture ? textureFor(*blendMapTexture) : nullptr;
            C3D_Tex* unit1 = blendMap ? blendMap : &gUnusedTex1;
            if (unit1 != gBound.mTexture1)
            {
                gBound.mTexture1 = unit1;
                C3D_TexBind(1, unit1);
            }
            setTexEnv(blendMap ? TexEnv_BlendMapped : texture ? TexEnv_Textured : TexEnv_VertexColor);
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
            uploadLighting(rs, sky == Sky_None && !simpleLighting);
            uploadMaterial(rs, sky);
            uploadTexMat(rs, sky);
            uploadFog(rs, sky);
            lap(gEmitProfile.mUniforms);

            if (particles)
            {
                if (gSceneBuffer == nullptr)
                    sceneBufferInfo(gTransient);
                setSceneBufferOffset(osConvertVirtToPhys(gTransient) - gBufferBase);
                C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(particleFirst), static_cast<int>(particleCount));
                ++gStats.mDraws;
                gStats.mTriangles += particleCount / 3;
                gStats.mShaderRuns[gDrawLights] += particleCount;
                gStats.mParticleVertices += particleCount;
                lap(gEmitProfile.mDraw);
                return;
            }
            if (gSceneBuffer == nullptr)
                sceneBufferInfo(gpu->mVbo);
            setSceneBufferOffset(gpu->mVboOffset);
            const bool verify = gVerifyThisFrame || !gpu->mVerified;
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
            if (gFrame % 60 == 0)
                checkDisplayState();
            if (!gCapturePath.empty())
            {
                captureAll(gCapturePath);
                gCapturePath.clear();
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

        // The bottom screen stays RGB565 and single-buffered (platform3ds textfb): only frames in which the UI on it
        // changed are drawn and transferred (citro3d only transfers targets drawn on in the frame).
        constexpr u32 kBottomTransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0)
            | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8)
            | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
        constexpr uint32_t kGuiMaxVertices = 65536; // 2.3 MB of linear memory at most

        bool gDrawsGui = false;
        GuiBatchSource gGuiSource = nullptr;
        C3D_RenderTarget* gBottomTarget = nullptr; // created when the GUI first owns the bottom screen
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
        };
        std::vector<GuiBatch> gGuiBatches;
        std::vector<GuiDraw> gGuiDraws;

        // Hardware test #26: both screens showed the top target's depth buffer at half size, then garbage. What the
        // render targets and the LCDs are set to, checked about once a second and logged when it changes: a target
        // whose fields change means something overwrote citro3d's memory; an LCD format or stride other than
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

        // A screenshot (the Debug tab's button): the top render target, the bottom one too (what the GPU drew there,
        // before the copy to the screen; the bottom screen as shown is saved separately, frame3ds.cpp), and the GPU's
        // rasterizer, texturing and framebuffer registers, to tell drawing problems from display ones.
        void captureAll(const std::string& path)
        {
            if (captureTarget(*gTarget, path))
                P3DS_LOGI("render3ds: top screen saved to %s", path.c_str());
            if (gBottomTarget != nullptr && gGuiOwnsBottom)
            {
                std::string bottom = path;
                const size_t at = bottom.rfind("top_");
                if (at != std::string::npos)
                    bottom.replace(at, 4, "bottomgpu_");
                if (bottom != path && captureTarget(*gBottomTarget, bottom))
                    P3DS_LOGI("render3ds: bottom render target saved to %s", bottom.c_str());
            }
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
            static TargetState top, bottom;
            checkTarget("top", gTarget, top);
            checkTarget("bottom", gBottomTarget, bottom);
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

        // Copies this frame's batches into the GUI buffer (texture coordinates of padded UI textures scaled to the
        // image) and returns a hash of what they draw.
        uint64_t prepareGuiDraws()
        {
            gGuiBatches.clear();
            gGuiDraws.clear();
            gGuiSource([](const GuiBatch& batch) { gGuiBatches.push_back(batch); });
            uint32_t total = 0;
            for (const GuiBatch& batch : gGuiBatches)
                total += batch.mCount;
            if (total == 0 || !ensureGuiCapacity(total))
                return 0;

            uint64_t hash = 0xCBF29CE484222325ull;
            auto mix = [&hash](uint64_t value) { hash = (hash ^ value) * 0x100000001B3ull; };
            uint32_t used = 0;
            for (const GuiBatch& batch : gGuiBatches)
            {
                C3D_Tex* texture = batch.mTexture != nullptr ? textureFor(*batch.mTexture) : nullptr;
                // A texture the GPU can't show (the local map's render-to-texture tiles until M8 step 5): left out
                // rather than drawn as a plain white quad.
                if (batch.mTexture != nullptr && texture == nullptr)
                    continue;
                float scaleU = 1.f, scaleV = 1.f;
                const osg::Image* image
                    = batch.mTexture != nullptr && batch.mTexture->getNumImages() > 0 ? batch.mTexture->getImage(0) : nullptr;
                const auto* cooked = dynamic_cast<const CookedImage*>(image);
                if (cooked != nullptr && cooked->padded())
                {
                    scaleU = float(cooked->sourceWidth()) / float(cooked->s());
                    scaleV = float(cooked->sourceHeight()) / float(cooked->t());
                }
                // Images made at runtime (MyGUI's font atlases) are uploaded with their first row at t = 0, the
                // cooked ones (which MyGUI's v = 0 at the top expects) flipped: mirror v for the runtime ones.
                const bool flipV = image != nullptr && cooked == nullptr;
                Vertex* out = gGuiBuffer + used;
                const auto* in = static_cast<const GuiVertex*>(batch.mVertices);
                for (uint32_t i = 0; i < batch.mCount; ++i)
                {
                    std::memcpy(out[i].mPosition, in[i].mPosition, 12);
                    out[i].mNormal[0] = out[i].mNormal[1] = 0.f;
                    out[i].mNormal[2] = 1.f;
                    std::memcpy(out[i].mColor, in[i].mColor, 4);
                    out[i].mTexCoord[0] = in[i].mTexCoord[0] * scaleU;
                    out[i].mTexCoord[1] = flipV ? 1.f - in[i].mTexCoord[1] : in[i].mTexCoord[1] * scaleV;
                }
                const auto* words = reinterpret_cast<const uint32_t*>(in);
                for (uint32_t i = 0, n = batch.mCount * sizeof(GuiVertex) / 4; i < n; ++i)
                    mix(words[i]);
                mix(reinterpret_cast<uintptr_t>(texture));
                mix(image != nullptr ? image->getModifiedCount() : 0);
                mix(static_cast<uint64_t>(batch.mBlend));
                gGuiDraws.push_back(GuiDraw{ used, batch.mCount, texture, batch.mBlend });
                used += batch.mCount;
            }
            flushToGpu(gGuiBuffer, used * sizeof(Vertex));
            return hash | 1; // never 0 ("redraw")
        }

        // Draws the GUI on one screen. The canvas's clip space (x right, y up) maps to the screen's by
        // x_s = scaleX * x, y_s = 2 * y + offsetY, then the screens' 90-degree rotation (x' = y_s, y' = -x_s, as
        // setProjection) and the fixed depth z' = -0.5 (no depth test).
        void drawGuiOn(C3D_RenderTarget* target, float scaleX, float offsetY)
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
                C3D_DrawArrays(GPU_TRIANGLES, static_cast<int>(draw.mFirst), static_cast<int>(draw.mCount));
                ++gStats.mGuiDraws;
            }
        }

        void drawGui()
        {
            // Only while the GUI owns the bottom screen: OpenMW's menus still go to the M6 text menus there, and
            // their MyGUI windows (desktop layouts) mustn't show on the top screen meanwhile.
            if (!gDrawsGui || !gGuiOwnsBottom || gGuiSource == nullptr || gFlatProjection < 0)
                return;
            p3ds::perf::Phase phase("guiDraw");
            const uint64_t hash = prepareGuiDraws();
            if (gGuiDraws.empty())
                return;

            useProgram(&gFlatProgram);
            if (gSceneBuffer == nullptr)
                sceneBufferInfo(gGuiBuffer);
            setSceneBufferOffset(osConvertVirtToPhys(gGuiBuffer) - gBufferBase);
            C3D_AlphaTest(false, GPU_ALWAYS, 0);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_CullFace(GPU_CULL_NONE);

            // Top screen: the canvas's upper half (y in [0, 1]).
            drawGuiOn(gTarget, 1.f, -1.f);
            // Bottom screen: x in [-0.8, 0.8] (canvas pixels 40..360), y in [-1, 0]; only when it changed.
            if (gGuiOwnsBottom && gBottomTarget != nullptr && hash != gBottomHash)
            {
                C3D_RenderTargetClear(gBottomTarget, C3D_CLEAR_COLOR, 0x000000FF, 0);
                drawGuiOn(gBottomTarget,
                    float(kGuiCanvasWidth) / float(kGuiCanvasWidth - 2 * kGuiBottomX), 1.f);
                gBottomHash = hash;
                ++gStats.mGuiBottomFrames;
            }

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
            if (clearBits)
                C3D_RenderTargetClear(
                    gTarget, static_cast<C3D_ClearBits>(clearBits), toRgba8(stage->getClearColor()), 0xFFFFFF);
            drawOn(gTarget);

            if (!gSelfTestDone)
            {
                runGpuSelfTest();
                if (gFailure)
                    return;
            }
            // A no-op after the first frame (citro3d's program stays gProgram, see useProgram); the first draw sets
            // its variant's entry point (gBound was reset).
            C3D_BindProgram(&gProgram);
            setupAttributes();
            gVerifyThisFrame = gVerifyFramesLeft > 0;
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

        bool drawStageHook(osgUtil::RenderStage* stage, osg::RenderInfo& renderInfo, osgUtil::RenderLeaf*&)
        {
            if (!gActive)
                return false;
            // Only the main scene camera is drawn for now. Pre-render (RTT) and post-render stages (the
            // post-processing canvas, MyGUI) are skipped: returning true keeps OSG from drawing them. After a
            // GPU failure nothing is drawn any more (and not through the stub GL either).
            if (!isMainCamera(stage->getCamera()))
                return true;
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
            // GX_CMDLIST_FLUSH: flush only the command list; we flush every buffer we write ourselves, so
            // citro3d doesn't need to flush the whole linear heap from the data cache every frame.
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            gInFrame = false;
            gStats.mGpuMs = C3D_GetDrawingTime();
            // The GPU's time for one frame (not the CPU's): explosions and big transparent effects can take long.
            // (A Rosalina or HOME Menu pause in between counts too.)
            static uint32_t debugSkipLogged = 0;
            if (gStats.mDebugSkipped != 0 && gFrame - debugSkipLogged >= 30)
            {
                P3DS_LOGI("render3ds: the debug switches left out %lu draws/particles in frame %lu",
                    (unsigned long)gStats.mDebugSkipped, (unsigned long)gFrame);
                debugSkipLogged = gFrame;
            }
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

        gDvlb = DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(scene_shbin)), scene_shbin_size);
        shaderProgramInit(&gProgram);
        shaderProgramSetVsh(&gProgram, &gDvlb->DVLE[0]);
        shaderProgramInit(&gProbeProgram);
        shaderProgramSetVsh(&gProbeProgram, &gDvlb->DVLE[1]);
        shaderProgramInit(&gFlatProgram);
        shaderProgramSetVsh(&gFlatProgram, &gDvlb->DVLE[2]);
        shaderProgramInit(&gProgram1);
        shaderProgramInit(&gProgram0);
        if (gDvlb->numDVLE >= 5)
        {
            shaderProgramSetVsh(&gProgram1, &gDvlb->DVLE[3]);
            shaderProgramSetVsh(&gProgram0, &gDvlb->DVLE[4]);
            gVariants = true;
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
        gFlatProjection = shaderInstanceGetUniformLocation(gFlatProgram.vertexShader, "projection");
        gFlatModelView = shaderInstanceGetUniformLocation(gFlatProgram.vertexShader, "modelView");
        if (gFlatProjection < 0 || gFlatModelView < 0)
            P3DS_LOGE("render3ds: flat shader uniforms not found: no GUI");
        for (shaderProgram_s* variant : { &gProgram1, &gProgram0 })
        {
            if (!gVariants)
                break;
            for (const char* name : { "projection", "modelView", "texMat", "texMat1", "lighting", "sunDir",
                     "sunDiffuse", "sunAmbient", "matDiffuse", "matAmbient", "matEmission", "colorSel", "lights" })
                if (shaderInstanceGetUniformLocation(variant->vertexShader, name)
                    != shaderInstanceGetUniformLocation(gProgram.vertexShader, name))
                {
                    P3DS_LOGE("render3ds: shader variant uniform %s moved; variants off (8 lights everywhere)", name);
                    gVariants = false;
                    break;
                }
        }
        P3DS_LOGI("render3ds: shaders: %lu instructions in %d programs (max 512), light variants %s",
            (unsigned long)gDvlb->DVLP.codeSize, int(gDvlb->numDVLE), gVariants ? "0/1/8" : "off");

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
        shaderProgramFree(&gProgram1);
        shaderProgramFree(&gProgram0);
        DVLB_Free(gDvlb);
        C3D_RenderTargetDelete(gTarget);
        if (gUnusedTex1Ready)
            C3D_TexDelete(&gUnusedTex1);
        C3D_Fini();
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

    void setGuiOwnsBottomScreen(bool owns)
    {
        if (owns == gGuiOwnsBottom || !gActive)
            return;
        if (owns && gBottomTarget == nullptr)
        {
            gBottomTarget = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, -1);
            if (gBottomTarget == nullptr)
            {
                P3DS_LOGE("render3ds: no bottom-screen render target: the GUI stays off the bottom screen");
                return;
            }
            C3D_RenderTargetSetOutput(gBottomTarget, GFX_BOTTOM, GFX_LEFT, kBottomTransferFlags);
        }
        gGuiOwnsBottom = owns;
        gBottomHash = 0;
        p3ds::textfb::setGpuOwned(owns);
        P3DS_LOGI("render3ds: bottom screen drawn by %s", owns ? "the GUI (GPU)" : "the text display (CPU)");
    }

    bool guiOwnsBottomScreen()
    {
        return gGuiOwnsBottom;
    }

    void requestCapture(const char* path)
    {
        gCapturePath = path;
    }

    const FrameStats& getLastFrameStats()
    {
        return gLastStats;
    }
}
