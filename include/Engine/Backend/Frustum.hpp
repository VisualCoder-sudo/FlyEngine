#pragma once
#include <cstring>
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"

// Frustum culling utility extracted from the City implementation.
// Works with the current rlgl modelview/projection matrices so the same
// code culls the main view, the mirrored reflection view, and the light's
// orthographic box (shadow pass).

struct Frustum {
    float planes[6][4] = {};  // 6 planes: left, right, bottom, top, near, far
    bool valid = false;

    // Extract frustum planes from the current view-projection matrix.
    static Frustum ExtractFromViewProj(const Matrix& view, const Matrix& proj) {
        Frustum f;
        const Matrix m = MatrixMultiply(view, proj);
        
        // Rows of the combined matrix
        const float r0[4] = { m.m0, m.m4, m.m8,  m.m12 };
        const float r1[4] = { m.m1, m.m5, m.m9,  m.m13 };
        const float r2[4] = { m.m2, m.m6, m.m10, m.m14 };
        const float r3[4] = { m.m3, m.m7, m.m11, m.m15 };
        
        // Degenerate projection check
        if (r3[0] == 0.0f && r3[1] == 0.0f && r3[2] == 0.0f && r3[3] == 0.0f) {
            return f; // invalid
        }
        
        const float* rows[3] = { r0, r1, r2 };
        for (int a = 0; a < 3; a++) {
            for (int k = 0; k < 4; k++) {
                // +w >= -axis  (left, bottom, near)
                f.planes[a * 2 + 0][k] = r3[k] + rows[a][k];
                // +w <= +axis  (right, top, far)
                f.planes[a * 2 + 1][k] = r3[k] - rows[a][k];
            }
        }
        f.valid = true;
        return f;
    }

    // Extract frustum from the currently bound rlgl matrices.
    // Every object asks for this once per draw, but the matrices only change
    // between passes (main view, each shadow cascade), so the planes are
    // recomputed only when view or projection actually differ from last call.
    static Frustum ExtractCurrent() {
        thread_local Matrix lastView{}, lastProj{};
        thread_local Frustum cached;
        thread_local bool haveCache = false;

        const Matrix view = rlGetMatrixModelview();
        const Matrix proj = rlGetMatrixProjection();
        if (haveCache && std::memcmp(&view, &lastView, sizeof(Matrix)) == 0 &&
                         std::memcmp(&proj, &lastProj, sizeof(Matrix)) == 0) {
            return cached;
        }
        lastView = view;
        lastProj = proj;
        cached = ExtractFromViewProj(view, proj);
        haveCache = true;
        return cached;
    }

    // Test if an axis-aligned bounding box intersects the frustum.
    // Returns true if the box is fully or partially inside the frustum.
    bool Intersects(const Vector3& min, const Vector3& max) const {
        if (!valid) return true; // No frustum = everything visible
        
        for (int i = 0; i < 6; i++) {
            const float* p = planes[i];
            // Select the corner of the AABB that is most "outside" this plane.
            const float x = p[0] >= 0.0f ? max.x : min.x;
            const float y = p[1] >= 0.0f ? max.y : min.y;
            const float z = p[2] >= 0.0f ? max.z : min.z;
            
            // If the "most outside" corner is outside the plane, the box is culled.
            if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) {
                return false;
            }
        }
        return true;
    }

    // Convenience: test a BoundingBox directly.
    bool Intersects(const BoundingBox& box) const {
        return Intersects(box.min, box.max);
    }
};