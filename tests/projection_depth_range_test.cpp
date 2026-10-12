// Tests for the depth-range split in the backend math layer.
//
// The raylib backend and the Vulkan backend are compiled into the same binary
// but need different clip-space depth ranges: OpenGL wants [-1, +1], Vulkan and
// Metal want [0, +1]. Getting this wrong does not fail to compile. It produces
// a projection that looks plausible and renders geometry at the wrong depth, so
// the symptom is either z-fighting or geometry clipped away entirely. These
// tests pin down both ranges, plus the derived behaviour the shadow pass and
// the water reflection pass depend on.
//
// Run: ctest -R projection_depth_range  (requires FLYENGINE_BUILD_TESTS=ON)

#include "Engine/Backend/Math.hpp"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void Check(const char* name, bool ok) {
    if (!ok) {
        std::printf("FAIL  %s\n", name);
        ++g_failures;
    } else {
        std::printf("ok    %s\n", name);
    }
}

void CheckNear(const char* name, float got, float want, float tol = 1e-4f) {
    if (!(std::fabs(got - want) <= tol)) {
        std::printf("FAIL  %s (got %g, want %g)\n", name, got, want);
        ++g_failures;
    } else {
        std::printf("ok    %s\n", name);
    }
}

// Post-divide z of a view-space point under a projection matrix. This is the
// value the rasteriser sees, so it is the thing that must land in the right
// range regardless of how the matrix was built.
float NdcZ(const gpu::Mat4& proj, float viewZ) {
    const gpu::Vec4 clip = math::TransformVec4(proj, {0.0f, 0.0f, viewZ, 1.0f});
    if (clip.w == 0.0f) return std::nan("");
    return clip.z / clip.w;
}

// cglm's right-handed convention places the eye at the origin looking down -Z,
// so near and far planes are negative view-space z.
constexpr float kNear = 0.1f;
constexpr float kFar = 100.0f;
constexpr float kNearViewZ = -kNear;
constexpr float kFarViewZ = -kFar;

} // namespace

