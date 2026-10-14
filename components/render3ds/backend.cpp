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

#include <citro3d.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
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

        // Set when the GPU hung: the GPU core was reset and nothing is drawn for the rest of the run, so the game
        // keeps running (headless) and HOME closes it normally instead of a frozen console.
        const char* gFailure = nullptr;
        bool gInFrame = false;

        struct UniformLocations
        {
            int mProjection, mModelView, mTexMat, mSunDir, mSunDiffuse, mSunAmbient, mMatDiffuse, mMatAmbient,
                mMatEmission, mColorSel, mLights;
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
                out[2] = FVec4_Scale(p[3], -1.f / gFar);
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
        };

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
            flushToGpu(gpu.mVbo, size);

            gpu.mVertexCount = count;
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
        struct Bound
        {
            const osg::RefMatrix* mProjection = nullptr;
            const osg::RefMatrix* mModelView = nullptr;
            const osg::Uniform* mLightBuffer = nullptr;
            unsigned mLightBufferModified = 0;
            int mLightCount = -1;
            int mLightSlotsLit = kMaxLights; // slots that may hold a real light (all, until written once)
            const osg::Uniform* mSun[3] = {};
            unsigned mSunModified[3] = {};
            const SceneUtil::Material* mMaterial = nullptr;
            float mAlpha = -1;
            const osg::TexMat* mTexMat = nullptr;
            bool mTexMatSet = false;
            C3D_Tex* mTexture = nullptr;
            int mTexEnv = -1; // 0 = vertex color, 1 = texture x vertex color
            FixedState mFixed;
            bool mFixedValid = false;
        } gBound;

        void setTexEnv(bool textured)
        {
            const int wanted = textured ? 1 : 0;
            if (gBound.mTexEnv == wanted)
                return;
            gBound.mTexEnv = wanted;
            C3D_TexEnv* env = C3D_GetTexEnv(0);
            C3D_TexEnvInit(env);
            if (textured)
            {
                C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
                C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
            }
            else
            {
                C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
                C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
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

        float uniformFloat(const Slot<osg::Uniform>& slot, float fallback)
        {
            float value = fallback;
            if (slot.mValue)
                slot.mValue->get(value);
            return value;
        }

        void setVec4(int location, const osg::Vec4f& v)
        {
            C3D_FVUnifSet(GPU_VERTEX_SHADER, location, v.x(), v.y(), v.z(), v.w());
        }

        // The shader's lights are unrolled (scene.v.pica): every slot is evaluated. Slots past `count` get an "off"
        // light (black, far away so no 1/0) - only those that held a real light before, to keep uploads small.
        // data: OpenMW's LightBuffer floats, 16 per light (lightmanager.cpp configure*(): row 0 position +
        // constant, row 1 ambient + linear, row 2 diffuse + quadratic, row 3 specular + radius).
        void writeLights(const float* data, int count)
        {
            const int slots = std::max(count, gBound.mLightSlotsLit);
            if (slots == 0)
                return;
            C3D_FVec* out = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mLights, slots * 4);
            for (int i = 0; i < slots; ++i)
            {
                if (i < count)
                {
                    const float* m = data + i * 16;
                    const float radius = std::max(m[15], 1.f);
                    out[i * 4 + 0] = FVec4_New(m[0], m[1], m[2], 4.f / radius);
                    out[i * 4 + 1] = FVec4_New(m[8], m[9], m[10], 0.f);
                    out[i * 4 + 2] = FVec4_New(m[4], m[5], m[6], 0.f);
                    out[i * 4 + 3] = FVec4_New(m[3], m[7], m[11], 0.f);
                }
                else
                {
                    out[i * 4 + 0] = FVec4_New(0.f, 0.f, 10000.f, 0.f);
                    out[i * 4 + 1] = FVec4_New(0.f, 0.f, 0.f, 0.f);
                    out[i * 4 + 2] = FVec4_New(0.f, 0.f, 0.f, 0.f);
                    out[i * 4 + 3] = FVec4_New(1.f, 0.f, 0.f, 0.f);
                }
            }
            gBound.mLightSlotsLit = count;
        }

        void uploadLighting(const ResolvedState& rs)
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
            if (rs.mPointLightCount.mValue)
                rs.mPointLightCount.mValue->get(count);
            count = std::clamp(count, 0, kMaxLights);
            if (buffer == nullptr || buffer->getFloatArray() == nullptr)
                count = 0;
            const unsigned modified = buffer ? buffer->getModifiedCount() : 0;
            if (buffer == gBound.mLightBuffer && modified == gBound.mLightBufferModified && count == gBound.mLightCount)
                return;
            gBound.mLightBuffer = buffer;
            gBound.mLightBufferModified = modified;
            gBound.mLightCount = count;

            if (count > 0)
                count = std::min<int>(count, static_cast<int>(buffer->getFloatArray()->size() / 16));
            writeLights(count > 0 ? &(*buffer->getFloatArray())[0] : nullptr, count);
        }

        void uploadMaterial(const ResolvedState& rs)
        {
            const float alpha = uniformFloat(rs.mAlpha, 1.f) * uniformFloat(rs.mActorFade, 1.f);
            const SceneUtil::Material* material = rs.mMaterial.mValue;
            if (material == gBound.mMaterial && alpha == gBound.mAlpha)
                return;
            gBound.mMaterial = material;
            gBound.mAlpha = alpha;

            static const SceneUtil::Material sDefault;
            const SceneUtil::Material& m = material ? *material : sDefault;
            osg::Vec4f emission = m.getEmission() * m.getEmissiveMultiplier();
            emission.a() = m.getEmission().a();
            setVec4(gLoc.mMatDiffuse, m.getDiffuse());
            setVec4(gLoc.mMatAmbient, m.getAmbient());
            setVec4(gLoc.mMatEmission, emission);
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
            C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mColorSel, d, a, e, alpha);
        }

        void uploadTexMat(const ResolvedState& rs)
        {
            const osg::TexMat* texMat = rs.mTexMat.mValue;
            if (gBound.mTexMatSet && texMat == gBound.mTexMat)
                return;
            gBound.mTexMat = texMat;
            gBound.mTexMatSet = true;
            osg::Matrixf m;
            if (texMat)
                m = texMat->getMatrix();
            C3D_FVec rows[4];
            setRows(rows, m, 4);
            C3D_FVec* out = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mTexMat, 2);
            out[0] = rows[0];
            out[1] = rows[1];
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

        void uploadFog(const ResolvedState& rs)
        {
            const float start = uniformFloat(rs.mFogStart, 0.f);
            const float end = uniformFloat(rs.mFogEnd, 0.f);
            osg::Vec4f color(0, 0, 0, 1);
            if (rs.mFogColor.mValue)
                rs.mFogColor.mValue->get(color);
            auto channel = [](float v) { return static_cast<u32>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
            const u32 packed = channel(color.r()) | channel(color.g()) << 8 | channel(color.b()) << 16;
            const bool enabled = end > start && end > 0 && gPerspective;

            if (gFog.mValid && gFog.mEnabled == enabled && gFog.mColor == packed && gFog.mStart == start
                && gFog.mEnd == end && gFog.mNear == gNear && gFog.mFar == gFar)
                return;
            const bool lutChanged = !gFog.mValid || gFog.mStart != start || gFog.mEnd != end || gFog.mNear != gNear
                || gFog.mFar != gFar;
            if (lutChanged)
                Log(Debug::Info) << "render3ds: fog " << start << ".." << end << " color " << std::hex << packed
                                    << std::dec << " near " << gNear << " far " << gFar;
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
                    const float distance = i / 128.f * gFar; // W-buffer depth = distance / far
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
                (unsigned long)(tex ? tex->param : 0), (unsigned long)gBufferBase, gBound.mLightCount,
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
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
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
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
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
                    setTexEnv(true);
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
                    setTexEnv(false);
                    C3D_DrawElements(GPU_TRIANGLES, 6, C3D_UNSIGNED_SHORT, ibo16);
                    if (!selfTestStep("L1 scene vertex layout, minimal shader"))
                        return false;
                }

                // L2..: the scene shader (straight-line, lights unrolled), one feature at a time.
                C3D_BindProgram(&gProgram);
                setupAttributes();
                bindVertexBuffer(vbo);
                setTestMatrices(gLoc.mProjection, gLoc.mModelView);
                C3D_FVec* texMat = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mTexMat, 2);
                texMat[0] = FVec4_New(1, 0, 0, 0);
                texMat[1] = FVec4_New(0, 1, 0, 0);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mSunDir, 0, 0, 1, 0);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mSunDiffuse, 0.5f, 0.5f, 0.5f, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mSunAmbient, 0.3f, 0.3f, 0.3f, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mMatDiffuse, 1, 1, 1, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mMatAmbient, 1, 1, 1, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mMatEmission, 0, 0, 0, 1);
                C3D_FVUnifSet(GPU_VERTEX_SHADER, gLoc.mColorSel, 1, 1, 0, 1);
                gBound.mLightSlotsLit = kMaxLights;
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
                setTexEnv(true);
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
                return selfTestStep("L14 + depth test and back-face culling");
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
            gpuFree(vbo);
            gpuFree(probeVbo);
            gpuFree(ibo16);
            gpuFree(ibo8);
            gpuFree(strip);
            gpuFree(white); // the GPU is idle: selfTestStep waited for it
        }

        // ------------------------------------------------------------------ leaves, bins, stages

        void drawLeaf(osgUtil::RenderLeaf* leaf)
        {
            if (gFailure)
                return;
            ++gStats.mLeaves;
            const osg::Geometry* geometry = leaf->_drawable ? leaf->_drawable->asGeometry() : nullptr;
            if (geometry == nullptr || leaf->_modelview == nullptr || leaf->_projection == nullptr
                || leaf->_parent == nullptr)
            {
                ++gStats.mSkipped; // particles, MyGUI, ... (not drawn yet)
                return;
            }
            GpuGeometry* gpu = geometryFor(*geometry);
            if (gpu == nullptr || gpu->mPrimitives.empty())
            {
                ++gStats.mSkipped;
                return;
            }
            const ResolvedState& rs = resolve(leaf->_parent);

            const FixedState fixed = computeFixedState(rs);
            if (fixed.mCull == 3)
                return;
            if (!gBound.mFixedValid || !(fixed == gBound.mFixed))
            {
                applyFixedState(fixed);
                gBound.mFixed = fixed;
                gBound.mFixedValid = true;
            }

            C3D_Tex* texture = rs.mTexture.mValue ? textureFor(*rs.mTexture.mValue) : nullptr;
            if (texture != gBound.mTexture)
            {
                gBound.mTexture = texture;
                if (texture)
                    C3D_TexBind(0, texture);
            }
            setTexEnv(texture != nullptr);

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
            uploadLighting(rs);
            uploadMaterial(rs);
            uploadTexMat(rs);
            uploadFog(rs);

            bindVertexBuffer(gpu->mVbo);
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
                if (verify)
                {
                    verifyDraw(*gpu, prim, *geometry);
                    if (gFailure)
                        return;
                }
            }
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
        void captureTarget(const std::string& path)
        {
            constexpr unsigned kW = 240, kH = 400; // render target, portrait as the LCD
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
            if (p3ds::screenshot::saveRotatedFramebuffer(path.c_str(), out.data(), 400, 240, p3ds::screenshot::Format::BGR8))
                P3DS_LOGI("render3ds: top screen saved to %s", path.c_str());
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
            if (!gCapturePath.empty())
            {
                captureTarget(gCapturePath);
                gCapturePath.clear();
            }
            ++gFrame;
            gResolvedCount = 0;
            gStats = FrameStats{};
            gBound = Bound{};
            processPendingDeletes();
            C3D_TexBind(0, nullptr); // the bound C3D_Tex may have been freed with its texture
            return true;
        }

        bool drawStageHook(osgUtil::RenderStage* stage, osg::RenderInfo&, osgUtil::RenderLeaf*&)
        {
            if (!gActive)
                return false;
            if (gFailure)
                return true; // nothing is drawn any more (and not through the stub GL either)
            // Only the main scene camera is drawn for now. Pre-render (RTT) and post-render stages (the
            // post-processing canvas, MyGUI) are skipped: returning true keeps OSG from drawing them.
            if (!isMainCamera(stage->getCamera()))
                return true;

            p3ds::perf::Phase phase("drawEmit");
            const uint64_t start = p3ds::memprobe::ticks();
            if (!beginFrame())
                return true;

            const u32 clearBits = (stage->getClearMask() & GL_COLOR_BUFFER_BIT ? C3D_CLEAR_COLOR : 0)
                | (stage->getClearMask() & (GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT) ? C3D_CLEAR_DEPTH : 0);
            if (clearBits)
                C3D_RenderTargetClear(
                    gTarget, static_cast<C3D_ClearBits>(clearBits), toRgba8(stage->getClearColor()), 0xFFFFFF);
            C3D_FrameDrawOn(gTarget);

            if (!gSelfTestDone)
            {
                runGpuSelfTest();
                if (gFailure)
                    return true;
            }
            C3D_BindProgram(&gProgram);
            setupAttributes();
            gVerifyThisFrame = gVerifyFramesLeft > 0;
            const uint32_t drawsBefore = gStats.mDraws;
            drawBin(stage);
            if (gFailure)
                return true;
            if (gVerifyThisFrame && gStats.mDraws > drawsBefore)
            {
                --gVerifyFramesLeft;
                P3DS_LOGI("render3ds: draw-by-draw check passed: %lu draws, each completed by the GPU",
                    (unsigned long)(gStats.mDraws - drawsBefore));
                gVerifiedDraws = 0;
            }
            gVerifyThisFrame = false;

            gStats.mEmitMs += static_cast<float>(p3ds::memprobe::ticks() - start) / CPU_TICKS_PER_MSEC;
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

        gDvlb = DVLB_ParseFile(const_cast<u32*>(reinterpret_cast<const u32*>(scene_shbin)), scene_shbin_size);
        shaderProgramInit(&gProgram);
        shaderProgramSetVsh(&gProgram, &gDvlb->DVLE[0]);
        shaderProgramInit(&gProbeProgram);
        shaderProgramSetVsh(&gProbeProgram, &gDvlb->DVLE[1]);
        shaderProgramInit(&gFlatProgram);
        shaderProgramSetVsh(&gFlatProgram, &gDvlb->DVLE[2]);
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
        gLoc.mSunDir = loc("sunDir");
        gLoc.mSunDiffuse = loc("sunDiffuse");
        gLoc.mSunAmbient = loc("sunAmbient");
        gLoc.mMatDiffuse = loc("matDiffuse");
        gLoc.mMatAmbient = loc("matAmbient");
        gLoc.mMatEmission = loc("matEmission");
        gLoc.mColorSel = loc("colorSel");
        gLoc.mLights = loc("lights");

        gLinearBasePa = osConvertVirtToPhys(reinterpret_cast<void*>(__ctru_linear_heap)) & ~7u;
        if (gLinearBasePa + __ctru_linear_heap_size > 0x28000000)
            gBufferBase = gLinearBasePa; // the default base wouldn't reach the end of the heap
        P3DS_LOGI("render3ds: linear heap va %08lx pa %08lx-%08lx (%lu KB); vertex/index base %08lx",
            (unsigned long)__ctru_linear_heap, (unsigned long)gLinearBasePa,
            (unsigned long)(gLinearBasePa + __ctru_linear_heap_size), (unsigned long)(__ctru_linear_heap_size / 1024),
            (unsigned long)gBufferBase);

        if (std::FILE* f = std::fopen("sdmc:/openmw/autorun.txt", "r"))
        {
            char buf[256] = {};
            std::fread(buf, 1, sizeof(buf) - 1, f);
            std::fclose(f);
            gSimulateHang = std::strstr(buf, "fakehang") != nullptr;
        }

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
            return; // citro3d's command queue never completes after a GPU hang: C3D_Fini would wait forever
        if (gInFrame)
        {
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            gInFrame = false;
        }
        // GPU objects of still-alive OSG objects stay allocated: the process is about to exit.
        shaderProgramFree(&gProgram);
        shaderProgramFree(&gProbeProgram);
        shaderProgramFree(&gFlatProgram);
        DVLB_Free(gDvlb);
        C3D_RenderTargetDelete(gTarget);
        C3D_Fini();
    }

    bool isActive()
    {
        return gActive && gFailure == nullptr;
    }

    const char* failure()
    {
        return gFailure;
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
