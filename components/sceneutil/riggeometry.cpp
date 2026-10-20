#include "riggeometry.hpp"

#include <osg/MatrixTransform>

#include <osgUtil/CullVisitor>

#include <components/debug/debuglog.hpp>
#include <components/misc/strings/algorithm.hpp>

#include "skeleton.hpp"
#include "util.hpp"
#ifdef OPENMW_PLATFORM_3DS
#include <algorithm>
#include <cmath>

#include <platform3ds/memprobe.hpp> // p3ds::perf::Phase (frame breakdown)
#endif

namespace SceneUtil
{

    RigGeometry::RigGeometry()
    {
        setNumChildrenRequiringUpdateTraversal(1);
        // update done in accept(NodeVisitor&)
    }

    RigGeometry::RigGeometry(const RigGeometry& copy, const osg::CopyOp& copyop)
        : Drawable(copy, copyop)
        , mData(copy.mData)
    {
        setSourceGeometry(copy.mSourceGeometry);
        setNumChildrenRequiringUpdateTraversal(1);
    }

#ifdef OPENMW_PLATFORM_3DS
    // OSG runs single-threaded on the 3DS (OpenThreads reports one processor): a frame's draw never overlaps the
    // next frame's update, so one output copy is enough. Saves a vertex and normal copy per animated mesh.
    constexpr unsigned int sOutputCopies = 1;
#else
    constexpr unsigned int sOutputCopies = 2;
#endif
    void RigGeometry::setSourceGeometry(osg::ref_ptr<osg::Geometry> sourceGeometry)
    {
        for (unsigned int i = 0; i < 2; ++i)
            mGeometry[i] = nullptr;

        mSourceGeometry = sourceGeometry;

        for (unsigned int i = 0; i < sOutputCopies; ++i)
        {
            const osg::Geometry& from = *sourceGeometry;

            // DO NOT COPY AND PASTE THIS CODE. Cloning osg::Geometry without also cloning its contained Arrays is
            // generally unsafe. In this specific case the operation is safe under the following two assumptions:
            // - When Arrays are removed or replaced in the cloned geometry, the original Arrays in their place must
            // outlive the cloned geometry regardless. (ensured by mSourceGeometry)
            // - Arrays that we add or replace in the cloned geometry must be explicitely forbidden from reusing
            // BufferObjects of the original geometry. (ensured by vbo below)
            mGeometry[i] = new osg::Geometry(from, osg::CopyOp::SHALLOW_COPY);
            mGeometry[i]->getOrCreateUserDataContainer()->addUserObject(mSourceGeometry.get());

            osg::Geometry& to = *mGeometry[i];
            to.setSupportsDisplayList(false);
            to.setUseVertexBufferObjects(true);
            to.setCullingActive(false); // make sure to disable culling since that's handled by this class
            to.setComputeBoundingBoxCallback(new CopyBoundingBoxCallback());
            to.setComputeBoundingSphereCallback(new CopyBoundingSphereCallback());

            // vertices and normals are modified every frame, so we need to deep copy them.
            // assign a dedicated VBO to make sure that modifications don't interfere with source geometry's VBO.
            osg::ref_ptr<osg::VertexBufferObject> vbo(new osg::VertexBufferObject);
            vbo->setUsage(GL_DYNAMIC_DRAW_ARB);

            osg::ref_ptr<osg::Array> vertexArray
                = static_cast<osg::Array*>(from.getVertexArray()->clone(osg::CopyOp::DEEP_COPY_ALL));
            if (vertexArray)
            {
                vertexArray->setVertexBufferObject(vbo);
                to.setVertexArray(vertexArray);
            }

            if (const osg::Array* normals = from.getNormalArray())
            {
                osg::ref_ptr<osg::Array> normalArray
                    = static_cast<osg::Array*>(normals->clone(osg::CopyOp::DEEP_COPY_ALL));
                if (normalArray)
                {
                    normalArray->setVertexBufferObject(vbo);
                    to.setNormalArray(normalArray, osg::Array::BIND_PER_VERTEX);
                }
            }

            if (const osg::Vec4Array* tangents = dynamic_cast<const osg::Vec4Array*>(from.getTexCoordArray(7)))
            {
                mSourceTangents = tangents;
                osg::ref_ptr<osg::Array> tangentArray
                    = static_cast<osg::Array*>(tangents->clone(osg::CopyOp::DEEP_COPY_ALL));
                tangentArray->setVertexBufferObject(vbo);
                to.setTexCoordArray(7, tangentArray, osg::Array::BIND_PER_VERTEX);
            }
            else
                mSourceTangents = nullptr;
        }
        if (sOutputCopies == 1)
            mGeometry[1] = mGeometry[0];
    }

