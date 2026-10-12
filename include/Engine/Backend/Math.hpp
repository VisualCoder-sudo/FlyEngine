#pragma once

// ============================================================================
// Engine math layer.
//
// Conventions (chosen to match cglm, and therefore Sokol / GLSL / Vulkan):
//   - Mat4 is column-major: m[col][row]. Translation lives in column 3.
//   - Right-handed, clip-space depth range [0, 1] is NOT assumed here; the
//     projection helpers below are cglm defaults, so pin CGLM_CONFIG_CLIP_CONTROL
//     explicitly in the build if you need a specific range.
//   - Vector/matrix math all operate on plain structs so the layer stays
//     free of cglm types in its public API.
//
// NOTE ON RAYLIB COMPATIBILITY:
//
//   raylib and cglm disagree in two independent ways, and both matter when
//   porting a raylib call site. This was verified empirically rather than
//   assumed; see tests/math_backend_parity_test.cpp, which asserts each rule.
//
//   1. Storage. raylib stores `Matrix` row-major (element(row, col) at
//      [row*4 + col]), Mat4 stores column-major (m[col][row]). So converting
//      raylib -> Mat4 requires a TRANSPOSE:
//
//          out.m[c][r] = (&rl.m0)[r * 4 + c]
//
//      Note this is the opposite of what the struct comment in raymath.h
//      ("first row is {m0, m4, m8, m12}") suggests; the fields are declared in
//      memory order m0, m1, m2, ..., so m0..m3 is actually the first column of
//      the memory block and rows are contiguous.
//
//   2. Multiplication. raylib's MatrixMultiply(left, right) uses row-vector
//      convention and returns the transpose of the equivalent column-major
//      product, so after conversion the operands must be REVERSED:
//
//          raylib MatrixMultiply(a, b)  ==  Mul(ToMat4(b), ToMat4(a))
//
// A consequence worth knowing: raylib's MM(scale, rotation, translation) reads
// left-to-right as scale-then-rotate-then-translate, which is why ComposeTRS
// below composes as T * R * S.
// ============================================================================

#include "GraphicsBackendTypes.hpp"

#include <cglm/cglm.h>
#include <cglm/affine.h>
#include <cglm/cam.h>
#include <cglm/mat4.h>
#include <cglm/quat.h>
#include <cglm/vec3.h>
#include <cglm/vec4.h>

// cglm/cam.h only includes the ortho/persp variants matching the configured
// CGLM_CONFIG_CLIP_CONTROL, so the depth-range variants this layer needs are
// not declared unless they are included explicitly. Each is include-guarded
// inside cglm, so including all four here is safe and keeps the depth range a
// per-call-site choice instead of a build-wide macro. See Perspective/Ortho.
#include <cglm/clipspace/ortho_rh_no.h>
#include <cglm/clipspace/ortho_rh_zo.h>
#include <cglm/clipspace/persp_rh_no.h>
#include <cglm/clipspace/persp_rh_zo.h>

#include <algorithm>
#include <cmath>

