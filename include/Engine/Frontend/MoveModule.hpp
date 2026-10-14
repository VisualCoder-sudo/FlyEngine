#pragma once
#include "raylib.h"
#include "raymath.h"
#include <cmath>

// Shared axis-constrained move for any movable thing (objects, water, ...).
// Grabs the axis line at the exact point under the cursor and keeps that point
// under the cursor, so there is no perspective drift.
struct MoveModule {
    Vector3 origin = { 0, 0, 0 };
    Vector3 dir = { 1, 0, 0 };
    float grabT = 0.0f;
    float lastOffset = 0.0f;

    // Closest point on the axis line to the mouse ray, as distance along dir.
    // Returns false if the ray is (nearly) parallel to the axis.
    static bool AxisParam(Ray ray, Vector3 o, Vector3 d, float& t) {
        Vector3 r = Vector3Normalize(ray.direction);
        Vector3 w = Vector3Subtract(o, ray.position);
        float b = Vector3DotProduct(d, r);
        float denom = 1.0f - b * b;
        if (denom < 1e-4f) return false;
        t = (b * Vector3DotProduct(r, w) - Vector3DotProduct(d, w)) / denom;
        return true;
    }

    void Begin(Ray ray, Vector3 axisOrigin, Vector3 axisDir) {
        origin = axisOrigin;
        dir = Vector3Normalize(axisDir);
        lastOffset = 0.0f;
        if (!AxisParam(ray, origin, dir, grabT)) grabT = 0.0f;
    }

    // World-space distance to move along the axis from the origin at Begin().
    float Offset(Ray ray) {
        float t;
        if (AxisParam(ray, origin, dir, t)) lastOffset = t - grabT;
        return lastOffset;
    }

    Vector3 Apply(Ray ray) { return Vector3Add(origin, Vector3Scale(dir, Offset(ray))); }
};