    osg::ref_ptr<osg::Geometry> RigGeometry::getSourceGeometry() const
    {
        return mSourceGeometry;
    }

    bool RigGeometry::initFromParentSkeleton(osg::NodeVisitor* nv)
    {
        const osg::NodePath& path = nv->getNodePath();
        for (osg::NodePath::const_reverse_iterator it = path.rbegin() + 1; it != path.rend(); ++it)
        {
            osg::Node* node = *it;
            if (node->asTransform())
                continue;
            if (Skeleton* skel = dynamic_cast<Skeleton*>(node))
            {
                mSkeleton = skel;
                break;
            }
        }

        if (!mSkeleton)
        {
            Log(Debug::Error) << "Error: A RigGeometry did not find its parent skeleton";
            return false;
        }

        if (!mData)
        {
            Log(Debug::Error) << "Error: No influence data set on RigGeometry";
            return false;
        }

        mNodes.clear();
        for (const BoneInfo& info : mData->mBones)
        {
            mNodes.push_back(mSkeleton->getBone(info.mName));
            if (!mNodes.back())
                Log(Debug::Error) << "Error: RigGeometry did not find bone " << info.mName;
        }

        return true;
    }

    void RigGeometry::cull(osg::NodeVisitor* nv)
    {
#ifdef OPENMW_PLATFORM_3DS
        p3ds::perf::Phase phase("skinning"); // CPU skinning, in the cull traversal
#endif
        if (!mSkeleton)
        {
            Log(Debug::Error)
                << "Error: RigGeometry rendering with no skeleton, should have been initialized by UpdateVisitor";
            // try to recover anyway, though rendering is likely to be incorrect.
            if (!initFromParentSkeleton(nv))
                return;
        }

        unsigned int traversalNumber = nv->getTraversalNumber();
#ifdef OPENMW_PLATFORM_3DS
        // M10: actors farther than ~11 m from the camera are skinned every other frame (half of them on even
        // frames, half on odd ones); the frame between draws the last skinned pose. CPU skinning was ~3.5% of the
        // main thread with two NPCs in view (the Zeste profile of 20/10); near ones (talking, fighting) keep
        // every frame.
        bool skipThisFrame = false;
        if (mLastFrameNumber != 0 && nv->getVisitorType() == osg::NodeVisitor::CULL_VISITOR
            && ((traversalNumber + (reinterpret_cast<uintptr_t>(mSkeleton) >> 4)) & 1) != 0)
        {
            const float distance
                = static_cast<osgUtil::CullVisitor*>(nv)->getDistanceToEyePoint(getBound().center(), false);
            skipThisFrame = distance > 800.f;
        }
        if (mLastFrameNumber == traversalNumber || skipThisFrame
            || (mLastFrameNumber != 0 && !mSkeleton->getActive()))
#else
        if (mLastFrameNumber == traversalNumber || (mLastFrameNumber != 0 && !mSkeleton->getActive()))
#endif
        {
            osg::Geometry& geom = *getGeometry(mLastFrameNumber);
            nv->pushOntoNodePath(&geom);
            nv->apply(geom);
            nv->popFromNodePath();
            return;
        }
        mLastFrameNumber = traversalNumber;
        osg::Geometry& geom = *getGeometry(mLastFrameNumber);

        mSkeleton->updateBoneMatrices(traversalNumber);
#ifdef OPENMW_PLATFORM_3DS
        // M10: GPU skinning. The backend draws the mesh's bone batches with this frame's bone matrices (the
        // pose, the drawn geometry's user data) instead of vertices skinned here and uploaded again: NPCs cost
        // 1.5-2.4 ms each on hardware that way. The first frame is still skinned below as well, so the drawable's
        // vertices hold a real pose for what reads them on the CPU (ray casts).
        bool firstPose = false;
        if (!mPoseChecked3DS)
        {
            mPoseChecked3DS = true;
            if (Render3DS::gpuSkinning())
                if (Render3DS::SkinMesh* mesh = skinMesh3DS())
                {
                    mPose3DS = new Render3DS::SkinPose;
                    mPose3DS->mMesh = mesh;
                    geom.setUserData(mPose3DS);
                    firstPose = true;
                }
        }
        if (mPose3DS)
        {
            const osg::Matrixf transform3DS
                = mSkinToSkelMatrix ? (*mSkinToSkelMatrix) * mData->mTransform : mData->mTransform;
            std::vector<osg::Matrixf>& bones = mPose3DS->mBones;
            bones.resize(mNodes.size());
            for (size_t i = 0; i < mNodes.size(); ++i)
            {
                if (mNodes[i] != nullptr)
                    bones[i] = mData->mBones[i].mInvBindMatrix * mNodes[i]->mMatrixInSkeletonSpace * transform3DS;
                else
                    bones[i] = osg::Matrixf(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0); // left out, as below
            }
            if (!firstPose)
            {
                nv->pushOntoNodePath(&geom);
                nv->apply(geom);
                nv->popFromNodePath();
                return;
            }
        }
#endif

        // skinning
        const osg::Vec3Array* positionSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getVertexArray());
        const osg::Vec3Array* normalSrc = static_cast<osg::Vec3Array*>(mSourceGeometry->getNormalArray());
        const osg::Vec4Array* tangentSrc = mSourceTangents;

