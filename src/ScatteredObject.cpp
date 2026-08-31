#include "ScatteredObject.hpp"
#include "Graphics.hpp"
#include "TextureManager.hpp"
#include "ProjectManager.hpp"
#include "FbxModel.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <filesystem>

namespace {

// Mirrors raylib's internal MAX_MATERIAL_MAPS (defined only in rmodels.c).
constexpr int kMaxMaterialMaps = 12;

// UnloadModel() frees a model's meshes and material structs but deliberately
// leaves the textures referenced by its map slots untouched (the user may
// share them between models). Unload every non-default one before the model
// goes away. `keepId` (0 = none) lets an object-owned texture that is also
// bound to a map slot be freed by its owning member instead.
void UnloadModelTextures(const Model& model, unsigned int keepId = 0) {
    if (model.materials == nullptr) return;
    for (int m = 0; m < model.materialCount; ++m) {
        const MaterialMap* maps = model.materials[m].maps;
        if (maps == nullptr) continue;
        for (int i = 0; i < kMaxMaterialMaps; ++i) {
            const Texture2D tex = maps[i].texture;
            if (tex.id != 0 && tex.id != rlGetTextureIdDefault() && tex.id != keepId) {
                UnloadTexture(tex);
            }
        }
    }
}

// Normalizes every vertex of `model` so it spans exactly [-0.5, 0.5] on each
// axis (given its current `bounds`), re-uploading the GPU buffer, and returns
// the original extent. Shared by SetSizeFromModel() (import time, where the
// extent becomes the new `size`) and NormalizeModelToUnitBox() (scene load,
// where `size` already comes from the save file and must not be touched).
Vector3 NormalizeMeshVertices(Model& model, BoundingBox bounds) {
    Vector3 extent = Vector3Subtract(bounds.max, bounds.min);
    if (extent.x < 1e-4f) extent.x = 1.0f;
    if (extent.y < 1e-4f) extent.y = 1.0f;
    if (extent.z < 1e-4f) extent.z = 1.0f;
    const Vector3 invExtent = { 1.0f / extent.x, 1.0f / extent.y, 1.0f / extent.z };

    for (int i = 0; i < model.meshCount; ++i) {
        Mesh& mesh = model.meshes[i];
        if (mesh.vertices == nullptr) continue;
        for (int j = 0; j < mesh.vertexCount; ++j) {
            float* v = &mesh.vertices[j * 3];
            v[0] = (v[0] - bounds.min.x) * invExtent.x - 0.5f;
            v[1] = (v[1] - bounds.min.y) * invExtent.y - 0.5f;
            v[2] = (v[2] - bounds.min.z) * invExtent.z - 0.5f;
        }
        UpdateMeshBuffer(mesh, 0, mesh.vertices, (int)(mesh.vertexCount * 3 * sizeof(float)), 0);
    }
    return extent;
}

// Draws one shared shape model at an arbitrary position/size/rotation.
// wireframeOverlay draws a slightly enlarged wireframe pass (used for the
// selection outline) instead of a solid, lit, textured pass.
void DrawShapeModel(ShapeType shape, Vector3 pos, Vector3 size, Matrix rotMat,
                     Color tint, Texture2D texture, bool wireframeOverlay) {
    Model& model = gfx::GetShapeModel(shape);
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texture;

    Vector3 drawSize = wireframeOverlay ? Vector3Scale(size, 1.02f) : size;

    Matrix scaleMat = MatrixScale(drawSize.x, drawSize.y, drawSize.z);
    Matrix transMat = MatrixTranslate(pos.x, pos.y, pos.z);
    model.transform = MatrixMultiply(MatrixMultiply(scaleMat, rotMat), transMat);

    if (wireframeOverlay) {
        rlEnableWireMode();
        DrawModel(model, Vector3Zero(), 1.0f, tint);
        rlDisableWireMode();
    } else {
        DrawModel(model, Vector3Zero(), 1.0f, tint);
    }
}

// Extracts the triangle list of a raylib mesh into unit-space pcoll triangles.
// Handles both indexed (indices are unsigned short in raylib) and unindexed
// meshes. The meshes feeding this are always normalized to [-0.5, 0.5], which
// matches the collider convention.
void ExtractMeshTriangles(const Mesh& mesh, std::vector<pcoll::Triangle>& out) {
    if (mesh.vertices == nullptr || mesh.triangleCount <= 0) return;
    auto finiteTri = [](const pcoll::Triangle& t) {
        return std::isfinite(t.a.x) && std::isfinite(t.a.y) && std::isfinite(t.a.z) &&
               std::isfinite(t.b.x) && std::isfinite(t.b.y) && std::isfinite(t.b.z) &&
               std::isfinite(t.c.x) && std::isfinite(t.c.y) && std::isfinite(t.c.z);
    };
    if (mesh.indices != nullptr) {
        for (int i = 0; i < mesh.triangleCount; ++i) {
            const unsigned short* idx = &mesh.indices[i * 3];
            // Guard against indices past the vertex array (raylib indexes are
            // unsigned short, so meshes with more than 65535 vertices get
            // wrapped/garbage indices that must not be read out of bounds).
            if ((int)idx[0] >= mesh.vertexCount ||
                (int)idx[1] >= mesh.vertexCount ||
                (int)idx[2] >= mesh.vertexCount) continue;
            const float* a = &mesh.vertices[idx[0] * 3];
            const float* b = &mesh.vertices[idx[1] * 3];
            const float* c = &mesh.vertices[idx[2] * 3];
            pcoll::Triangle t{
                Vector3{ a[0], a[1], a[2] },
                Vector3{ b[0], b[1], b[2] },
                Vector3{ c[0], c[1], c[2] } };
            if (finiteTri(t)) out.push_back(t);
        }
    } else {
        int triCount = mesh.triangleCount;
        int maxTri = mesh.vertexCount / 3;
        if (triCount > maxTri) triCount = maxTri;
        for (int i = 0; i < triCount; ++i) {
            const float* a = &mesh.vertices[(i * 3 + 0) * 3];
            const float* b = &mesh.vertices[(i * 3 + 1) * 3];
            const float* c = &mesh.vertices[(i * 3 + 2) * 3];
            pcoll::Triangle t{
                Vector3{ a[0], a[1], a[2] },
                Vector3{ b[0], b[1], b[2] },
                Vector3{ c[0], c[1], c[2] } };
            if (finiteTri(t)) out.push_back(t);
        }
    }
}

} // namespace

