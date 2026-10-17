#include "../../include/Engine/CityMeshes.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

Mesh cityWedgeMesh{}; // unit corner wedge for angled parcels
Mesh citySlantMesh{}; // unit sheared slab for silhouette variety
Mesh cityGableMesh{}; // unit box with a pitched roof
Mesh cityTowerMesh{}; // unit stepped tower
Mesh cityShedMesh{};
Mesh cityBusMesh{}, cityBusGlassMesh{}, cityFountainMesh{};
Mesh cityBenchMesh{}, cityHydrantMesh{}, cityBollardMesh{}, cityBusStopMesh{}, citySignMesh{};   // sidewalk furniture
Mesh cityLampMesh{}, cityTreeMesh{}, cityCarMesh{}, cityCarGlassMesh{}, cityWheelMesh{}, cityPersonMesh{}, citySignalMesh{}; // props and agents

Mesh GenerateWedgeMesh() {
    Mesh mesh = { 0 };

    const int vertexCount = 18;
    const int indexCount = 24;

    float vertices[] = {
        -0.5f, -0.5f, -0.5f,
        -0.5f,  0.5f, -0.5f,
         0.5f, -0.5f, -0.5f,
        -0.5f, -0.5f,  0.5f,
         0.5f, -0.5f, 0.5f,
        -0.5f,  0.5f,  0.5f,
        -0.5f, -0.5f, -0.5f,
         0.5f, -0.5f, -0.5f,
         0.5f, -0.5f,  0.5f,
        -0.5f, -0.5f,  0.5f,
        -0.5f, -0.5f, -0.5f,
        -0.5f, -0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,
        -0.5f,  0.5f, -0.5f,
        0.5f, -0.5f, -0.5f,
        -0.5f,  0.5f, -0.5f,
        -0.5f,  0.5f, 0.5f,
        0.5f, -0.5f,  0.5f,
    };

    float texcoords[] = {
        0.0f, 0.0f,
        0.0f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
    };

    float normals[] = {
        0.0f,  0.0f, -1.0f,
         0.0f,  0.0f, -1.0f,
         0.0f,  0.0f, -1.0f,
        0.0f,  0.0f,  1.0f,
         0.0f,  0.0f,  1.0f,
         0.0f,  0.0f,  1.0f,
        0.0f, -1.0f,  0.0f,
         0.0f, -1.0f,  0.0f,
         0.0f, -1.0f,  0.0f,
         0.0f, -1.0f,  0.0f,
        -1.0f, 0.0f, 0.0f,
        -1.0f,  0.0f,  0.0f,
        -1.0f, 0.0f,  0.0f,
        -1.0f,  0.0f,  0.0f,
        0.7071f,  0.7071f, 0.0f,
         0.7071f,  0.7071f, 0.0f,
         0.7071f,  0.7071f, 0.0f,
         0.7071f,  0.7071f, 0.0f,
    };

    unsigned short indices[] = {
         0,  1,  2,
         3,  4,  5,
         6,  7,  8,  6,  8,  9,
        10, 11, 12, 10, 12, 13,
        14, 15, 16, 14, 16, 17,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;

    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));

    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));

    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));

    mesh.indices = (unsigned short*)RL_MALLOC(indexCount * sizeof(unsigned short));
    memcpy(mesh.indices, indices, indexCount * sizeof(unsigned short));

    UploadMesh(&mesh, false);

    return mesh;
}

