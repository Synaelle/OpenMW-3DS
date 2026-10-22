#include "objects.hpp"
#include <components/misc/omw3dsdetail.hpp>

#include <osg/Group>
#ifdef OPENMW_PLATFORM_3DS
#include <cmath>
#endif

#include <components/misc/resourcehelpers.hpp>
#include <components/misc/strings/algorithm.hpp>
#include <components/sceneutil/positionattitudetransform.hpp>
#include <components/sceneutil/unrefqueue.hpp>
#include <components/sceneutil/userdata.hpp>
#include <components/sceneutil/visitor.hpp>

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include "animation.hpp"
#include "creatureanimation.hpp"
#include "esm4npcanimation.hpp"
#include "npcanimation.hpp"
#include "vismask.hpp"

namespace MWRender
{
#ifdef OPENMW3DS_DEBUG
    namespace
    {
        // The perf log's "cull.cells" phase: the cull traversal of the objects of the loaded cells.
        class CellCullPhase : public osg::NodeCallback
        {
        public:
            void operator()(osg::Node* node, osg::NodeVisitor* nv) override
            {
                p3ds::perf::Phase phase("cull.cells");
                traverse(node, nv);
            }
        };
    }
#endif

    Objects::Objects(Resource::ResourceSystem* resourceSystem, const osg::ref_ptr<osg::Group>& rootNode,
        SceneUtil::UnrefQueue& unrefQueue)
        : mRootNode(rootNode)
        , mResourceSystem(resourceSystem)
        , mUnrefQueue(unrefQueue)
    {
    }

    Objects::~Objects()
    {
        mObjects.clear();

        for (CellMap::iterator iter = mCellSceneNodes.begin(); iter != mCellSceneNodes.end(); ++iter)
            iter->second->getParent(0)->removeChild(iter->second);
        mCellSceneNodes.clear();
#ifdef OPENMW_PLATFORM_3DS
        mCellAreas.clear();
#endif
    }

    void Objects::insertBegin(const MWWorld::Ptr& ptr)
    {
        OMW3DS_DETAIL("obj.begin");
        assert(mObjects.find(ptr.mRef) == mObjects.end());

        osg::ref_ptr<osg::Group> cellnode;

        CellMap::iterator found = mCellSceneNodes.find(ptr.getCell());
        if (found == mCellSceneNodes.end())
        {
            cellnode = new osg::Group;
            cellnode->setName("Cell Root");
#ifdef OPENMW3DS_DEBUG
            cellnode->addCullCallback(new CellCullPhase);
#endif
            mRootNode->addChild(cellnode);
            mCellSceneNodes[ptr.getCell()] = cellnode;
        }
        else
            cellnode = found->second;

        osg::ref_ptr<SceneUtil::PositionAttitudeTransform> insert(new SceneUtil::PositionAttitudeTransform);
#ifdef OPENMW_PLATFORM_3DS
        // Objects grouped by 2048-unit square of their position under the cell node: the cull traversal rejects an
        // off-screen square with one bounding-sphere test instead of one per object (~1800 objects in the 3x3
        // exterior cells, ~150 of them in view at the 2560-unit view distance), and an actor moving dirties the
        // bound of its square's group (tens of children), not of the whole cell (hundreds). Moving objects stay
        // in their square's group (its bound follows them).
        {
            constexpr float areaSize = 2048.f;
            const float* pos = ptr.getRefData().getPosition().pos;
            const std::pair<int, int> area(
                static_cast<int>(std::floor(pos[0] / areaSize)), static_cast<int>(std::floor(pos[1] / areaSize)));
            osg::ref_ptr<osg::Group>& areaNode = mCellAreas[ptr.getCell()][area];
            if (areaNode == nullptr)
            {
                areaNode = new osg::Group;
                areaNode->setName("Cell Area");
                cellnode->addChild(areaNode);
            }
            areaNode->addChild(insert);
        }
#else
        cellnode->addChild(insert);
#endif

        SceneUtil::addUserData(*insert, ptr);

        const float* f = ptr.getRefData().getPosition().pos;

        insert->setPosition(osg::Vec3(f[0], f[1], f[2]));

        const float scale = ptr.getCellRef().getScale();
        osg::Vec3f scaleVec(scale, scale, scale);
        ptr.getClass().adjustScale(ptr, scaleVec, true);
        insert->setScale(scaleVec);

        ptr.getRefData().setBaseNode(std::move(insert));
    }

    void Objects::insertModel(const MWWorld::Ptr& ptr, const std::string& mesh, bool allowLight)
    {
        OMW3DS_DETAIL("obj.model");
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Object);
        bool animated = ptr.getClass().useAnim();
        std::string animationMesh = mesh;
        if (animated && !mesh.empty())
        {
            animationMesh = Misc::ResourceHelpers::correctActorModelPath(
                VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
            if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
                animated = false;
        }

        auto anim = std::make_unique<ObjectAnimation>(ptr, animationMesh, mResourceSystem, animated, allowLight);
#ifdef OPENMW_PLATFORM_3DS
        // Perf batch 2: a still object's particle systems (a candle's flame, a brazier's smoke) can share one
        // recorded loop per kind (render3ds backend.cpp flameLoop). Actors' and spells' stay live.
        if (!animated)
        {
            SceneUtil::FindByClassVisitor finder("ParticleSystem");
            ptr.getRefData().getBaseNode()->accept(finder);
            for (osg::Node* node : finder.mFoundNodes)
                node->setUserValue("still3ds", true);
        }
#endif

        mObjects.emplace(ptr.mRef, std::move(anim));
    }