static Vector3 s_lodCameraPos = {0.0f, 0.0f, 0.0f};

void SetLODCameraPos(Vector3 pos) {
    s_lodCameraPos = pos;
}

ScatteredObject::ScatteredObject(Vector3 pos, Vector3 size, Color color, ShapeType shape)
    : pos(pos), size(size), color(color), shape(shape) {}

ScatteredObject::~ScatteredObject() {
    if (hasOwnModel) {
        UnloadModel(model);
    }
    // Texture is managed by TextureManager via refcount
    if (!texturePath.empty()) {
        textureManager::UnregisterTexture(texturePath);
    }
}

void ScatteredObject::Update(float dt) {
    // Custom update logic per object can go here
}

void ScatteredObject::SetTexture(const std::string& path) {
    // Register with TextureManager (copies to project if needed)
    const auto& project = ::project::GetCurrentProject();
    if (project.path.empty()) return;
    
    std::string relPath = textureManager::RegisterTexture(path, "");
    if (relPath.empty()) return;
    
    // Unregister old texture if any
    if (!texturePath.empty()) {
        textureManager::UnregisterTexture(texturePath);
    }
    
    texturePath = relPath;
    
    // Update model materials with GPU texture from TextureManager
    Texture2D gpuTex = textureManager::GetGPUTexture(texturePath);
    if (gpuTex.id != 0 && hasOwnModel && model.materials != nullptr) {
        for (int i = 0; i < model.materialCount; ++i) {
            if (model.materials[i].maps != nullptr) {
                model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = gpuTex;
            }
        }
    }
}