        osg::Vec3Array* positionDst = static_cast<osg::Vec3Array*>(geom.getVertexArray());
        osg::Vec3Array* normalDst = static_cast<osg::Vec3Array*>(geom.getNormalArray());
        osg::Vec4Array* tangentDst = static_cast<osg::Vec4Array*>(geom.getTexCoordArray(7));

        std::vector<osg::Matrixf> boneMatrices(mNodes.size());
        std::vector<Bone*>::const_iterator bone = mNodes.begin();
        std::vector<BoneInfo>::const_iterator boneInfo = mData->mBones.begin();
        for (osg::Matrixf& boneMat : boneMatrices)
        {
            if (*bone != nullptr)
                boneMat = boneInfo->mInvBindMatrix * (*bone)->mMatrixInSkeletonSpace;
            ++bone;
            ++boneInfo;
        }

        osg::Matrixf transform;
        if (mSkinToSkelMatrix)
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;

        for (const auto& [influences, vertices] : mData->mInfluences)
        {
            osg::Matrixf resultMat(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1);

            for (const auto& [index, weight] : influences)
            {
                if (mNodes[index] == nullptr)
                    continue;
                const float* boneMatPtr = boneMatrices[index].ptr();
                float* resultMatPtr = resultMat.ptr();
                for (int i = 0; i < 16; ++i, ++resultMatPtr, ++boneMatPtr)
                    if (i % 4 != 3)
                        *resultMatPtr += *boneMatPtr * weight;
            }

            resultMat *= transform;

            for (unsigned short vertex : vertices)
            {
                (*positionDst)[vertex] = resultMat.preMult((*positionSrc)[vertex]);
                if (normalDst)
                    (*normalDst)[vertex] = osg::Matrixf::transform3x3((*normalSrc)[vertex], resultMat);

                if (tangentDst)
                {
                    const osg::Vec4f& srcTangent = (*tangentSrc)[vertex];
                    osg::Vec3f transformedTangent = osg::Matrixf::transform3x3(
                        osg::Vec3f(srcTangent.x(), srcTangent.y(), srcTangent.z()), resultMat);
                    (*tangentDst)[vertex] = osg::Vec4f(transformedTangent, srcTangent.w());
                }
            }
        }

        positionDst->dirty();
        if (normalDst)
            normalDst->dirty();
        if (tangentDst)
            tangentDst->dirty();

        geom.osg::Drawable::dirtyGLObjects();