    void Objects::insertCreature(const MWWorld::Ptr& ptr, const std::string& mesh, bool weaponsShields)
    {
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Actor);

        bool animated = true;
        std::string animationMesh
            = Misc::ResourceHelpers::correctActorModelPath(VFS::Path::toNormalized(mesh), mResourceSystem->getVFS());
        if (animationMesh == mesh && Misc::StringUtils::ciEndsWith(animationMesh, ".nif"))
            animated = false;

        // CreatureAnimation
        std::unique_ptr<ActorAnimation> anim;

        if (weaponsShields)
            anim = std::make_unique<CreatureWeaponAnimation>(ptr, animationMesh, mResourceSystem, animated);
        else
            anim = std::make_unique<CreatureAnimation>(ptr, animationMesh, mResourceSystem, animated);

        ActorAnimation* listener = anim.get();
        if (mObjects.emplace(ptr.mRef, std::move(anim)).second)
            ptr.getClass().getContainerStore(ptr).setContListener(listener);
    }

    void Objects::insertNPC(const MWWorld::Ptr& ptr)
    {
        OMW3DS_DETAIL("obj.npc");
        insertBegin(ptr);
        ptr.getRefData().getBaseNode()->setNodeMask(Mask_Actor);

        if (ptr.getType() == ESM::REC_NPC_4)
        {
            mObjects.emplace(ptr.mRef,
                std::make_unique<ESM4NpcAnimation>(
                    ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem));
        }
        else
        {
            auto anim = std::make_unique<NpcAnimation>(
                ptr, osg::ref_ptr<osg::Group>(ptr.getRefData().getBaseNode()), mResourceSystem);

            NpcAnimation* listener = anim.get();
            if (mObjects.emplace(ptr.mRef, std::move(anim)).second)
            {
                ptr.getClass().getInventoryStore(ptr).setInvListener(listener);
                ptr.getClass().getInventoryStore(ptr).setContListener(listener);
            }
        }
    }

    bool Objects::removeObject(const MWWorld::Ptr& ptr)
    {
        if (!ptr.getRefData().getBaseNode())
            return true;

        const auto iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
        {
            iter->second->removeFromScene();
            mUnrefQueue.push(std::move(iter->second));
            mObjects.erase(iter);

            if (ptr.getClass().isActor())
            {
                if (ptr.getClass().hasInventoryStore(ptr))
                    ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);

                ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
            }

            ptr.getRefData().getBaseNode()->getParent(0)->removeChild(ptr.getRefData().getBaseNode());

            ptr.getRefData().setBaseNode(nullptr);
            return true;
        }
        return false;
    }

    void Objects::removeCell(const MWWorld::CellStore* store)
    {
        for (PtrAnimationMap::iterator iter = mObjects.begin(); iter != mObjects.end();)
        {
            MWWorld::Ptr ptr = iter->second->getPtr();
            if (ptr.getCell() == store)
            {
                if (ptr.getClass().isActor() && ptr.getRefData().getCustomData())
                {
                    if (ptr.getClass().hasInventoryStore(ptr))
                        ptr.getClass().getInventoryStore(ptr).setInvListener(nullptr);
                    ptr.getClass().getContainerStore(ptr).setContListener(nullptr);
                }

                iter->second->removeFromScene();
                mUnrefQueue.push(std::move(iter->second));
                iter = mObjects.erase(iter);
            }
            else
                ++iter;
        }

        CellMap::iterator cell = mCellSceneNodes.find(store);
        if (cell != mCellSceneNodes.end())
        {
            cell->second->getParent(0)->removeChild(cell->second);
            mCellSceneNodes.erase(cell);
        }
#ifdef OPENMW_PLATFORM_3DS
        mCellAreas.erase(store);
#endif
    }

    void Objects::updatePtr(const MWWorld::Ptr& old, const MWWorld::Ptr& cur)
    {
        osg::ref_ptr<osg::Node> objectNode = cur.getRefData().getBaseNode();
        if (!objectNode)
            return;

        MWWorld::CellStore* newCell = cur.getCell();

        osg::Group* cellnode;
        if (mCellSceneNodes.find(newCell) == mCellSceneNodes.end())
        {
            cellnode = new osg::Group;
            mRootNode->addChild(cellnode);
            mCellSceneNodes[newCell] = cellnode;
        }
        else
        {
            cellnode = mCellSceneNodes[newCell];
        }

        if (MWWorld::Ptr* ptr = SceneUtil::findUserData<MWWorld::Ptr>(*objectNode))
            *ptr = cur;

        if (objectNode->getNumParents())
            objectNode->getParent(0)->removeChild(objectNode);
        cellnode->addChild(objectNode);

        PtrAnimationMap::iterator iter = mObjects.find(old.mRef);
        if (iter != mObjects.end())
            iter->second->updatePtr(cur);
    }

    Animation* Objects::getAnimation(const MWWorld::Ptr& ptr)
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second.get();

        return nullptr;
    }

    const Animation* Objects::getAnimation(const MWWorld::ConstPtr& ptr) const
    {
        PtrAnimationMap::const_iterator iter = mObjects.find(ptr.mRef);
        if (iter != mObjects.end())
            return iter->second.get();

        return nullptr;
    }

}