std::string ScatteredObject::GetTexturePath() const {
    return texturePath;
}

void ScatteredObject::SetTexturePath(const std::string& projectRelativePath, const std::string& projectDir) {
    // Unregister old texture if any
    if (!texturePath.empty()) {
        textureManager::UnregisterTexture(texturePath);
    }
    
    texturePath = projectRelativePath;
    
    if (!texturePath.empty()) {
        textureManager::RegisterTexture(
            (std::filesystem::path(projectDir) / texturePath).generic_string(),
            ""
        );
        // Mark for lazy GPU load on next draw instead of decoding PNG now
        textureNeedsBind = true;
    }
}

void ScatteredObject::ClearTexture(const std::string& projectDir) {
    if (!texturePath.empty()) {
        textureManager::UnregisterTexture(texturePath);
        texturePath.clear();
    }
    // Reset to default texture
    if (hasOwnModel && model.materials != nullptr) {
        for (int i = 0; i < model.materialCount; ++i) {
            if (model.materials[i].maps != nullptr) {
                model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = gfx::GetDefaultTexture();
            }
        }
    }
}

Texture2D ScatteredObject::GetTexture() const {
    if (!texturePath.empty()) {
        // Use LOD path if available for this level, otherwise fall back to full-res
        std::string lodPath = textureManager::GetLODPath(texturePath, currentLODLevel);
        if (!lodPath.empty()) {
            Texture2D tex = textureManager::GetGPUTexture(lodPath);
            if (tex.id != 0) return tex;
        }
        return textureManager::GetGPUTexture(texturePath);
    }
    return gfx::GetDefaultTexture();
}

bool ScatteredObject::SetModel(const std::string& path) {
    Model loaded = { 0 };
    // FBX is not supported by raylib's built-in loaders, so route it through
    // the ufbx-backed loader. All other formats use raylib's LoadModel().
    if (IsFBXPath(path)) {
        if (!LoadFBXIntoModel(path, loaded)) {
            TraceLog(LOG_WARNING, "COLLISION: Could not load FBX mesh: %s", path.c_str());
            return false;
        }
    } else {
        loaded = LoadModel(path.c_str());
    }

    if (loaded.meshCount == 0 || loaded.meshes == nullptr) {
        if (loaded.meshes != nullptr) UnloadModel(loaded); // malformed but allocated
        return false;
    }

    // Loaders normally always produce materials; guard the degenerate case.
    if (loaded.materials == nullptr || loaded.materialCount == 0) {
        loaded.materialCount = 1;
        loaded.materials = (Material*)MemAlloc(sizeof(Material));
        loaded.materials[0] = LoadMaterialDefault();
    }

    // Give every material the editor's lit shader so imported meshes receive
    // the same lighting and shadows as the primitive shapes.
    for (int i = 0; i < loaded.materialCount; ++i) {
        loaded.materials[i].shader = gfx::GetLitShader();
    }

    if (hasOwnModel) {
        UnloadModel(model);
    }
    model = loaded;
    modelPath = path;
    hasOwnModel = true;
    geometryVersion++;

    if (!texturePath.empty()) {
        Texture2D gpuTex = textureManager::GetGPUTexture(texturePath);
        if (gpuTex.id != 0 && model.materials != nullptr) {
            for (int i = 0; i < model.materialCount; ++i) {
                if (model.materials[i].maps != nullptr) {
                    model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = textureManager::GetGPUTexture(texturePath);
                }
            }
        }
    }
    return true;
}

const std::string& ScatteredObject::GetModelPath() const { return modelPath; }

bool ScatteredObject::HasModel() const { return hasOwnModel; }

