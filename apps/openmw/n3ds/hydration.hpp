#ifndef OPENMW_APPS_OPENMW_N3DS_HYDRATION_H
#define OPENMW_APPS_OPENMW_N3DS_HYDRATION_H

#include <osg/Vec3f>

#include <vector>

namespace MWWorld
{
    class Ptr;
    class Scene;
}

// Radial hydration (the Vita port's technique, M5). In exterior cells, an object is only in the scene
// (rendering, physics, mechanics, Lua "active") while it is near the player. The 3x3 grid of exterior cells
// keeps all its references, but their models, collision shapes and actor controllers don't fit in memory
// together. Fog hides everything past the viewing distance anyway. Interiors are not affected.
namespace omw3ds::hydration
{
    // Objects are inserted inside kInsertRadius and removed outside kRemoveRadius (horizontal distance to the
    // player): the viewing distance (2560) plus a margin for the size of objects, and some hysteresis.
    constexpr float kInsertRadius = 3072.f;
    constexpr float kRemoveRadius = 3584.f;
    // Actors come in closer (still inside the thick fog): the floor under them (a dock, a bridge) must already be
    // there, or they fall through to the ground below (hardware test #17: a guard under the Seyda Neen docks).
    constexpr float kActorInsertRadius = 2304.f;
    // Objects this much farther out get their models loaded in the background (Scene::preload, the work queue
    // thread), so inserting them later doesn't read the SD card on the main thread (hardware test #18: stalls of
    // up to 0.65 s while walking). The finished work items hold the models until the object is inserted (or leaves
    // the ring), past the resource cache's 5 s expiry.
    constexpr float kPreloadMargin = 768.f;

    // Scene::loadCell: the position the cell is loaded around (the player's).
    void setLoadCenter(const osg::Vec3f& position);

    // Scene::insertCell (exterior cells): drops the objects outside kInsertRadius from the load center.
    void filterInsert(std::vector<MWWorld::Ptr>& toInsert);

    // Scene::update, every frame: checks one active exterior cell per frame, inserting the objects that came
    // within range once their preload is done (scenery before actors, nearest first, within a time budget per
    // frame) and removing those beyond kRemoveRadius.
    void update(MWWorld::Scene& scene);
}

#endif
