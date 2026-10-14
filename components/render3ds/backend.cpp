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

#include <platform3ds/log.hpp>
#include <platform3ds/memprobe.hpp>
#include <platform3ds/screenshot.hpp>

#include <citro3d.h>

#include <algorithm>
#include <cmath>
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
        bool gInFrame = false;

        struct UniformLocations
        {
            int mProjection, mModelView, mTexMat, mSunDir, mSunDiffuse, mSunAmbient, mMatDiffuse, mMatAmbient,
                mMatEmission, mColorSel, mLights, mLightLoop, mHasLights;
        } gLoc;

        FrameStats gStats;
        FrameStats gLastStats;
        uint32_t gFrame = 0; // backend frame counter (resolved-state cache stamp)
        float gNear = 1, gFar = 1000; // current perspective projection (fog LUT)

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
            uint8_t mStride = 0;
            uint8_t mLayout = 0; // bit per Attribute present in the VBO
            u32 mPermutation = 0;
            uint8_t mAttribCount = 0;
            osg::Vec4f mOverallColor{ 1, 1, 1, 1 }; // fixed color (0..255) when no per-vertex colors
            void* mIbo = nullptr;
            uint32_t mIboSize = 0;
            std::vector<GpuPrimitive> mPrimitives;

            // Change detection.
            const osg::Array* mArrays[Attr_Count] = {};
            unsigned mModified[Attr_Count] = {};
            unsigned mPrimitiveSignature = 0;
            bool mValid = false;

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

            uint8_t layout = 1 << Attr_Position;
            uint8_t stride = 12;
            if (normals)
            {
                layout |= 1 << Attr_Normal;
                stride += 12;
            }
            if (colors4f || colors4ub)
            {
                layout |= 1 << Attr_Color;
                stride += 4;
            }
            if (texcoords)
            {
                layout |= 1 << Attr_TexCoord;
                stride += 8;
            }

            // Constant color for BIND_OVERALL (or missing) colors, in the shader's 0..255 scale.
            gpu.mOverallColor = osg::Vec4f(255, 255, 255, 255);
            if (const osg::Array* overall = geometry.getColorArray();
                overall && overall->getBinding() == osg::Array::BIND_OVERALL && overall->getNumElements() > 0)
            {
                if (auto* c = dynamic_cast<const osg::Vec4Array*>(overall))
                    gpu.mOverallColor = (*c)[0] * 255.f;
                else if (auto* cub = dynamic_cast<const osg::Vec4ubArray*>(overall))
                    gpu.mOverallColor = osg::Vec4f((*cub)[0].r(), (*cub)[0].g(), (*cub)[0].b(), (*cub)[0].a());
            }

            const uint32_t size = count * stride;
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

            uint8_t* out = static_cast<uint8_t*>(gpu.mVbo);
            for (unsigned i = 0; i < count; ++i)
            {
                std::memcpy(out, &(*positions)[i], 12);
                out += 12;
                if (normals)
                {
                    std::memcpy(out, &(*normals)[i], 12);
                    out += 12;
                }
                if (colors4f)
                {
                    const osg::Vec4f& c = (*colors4f)[i];
                    out[0] = static_cast<uint8_t>(std::clamp(c.r(), 0.f, 1.f) * 255.f + 0.5f);
                    out[1] = static_cast<uint8_t>(std::clamp(c.g(), 0.f, 1.f) * 255.f + 0.5f);
                    out[2] = static_cast<uint8_t>(std::clamp(c.b(), 0.f, 1.f) * 255.f + 0.5f);
                    out[3] = static_cast<uint8_t>(std::clamp(c.a(), 0.f, 1.f) * 255.f + 0.5f);
                    out += 4;
                }
                else if (colors4ub)
                {
                    std::memcpy(out, &(*colors4ub)[i], 4);
                    out += 4;
                }
                if (texcoords)
                {
                    std::memcpy(out, &(*texcoords)[i], 8);
                    out += 8;
                }
            }
            flushToGpu(gpu.mVbo, size);

            gpu.mVertexCount = count;
            gpu.mStride = stride;
            gpu.mLayout = layout;
            gpu.mAttribCount = 0;
            gpu.mPermutation = 0;
            for (int a = 0; a < Attr_Count; ++a)
                if (layout & (1 << a))
                    gpu.mPermutation |= static_cast<u32>(a) << (4 * gpu.mAttribCount++);

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
                        gpu.mPrimitives.push_back({ mode, static_cast<uint32_t>(arrays->getCount()),
                            static_cast<uint32_t>(arrays->getFirst()), -1,
                            triangleCount(set->getMode(), arrays->getCount()) });
                        break;
                    }
                    case osg::PrimitiveSet::DrawArrayLengthsPrimitiveType:
                    {
                        auto* lengths = static_cast<const osg::DrawArrayLengths*>(set.get());
                        uint32_t first = lengths->getFirst();
                        for (GLint length : *lengths)
                        {
                            gpu.mPrimitives.push_back({ mode, static_cast<uint32_t>(length), first, -1,
                                triangleCount(set->getMode(), length) });
                            first += length;
                        }
                        break;
                    }
                    case osg::PrimitiveSet::DrawElementsUBytePrimitiveType:
                    {
                        auto* elements = static_cast<const osg::DrawElementsUByte*>(set.get());
                        std::memcpy(indexOut, elements->getDataPointer(), elements->size());
                        gpu.mPrimitives.push_back({ mode, static_cast<uint32_t>(elements->size()), offset,
                            C3D_UNSIGNED_BYTE, triangleCount(set->getMode(), elements->size()) });
                        offset += (elements->size() + 3) & ~3u;
                        break;
                    }
                    case osg::PrimitiveSet::DrawElementsUShortPrimitiveType:
                    {
                        auto* elements = static_cast<const osg::DrawElementsUShort*>(set.get());
                        std::memcpy(indexOut, elements->getDataPointer(), elements->size() * 2);
                        gpu.mPrimitives.push_back({ mode, static_cast<uint32_t>(elements->size()), offset,
                            C3D_UNSIGNED_SHORT, triangleCount(set->getMode(), elements->size()) });
                        offset += (elements->size() * 2 + 3) & ~3u;
                        break;
                    }
                    case osg::PrimitiveSet::DrawElementsUIntPrimitiveType:
                    {
                        auto* elements = static_cast<const osg::DrawElementsUInt*>(set.get());
                        auto* out16 = reinterpret_cast<uint16_t*>(indexOut);
                        bool fits = true;
                        for (size_t i = 0; i < elements->size(); ++i)
                        {
                            const unsigned index = (*elements)[i];
                            fits &= index < 65536;
                            out16[i] = static_cast<uint16_t>(index);
                        }
                        if (fits)
                            gpu.mPrimitives.push_back({ mode, static_cast<uint32_t>(elements->size()), offset,
                                C3D_UNSIGNED_SHORT, triangleCount(set->getMode(), elements->size()) });
                        offset += (elements->size() * 2 + 3) & ~3u;
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
                if (verticesChanged)
                    gpu->mValid = buildVertices(*gpu, geometry);
                if (gpu->mValid && primitiveSignature(geometry) != gpu->mPrimitiveSignature)
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
            const osg::Uniform* mSun[3] = {};
            unsigned mSunModified[3] = {};
            const SceneUtil::Material* mMaterial = nullptr;
            float mAlpha = -1;
            const osg::TexMat* mTexMat = nullptr;
            bool mTexMatSet = false;
            C3D_Tex* mTexture = nullptr;
            int mTexEnv = -1; // 0 = vertex color, 1 = texture x vertex color
            uint8_t mLayout = 0xFF;
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

        void setAttributeLayout(const GpuGeometry& gpu)
        {
            if (gBound.mLayout != gpu.mLayout)
            {
                gBound.mLayout = gpu.mLayout;
                C3D_AttrInfo* attr = C3D_GetAttrInfo();
                AttrInfo_Init(attr);
                // Attribute index == input register, so fixed attribute ids are registers too.
                for (int a = 0; a < Attr_Count; ++a)
                {
                    if (!(gpu.mLayout & (1 << a)))
                    {
                        AttrInfo_AddFixed(attr, a);
                        continue;
                    }
                    switch (a)
                    {
                        case Attr_Position:
                        case Attr_Normal:
                            AttrInfo_AddLoader(attr, a, GPU_FLOAT, 3);
                            break;
                        case Attr_Color:
                            AttrInfo_AddLoader(attr, a, GPU_UNSIGNED_BYTE, 4);
                            break;
                        case Attr_TexCoord:
                            AttrInfo_AddLoader(attr, a, GPU_FLOAT, 2);
                            break;
                    }
                }
            }
            // Fixed values for the missing attributes (per geometry: the overall color varies).
            if (!(gpu.mLayout & (1 << Attr_Normal)))
                C3D_FixedAttribSet(Attr_Normal, 0.f, 0.f, 1.f, 0.f);
            if (!(gpu.mLayout & (1 << Attr_Color)))
                C3D_FixedAttribSet(Attr_Color, gpu.mOverallColor.r(), gpu.mOverallColor.g(), gpu.mOverallColor.b(),
                    gpu.mOverallColor.a());
            if (!(gpu.mLayout & (1 << Attr_TexCoord)))
                C3D_FixedAttribSet(Attr_TexCoord, 0.f, 0.f, 0.f, 0.f);

            C3D_BufInfo* buf = C3D_GetBufInfo();
            BufInfo_Init(buf);
            BufInfo_Add(buf, gpu.mVbo, gpu.mStride, gpu.mAttribCount, gpu.mPermutation);
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

            C3D_BoolUnifSet(GPU_VERTEX_SHADER, gLoc.mHasLights, count > 0);
            if (count == 0)
                return;
            C3D_IVUnifSet(GPU_VERTEX_SHADER, gLoc.mLightLoop, count - 1, 0, 4, 0);
            const osg::FloatArray& data = *buffer->getFloatArray();
            count = std::min<int>(count, static_cast<int>(data.size() / 16));
            C3D_FVec* out = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, gLoc.mLights, count * 4);
            for (int i = 0; i < count; ++i)
            {
                // components/sceneutil/lightmanager.cpp configure*(): row 0 position + constant, row 1 ambient +
                // linear, row 2 diffuse + quadratic, row 3 specular + radius.
                const float* m = &data[i * 16];
                const float radius = std::max(m[15], 1.f);
                out[i * 4 + 0] = FVec4_New(m[0], m[1], m[2], 4.f / radius);
                out[i * 4 + 1] = FVec4_New(m[8], m[9], m[10], 0.f);
                out[i * 4 + 2] = FVec4_New(m[4], m[5], m[6], 0.f);
                out[i * 4 + 3] = FVec4_New(m[3], m[7], m[11], 0.f);
            }
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

        // ------------------------------------------------------------------ leaves, bins, stages

        void drawLeaf(osgUtil::RenderLeaf* leaf)
        {
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

            setAttributeLayout(*gpu);
            for (const GpuPrimitive& prim : gpu->mPrimitives)
            {
                if (prim.mIndexType < 0)
                    C3D_DrawArrays(prim.mMode, static_cast<int>(prim.mFirst), static_cast<int>(prim.mCount));
                else
                    C3D_DrawElements(prim.mMode, static_cast<int>(prim.mCount), prim.mIndexType,
                        static_cast<uint8_t*>(gpu->mIbo) + prim.mFirst);
                ++gStats.mDraws;
                gStats.mTriangles += prim.mTriangles;
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
            // Waits for the GPU to finish the previous frame (not for vblank: C3D_FRAME_SYNCDRAW would).
            if (!C3D_FrameBegin(0))
                return false;
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
            return true;
        }

        bool drawStageHook(osgUtil::RenderStage* stage, osg::RenderInfo&, osgUtil::RenderLeaf*&)
        {
            if (!gActive)
                return false;
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

            C3D_BindProgram(&gProgram);
            drawBin(stage);

            gStats.mEmitMs += static_cast<float>(p3ds::memprobe::ticks() - start) / CPU_TICKS_PER_MSEC;
            return true;
        }

        void swapHook()
        {
            if (!gInFrame)
                return;
            p3ds::perf::Phase phase("frameEnd");
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
        if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2))
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
        gLoc.mLightLoop = loc("lightLoop");
        gLoc.mHasLights = loc("hasLights");

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
        if (gInFrame)
        {
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            gInFrame = false;
        }
        // GPU objects of still-alive OSG objects stay allocated: the process is about to exit.
        shaderProgramFree(&gProgram);
        DVLB_Free(gDvlb);
        C3D_RenderTargetDelete(gTarget);
        C3D_Fini();
    }

    bool isActive()
    {
        return gActive;
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
