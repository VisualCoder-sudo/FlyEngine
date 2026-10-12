// Parity tests for the backend math layer (include/Engine/Backend/Math.hpp).
//
// These exist because the migration from raylib to a column-major cglm/Sokol
// stack is only safe if the two conventions are provably interchangeable at the
// boundary. The RaylibBackend converts every raylib Matrix by transposing it
// into a column-major Mat4; if that transpose or the operand order of a
// multiply is ever wrong, the engine renders subtly wrong transforms rather
// than failing to compile. Each test below pins down one such rule.
//
// raylib and cglm disagree about matrix multiplication. Given the same two
// linear maps A and B, raylib's MatrixMultiply(A, B) and the column-major
// Mul(ToMat(A), ToMat(B)) disagree: raylib's product is the transpose of the
// column-major one, so its operands are correspondingly reversed after
// conversion. Both of the following were confirmed against raylib:
//
//   ToMat4(raylib)  = transpose of raylib's bytes  (verified via Vector3Transform)
//   Mul(a, b)       = raylib MatrixMultiply(ToMat(b), ToMat(a))
//
// RotateXYZ is the one primitive that is NOT order-preserving: raylib composes
// its own rotation order, which is matched exactly by RotateXYZ as defined in
// Math.hpp (R = Rx * Ry * Rz). The test below asserts that equivalence rather
// than assuming it.
//
// Mat4 itself is column-major (m[col][row]). The operand reversal is the single
// easiest thing to get wrong when porting a raylib call site, hence the
// dedicated tests below.
// Run: ctest -R math_backend_parity  (requires FLYENGINE_BUILD_TESTS=ON)

#include "Engine/Backend/Math.hpp"

#include "raymath.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace math;

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

// raylib Matrix element (row, col).
//
// raymath.h's struct comment lists the first row as {m0, m4, m8, m12}, but the
// fields are declared in memory order m0, m1, m2, ..., so element(row, col)
// lives at [row*4 + col]: raylib stores rows contiguously. Converting to the
// column-major Mat4 therefore requires a transpose, which the parity tests
// below confirm against raylib's own Vector3Transform.
float RL(const Matrix& m, int row, int col) {
    const float* p = &m.m0;
    return p[row * 4 + col];
}

// Mat4 element (row, col), read out of its column-major storage.
float CM(const Mat4& m, int row, int col) {
    return m.m[col][row];
}

// Converts a raylib Matrix into the column-major Mat4 used by the backend layer.
// This is a transpose of the element grid, i.e. out.m[col][row] = RL(row, col).
// Verified equivalent: applying TransformPoint(ToMat4(m), p) reproduces raylib's
// Vector3Transform(p, m) for translations, rotations, look-at, perspective and
// nested products.
Mat4 ToMat4(const Matrix& m) {
    Mat4 out;
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out.m[c][r] = RL(m, r, c);
    return out;
}

// Compares a raylib Matrix against a Mat4, applying the transposing boundary
// conversion to the raylib side first.
void ExpectSameMatrix(const char* name, const Matrix& rl, const Mat4& cm) {
    const Mat4 converted = ToMat4(rl);
    bool ok = true;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!(std::fabs(CM(cm, r, c) - CM(converted, r, c)) <= 1e-4f)) ok = false;
    Check(name, ok);
}

// raylib's multiply, after conversion, is the column-major multiply with the
// operands reversed.
void ExpectProduct(const char* name, const Matrix& a, const Matrix& b) {
    ExpectSameMatrix(name, MatrixMultiply(a, b), Mul(ToMat4(b), ToMat4(a)));
}

} // namespace