        nv->pushOntoNodePath(&geom);
        nv->apply(geom);
        nv->popFromNodePath();
    }

    void RigGeometry::updateBounds(osg::NodeVisitor* nv)
    {
        if (!mSkeleton)
        {
            if (!initFromParentSkeleton(nv))
                return;
        }

        if (!mSkeleton->getActive() && !mBoundsFirstFrame)
            return;
        mBoundsFirstFrame = false;

        mSkeleton->updateBoneMatrices(nv->getTraversalNumber());

        updateSkinToSkelMatrix(nv->getNodePath());

        osg::BoundingBox box;
        osg::Matrixf transform;
        if (mSkinToSkelMatrix)
            transform = (*mSkinToSkelMatrix) * mData->mTransform;
        else
            transform = mData->mTransform;

        size_t index = 0;
        for (const BoneInfo& info : mData->mBones)
        {
            const Bone* bone = mNodes[index++];
            if (bone == nullptr)
                continue;

            osg::BoundingSpheref bs = info.mBoundSphere;
            transformBoundingSphere(bone->mMatrixInSkeletonSpace * transform, bs);
            box.expandBy(bs);
        }

        if (box != _boundingBox)
        {
            _boundingBox = box;
            _boundingSphere = osg::BoundingSphere(_boundingBox);
            _boundingSphereComputed = true;
            for (unsigned int i = 0; i < getNumParents(); ++i)
                getParent(i)->dirtyBound();

            for (unsigned int i = 0; i < 2; ++i)
            {
                osg::Geometry& geom = *mGeometry[i];
                static_cast<CopyBoundingBoxCallback*>(geom.getComputeBoundingBoxCallback())->boundingBox = _boundingBox;
                static_cast<CopyBoundingSphereCallback*>(geom.getComputeBoundingSphereCallback())->boundingSphere
                    = _boundingSphere;
                geom.dirtyBound();
            }
        }
    }

    void RigGeometry::updateSkinToSkelMatrix(const osg::NodePath& nodePath)
    {
        if (mSkinToSkelMatrix)
            mSkinToSkelMatrix->makeIdentity();
        auto skeletonRoot = std::find(nodePath.begin(), nodePath.end(), mSkeleton);
        if (skeletonRoot == nodePath.end())
            return;
        skeletonRoot++;
        auto skinRoot = nodePath.end();
        if (!mData->mRootBone.empty())
            skinRoot = std::find_if(skeletonRoot, nodePath.end(),
                [&](const osg::Node* node) { return Misc::StringUtils::ciEqual(node->getName(), mData->mRootBone); });
        if (skinRoot == nodePath.end())
        {
            // Failed to find skin root, cancel out everything up till the trishape.
            // Our parent node is the trishape's transform
            skinRoot = nodePath.end() - 2;
            if ((*skinRoot)->getName() != getName()) // but maybe it can get optimized out
                skinRoot++;
        }
        else
            skinRoot++;
        for (auto it = skeletonRoot; it != skinRoot; ++it)
        {
            const osg::Node* node = *it;
            if (const osg::Transform* trans = node->asTransform())
            {
                const osg::MatrixTransform* matrixTrans = trans->asMatrixTransform();
                if (matrixTrans && matrixTrans->getMatrix().isIdentity())
                    continue;
                if (!mSkinToSkelMatrix)
                    mSkinToSkelMatrix = new osg::RefMatrix;
                trans->computeWorldToLocalMatrix(*mSkinToSkelMatrix, nullptr);
            }
        }
    }

