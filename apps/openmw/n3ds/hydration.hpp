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
// (rendering, physics, mechanics, Lua "active") while it is near the player; the 3x3 grid of exterior cells
// keeps all its references, but their models, collision shapes and actor controllers don't fit in memory
// together. Fog hides everything past the viewing distance anyway. Interiors are not affected.
namespace omw3ds::hydration
{
    // Objects are inserted inside kInsertRadius and removed outside kRemoveRadius (horizontal distance to the
    // player): the viewing distance (2560) plus a margin for the size of objects, and some hysteresis.
    constexpr float kInsertRadius = 3072.f;
    constexpr float kRemoveRadius = 3584.f;

    // Scene::loadCell: the position the cell is loaded around (the player's).
    void setLoadCenter(const osg::Vec3f& position);

    // Scene::insertCell (exterior cells): drops the objects outside kInsertRadius from the load center.
    void filterInsert(std::vector<MWWorld::Ptr>& toInsert);

    // Scene::update, every frame: checks one active exterior cell per frame, inserting the objects that came
    // within kInsertRadius (a few per frame) and removing those beyond kRemoveRadius.
    void update(MWWorld::Scene& scene);
}

#endif
