// The stepped tower must have an upward-facing roof on its base ledge (the base is wider than the shaft).
// It used to be culled from above because its winding was judged against the whole shape's origin.
// Opens a window (needs GL for the mesh upload); not registered with ctest: build/tests/city_tower_mesh_test
#include "raylib.h"
#include "Engine.hpp"
#include "Engine/Graphics.hpp"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int main() {
    Engine engine(320, 240, "city_tower_mesh_test", 60);
    const Mesh m = gfx::GetCityShapeMesh(4);   // stepped tower
    CHECK(m.vertices && m.indices && m.triangleCount > 0);

    // Every triangle's winding must agree with its stored (outward) normal.
    int badWinding = 0, ledgeUp = 0, ledgeDown = 0;
    for (int t = 0; t < m.triangleCount; t++) {
        const unsigned short i0 = m.indices[t * 3], i1 = m.indices[t * 3 + 1], i2 = m.indices[t * 3 + 2];
        const Vector3 a = { m.vertices[i0 * 3], m.vertices[i0 * 3 + 1], m.vertices[i0 * 3 + 2] };
        const Vector3 b = { m.vertices[i1 * 3], m.vertices[i1 * 3 + 1], m.vertices[i1 * 3 + 2] };
        const Vector3 c = { m.vertices[i2 * 3], m.vertices[i2 * 3 + 1], m.vertices[i2 * 3 + 2] };
        const Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        const Vector3 sn = { m.normals[i0 * 3], m.normals[i0 * 3 + 1], m.normals[i0 * 3 + 2] };
        if (Vector3DotProduct(n, sn) <= 0.0f) badWinding++;
        // The base's roof: the flat faces at the base's top height (y = -0.05).
        const float cy = (a.y + b.y + c.y) / 3.0f;
        if (std::fabs(cy + 0.05f) < 1e-4f && std::fabs(sn.y) > 0.99f) {
            if (sn.y > 0.0f) ledgeUp++; else ledgeDown++;
        }
    }
    std::printf("tower: %d triangles, %d with winding/normal mismatch, ledge faces up=%d down=%d\n", m.triangleCount, badWinding, ledgeUp, ledgeDown);
    CHECK(badWinding == 0);
    CHECK(ledgeUp > 0);
    CHECK(ledgeDown == 0);

    std::printf(g_fail ? "city_tower_mesh_test: %d FAILED\n" : "city_tower_mesh_test: ok\n", g_fail);
    return g_fail ? 1 : 0;
}