BoundingBox ScatteredObject::GetModelBounds() const {
    BoundingBox box{};
    if (!hasOwnModel || model.meshes == nullptr) return box;
    box = GetMeshBoundingBox(model.meshes[0]);
    for (int i = 1; i < model.meshCount; ++i) {
        BoundingBox m = GetMeshBoundingBox(model.meshes[i]);
        box.min.x = fminf(box.min.x, m.min.x);
        box.min.y = fminf(box.min.y, m.min.y);
        box.min.z = fminf(box.min.z, m.min.z);
        box.max.x = fmaxf(box.max.x, m.max.x);
        box.max.y = fmaxf(box.max.y, m.max.y);
        box.max.z = fmaxf(box.max.z, m.max.z);
    }
    return box;
}

void ScatteredObject::SetSizeFromModel() {
    if (!hasOwnModel || model.meshes == nullptr) return;

    // Normalizing sets the mesh's own extent as the object's size, so it
    // renders inside GetBoundingBox() (pos +/- size/2), exactly like a
    // primitive. Used at import time, when there is no saved size yet.
    size = NormalizeMeshVertices(model, GetModelBounds());
    geometryVersion++;
}

void ScatteredObject::NormalizeModelToUnitBox() {
    if (!hasOwnModel || model.meshes == nullptr) return;

    // Same normalization as SetSizeFromModel(), but `size` is left untouched:
    // used when reloading a scene, where size was already read from the save
    // file and must be preserved rather than re-derived from the mesh.
    NormalizeMeshVertices(model, GetModelBounds());
    geometryVersion++;
}

std::unique_ptr<ScatteredObject> ScatteredObject::Clone() const {
    auto copy = std::make_unique<ScatteredObject>(pos, size, color, shape);
    copy->SetName(name);
    *copy->GetPosPtr() = pos;
    *copy->GetSizePtr() = size;
    *copy->GetRotationPtr() = rotation;
    *copy->GetOriginPtr() = origin;
    copy->SetVelocity(velocity);
    copy->SetAngularVelocity(angularVelocity);
    if (mass > 0.0f) copy->SetMass(mass);
    copy->anchored = anchored;
    copy->runOnPlay = runOnPlay;
    copy->script = script;
    copy->texturePath = texturePath;
    if (!texturePath.empty()) copy->textureNeedsBind = true;
    
    // Re-register texture for the clone (increments refcount)
    if (!texturePath.empty()) {
        const auto& project = ::project::GetCurrentProject();
        if (!project.path.empty()) {
            textureManager::RegisterTexture(
                (std::filesystem::path(project.path) / texturePath).generic_string(),
                ""
            );
        }
    }
    
    // The clone owns its own loaded model (same file, fresh GPU resources);
    // a shared model handle would double-free when either object is destroyed.
    if (hasOwnModel && !modelPath.empty()) {
        copy->SetModel(modelPath);
        copy->SetSizeFromModel(); // re-normalizes to the same unit box as the source
        *copy->GetSizePtr() = size;
    }
    copy->collisionAccuracy = collisionAccuracy;
    copy->canCollide = canCollide;
    copy->transparency = transparency;
    return copy;
}

void ScatteredObject::UpdateLOD() {
    if (texturePath.empty()) return;
    if (!textureManager::IsPresetTexture(texturePath)) return;

    float dist = Vector3Distance(s_lodCameraPos, pos);
    int newLevel = textureManager::GetLODLevel(dist);
    if (newLevel == currentLODLevel) return;

    int oldLevel = currentLODLevel;
    currentLODLevel = newLevel;

    // For own-model objects, rebind the LOD texture to material slots.
    // Primitives get the right texture from GetTexture() each frame.
    if (hasOwnModel && model.materials != nullptr) {
        Texture2D tex = GetTexture();
        if (tex.id != 0) {
            for (int i = 0; i < model.materialCount; ++i) {
                if (model.materials[i].maps != nullptr) {
                    model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = tex;
                }
            }
        }
    }
}