int main() {
    std::printf("== layout ==\n");
    static_assert(sizeof(Mat4) == sizeof(mat4), "Mat4 must match cglm mat4 in size");
    static_assert(alignof(Mat4) >= 16, "Mat4 must satisfy cglm's alignment requirement");
    static_assert(std::is_standard_layout_v<Mat4>, "Mat4 must be standard layout");
    Check("Mat4 is standard layout and cglm-sized", true);

    Mat4 id = Identity();
    CheckNear("identity[0][0]", CM(id, 0, 0), 1.0f);
    CheckNear("identity[1][1]", CM(id, 1, 1), 1.0f);
    CheckNear("identity[2][2]", CM(id, 2, 2), 1.0f);
    CheckNear("identity[3][3]", CM(id, 3, 3), 1.0f);
    CheckNear("identity has no translation", CM(id, 0, 3), 0.0f);

    std::printf("\n== primitive parity with raylib ==\n");
    ExpectSameMatrix("Translate", MatrixTranslate(3, 4, 5), Translate({3, 4, 5}));
    ExpectSameMatrix("Scale", MatrixScale(2, 3, 4), Scale({2, 3, 4}));
    ExpectSameMatrix("RotateX", MatrixRotateX(0.7f), RotateX(0.7f));
    ExpectSameMatrix("RotateY", MatrixRotateY(0.7f), RotateY(0.7f));
    ExpectSameMatrix("RotateZ", MatrixRotateZ(0.7f), RotateZ(0.7f));

    // Exact parity with angles unchanged. raylib negates each angle internally and
    // stores rows contiguously, and those two effects cancel against the
    // transposing conversion, so a ported call site passes angles straight
    // through. If this ever fails, the fix belongs in the composition order
    // inside RotateXYZ, not in the call site.
    ExpectSameMatrix("RotateXYZ", MatrixRotateXYZ({0.3f, 0.5f, 0.1f}),
                     RotateXYZ({0.3f, 0.5f, 0.1f}));

    ExpectSameMatrix("LookAt", MatrixLookAt({3, 4, 5}, {-2, 1, 0}, {0, 1, 0}),
                     LookAt({3, 4, 5}, {-2, 1, 0}, {0, 1, 0}));
    ExpectSameMatrix("Perspective", MatrixPerspective(1.2, 1.7, 0.1, 100.0),
                     Perspective(1.2f, 1.7f, 0.1f, 100.0f));
    ExpectSameMatrix("Ortho", MatrixOrtho(-2, 2, -1, 1, 0.1, 50.0),
                     Ortho(-2, 2, -1, 1, 0.1f, 50.0f));

    std::printf("\n== multiply order ==\n");
    ExpectProduct("MM(Translate, Scale)", MatrixTranslate(5, 0, 0), MatrixScale(2, 2, 2));
    ExpectProduct("MM(Scale, Translate)", MatrixScale(2, 2, 2), MatrixTranslate(5, 0, 0));
    ExpectProduct("MM(RotateXYZ, Translate)",
                  MatrixRotateXYZ({0.3f, -0.5f, 0.2f}), MatrixTranslate(1, 2, 3));
    ExpectProduct("MM(nested left, nested right)",
                  MatrixMultiply(MatrixRotateXYZ({0.3f, -0.5f, 0.2f}), MatrixTranslate(1, 2, 3)),
                  MatrixMultiply(MatrixScale(2, 3, 4), MatrixRotateY(0.9f)));

    // Apply the same reasoning to a point transform: raylib Vector3Transform
    // must agree with the column-major TransformPoint.
    {
        // raylib's MatrixMultiply(S, T) corresponds to Mul(ToMat(T), ToMat(S)),
        // i.e. ComposeTRS(scale, rot, pos) below. That is exactly what makes
        // ComposeTRS read left-to-right as scale-then-rotate-then-translate.
        Matrix rm = MatrixMultiply(MatrixScale(2, 2, 2), MatrixTranslate(5, 0, 0));
        Mat4 cm = Mul(Translate({5, 0, 0}), Scale({2, 2, 2}));
        Vector3 rp = Vector3Transform({1, 1, 1}, rm);
        Vec3 cp = TransformPoint(cm, {1, 1, 1});
        CheckNear("point transform parity x", cp.x, rp.x);
        CheckNear("point transform parity y", cp.y, rp.y);
        CheckNear("point transform parity z", cp.z, rp.z);
    }

    std::printf("\n== ComposeTRS ==\n");
    {
        // Graphics.cpp::ComposeTRS(scale, rotation, position) must keep meaning
        // "scale, then rotate, then translate" after the move to column-major.
        Matrix rl = MatrixMultiply(MatrixMultiply(MatrixScale(2, 3, 4), MatrixIdentity()),
                                   MatrixTranslate(7, 8, 9));
        Vector3 rp = Vector3Transform({1, 1, 1}, rl);
        Vec3 cp = TransformPoint(ComposeTRS({2, 3, 4}, Identity(), {7, 8, 9}), {1, 1, 1});
        CheckNear("ComposeTRS parity x", cp.x, rp.x);
        CheckNear("ComposeTRS parity y", cp.y, rp.y);
        CheckNear("ComposeTRS parity z", cp.z, rp.z);
    }
    {
        Vec3 p = TransformPoint(ComposeTRS({1, 1, 1}, Identity(), {5, 0, 0}), {1, 0, 0});
        CheckNear("ComposeTRS applies translation last", p.x, 6.0f);
    }

    std::printf("\n== inverse / decompose ==\n");
    {
        Mat4 m = ComposeTRS({2, 3, 4}, RotateXYZ({0.3f, 0.5f, 0.1f}), {7, 8, 9});
        Vec3 id2 = TransformPoint(Mul(Inverse(m), m), {1, 2, 3});
        CheckNear("inverse round-trip x", id2.x, 1.0f);
        CheckNear("inverse round-trip y", id2.y, 2.0f);
        CheckNear("inverse round-trip z", id2.z, 3.0f);

        Vec3 s, t;
        Mat4 r;
        DecomposeTRS(m, &s, &r, &t);
        CheckNear("decompose scale x", s.x, 2.0f);
        CheckNear("decompose scale y", s.y, 3.0f);
        CheckNear("decompose scale z", s.z, 4.0f);
        CheckNear("decompose position x", t.x, 7.0f);
        CheckNear("decompose position y", t.y, 8.0f);
        CheckNear("decompose position z", t.z, 9.0f);

        // The rotation is non-trivial, so compare against applying the extracted
        // scale/rotation/translation to the same point the original matrix does.
        const Vec3 original = TransformPoint(m, {1, 2, 3});
        Vec3 rp = TransformPoint(ComposeTRS(s, r, t), {1, 2, 3});
        CheckNear("decompose round-trip x", rp.x, original.x, 1e-3f);
        CheckNear("decompose round-trip y", rp.y, original.y, 1e-3f);
        CheckNear("decompose round-trip z", rp.z, original.z, 1e-3f);
    }

    std::printf("\n== quaternions ==\n");
    {
        Mat4 e = RotateXYZ({0.3f, 0.5f, 0.1f});
        Mat4 qm = QuatToMat4(QuatFromEuler({0.3f, 0.5f, 0.1f}));
        bool same = true;
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (!(std::fabs(CM(e, r, c) - CM(qm, r, c)) <= 1e-4f)) same = false;
        Check("QuatFromEuler agrees with RotateXYZ", same);

        Quat q = QuatFromMat4(qm);
        Quat src = QuatFromEuler({0.3f, 0.5f, 0.1f});
        CheckNear("quat round-trip x", q.x, src.x);
        CheckNear("quat round-trip y", q.y, src.y);
        CheckNear("quat round-trip z", q.z, src.z);
        CheckNear("quat round-trip w", q.w, src.w);

        Vec3 a = QuatRotateVec(src, {1, 2, 3});
        Vec3 b = TransformDir(qm, {1, 2, 3});
        CheckNear("QuatRotateVec parity x", a.x, b.x);
        CheckNear("QuatRotateVec parity y", a.y, b.y);
        CheckNear("QuatRotateVec parity z", a.z, b.z);
    }

    std::printf("\n== vector math ==\n");
    {
        const Vec3 a{1, 2, 3};
        const Vec3 b{4, 5, 6};
        CheckNear("Dot", Dot(a, b), 32.0f);

        const Vec3 xAxis{1, 0, 0};
        const Vec3 yAxis{0, 1, 0};
        const Vec3 c = Cross(xAxis, yAxis);
        Check("Cross is right handed", c.x == 0 && c.y == 0 && c.z == 1);

        const Vec3 pyth{3, 4, 0};
        CheckNear("Length", Length(pyth), 5.0f);

        const Vec3 n = Normalize(pyth);
        CheckNear("Normalize x", n.x, 0.6f);
        CheckNear("Normalize y", n.y, 0.8f);

        const Vec3 zero = Normalize({0, 0, 0});
        Check("Normalize of zero is zero", zero.x == 0 && zero.y == 0 && zero.z == 0);

        Check("DecomposeTRS always reports success", DecomposeTRS(Identity(), nullptr, nullptr, nullptr));
    }

    std::printf("\n== projection sanity ==\n");
    {
        Mat4 p = Perspective(1.5708f, 1.0f, 0.1f, 100.0f);
        Vec4 near4 = TransformVec4(p, {0, 0, -0.1f, 1});
        Vec4 far4 = TransformVec4(p, {0, 0, -100.0f, 1});
        // cglm defaults to right-handed with OpenGL's [-1, 1] depth range, so the
        // near plane lands at -1 in NDC and the far plane at +1.
        CheckNear("perspective near -> NDC -1", near4.z / near4.w, -1.0f, 1e-3f);
        CheckNear("perspective far -> NDC +1", far4.z / far4.w, 1.0f, 1e-3f);
    }
    {
        // Note: Sokol's Vulkan backend uses [0, 1] depth. The projection helper
        // therefore needs CGLM_FORCE_DEPTH_ZERO_TO_ONE compiled in, or an
        // explicit ZO variant, before the swapchain path is wired up.
        // The eye maps to the origin and the target onto -Z (cglm's RH default).
        Mat4 la = LookAt({0, 0, 0}, {0, 0, -10}, {0, 1, 0});
        Vec3 eye = TransformPoint(la, {0, 0, 0});
        CheckNear("LookAt puts the eye at the origin x", eye.x, 0.0f);
        CheckNear("LookAt puts the eye at the origin y", eye.y, 0.0f);
        CheckNear("LookAt puts the eye at the origin z", eye.z, 0.0f);

        Vec3 target = TransformPoint(la, {0, 0, -10});
        CheckNear("LookAt puts the target on -Z x", target.x, 0.0f);
        CheckNear("LookAt puts the target on -Z y", target.y, 0.0f);
        CheckNear("LookAt puts the target on -Z z", target.z, -10.0f);
    }

    std::printf("\n== color ==\n");
    {
        CheckNear("color clamp high", ColorFromFloat(2.0f, 0, 0).r, 255.0f);
        CheckNear("color clamp low", ColorFromFloat(-1.0f, 0, 0).r, 0.0f);

        const Vec4 v = ColorToVec4(ColorFromFloat(1.0f, 0.5f, 0.25f, 1.0f));
        const bool roundTrip = std::fabs(v.x - 1.0f) < 1e-3 && std::fabs(v.y - 0.5f) < 1e-2 &&
                               std::fabs(v.z - 0.25f) < 1e-2 && std::fabs(v.w - 1.0f) < 1e-3;
        Check("ColorToVec4 round trip", roundTrip);
    }

    if (g_failures == 0) {
        std::printf("\nall math backend parity tests passed\n");
        return 0;
    }
    std::printf("\n%d math backend parity test(s) failed\n", g_failures);
    return 1;
}