// Corner wedge: a right-isoceles triangular prism (unit cube footprint, apex at
// (0.5, 0.5)). Fills the triangular parcel where two angled streets meet.
Mesh GenerateCityWedgeMesh() {
    Mesh mesh = { 0 };

    const float invSqrt2 = 0.70710678f;
    const int vertexCount = 18;
    const int indexCount = 24;

    float vertices[vertexCount * 3] = {
        // bottom face (A,C,B) y=-0.5
         0.5f, -0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,     0.5f, -0.5f, -0.5f,
        // top face (a,b,c) y=+0.5
         0.5f,  0.5f,  0.5f,     0.5f,  0.5f, -0.5f,    -0.5f,  0.5f,  0.5f,
        // +x leg (A,B,b,a)
         0.5f, -0.5f,  0.5f,     0.5f, -0.5f, -0.5f,
         0.5f,  0.5f, -0.5f,     0.5f,  0.5f,  0.5f,
        // +z leg (A,a,c,C)
         0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,
        // hypotenuse (B,C,c,b)
         0.5f, -0.5f, -0.5f,    -0.5f, -0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,     0.5f,  0.5f, -0.5f,
    };
    float normals[vertexCount * 3] = {
        0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,
        0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,
        -invSqrt2, 0.0f, -invSqrt2,   -invSqrt2, 0.0f, -invSqrt2,
        -invSqrt2, 0.0f, -invSqrt2,   -invSqrt2, 0.0f, -invSqrt2,
    };
    float texcoords[vertexCount * 2] = {
        0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
    };
    unsigned short indices[indexCount] = {
        0, 1, 2,
        3, 5, 4,
        6, 8, 7,  6, 9, 8,
        10, 12, 11,  10, 13, 12,
        14, 16, 15,  14, 17, 16,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    for (int i = 0; i < vertexCount * 4; i++) mesh.colors[i] = 255;
    mesh.indices = (unsigned short*)RL_MALLOC(indexCount * sizeof(unsigned short));
    memcpy(mesh.indices, indices, indexCount * sizeof(unsigned short));

    UploadMesh(&mesh, false);

    return mesh;
}

// Sheared slab: unit footprint with its top pushed +z by 0.35 (a parallelogram
// cross-section), giving buildings a slanted rather than boxy silhouette.
Mesh GenerateCitySlantMesh() {
    Mesh mesh = { 0 };

    const float sh = 0.35f;
    const int vertexCount = 24;
    const int indexCount = 36;

    float vertices[vertexCount * 3] = {
        // bottom (B0,B1,B2,B3)
        -0.5f, -0.5f, -0.5f,     0.5f, -0.5f, -0.5f,     0.5f, -0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,
        // top (T3,T2,T1,T0)
        -0.5f,  0.5f,  0.5f + sh,  0.5f,  0.5f,  0.5f + sh,  0.5f,  0.5f, -0.5f + sh,   -0.5f,  0.5f, -0.5f + sh,
        // +x (B1,B2,T2,T1)
         0.5f, -0.5f, -0.5f,     0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f + sh,    0.5f,  0.5f, -0.5f + sh,
        // -x (B3,B0,T0,T3)
        -0.5f, -0.5f,  0.5f,    -0.5f, -0.5f, -0.5f,    -0.5f,  0.5f, -0.5f + sh,   -0.5f,  0.5f,  0.5f + sh,
        // +z ramp (B2,T2,T3,B3)
         0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f + sh,  -0.5f,  0.5f,  0.5f + sh,   -0.5f, -0.5f,  0.5f,
        // -z ramp (B0,T0,T1,B1)
        -0.5f, -0.5f, -0.5f,    -0.5f,  0.5f, -0.5f + sh,   0.5f,  0.5f, -0.5f + sh,    0.5f, -0.5f, -0.5f,
    };
    float normals[vertexCount * 3] = {
        0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,
        0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,
        0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,
        0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,
    };
    float texcoords[vertexCount * 2] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    unsigned short indices[indexCount] = {
        0, 2, 1,  0, 3, 2,
        4, 6, 5,  4, 7, 6,
        8, 10, 9,  8, 11, 10,
        12, 14, 13,  12, 15, 14,
        16, 18, 17,  16, 19, 18,
        20, 22, 21,  20, 23, 22,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    for (int i = 0; i < vertexCount * 4; i++) mesh.colors[i] = 255;
    mesh.indices = (unsigned short*)RL_MALLOC(indexCount * sizeof(unsigned short));
    memcpy(mesh.indices, indices, indexCount * sizeof(unsigned short));

    UploadMesh(&mesh, false);

    return mesh;
}


// Flat-shaded unit mesh from a triangle soup (each triangle gets its own vertices and face normal;
// wound so the face normal points away from the shape's centre).
// triTex (optional, one per triangle) goes into the texcoords: the stepped tower marks each tier with
// (-1 - bottom, top), both as a fraction of the shape's height, so the facade shader can lay out whole floors per tier.
Mesh BuildFlatShapeMesh(const std::vector<Vector3>& tris, const std::vector<Color>* triColors = nullptr,
                        const std::vector<Vector3>* triInside = nullptr, const std::vector<Vector2>* triTex = nullptr) {
    Mesh mesh = { 0 };
    const int vc = (int)tris.size();
    mesh.vertexCount = vc;
    mesh.triangleCount = vc / 3;
    mesh.vertices = (float*)RL_MALLOC(vc * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vc * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vc * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vc * 4);
    mesh.indices = (unsigned short*)RL_MALLOC(vc * sizeof(unsigned short));
    for (int t = 0; t + 2 < vc; t += 3) {
        Vector3 a = tris[(size_t)t], b = tris[(size_t)t + 1], c = tris[(size_t)t + 2];
        Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        Vector3 cen = { (a.x + b.x + c.x) / 3.0f, (a.y + b.y + c.y) / 3.0f, (a.z + b.z + c.z) / 3.0f };
        if (triInside && (size_t)(t / 3) < triInside->size()) cen = Vector3Subtract(cen, (*triInside)[(size_t)(t / 3)]);   // relative to the primitive's centre
        if (Vector3DotProduct(n, cen) < 0.0f) { std::swap(b, c); n = Vector3Negate(n); }
        n = Vector3Normalize(n);
        const Vector3 v[3] = { a, b, c };
        for (int k = 0; k < 3; k++) {
            const int i = t + k;
            mesh.vertices[i * 3] = v[k].x; mesh.vertices[i * 3 + 1] = v[k].y; mesh.vertices[i * 3 + 2] = v[k].z;
            mesh.normals[i * 3] = n.x; mesh.normals[i * 3 + 1] = n.y; mesh.normals[i * 3 + 2] = n.z;
            const Vector2 tex = (triTex && (size_t)(t / 3) < triTex->size()) ? (*triTex)[(size_t)(t / 3)] : Vector2{ 0.0f, 0.0f };
            mesh.texcoords[i * 2] = tex.x; mesh.texcoords[i * 2 + 1] = tex.y;
            Color col = WHITE;
            if (triColors && (size_t)(t / 3) < triColors->size()) col = (*triColors)[(size_t)(t / 3)];
            mesh.colors[i * 4] = col.r; mesh.colors[i * 4 + 1] = col.g; mesh.colors[i * 4 + 2] = col.b; mesh.colors[i * 4 + 3] = col.a;
            mesh.indices[i] = (unsigned short)i;
        }
    }
    UploadMesh(&mesh, false);
    return mesh;
}

void AddBoxTris(std::vector<Vector3>& t, float x0, float y0, float z0, float x1, float y1, float z1, bool bottom) {
    const Vector3 p[8] = { {x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},{x0,y1,z0},{x1,y1,z0},{x1,y1,z1},{x0,y1,z1} };
    auto quad = [&](int a, int b, int c, int d) { t.push_back(p[a]); t.push_back(p[b]); t.push_back(p[c]); t.push_back(p[a]); t.push_back(p[c]); t.push_back(p[d]); };
    quad(4,5,6,7);                       // top
    quad(0,1,5,4); quad(1,2,6,5); quad(2,3,7,6); quad(3,0,4,7);   // sides
    if (bottom) quad(0,3,2,1);
}

// Box with a gabled roof: walls up to y=+0.5-ridge, ridge along local x at +0.5.
Mesh GenerateCityGableMesh() {
    std::vector<Vector3> t;
    const float eave = 0.12f;   // eave height as a fraction of the unit (the rest is roof)
    const float ey = 0.5f - eave;
    AddBoxTris(t, -0.5f, -0.5f, -0.5f, 0.5f, ey, 0.5f, true);
    // Roof prism: two slopes + two gable ends.
    const Vector3 a0 = { -0.5f, ey, -0.5f }, a1 = { 0.5f, ey, -0.5f }, b0 = { -0.5f, ey, 0.5f }, b1 = { 0.5f, ey, 0.5f };
    const Vector3 r0 = { -0.5f, 0.5f, 0.0f }, r1 = { 0.5f, 0.5f, 0.0f };
    t.insert(t.end(), { a0, a1, r1, a0, r1, r0 });   // -z slope
    t.insert(t.end(), { b1, b0, r0, b1, r0, r1 });   // +z slope
    t.insert(t.end(), { a0, r0, b0 });               // -x gable
    t.insert(t.end(), { a1, b1, r1 });               // +x gable
    return BuildFlatShapeMesh(t);
}

// Box with a single-slope roof: the roof drops from the full height at -z to 75% at +z.
Mesh GenerateCityShedMesh() {
    std::vector<Vector3> t;
    const float lo = 0.5f - 0.25f;
    const Vector3 p0 = { -0.5f, -0.5f, -0.5f }, p1 = { 0.5f, -0.5f, -0.5f }, p2 = { 0.5f, -0.5f, 0.5f }, p3 = { -0.5f, -0.5f, 0.5f };
    const Vector3 q0 = { -0.5f, 0.5f, -0.5f }, q1 = { 0.5f, 0.5f, -0.5f }, q2 = { 0.5f, lo, 0.5f }, q3 = { -0.5f, lo, 0.5f };
    t.insert(t.end(), { p0, p2, p1, p0, p3, p2 });          // bottom
    t.insert(t.end(), { q0, q3, q2, q0, q2, q1 });          // sloped roof
    t.insert(t.end(), { p0, p1, q1, p0, q1, q0 });          // -z wall
    t.insert(t.end(), { p3, q2, p2, p3, q3, q2 });          // +z wall
    t.insert(t.end(), { p0, q0, q3, p0, q3, p3 });          // -x wall
    t.insert(t.end(), { p1, p2, q2, p1, q2, q1 });          // +x wall
    return BuildFlatShapeMesh(t);
}

// Stepped tower: base (full footprint, lower 45%), shaft (80% wide, to 85%), crown (55% wide, to the top).
Mesh GenerateCityTowerMesh() {
    // Winding is decided per box against that box's own centre: against the whole shape's origin the
    // base's top face (y = -0.05, below the origin) came out inverted and the ledge had no roof.
    std::vector<Vector3> t, in;
    std::vector<Vector2> tex;
    const auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1, bool bottom) {
        const size_t before = t.size();
        AddBoxTris(t, x0, y0, z0, x1, y1, z1, bottom);
        in.insert(in.end(), (t.size() - before) / 3, Vector3{ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f });
        // Tier bottom / top as fractions of the height (the shader lays out whole floors between them).
        tex.insert(tex.end(), (t.size() - before) / 3, Vector2{ -1.0f - (y0 + 0.5f), y1 + 0.5f });
    };
    box(-0.5f, -0.5f, -0.5f, 0.5f, -0.05f, 0.5f, true);
    box(-0.4f, -0.05f, -0.4f, 0.4f, 0.35f, 0.4f, false);
    box(-0.27f, 0.35f, -0.27f, 0.27f, 0.5f, 0.27f, false);
    return BuildFlatShapeMesh(t, nullptr, &in, &tex);
}

Mesh BuildColoredShapeMesh(const std::vector<Vector3>& tris, const std::vector<Color>& triColors, const std::vector<Vector3>& inside) {
    return BuildFlatShapeMesh(tris, &triColors, &inside);
}

struct ShapeBuilder {
    std::vector<Vector3> t;
    std::vector<Color> c;
    std::vector<Vector3> in;   // per triangle: a point inside its primitive (for outward winding)
    void Box(float x0, float y0, float z0, float x1, float y1, float z1, Color col) {
        const size_t before = t.size();
        AddBoxTris(t, x0, y0, z0, x1, y1, z1, true);
        c.insert(c.end(), (t.size() - before) / 3, col);
        in.insert(in.end(), (t.size() - before) / 3, Vector3{ (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f });
    }
    // Upright prism (a cylinder with `sides` sides) from y0 to y1; the top is capped, the bottom is not.
    void Cyl(float cx, float y0, float cz, float r, float y1, int sides, Color col) {
        const Vector3 inside = { cx, (y0 + y1) * 0.5f, cz };
        for (int i = 0; i < sides; i++) {
            const float a0 = (float)i / (float)sides * 2.0f * PI, a1 = (float)(i + 1) / (float)sides * 2.0f * PI;
            const Vector3 b0 = { cx + r * cosf(a0), y0, cz + r * sinf(a0) }, b1 = { cx + r * cosf(a1), y0, cz + r * sinf(a1) };
            const Vector3 t0 = { b0.x, y1, b0.z }, t1 = { b1.x, y1, b1.z };
            t.push_back(b0); t.push_back(b1); t.push_back(t1); c.push_back(col); in.push_back(inside);
            t.push_back(b0); t.push_back(t1); t.push_back(t0); c.push_back(col); in.push_back(inside);
            t.push_back({ cx, y1, cz }); t.push_back(t0); t.push_back(t1); c.push_back(col); in.push_back(inside);
        }
    }
    // Pyramid-ish crown (octahedron-like) for trees: 4-sided bipyramid.
    void Crown(float cx, float cy, float cz, float r, float h, Color col) {
        const Vector3 top = { cx, cy + h, cz }, bot = { cx, cy, cz };
        const Vector3 q[4] = { { cx + r, cy + h * 0.45f, cz }, { cx, cy + h * 0.45f, cz + r }, { cx - r, cy + h * 0.45f, cz }, { cx, cy + h * 0.45f, cz - r } };
        for (int i = 0; i < 4; i++) {
            const Vector3 a = q[i], b = q[(i + 1) % 4];
            t.push_back(top); t.push_back(a); t.push_back(b); c.push_back(col); in.push_back({ cx, cy + h * 0.45f, cz });
            t.push_back(bot); t.push_back(b); t.push_back(a); c.push_back(col); in.push_back({ cx, cy + h * 0.45f, cz });
        }
    }
};

// Unit prop meshes. They are authored at real-world size (metres); the city instances them with a
// uniform scale of 1 so a streetlight is ~7 m tall. Origin: centre of the bottom face at y = -0.5
// would waste the unit cube convention, so props use y = 0 at the ground and the instance matrix
// translates them to the surface directly.
Mesh GenerateCityLampMesh() {
    ShapeBuilder b;
    b.Box(-0.07f, 0.0f, -0.07f, 0.07f, 5.6f, 0.07f, Color{ 70, 72, 78, 255 });       // pole
    b.Box(-0.07f, 5.5f, -0.07f, 0.07f, 5.64f, 1.1f, Color{ 70, 72, 78, 255 });       // arm toward +z
    b.Box(-0.22f, 5.38f, 0.8f, 0.22f, 5.52f, 1.4f, Color{ 250, 232, 170, 248 });      // lamp head (alpha 248 = a street lamp: switches on at dusk, see LampOn in lit.glsl)
    b.Box(-0.18f, 0.0f, -0.18f, 0.18f, 0.35f, 0.18f, Color{ 60, 62, 68, 255 });       // base
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Sidewalk furniture. Authored in metres, origin on the ground, +z toward the road.
// Bus: 11 m long along local +x (forward), 2.5 m wide. The body is white so the instance tint colours it (the
// line colour); windows and lights are a second, untinted mesh.
Mesh GenerateCityBusMesh() {
    ShapeBuilder b;
    b.Box(-5.5f, 0.4f, -1.25f, 5.5f, 3.0f, 1.25f, WHITE);                                // body
    b.Box(-5.5f, 3.0f, -1.2f, 5.5f, 3.08f, 1.2f, Color{ 215, 215, 220, 255 });          // roof cap (tinted a little)
    b.Box(-5.4f, 0.28f, -1.15f, 5.4f, 0.4f, 1.15f, Color{ 60, 62, 68, 255 });            // skirt
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBusGlassMesh() {
    ShapeBuilder b;
    const Color glass{ 34, 48, 62, 255 };
    for (float z : { -1.252f, 1.252f }) b.Box(-5.0f, 1.45f, z - 0.005f, 4.6f, 2.65f, z + 0.005f, glass);   // side windows
    b.Box(5.5f, 1.3f, -1.1f, 5.512f, 2.7f, 1.1f, glass);                                  // windscreen
    b.Box(-5.512f, 1.5f, -1.0f, -5.5f, 2.6f, 1.0f, glass);                                // rear window
    b.Box(5.49f, 1.0f, -1.1f, 5.516f, 1.15f, 1.1f, Color{ 255, 190, 80, 250 });          // destination sign (glows at night)
    for (float z : { -0.9f, 0.9f }) {
        b.Box(5.49f, 0.6f, z - 0.2f, 5.52f, 0.85f, z + 0.2f, Color{ 255, 244, 205, 250 });   // headlights
        b.Box(-5.52f, 0.6f, z - 0.2f, -5.49f, 0.85f, z + 0.2f, Color{ 230, 30, 25, 250 });   // tail lights
    }
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBenchMesh() {
    ShapeBuilder b;
    const Color wood{ 128, 92, 58, 255 }, iron{ 52, 54, 60, 255 };
    b.Box(-0.8f, 0.42f, -0.22f, 0.8f, 0.48f, 0.22f, wood);          // seat
    b.Box(-0.8f, 0.48f, -0.24f, 0.8f, 0.95f, -0.18f, wood);         // backrest (away from the road)
    for (float x : { -0.7f, 0.7f }) b.Box(x - 0.04f, 0.0f, -0.2f, x + 0.04f, 0.42f, 0.2f, iron);   // legs
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Round stone fountain: a wide basin of water with a rim, a pedestal, a bowl and a jet. Authored in metres, origin at the centre of its base.
Mesh GenerateCityFountainMesh() {
    ShapeBuilder b;
    const Color stone{ 172, 170, 162, 255 }, water{ 74, 142, 192, 255 }, jet{ 196, 228, 246, 255 };
    b.Cyl(0.0f, 0.0f, 0.0f, 2.7f, 0.55f, 14, stone);          // basin wall (its rim is the top edge)
    b.Cyl(0.0f, 0.0f, 0.0f, 2.35f, 0.58f, 14, water);         // the water inside it
    b.Cyl(0.0f, 0.5f, 0.0f, 0.45f, 1.7f, 8, stone);           // pedestal
    b.Cyl(0.0f, 1.3f, 0.0f, 1.25f, 1.55f, 12, stone);         // upper bowl
    b.Cyl(0.0f, 1.3f, 0.0f, 1.02f, 1.58f, 12, water);
    b.Cyl(0.0f, 1.5f, 0.0f, 0.12f, 2.7f, 6, jet);             // the jet
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityHydrantMesh() {
    ShapeBuilder b;
    const Color red{ 196, 38, 32, 255 }, cap{ 160, 28, 24, 255 };
    b.Box(-0.11f, 0.0f, -0.11f, 0.11f, 0.62f, 0.11f, red);          // body
    b.Box(-0.14f, 0.62f, -0.14f, 0.14f, 0.74f, 0.14f, cap);         // cap
    b.Box(-0.2f, 0.34f, -0.06f, 0.2f, 0.46f, 0.06f, red);           // side nozzles
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBollardMesh() {
    ShapeBuilder b;
    b.Box(-0.07f, 0.0f, -0.07f, 0.07f, 0.85f, 0.07f, Color{ 96, 98, 106, 255 });
    b.Box(-0.075f, 0.62f, -0.075f, 0.075f, 0.74f, 0.075f, Color{ 236, 200, 40, 255 });   // reflective band
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityBusStopMesh() {
    ShapeBuilder b;
    const Color frame{ 58, 62, 72, 255 }, roof{ 40, 90, 150, 255 }, glass{ 120, 150, 170, 255 }, seat{ 120, 88, 56, 255 };
    for (float x : { -1.45f, 1.45f }) b.Box(x - 0.04f, 0.0f, -0.46f, x + 0.04f, 2.4f, -0.38f, frame);   // posts
    b.Box(-1.6f, 2.4f, -0.55f, 1.6f, 2.52f, 0.55f, roof);                                              // roof
    b.Box(-1.45f, 0.35f, -0.46f, 0.7f, 2.3f, -0.43f, glass);                                           // back glass
    b.Box(0.78f, 0.35f, -0.46f, 1.45f, 2.3f, -0.43f, Color{ 255, 238, 190, 250 });                     // advert panel (emissive at night)
    b.Box(-1.2f, 0.42f, -0.4f, 0.6f, 0.48f, -0.05f, seat);                                             // bench
    b.Box(1.9f, 0.0f, 0.2f, 1.96f, 2.5f, 0.26f, frame);                                                // stop sign pole
    b.Box(1.74f, 2.1f, 0.17f, 2.12f, 2.5f, 0.3f, Color{ 30, 110, 190, 255 });                          // stop sign
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCitySignMesh() {
    ShapeBuilder b;
    b.Box(-0.03f, 0.0f, -0.03f, 0.03f, 2.7f, 0.03f, Color{ 90, 92, 100, 255 });                        // pole
    b.Box(-0.5f, 2.4f, -0.05f, 0.5f, 2.66f, 0.05f, Color{ 24, 88, 52, 255 });                          // name plate
    b.Box(-0.46f, 2.43f, 0.05f, 0.46f, 2.63f, 0.052f, Color{ 238, 238, 232, 255 });                    // plate text strip
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityTreeMesh() {
    ShapeBuilder b;
    b.Box(-0.14f, 0.0f, -0.14f, 0.14f, 2.0f, 0.14f, Color{ 96, 70, 46, 255 });
    b.Crown(0.0f, 1.6f, 0.0f, 1.5f, 3.4f, Color{ 62, 128, 66, 255 });
    b.Crown(0.0f, 2.8f, 0.0f, 1.1f, 2.6f, Color{ 78, 150, 78, 255 });
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Car: 4.2 m long along local +x, 1.8 m wide. Body and roof are white so the instance tint colours them;
// the windows and wheels are separate meshes (untinted) so black cars still have glass and tyres.
// Cabin rings (x range, half width, height) give a slanted, rounded windscreen and rear window.
struct CabinRing { float xr, xf, hw, y; };
constexpr CabinRing kCabin[3] = { { -1.30f, 1.20f, 0.80f, 0.95f }, { -1.04f, 0.74f, 0.75f, 1.21f }, { -0.60f, 0.22f, 0.64f, 1.44f } };

Mesh GenerateCityCarMesh() {
    ShapeBuilder b;
    b.Box(-2.1f, 0.35f, -0.9f, 2.1f, 0.95f, 0.9f, WHITE);                              // body
    // Roof cap (white, tinted).
    const CabinRing& r = kCabin[2];
    const Vector3 in{ -0.05f, 1.2f, 0.0f };
    const Vector3 q[4] = { { r.xr, r.y, -r.hw }, { r.xf, r.y, -r.hw }, { r.xf, r.y, r.hw }, { r.xr, r.y, r.hw } };
    for (int k : { 0, 1, 2, 0, 2, 3 }) { b.t.push_back(q[k]); b.c.push_back(WHITE); b.in.push_back(in); }
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityCarGlassMesh() {
    ShapeBuilder b;
    const Color glass{ 38, 52, 66, 255 };
    const Vector3 in{ -0.05f, 1.2f, 0.0f };
    auto quad = [&](Vector3 p0, Vector3 p1, Vector3 p2, Vector3 p3) {
        for (const Vector3& v : { p0, p1, p2, p0, p2, p3 }) { b.t.push_back(v); b.c.push_back(glass); b.in.push_back(in); }
    };
    for (int i = 0; i < 2; i++) {
        const CabinRing &lo = kCabin[i], &hi = kCabin[i + 1];
        quad({ lo.xf, lo.y, -lo.hw }, { lo.xf, lo.y, lo.hw }, { hi.xf, hi.y, hi.hw }, { hi.xf, hi.y, -hi.hw });   // windscreen
        quad({ lo.xr, lo.y, -lo.hw }, { lo.xr, lo.y, lo.hw }, { hi.xr, hi.y, hi.hw }, { hi.xr, hi.y, -hi.hw });   // rear window
        quad({ lo.xr, lo.y, lo.hw }, { lo.xf, lo.y, lo.hw }, { hi.xf, hi.y, hi.hw }, { hi.xr, hi.y, hi.hw });     // +z side
        quad({ lo.xr, lo.y, -lo.hw }, { lo.xf, lo.y, -lo.hw }, { hi.xf, hi.y, -hi.hw }, { hi.xr, hi.y, -hi.hw }); // -z side
    }
    // Head and tail lights (alpha 250 = emissive at night). Untinted, so every car colour has them.
    const Color head{ 255, 244, 205, 250 }, tail{ 230, 30, 25, 250 };
    for (float z : { -0.62f, 0.62f }) {
        const size_t before = b.t.size();
        AddBoxTris(b.t, 2.08f, 0.55f, z - 0.14f, 2.14f, 0.70f, z + 0.14f, true);
        b.c.insert(b.c.end(), (b.t.size() - before) / 3, head);
        b.in.insert(b.in.end(), (b.t.size() - before) / 3, Vector3{ 2.11f, 0.62f, z });
        const size_t before2 = b.t.size();
        AddBoxTris(b.t, -2.14f, 0.55f, z - 0.14f, -2.08f, 0.70f, z + 0.14f, true);
        b.c.insert(b.c.end(), (b.t.size() - before2) / 3, tail);
        b.in.insert(b.in.end(), (b.t.size() - before2) / 3, Vector3{ -2.11f, 0.62f, z });
    }
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Wheel: round tyre (axis along z, radius 0.32 m) with a hub and a spoke so the roll is visible.
Mesh GenerateCityWheelMesh() {
    ShapeBuilder b;
    const int n = 20;
    const float R = 0.32f, W = 0.12f, H = 0.19f;
    const Color tyre{ 22, 22, 24, 255 }, hub{ 150, 154, 162, 255 };
    const Vector3 o{ 0, 0, 0 };
    auto tri = [&](Vector3 a, Vector3 bb, Vector3 c, Color col) { b.t.push_back(a); b.t.push_back(bb); b.t.push_back(c); b.c.push_back(col); b.in.push_back(o); };
    for (int i = 0; i < n; i++) {
        const float a0 = 2.0f * PI * (float)i / n, a1 = 2.0f * PI * (float)(i + 1) / n;
        const Vector3 p0{ cosf(a0) * R, sinf(a0) * R, 0 }, p1{ cosf(a1) * R, sinf(a1) * R, 0 };
        const Vector3 h0{ cosf(a0) * H, sinf(a0) * H, 0 }, h1{ cosf(a1) * H, sinf(a1) * H, 0 };
        for (float z : { -W, W }) {
            const Vector3 a{ p0.x, p0.y, z }, c{ p1.x, p1.y, z }, ha{ h0.x, h0.y, z }, hc{ h1.x, h1.y, z };
            tri(ha, hc, Vector3{ 0, 0, z }, hub);                         // hub disc
            tri(a, c, hc, tyre); tri(a, hc, ha, tyre);                      // tyre sidewall ring
        }
        const Vector3 t0{ p0.x, p0.y, -W }, t1{ p1.x, p1.y, -W }, t2{ p1.x, p1.y, W }, t3{ p0.x, p0.y, W };
        tri(t0, t1, t2, tyre); tri(t0, t2, t3, tyre);                      // tread
    }
    b.Box(-H, -0.035f, -W - 0.01f, H, 0.035f, W + 0.01f, Color{ 215, 218, 224, 255 });   // spoke across the hub
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

// Traffic-light pole with a dark three-lamp housing; the lit lamps are drawn separately.
// Local +z is the direction the lamps face (toward arriving traffic).
Mesh GenerateCitySignalMesh() {
    ShapeBuilder b;
    b.Box(-0.06f, 0.0f, -0.06f, 0.06f, 4.2f, 0.06f, Color{ 70, 72, 78, 255 });
    b.Box(-0.2f, 3.0f, -0.07f, 0.2f, 4.5f, 0.22f, Color{ 26, 26, 30, 255 });
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}

Mesh GenerateCityPersonMesh() {
    ShapeBuilder b;
    b.Box(-0.16f, 0.0f, -0.1f, 0.16f, 0.85f, 0.1f, Color{ 50, 60, 90, 255 });         // legs
    b.Box(-0.2f, 0.85f, -0.12f, 0.2f, 1.45f, 0.12f, WHITE);                            // torso (tinted)
    b.Box(-0.12f, 1.45f, -0.1f, 0.12f, 1.72f, 0.1f, Color{ 224, 188, 160, 255 });      // head
    return BuildColoredShapeMesh(b.t, b.c, b.in);
}
}

namespace gfx {

Mesh GenerateWedgeShapeMesh() { return GenerateWedgeMesh(); }

void InitCityMeshes() {
    cityWedgeMesh = GenerateCityWedgeMesh();
    citySlantMesh = GenerateCitySlantMesh();
    cityGableMesh = GenerateCityGableMesh();
    cityTowerMesh = GenerateCityTowerMesh();
    cityLampMesh = GenerateCityLampMesh();
    cityTreeMesh = GenerateCityTreeMesh();
    cityCarMesh = GenerateCityCarMesh();
    cityPersonMesh = GenerateCityPersonMesh();
    citySignalMesh = GenerateCitySignalMesh();
    cityShedMesh = GenerateCityShedMesh();
    cityCarGlassMesh = GenerateCityCarGlassMesh();
    cityWheelMesh = GenerateCityWheelMesh();
    cityBenchMesh = GenerateCityBenchMesh();
    cityHydrantMesh = GenerateCityHydrantMesh();
    cityBollardMesh = GenerateCityBollardMesh();
    cityBusStopMesh = GenerateCityBusStopMesh();
    citySignMesh = GenerateCitySignMesh();
    cityBusMesh = GenerateCityBusMesh();
    cityBusGlassMesh = GenerateCityBusGlassMesh();
    cityFountainMesh = GenerateCityFountainMesh();
}

void ShutdownCityMeshes() {
    UnloadMesh(cityWedgeMesh);
    UnloadMesh(citySlantMesh);
    UnloadMesh(cityGableMesh);
    UnloadMesh(cityTowerMesh);
    UnloadMesh(cityLampMesh);
    UnloadMesh(cityTreeMesh);
    UnloadMesh(cityCarMesh);
    UnloadMesh(cityPersonMesh);
    UnloadMesh(citySignalMesh);
    UnloadMesh(cityShedMesh);
    UnloadMesh(cityCarGlassMesh);
    UnloadMesh(cityWheelMesh);
    UnloadMesh(cityBenchMesh);
    UnloadMesh(cityHydrantMesh);
    UnloadMesh(cityBollardMesh);
    UnloadMesh(cityBusStopMesh);
    UnloadMesh(citySignMesh);
    UnloadMesh(cityBusMesh);
    UnloadMesh(cityBusGlassMesh);
    UnloadMesh(cityFountainMesh);
}

const Mesh* CityShapeMesh(int shape) {
    switch (shape) {
        case 1: return &cityWedgeMesh;
        case 2: return &citySlantMesh;
        case 3: return &cityGableMesh;
        case 4: return &cityTowerMesh;
        case 5: return &cityLampMesh;
        case 6: return &cityTreeMesh;
        case 7: return &cityCarMesh;
        case 8: return &cityPersonMesh;
        case 9: return &citySignalMesh;
        case 10: return &cityShedMesh;
        case 11: return &cityCarGlassMesh;
        case 12: return &cityWheelMesh;
        case 13: return &cityBenchMesh;
        case 14: return &cityHydrantMesh;
        case 15: return &cityBollardMesh;
        case 16: return &cityBusStopMesh;
        case 17: return &citySignMesh;
        case 18: return &cityBusMesh;
        case 19: return &cityBusGlassMesh;
        case 20: return &cityFountainMesh;
        default: return nullptr;
    }
}

} // namespace gfx