int main() {
    std::printf("== perspective, OpenGL depth range [-1, +1] ==\n");
    {
        const gpu::Mat4 p = math::Perspective(1.0f, 1.0f, kNear, kFar);
        CheckNear("perspective NO: near -> -1", NdcZ(p, kNearViewZ), -1.0f, 1e-3f);
        CheckNear("perspective NO: far  -> +1", NdcZ(p, kFarViewZ), 1.0f, 1e-3f);
        // Monotonic: NDC z must increase with distance for the depth test to work.
        Check("perspective NO: depth is monotonic",
              NdcZ(p, kNearViewZ) < NdcZ(p, -1.0f) &&
                  NdcZ(p, -1.0f) < NdcZ(p, kFarViewZ));
    }

    std::printf("\n== perspective, Vulkan/Metal depth range [0, +1] ==\n");
    {
        const gpu::Mat4 p = math::PerspectiveZO(1.0f, 1.0f, kNear, kFar);
        CheckNear("perspective ZO: near -> 0", NdcZ(p, kNearViewZ), 0.0f, 1e-3f);
        CheckNear("perspective ZO: far  -> +1", NdcZ(p, kFarViewZ), 1.0f, 1e-3f);
        Check("perspective ZO: depth is monotonic",
              NdcZ(p, kNearViewZ) < NdcZ(p, -1.0f) &&
                  NdcZ(p, -1.0f) < NdcZ(p, kFarViewZ));
        // The whole point of the split: at the same view-space depth, the ZO and
        // NO matrices must disagree, or one of the two backends is getting the
        // other's range and this whole distinction is decorative.
        const gpu::Mat4 q = math::Perspective(1.0f, 1.0f, kNear, kFar);
        Check("perspective ZO and NO differ at mid depth",
              std::fabs(NdcZ(p, -10.0f) - NdcZ(q, -10.0f)) > 1e-3f);
        // The exact relation is ndc_no = 2 * ndc_zo - 1, i.e. an affine remap of
        // the ZO depth onto OpenGL's range. It is NOT a fixed +0.5 offset in
        // NDC space, because the clip-space w differs between the two matrices
        // and the divide happens before the comparison.
        CheckNear("perspective ndc_no = 2 * ndc_zo - 1", NdcZ(q, -10.0f),
                  2.0f * NdcZ(p, -10.0f) - 1.0f, 1e-3f);
    }

    std::printf("\n== orthographic, OpenGL depth range [-1, +1] ==\n");
    {
        const gpu::Mat4 o = math::Ortho(-50.0f, 50.0f, -50.0f, 50.0f, kNear, kFar);
        CheckNear("ortho NO: near -> -1", NdcZ(o, kNearViewZ), -1.0f, 1e-3f);
        CheckNear("ortho NO: far  -> +1", NdcZ(o, kFarViewZ), 1.0f, 1e-3f);
    }

    std::printf("\n== orthographic, Vulkan/Metal depth range [0, +1] ==\n");
    {
        // This is the matrix the directional-light shadow pass must use. Its near
        // plane landing at exactly 0 is what makes a depth comparison against a
        // sampled shadow-map value mean the same thing it did on OpenGL.
        const gpu::Mat4 zo = math::OrthoZO(-50.0f, 50.0f, -50.0f, 50.0f, kNear, kFar);
        const gpu::Mat4 no = math::Ortho(-50.0f, 50.0f, -50.0f, 50.0f, kNear, kFar);
        CheckNear("ortho ZO: near -> 0", NdcZ(zo, kNearViewZ), 0.0f, 1e-3f);
        CheckNear("ortho ZO: far  -> +1", NdcZ(zo, kFarViewZ), 1.0f, 1e-3f);
        Check("ortho ZO: depth is monotonic", NdcZ(zo, kNearViewZ) < NdcZ(zo, kFarViewZ));

        // For an orthographic projection w is constant, so the remap from ZO to
        // OpenGL's range is an exact affine transform that can be applied to a
        // stored depth value. This is the conversion a shadow-map sampler would
        // need if a ZO shadow map were read by an NO pipeline.
        CheckNear("ortho ndc_no = 2 * ndc_zo - 1", NdcZ(no, -10.0f),
                  2.0f * NdcZ(zo, -10.0f) - 1.0f, 1e-3f);

        // Lateral extents must be identical between the two ranges; only depth
        // should differ. A shadow map that is wider or narrower than intended
        // silently changes its texel footprint and therefore the PCF radius.
        for (int col = 0; col < 2; ++col)
            for (int row = 0; row < 4; ++row)
                CheckNear("ortho ZO/NO agree on x/y", zo.m[col][row], no.m[col][row], 1e-4f);
    }

    std::printf("\n== shadow-map depth comparison ==\n");
    {
        // The scenario this exists for: a fragment inside the shadow frustum is
        // rendered into a ZO shadow map, then compared against a stored depth
        // value. Both sides must use the same range or every fragment reads as
        // lit or occluded regardless of the actual bias.
        const gpu::Mat4 lightView =
            math::LookAt({0.0f, 50.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f});
        const gpu::Mat4 lightProj = math::OrthoZO(-25.0f, 25.0f, -25.0f, 25.0f, 1.0f, 100.0f);

        // An occluder 10 units above the receiver, both on the ground plane.
        const gpu::Vec3 occluder{0.0f, 10.0f, 0.0f};
        const gpu::Vec3 receiver{0.0f, 0.0f, 0.0f};

        const gpu::Vec4 occluderClip =
            math::TransformVec4(math::Mul(lightProj, lightView), {occluder.x, occluder.y,
                                                                 occluder.z, 1.0f});
        const gpu::Vec4 receiverClip =
            math::TransformVec4(math::Mul(lightProj, lightView),
                                {receiver.x, receiver.y, receiver.z, 1.0f});

        const float occluderDepth = occluderClip.z / occluderClip.w;
        const float receiverDepth = receiverClip.z / receiverClip.w;

        Check("both fragments are inside the shadow frustum",
              occluderDepth >= 0.0f && occluderDepth <= 1.0f && receiverDepth >= 0.0f &&
                  receiverDepth <= 1.0f);
        // Higher geometry is nearer the light, so it must store a smaller depth.
        Check("occluder stores a smaller depth than receiver", occluderDepth < receiverDepth);
        // The receiver should read as occluded: receiver depth > stored occluder
        // depth fails the <= comparison, which is what a shadow factor of 0 wants.
        Check("receiver reads as occluded", receiverDepth - occluderDepth > 1e-3f);
    }

    std::printf("\n== the two ranges are not interchangeable ==\n");
    {
        // Guards against a "simplification" that collapses both helpers onto
        // one another. Feeding a ZO projection where an NO one is expected must
        // produce a near plane at 0, not -1, because that is the bug this split
        // exists to make impossible to reintroduce silently.
        const gpu::Mat4 zo = math::PerspectiveZO(1.0f, 1.0f, kNear, kFar);
        Check("ZO projection does not produce -1 at near",
              std::fabs(NdcZ(zo, kNearViewZ) + 1.0f) > 1e-3f);
        const gpu::Mat4 no = math::Perspective(1.0f, 1.0f, kNear, kFar);
        Check("NO projection does not produce 0 at near",
              std::fabs(NdcZ(no, kNearViewZ)) > 1e-3f);
    }

    if (g_failures == 0) {
        std::printf("\nall projection depth range tests passed\n");
        return 0;
    }
    std::printf("\n%d projection depth range test(s) failed\n", g_failures);
    return 1;
}