void ScatteredObject::Draw() {
    // Lazy texture bind: was deferred from SetTexturePath to avoid blocking
    if (textureNeedsBind && hasOwnModel && !texturePath.empty() && model.materials != nullptr) {
        Texture2D gpuTex = textureManager::GetGPUTexture(texturePath);
        if (gpuTex.id != 0) {
            for (int i = 0; i < model.materialCount; ++i) {
                if (model.materials[i].maps != nullptr) {
                    model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = gpuTex;
                }
            }
        }
        textureNeedsBind = false;
    }

    // Update LOD before drawing
    UpdateLOD();

    // Apply transparency to alpha channel (0 = visible, 1 = invisible)
    Color drawColor = color;
    drawColor.a = (unsigned char)(color.a * (1.0f - transparency));

    if (hasOwnModel) {
        Matrix rotMat = useRenderRotation
            ? renderRotation
            : MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
        Matrix scaleMat = MatrixScale(size.x, size.y, size.z);
        Matrix transMat = MatrixTranslate(pos.x, pos.y, pos.z);
        model.transform = MatrixMultiply(MatrixMultiply(scaleMat, rotMat), transMat);
        DrawModel(model, Vector3Zero(), 1.0f, drawColor);

        if (isSelected) {
            Matrix ws = MatrixScale(size.x * 1.02f, size.y * 1.02f, size.z * 1.02f);
            model.transform = MatrixMultiply(MatrixMultiply(ws, rotMat), transMat);
            rlEnableWireMode();
            DrawModel(model, Vector3Zero(), 1.0f, WHITE);
            rlDisableWireMode();
            model.transform = MatrixMultiply(MatrixMultiply(scaleMat, rotMat), transMat);
        }
        return;
    }

    Matrix rotMat = useRenderRotation
        ? renderRotation
        : MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
    DrawShapeModel(shape, pos, size, rotMat, drawColor, GetTexture(), false);

    if (isSelected) {
        DrawShapeModel(shape, pos, size, rotMat, WHITE, GetTexture(), true);
    }
}

BoundingBox ScatteredObject::GetBoundingBox() const {
    Vector3 halfSize = { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
    Matrix rot = MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });

    BoundingBox box;
    box.min = {  1e30f,  1e30f,  1e30f };
    box.max = { -1e30f, -1e30f, -1e30f };

    for (int i = 0; i < 8; ++i) {
        Vector3 corner = {
            (i & 1) ?  halfSize.x : -halfSize.x,
            (i & 2) ?  halfSize.y : -halfSize.y,
            (i & 4) ?  halfSize.z : -halfSize.z
        };
        Vector3 world = Vector3Add(pos, Vector3Transform(corner, rot));
        box.min.x = fminf(box.min.x, world.x);
        box.min.y = fminf(box.min.y, world.y);
        box.min.z = fminf(box.min.z, world.z);
        box.max.x = fmaxf(box.max.x, world.x);
        box.max.y = fmaxf(box.max.y, world.y);
        box.max.z = fmaxf(box.max.z, world.z);
    }
    return box;
}

BoundingBox ModelGroup::GetBounds() const {
    BoundingBox box{};
    if (members.empty()) {
        box.min = { 0.0f, 0.0f, 0.0f };
        box.max = { 0.0f, 0.0f, 0.0f };
        return box;
    }
    bool first = true;
    for (const auto* member : members) {
        if (!member) continue;
        BoundingBox m = member->GetBoundingBox();
        if (first) {
            box.min = m.min;
            box.max = m.max;
            first = false;
        } else {
            box.min.x = fminf(box.min.x, m.min.x);
            box.min.y = fminf(box.min.y, m.min.y);
            box.min.z = fminf(box.min.z, m.min.z);
            box.max.x = fmaxf(box.max.x, m.max.x);
            box.max.y = fmaxf(box.max.y, m.max.y);
            box.max.z = fmaxf(box.max.z, m.max.z);
        }
    }
    return box;
}

Vector3* ScatteredObject::GetPosPtr() { return &pos; }
Vector3* ScatteredObject::GetSizePtr() { return &size; }
Color* ScatteredObject::GetColorPtr() { return &color; }
float* ScatteredObject::GetRotationYPtr() { return &rotation.y; }
Vector3* ScatteredObject::GetRotationPtr() { return &rotation; }
ShapeType ScatteredObject::GetShapeType() const { return shape; }

