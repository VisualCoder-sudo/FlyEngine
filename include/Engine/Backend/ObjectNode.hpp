#pragma once
#include <string>

// A child node attached to a ScatteredObject: a light, a decal or a repeating texture.
// All node kinds share one plain struct; the fields a kind does not use are ignored.
enum class ObjectNodeType {
    SpotLight,
    PointLight,
    SurfaceLight,
    Decal,
    Texture
};

// The object's local axis a node faces / projects along.
enum class NodeFace {
    Front,   // -Z
    Back,    // +Z
    Left,    // -X
    Right,   // +X
    Top,     // +Y
    Bottom   // -Y
};

enum class LightFalloff {
    Linear,
    InverseSquare
};

struct ObjectNode {
    ObjectNodeType type = ObjectNodeType::PointLight;
    std::string name;
    bool enabled = true;
    NodeFace face = NodeFace::Front;

    // Lights.
    float color[3] = { 1.0f, 1.0f, 1.0f };
    float brightness = 1.0f;
    float range = 16.0f;
    float angle = 45.0f;                          // spot / surface cone half-angle in degrees
    float surfaceSize[2] = { 1.0f, 1.0f };        // surface light emitter, fraction of the face
    LightFalloff falloff = LightFalloff::InverseSquare;
    bool castShadows = true;                      // objects and the player shadow the light (global cap: Lighting maxShadowCasters)
    int shadowRes = 1024;                         // 128..4096

    // Decal / Texture.
    std::string texturePath;                      // project-relative
    float tint[3] = { 1.0f, 1.0f, 1.0f };
    float transparency = 0.0f;                    // 0 = opaque, 1 = invisible
    float studsPerTile[2] = { 4.0f, 4.0f };       // Texture: world size of one tile
    float offset[2] = { 0.0f, 0.0f };             // Texture: shift across the face, in tiles

    bool IsLight() const { return type == ObjectNodeType::SpotLight || type == ObjectNodeType::PointLight || type == ObjectNodeType::SurfaceLight; }
    bool IsSurfaceNode() const { return type == ObjectNodeType::Decal || type == ObjectNodeType::Texture; }
};

inline const char* ObjectNodeTypeName(ObjectNodeType t) {
    switch (t) {
        case ObjectNodeType::SpotLight: return "SpotLight";
        case ObjectNodeType::PointLight: return "PointLight";
        case ObjectNodeType::SurfaceLight: return "SurfaceLight";
        case ObjectNodeType::Decal: return "Decal";
        case ObjectNodeType::Texture: return "Texture";
    }
    return "Node";
}

inline const char* NodeFaceName(NodeFace f) {
    static const char* names[] = { "Front", "Back", "Left", "Right", "Top", "Bottom" };
    return names[(int)f];
}

// Object-local unit axes of a face: its outward normal n, and the two directions u, v that run across it.
inline void NodeFaceAxes(NodeFace f, float n[3], float u[3], float v[3]) {
    for (int i = 0; i < 3; ++i) n[i] = u[i] = v[i] = 0.0f;
    // u runs to the viewer's right and v up the picture, as seen from outside the face.
    switch (f) {
        case NodeFace::Front:  n[2] = -1.0f; u[0] = -1.0f; v[1] = 1.0f; break;
        case NodeFace::Back:   n[2] =  1.0f; u[0] =  1.0f; v[1] = 1.0f; break;
        case NodeFace::Left:   n[0] = -1.0f; u[2] =  1.0f; v[1] = 1.0f; break;
        case NodeFace::Right:  n[0] =  1.0f; u[2] = -1.0f; v[1] = 1.0f; break;
        case NodeFace::Top:    n[1] =  1.0f; u[0] =  1.0f; v[2] = -1.0f; break;
        case NodeFace::Bottom: n[1] = -1.0f; u[0] =  1.0f; v[2] =  1.0f; break;
    }
}

// New node with kind-appropriate defaults.
inline ObjectNode MakeNode(ObjectNodeType t) {
    ObjectNode n;
    n.type = t;
    n.name = ObjectNodeTypeName(t);
    if (t == ObjectNodeType::SurfaceLight) n.face = NodeFace::Top;
    if (t == ObjectNodeType::SpotLight) n.face = NodeFace::Bottom;
    if (t == ObjectNodeType::Decal) n.studsPerTile[0] = n.studsPerTile[1] = 1.0f;
    return n;
}