#ifdef OPENMW_PLATFORM_3DS
    // The mesh's triangles grouped into batches of at most Render3DS::kSkinBones bones (the skinned shader's
    // palette), in order: a triangle starts a new batch when its bones don't fit in the current one. Each vertex
    // keeps its 4 strongest bones, weights normalized to 255 (the CPU path sums the weighted matrices; NIF weights
    // sum to 1). Shared through mData by every copy of the mesh; null when the mesh can't be skinned this way
    // (primitives other than triangle lists and strips, vertices without bones): it stays on the CPU.
    Render3DS::SkinMesh* RigGeometry::skinMesh3DS()
    {
        InfluenceData& data = *mData;
        if (data.mSkinBuilt3DS && data.mSkinSource3DS == mSourceGeometry.get())
            return data.mSkin3DS.get();
        data.mSkinBuilt3DS = true;
        data.mSkinSource3DS = mSourceGeometry.get();
        data.mSkin3DS = nullptr;

        const auto* positions = dynamic_cast<const osg::Vec3Array*>(mSourceGeometry->getVertexArray());
        if (positions == nullptr || positions->empty() || positions->size() > 65535)
            return nullptr;
        const size_t vertexCount = positions->size();
        const size_t boneCount = data.mBones.size();

        struct Influence
        {
            uint16_t mBones[4] = {};
            uint8_t mWeights[4] = {};
            uint8_t mCount = 0;
        };
        std::vector<Influence> influences(vertexCount);
        for (const auto& [weights, vertices] : data.mInfluences)
        {
            BoneWeights strongest = weights;
            std::sort(strongest.begin(), strongest.end(),
                [](const BoneWeight& a, const BoneWeight& b) { return a.second > b.second; });
            if (strongest.size() > 4)
                strongest.resize(4);
            float sum = 0.f;
            for (const auto& [bone, weight] : strongest)
            {
                if (bone >= boneCount || weight < 0.f)
                    return nullptr;
                sum += weight;
            }
            if (sum <= 0.f)
                return nullptr;
            Influence influence;
            int total = 0;
            for (const auto& [bone, weight] : strongest)
            {
                influence.mBones[influence.mCount] = static_cast<uint16_t>(bone);
                const int w = static_cast<int>(std::lround(weight / sum * 255.f));
                influence.mWeights[influence.mCount++] = static_cast<uint8_t>(w);
                total += w;
            }
            influence.mWeights[0] = static_cast<uint8_t>(std::clamp(influence.mWeights[0] + 255 - total, 0, 255));
            for (unsigned short vertex : vertices)
                if (vertex < vertexCount)
                    influences[vertex] = influence;
        }

        std::vector<uint16_t> triangles;
        for (const auto& set : mSourceGeometry->getPrimitiveSetList())
        {
            const unsigned count = set->getNumIndices();
            if (set->getMode() == osg::PrimitiveSet::TRIANGLES)
            {
                for (unsigned i = 0; i + 2 < count; i += 3)
                    for (unsigned k = 0; k < 3; ++k)
                        triangles.push_back(static_cast<uint16_t>(set->index(i + k)));
            }
            else if (set->getMode() == osg::PrimitiveSet::TRIANGLE_STRIP)
            {
                for (unsigned i = 2; i < count; ++i)
                {
                    const unsigned a = set->index(i - 2), b = set->index(i - 1), c = set->index(i);
                    if (a == b || b == c || a == c)
                        continue;
                    // every other triangle of a strip is wound the other way
                    const unsigned first = (i & 1) ? b : a, second = (i & 1) ? a : b;
                    triangles.insert(triangles.end(),
                        { static_cast<uint16_t>(first), static_cast<uint16_t>(second), static_cast<uint16_t>(c) });
                }
            }
            else
                return nullptr;
        }
        if (triangles.empty())
            return nullptr;
        for (uint16_t vertex : triangles)
            if (vertex >= vertexCount || influences[vertex].mCount == 0)
                return nullptr;

        osg::ref_ptr<Render3DS::SkinMesh> mesh = new Render3DS::SkinMesh;
        mesh->mSource = mSourceGeometry;
        std::vector<int> local(vertexCount, -1); // vertex -> batch vertex
        std::vector<int> slotOf(boneCount, -1); // bone -> batch palette slot
        auto startBatch = [&] {
            if (!mesh->mBatches.empty())
            {
                const Render3DS::SkinBatch& done = mesh->mBatches.back();
                for (uint16_t vertex : done.mVertices)
                    local[vertex] = -1;
                for (uint16_t bone : done.mBones)
                    slotOf[bone] = -1;
            }
            mesh->mBatches.emplace_back();
        };
        for (size_t t = 0; t + 2 < triangles.size(); t += 3)
        {
            uint16_t fresh[12];
            size_t freshCount = 0;
            auto collect = [&] {
                freshCount = 0;
                for (size_t k = 0; k < 3; ++k)
                {
                    const Influence& influence = influences[triangles[t + k]];
                    for (int i = 0; i < influence.mCount; ++i)
                    {
                        const uint16_t bone = influence.mBones[i];
                        if (slotOf[bone] < 0 && std::find(fresh, fresh + freshCount, bone) == fresh + freshCount)
                            fresh[freshCount++] = bone;
                    }
                }
            };
            collect();
            if (mesh->mBatches.empty() || mesh->mBatches.back().mBones.size() + freshCount > Render3DS::kSkinBones
                || mesh->mBatches.back().mVertices.size() + 3 > 65535)
            {
                startBatch();
                collect();
            }
            Render3DS::SkinBatch& batch = mesh->mBatches.back();
            for (size_t i = 0; i < freshCount; ++i)
            {
                slotOf[fresh[i]] = static_cast<int>(batch.mBones.size());
                batch.mBones.push_back(fresh[i]);
            }
            for (size_t k = 0; k < 3; ++k)
            {
                const uint16_t vertex = triangles[t + k];
                if (local[vertex] < 0)
                {
                    local[vertex] = static_cast<int>(batch.mVertices.size());
                    batch.mVertices.push_back(vertex);
                    const Influence& influence = influences[vertex];
                    for (int i = 0; i < 4; ++i)
                    {
                        const bool used = i < influence.mCount;
                        batch.mSlots.push_back(used ? static_cast<uint8_t>(slotOf[influence.mBones[i]] * 3) : 0);
                        batch.mWeights.push_back(used ? influence.mWeights[i] : 0);
                    }
                }
                batch.mIndices.push_back(static_cast<uint16_t>(local[vertex]));
            }
        }
        data.mSkin3DS = mesh;
        return mesh.get();
    }
