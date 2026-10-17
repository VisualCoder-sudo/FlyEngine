#pragma once
#include "raylib.h"

// Procedural unit meshes for the city instancer (buildings, props, agents). Needs a GL context.
namespace gfx {

Mesh GenerateWedgeShapeMesh();   // the wedge ShapeType's mesh
void InitCityMeshes();
void ShutdownCityMeshes();
// 0 is the plain box (not owned here) and unknown shapes return nullptr; see GetCityShapeMesh().
const Mesh* CityShapeMesh(int shape);

} // namespace gfx
