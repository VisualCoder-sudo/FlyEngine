#pragma once
#include <vector>
#include "raylib.h"

// CPU occlusion culling for the main 3D pass.
//
// Large opaque boxes (walls, roofs, big blocks) are rasterized into a small depth buffer once per frame;
// objects whose bounding box lies completely behind that depth are not drawn. The test is conservative: it
// only reports "hidden" when every depth texel the object's box could touch is covered by an occluder that is
// in front of it, so the worst case is drawing something that was not needed, never dropping something visible.
//
// BeginFrame() takes the camera from the current rlgl matrices (call it inside BeginMode3D()), and the buffer
// is only used while those matrices are unchanged, so the shadow and reflection passes -- which draw with other
// matrices -- are never culled by it.
namespace occlusion {

struct OccluderBox {
    Vector3 corners[8];   // world-space corners of a solid box (any order; see kBoxFaces in Occlusion.cpp)
    float volume = 0.0f;  // used to keep only the biggest occluders
};

void BeginFrame(std::vector<OccluderBox>& occluders);
void EndFrame();

// True if `box` is certainly hidden behind the occluders of this frame.
bool IsOccluded(const BoundingBox& box);

} // namespace occlusion