#endif

    void RigGeometry::setBoneInfo(std::vector<BoneInfo>&& bones)
    {
        if (!mData)
            mData = std::make_shared<InfluenceData>();

        mData->mBones = std::move(bones);
    }

    void RigGeometry::setInfluences(const std::vector<BoneWeights>& influences)
    {
        if (!mData)
            mData = std::make_shared<InfluenceData>();

        std::map<BoneWeights, VertexList> influencesToVertices;
        for (size_t i = 0; i < influences.size(); i++)
            influencesToVertices[influences[i]].emplace_back(static_cast<VertexList::value_type>(i));
        influencesToVertices.erase(BoneWeights());

        mData->mInfluences.reserve(influencesToVertices.size());
        mData->mInfluences.assign(influencesToVertices.begin(), influencesToVertices.end());
    }

    void RigGeometry::setTransform(osg::Matrixf&& transform)
    {
        if (!mData)
            mData = std::make_shared<InfluenceData>();
        mData->mTransform = transform;
    }

    void RigGeometry::setRootBone(std::string_view name)
    {
        if (!mData)
            mData = std::make_shared<InfluenceData>();
        mData->mRootBone = name;
    }

    void RigGeometry::accept(osg::NodeVisitor& nv)
    {
        if (!nv.validNodeMask(*this))
            return;

        nv.pushOntoNodePath(this);

        if (nv.getVisitorType() == osg::NodeVisitor::CULL_VISITOR)
        {
            // The cull visitor won't be applied to the node itself,
            // but we want to use its state to render the child geometry.
            osg::StateSet* stateset = getStateSet();
            osgUtil::CullVisitor* cv = static_cast<osgUtil::CullVisitor*>(&nv);
            if (stateset)
                cv->pushStateSet(stateset);

            cull(&nv);
            if (stateset)
                cv->popStateSet();
        }
        else if (nv.getVisitorType() == osg::NodeVisitor::UPDATE_VISITOR)
            updateBounds(&nv);
        else
            nv.apply(*this);

        nv.popFromNodePath();
    }

    void RigGeometry::accept(osg::PrimitiveFunctor& func) const
    {
        getGeometry(mLastFrameNumber)->accept(func);
    }

    osg::Geometry* RigGeometry::getGeometry(unsigned int frame) const
    {
        return mGeometry[frame % 2].get();
    }

}
