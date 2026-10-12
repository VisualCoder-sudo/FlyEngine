#pragma once

#include "box3d/box3d.h"

namespace city {

// Play-mode collision for cities. Simulation::StartPlay attaches its world;
// every city tile then owns one static body (building boxes/hulls plus a road
// and pad surface mesh) that is rebuilt whenever the tile is. Detach before the
// world is destroyed.
void AttachPhysicsWorld(b3WorldId world, float friction, float restitution);
void DetachPhysicsWorld();

} // namespace city