namespace math {

// The backend-agnostic types live in namespace gfx (see GraphicsBackendTypes.hpp
// for why they are not global). Re-exported here so `math::Vec3` and `gpu::Vec3`
// name the same type and existing `using namespace math;` call sites keep working.
using gpu::Vec2;
using gpu::Vec3;
using gpu::Vec4;
using gpu::Mat4;
using gpu::Quat;
using gpu::Color;

// ============================================================================
// cglm bridging
//
// Mat4 is layout-compatible with cglm's mat4, so these reinterpretations are
// valid by construction. Const inputs need a copy because cglm's scalar-entry
// points take non-const pointers.
// ============================================================================

inline mat4&       Raw(Mat4& m)       noexcept { return *reinterpret_cast<mat4*>(m.m); }
inline const mat4& Raw(const Mat4& m) noexcept { return *reinterpret_cast<const mat4*>(m.m); }
inline vec3&       Raw(Vec3& v)       noexcept { return *reinterpret_cast<vec3*>(&v); }
inline versor&     Raw(Quat& q)       noexcept { return *reinterpret_cast<versor*>(&q); }

// ============================================================================
// Vec2
// ============================================================================

inline Vec2 Vec2Zero() noexcept { return {0, 0}; }
inline Vec2 Vec2One()  noexcept { return {1, 1}; }

inline Vec2 Add(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
inline Vec2 Sub(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
inline Vec2 Mul(Vec2 a, float s) noexcept { return {a.x * s, a.y * s}; }
inline Vec2 Div(Vec2 a, float s) noexcept { return {a.x / s, a.y / s}; }

inline float Dot(Vec2 a, Vec2 b) noexcept { return a.x * b.x + a.y * b.y; }
inline float Length(Vec2 v) noexcept { return std::sqrt(Dot(v, v)); }
inline Vec2 Normalize(Vec2 v) noexcept { const float l = Length(v); return l > 0 ? Div(v, l) : Vec2Zero(); }
inline Vec2 Lerp(Vec2 a, Vec2 b, float t) noexcept { return Add(a, Mul(Sub(b, a), t)); }

// ============================================================================
// Vec3
// ============================================================================

inline Vec3 Vec3Zero()    noexcept { return {0, 0, 0}; }
inline Vec3 Vec3One()     noexcept { return {1, 1, 1}; }
inline Vec3 Vec3Up()      noexcept { return {0, 1, 0}; }
inline Vec3 Vec3Down()    noexcept { return {0, -1, 0}; }
inline Vec3 Vec3Left()    noexcept { return {-1, 0, 0}; }
inline Vec3 Vec3Right()   noexcept { return {1, 0, 0}; }
inline Vec3 Vec3Forward() noexcept { return {0, 0, -1}; }
inline Vec3 Vec3Backward() noexcept { return {0, 0, 1}; }

inline Vec3 Add(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Mul(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 Div(Vec3 a, float s) noexcept { return {a.x / s, a.y / s, a.z / s}; }
inline Vec3 Neg(Vec3 a) noexcept { return {-a.x, -a.y, -a.z}; }

inline float Dot(Vec3 a, Vec3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Vec3 Cross(Vec3 a, Vec3 b) noexcept {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

inline float Length(Vec3 v)   noexcept { return std::sqrt(Dot(v, v)); }
inline float LengthSq(Vec3 v) noexcept { return Dot(v, v); }

inline Vec3 Normalize(Vec3 v) noexcept {
    const float l = Length(v);
    return l > 0 ? Div(v, l) : Vec3Zero();
}

inline Vec3 Lerp(Vec3 a, Vec3 b, float t) noexcept { return Add(a, Mul(Sub(b, a), t)); }
inline Vec3 Min(Vec3 a, Vec3 b) noexcept { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Vec3 Max(Vec3 a, Vec3 b) noexcept { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }
inline Vec3 Clamp(Vec3 v, Vec3 lo, Vec3 hi) noexcept { return Max(lo, Min(v, hi)); }

// ============================================================================
// Vec4
// ============================================================================

inline Vec4 Vec4Zero() noexcept { return {0, 0, 0, 0}; }
inline Vec4 Vec4One()  noexcept { return {1, 1, 1, 1}; }

inline Vec4 Add(Vec4 a, Vec4 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w}; }
inline Vec4 Sub(Vec4 a, Vec4 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w}; }
inline Vec4 Mul(Vec4 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s, a.w * s}; }
inline Vec4 Div(Vec4 a, float s) noexcept { return {a.x / s, a.y / s, a.z / s, a.w / s}; }
inline float Dot(Vec4 a, Vec4 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }

// ============================================================================
// Mat4
// ============================================================================

// Forward declarations. RotateXYZ, ComposeTRS and QuatFromEuler are all
// defined before the primitives they compose from.
inline Mat4 Mul(Mat4 a, Mat4 b) noexcept;
inline Vec3 TransformPoint(Mat4 m, Vec3 v) noexcept;
inline Vec4 TransformVec4(Mat4 m, Vec4 v) noexcept;
inline Quat QuatMul(Quat a, Quat b) noexcept;
inline Quat QuatFromAxisAngle(Vec3 axis, float angle) noexcept;

inline Mat4 Identity() noexcept { Mat4 r; glm_mat4_identity(Raw(r)); return r; }

inline Mat4 Translate(Vec3 v) noexcept {
    Mat4 r; glm_mat4_identity(Raw(r));
    glm_translate(Raw(r), (vec3){v.x, v.y, v.z});
    return r;
}

inline Mat4 Scale(Vec3 v) noexcept {
    Mat4 r; glm_mat4_identity(Raw(r));
    glm_scale(Raw(r), (vec3){v.x, v.y, v.z});
    return r;
}

inline Mat4 RotateX(float rad) noexcept { Mat4 r; glm_mat4_identity(Raw(r)); glm_rotate_x(Raw(r), rad, Raw(r)); return r; }
inline Mat4 RotateY(float rad) noexcept { Mat4 r; glm_mat4_identity(Raw(r)); glm_rotate_y(Raw(r), rad, Raw(r)); return r; }
inline Mat4 RotateZ(float rad) noexcept { Mat4 r; glm_mat4_identity(Raw(r)); glm_rotate_z(Raw(r), rad, Raw(r)); return r; }

// Composition order is R = Rx * Ry * Rz, i.e. Z is applied first.
//
// NOTE: this is chosen so that it is bit-equivalent to raylib's
// MatrixRotateXYZ after the transposing boundary conversion, with the Euler
// angles passed through unchanged. raylib negates each angle internally and
// writes rows contiguously, and the two effects cancel. Verified for all six
// axis permutations in tests/math_backend_parity_test.cpp. Porting a raylib
// call site therefore needs no sign change, only the multiplication order.
inline Mat4 RotateXYZ(Vec3 euler) noexcept {
    return Mul(RotateX(euler.x), Mul(RotateY(euler.y), RotateZ(euler.z)));
}

inline Mat4 Rotate(Vec3 axis, float angle) noexcept {
    Mat4 r; glm_mat4_identity(Raw(r));
    glm_rotate(Raw(r), angle, (vec3){axis.x, axis.y, axis.z});
    return r;
}

// Column-major product: result applies `b` first, then `a`.
// This is the cglm convention and matches raylib's ComposeTRS call ordering
// once the raylib->column-major transpose is accounted for at the boundary.
inline Mat4 Mul(Mat4 a, Mat4 b) noexcept { Mat4 r; glm_mat4_mul(Raw(a), Raw(b), Raw(r)); return r; }

// Transform a point/direction by a matrix. Overload kept next to Mul(Mat4,Mat4)
// so that `Mul(mat, vec)` resolves at the point of use; the declaration order
// matters, so it is repeated as an overload rather than relying on ADL.
inline Vec3 Mul(Mat4 m, Vec3 v) noexcept { return TransformPoint(m, v); }
inline Vec4 Mul(Mat4 m, Vec4 v) noexcept { return TransformVec4(m, v); }

inline Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    Mat4 m;
    glm_lookat((vec3){eye.x, eye.y, eye.z}, (vec3){target.x, target.y, target.z},
               (vec3){up.x, up.y, up.z}, Raw(m));
    return m;
}

// ============================================================================
// Projections
//
// Clip-space depth range is the one convention that cannot be shared between
// backends, so it is resolved by function name rather than by a build-wide macro:
//
//   Perspective / Ortho          -> [-1, +1]  (OpenGL; what raylib produces)
//   PerspectiveZO / OrthoZO      -> [ 0, +1]  (Vulkan, Metal, D3D)
//
// Both exist in the same binary because the raylib and Vulkan backends are both
// compiled into it. Using a global CGLM_CONFIG_CLIP_CONTROL instead would force
// one range on every backend in the process and silently break the other.
//
// Depth range also changes what a shadow map means. An orthographic shadow
// projection built with Ortho() has its near plane at NDC -1, so a depth bias
// tuned against it does not transfer to an OrthoZO() projection. Pick the
// variant matching the depth attachment you are rendering into.
//
// The ZO variants call cglm's explicit *_rh_zo entry points directly rather
// than going through glm_perspective/glm_ortho, whose behaviour depends on
// CGLM_CONFIG_CLIP_CONTROL.
// ============================================================================

// Right-handed, OpenGL depth range [-1, +1]. Matches raylib's MatrixPerspective.
inline Mat4 Perspective(float fovy, float aspect, float near, float far) noexcept {
    Mat4 m;
    glm_perspective_rh_no(fovy, aspect, near, far, Raw(m));
    return m;
}

// Right-handed, OpenGL depth range [-1, +1]. Matches raylib's MatrixOrtho.
inline Mat4 Ortho(float left, float right, float bottom, float top, float near, float far) noexcept {
    Mat4 m;
    glm_ortho_rh_no(left, right, bottom, top, near, far, Raw(m));
    return m;
}

// Right-handed, Vulkan/Metal/D3D depth range [0, +1].
inline Mat4 PerspectiveZO(float fovy, float aspect, float near, float far) noexcept {
    Mat4 m;
    glm_perspective_rh_zo(fovy, aspect, near, far, Raw(m));
    return m;
}

// Right-handed orthographic with depth range [0, +1]. This is the variant the
// directional-light shadow pass must use on Vulkan: the shadow map is a depth
// attachment sampled with CompareFunc, and its NDC range has to match the range
// the pipeline was built with.
inline Mat4 OrthoZO(float left, float right, float bottom, float top, float near, float far) noexcept {
    Mat4 m;
    glm_ortho_rh_zo(left, right, bottom, top, near, far, Raw(m));
    return m;
}



inline Mat4 Transpose(Mat4 m) noexcept { Mat4 r; glm_mat4_transpose_to(Raw(m), Raw(r)); return r; }
inline Mat4 Inverse(Mat4 m)    noexcept { Mat4 r; glm_mat4_inv(Raw(m), Raw(r)); return r; }

// Affine-only inverse (no perspective divide) - matches raylib's normal-matrix path.
inline Mat4 InverseFast(Mat4 m) noexcept { Mat4 r; glm_mat4_inv_fast(Raw(m), Raw(r)); return r; }

// ============================================================================
// Point / direction transforms
// ============================================================================

inline Vec4 TransformVec4(Mat4 m, Vec4 v) noexcept {
    vec4 out;
    glm_mat4_mulv(Raw(m), (vec4){v.x, v.y, v.z, v.w}, out);
    return {out[0], out[1], out[2], out[3]};
}

inline Vec3 TransformPoint(Mat4 m, Vec3 v) noexcept {
    const Vec4 r = TransformVec4(m, {v.x, v.y, v.z, 1.0f});
    if (r.w != 0.0f && r.w != 1.0f) {
        const float iw = 1.0f / r.w;
        return {r.x * iw, r.y * iw, r.z * iw};
    }
    return {r.x, r.y, r.z};
}

inline Vec3 TransformDir(Mat4 m, Vec3 v) noexcept {
    const Vec4 r = TransformVec4(m, {v.x, v.y, v.z, 0.0f});
    return {r.x, r.y, r.z};
}

// Normal matrix = transpose(inverse(upper-3x3)). Kept as a full mat4 so it can
// be fed straight to a shader that declares a mat4 normal matrix, matching
// raylib's SHADER_LOC_MATRIX_NORMAL.
inline Mat4 NormalMatrix(Mat4 model) noexcept { return Transpose(InverseFast(model)); }

inline Vec3 TransformNormal(Mat4 m, Vec3 v) noexcept {
    return TransformDir(NormalMatrix(m), v);
}

// ============================================================================
// TRS helpers
// ============================================================================

// Builds T * R * S, so the vertex is scaled first, then rotated, then
// translated - i.e. standard TRS behaviour, verified to match the existing
// raylib `ComposeTRS` in Graphics.cpp, which produces the same result despite
// raylib storing matrices row-major and multiplying in the opposite order.
inline Mat4 ComposeTRS(Vec3 scale, Mat4 rotation, Vec3 position) noexcept {
    return Mul(Mul(Translate(position), rotation), Scale(scale));
}

inline Mat4 TRS(Vec3 translation, Vec3 rotation, Vec3 scale) noexcept {
    return ComposeTRS(scale, RotateXYZ(rotation), translation);
}

// Extracts translation from column 3.
inline Vec3 TranslationOf(Mat4 m) noexcept { return {m.m[3][0], m.m[3][1], m.m[3][2]}; }

// Extracts per-axis scale from the length of each basis column.
inline Vec3 ScaleOf(Mat4 m) noexcept {
    return {
        std::sqrt(m.m[0][0] * m.m[0][0] + m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2]),
        std::sqrt(m.m[1][0] * m.m[1][0] + m.m[1][1] * m.m[1][1] + m.m[1][2] * m.m[1][2]),
        std::sqrt(m.m[2][0] * m.m[2][0] + m.m[2][1] * m.m[2][1] + m.m[2][2] * m.m[2][2])
    };
}

// Strips translation and scale, leaving the rotation basis. Assumes no shear.
inline Mat4 RotationOf(Mat4 m) noexcept {
    const Vec3 s = ScaleOf(m);
    Mat4 r = m;
    for (int c = 0; c < 3; ++c) {
        const float sc = (c == 0) ? s.x : (c == 1) ? s.y : s.z;
        const float inv = sc != 0 ? 1.0f / sc : 0.0f;
        for (int row = 0; row < 3; ++row) r.m[c][row] *= inv;
    }
    r.m[3][0] = r.m[3][1] = r.m[3][2] = 0.0f;
    r.m[3][3] = 1.0f;
    return r;
}

inline bool DecomposeTRS(Mat4 m, Vec3* outScale, Mat4* outRotation, Vec3* outPosition) noexcept {
    if (outPosition) *outPosition = TranslationOf(m);
    if (outScale)    *outScale    = ScaleOf(m);
    if (outRotation) *outRotation = RotationOf(m);
    return true;
}

// ============================================================================
// Quaternions
// ============================================================================

inline Quat QuatIdentity() noexcept { return {0, 0, 0, 1}; }

inline Quat QuatFromAxisAngle(Vec3 axis, float angle) noexcept {
    Quat q;
    glm_quatv(Raw(q), angle, (vec3){axis.x, axis.y, axis.z});
    return q;
}

// Uses the same R = Rx * Ry * Rz order as RotateXYZ, so
// QuatToMat4(QuatFromEuler(a)) and RotateXYZ(a) agree. cglm's glm_quatv takes
// (angle, axis) rather than Euler angles, hence the per-axis composition.
inline Quat QuatFromEuler(Vec3 euler) noexcept {
    const Quat qx = QuatFromAxisAngle(Vec3{1, 0, 0}, euler.x);
    const Quat qy = QuatFromAxisAngle(Vec3{0, 1, 0}, euler.y);
    const Quat qz = QuatFromAxisAngle(Vec3{0, 0, 1}, euler.z);
    return QuatMul(qx, QuatMul(qy, qz));
}

// glm_quat_mat4 is the correct variant for cglm's column-major mat4, and is the
// exact inverse of glm_mat4_quat used by QuatFromMat4. The _t (transposed)
// variant is NOT equivalent here and would break the round trip.
inline Mat4 QuatToMat4(Quat q) noexcept {
    Mat4 m;
    glm_quat_mat4(Raw(q), Raw(m));
    return m;
}

inline Quat QuatFromMat4(Mat4 m) noexcept {
    Quat q;
    glm_mat4_quat(Raw(m), Raw(q));
    return q;
}

inline Quat QuatMul(Quat a, Quat b) noexcept {
    Quat q;
    glm_quat_mul(Raw(a), Raw(b), Raw(q));
    return q;
}

inline Quat QuatSlerp(Quat a, Quat b, float t) noexcept {
    Quat q;
    glm_quat_slerp(Raw(a), Raw(b), t, Raw(q));
    return q;
}

inline Quat QuatNormalize(Quat q) noexcept {
    Quat r;
    glm_quat_normalize_to(Raw(q), Raw(r));
    return r;
}

inline Quat QuatConjugate(Quat q) noexcept { return {-q.x, -q.y, -q.z, q.w}; }

inline float QuatLength(Quat q) noexcept {
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

inline Quat QuatInverse(Quat q) noexcept {
    Quat r;
    glm_quat_inv(Raw(q), Raw(r));
    return r;
}

inline Vec3 QuatRotateVec(Quat q, Vec3 v) noexcept {
    vec3 out;
    glm_quat_rotatev(Raw(q), (vec3){v.x, v.y, v.z}, out);
    return {out[0], out[1], out[2]};
}

// ============================================================================
// Ray / plane
// ============================================================================

inline bool RayPlaneIntersect(Vec3 rayOrigin, Vec3 rayDir, Vec3 planePoint, Vec3 planeNormal,
                              float* outDist) noexcept {
    const float denom = Dot(rayDir, planeNormal);
    if (std::abs(denom) < 1e-6f) return false;
    const float t = Dot(Sub(planePoint, rayOrigin), planeNormal) / denom;
    if (outDist) *outDist = t;
    return t >= 0;
}

inline Vec3 ProjectPointToPlane(Vec3 point, Vec3 planePoint, Vec3 planeNormal) noexcept {
    return Sub(point, Mul(planeNormal, Dot(Sub(point, planePoint), planeNormal)));
}

// ============================================================================
// Color
// ============================================================================

inline Color ColorFromFloat(float r, float g, float b, float a = 1.0f) noexcept {
    return {
        static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f),
        static_cast<uint8_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f)
    };
}

inline Vec4 ColorToVec4(Color c) noexcept {
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}

inline Color ColorFromVec4(Vec4 v) noexcept { return ColorFromFloat(v.x, v.y, v.z, v.w); }

constexpr Color COLOR_WHITE     { 255, 255, 255, 255 };
constexpr Color COLOR_BLACK     { 0, 0, 0, 255 };
constexpr Color COLOR_RED       { 255, 0, 0, 255 };
constexpr Color COLOR_GREEN     { 0, 255, 0, 255 };
constexpr Color COLOR_BLUE      { 0, 0, 255, 255 };
constexpr Color COLOR_YELLOW    { 255, 255, 0, 255 };
constexpr Color COLOR_CYAN      { 0, 255, 255, 255 };
constexpr Color COLOR_MAGENTA   { 255, 0, 255, 255 };
constexpr Color COLOR_GRAY      { 128, 128, 128, 255 };
constexpr Color COLOR_LIGHTGRAY { 200, 200, 200, 255 };
constexpr Color COLOR_DARKGRAY  { 80, 80, 80, 255 };
constexpr Color COLOR_RAYWHITE  { 245, 245, 245, 255 };

} // namespace math