Vector3* ScatteredObject::GetOriginPtr() { return &origin; }

Vector3 ScatteredObject::GetOriginWorld() const {
    Matrix rotMat = MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
    return Vector3Add(pos, Vector3Transform(origin, rotMat));
}
const std::string& ScatteredObject::GetName() const { return name; }
void ScatteredObject::SetName(const std::string& newName) { name = newName.empty() ? "Object" : newName; }

Vector3 ScatteredObject::GetVelocity() const { return velocity; }
void ScatteredObject::SetVelocity(Vector3 v) { velocity = v; }
Vector3 ScatteredObject::GetAngularVelocity() const { return angularVelocity; }
void ScatteredObject::SetAngularVelocity(Vector3 v) { angularVelocity = v; }

float ScatteredObject::GetMass() const {
    if (mass > 0.0f) return mass;

    // Auto mass: derived from the current size so scaling an object retunes it
    // automatically (the same volume rules the physics body uses).
    // Default density: 500 kg/m³ (wood-like), so objects float with ~half volume submerged.
    const float defaultDensity = 500.0f;
    const Vector3& s = size;
    float volume = 0.0f;
    switch (shape) {
        case ShapeType::Sphere: {
            float r = (s.x + s.y + s.z) / 6.0f;
            volume = (4.0f / 3.0f) * 3.14159265f * r * r * r;
            break;
        }
        case ShapeType::Wedge:
            volume = 0.5f * s.x * s.y * s.z;
            break;
        case ShapeType::Cube:
        case ShapeType::Cylinder:
            volume = s.x * s.y * s.z;
            break;
    }
    return volume > 1e-3f ? volume * defaultDensity : 1e-3f;
}

void ScatteredObject::SetMass(float m) {
    mass = m > 1e-3f ? m : 1e-3f;
}

void ScatteredObject::ResetMassAuto() {
    mass = 0.0f;
}

bool ScatteredObject::IsMassAuto() const { return mass <= 0.0f; }

void ScatteredObject::SetRenderRotation(const Matrix& rot) {
    renderRotation = rot;
    useRenderRotation = true;
}

void ScatteredObject::ClearRenderRotation() {
    useRenderRotation = false;
    renderRotation = MatrixIdentity();
}

float ScatteredObject::GetTransparency() const {
    return transparency;
}

void ScatteredObject::SetTransparency(float t) {
    transparency = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
}

float* ScatteredObject::GetTransparencyPtr() {
    return &transparency;
}

bool ScatteredObject::IsCollisionAccuracySupported() const {
    // Plain cubes are always axis-aligned boxes; everything else (sphere,
    // cylinder, wedge primitives and all imported meshes) supports the
    // configurable hull/triangle colliders.
    return hasOwnModel || shape != ShapeType::Cube;
}

pcoll::CollisionAccuracy ScatteredObject::GetCollisionAccuracy() const {
    return IsCollisionAccuracySupported() ? collisionAccuracy : pcoll::CollisionAccuracy::Box;
}

void ScatteredObject::SetCollisionAccuracy(pcoll::CollisionAccuracy accuracy) {
    if (IsCollisionAccuracySupported()) collisionAccuracy = accuracy;
}

const pcoll::Collider& ScatteredObject::GetCollider() const {
    int key = (int)GetCollisionAccuracy() * 100003 + (int)geometryVersion;
    if (colliderCacheKey == key) return colliderCache;

    std::vector<pcoll::Triangle> tris;
    if (hasOwnModel) {
        for (int i = 0; i < model.meshCount; ++i) ExtractMeshTriangles(model.meshes[i], tris);
    } else {
        Model& m = gfx::GetShapeModel(shape);
        for (int i = 0; i < m.meshCount; ++i) ExtractMeshTriangles(m.meshes[i], tris);
    }

    colliderCacheKey = key;
    colliderCache = pcoll::BuildCollider(GetCollisionAccuracy(), tris);
    return colliderCache;
}