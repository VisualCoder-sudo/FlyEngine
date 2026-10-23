#include <algorithm>
#include <chrono>
#include <future>
#include <unordered_map>
#include <cstring>
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Backend/Csg.hpp"
#include "../../../include/Graphics/Graphics.hpp"
#include "../../../include/Graphics/PostFX.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Graphics/Occlusion.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/FbxModel.hpp"
#include "../../../include/Engine/TechnicalTools.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cmath>
#include <filesystem>
#include "Engine/U8Path.hpp"

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
    std::vector<unsigned int> freed;   // one file texture is often bound to several slots/materials: free it once
    for (int m = 0; m < model.materialCount; ++m) {
        const MaterialMap* maps = model.materials[m].maps;
        if (maps == nullptr) continue;
        for (int i = 0; i < kMaxMaterialMaps; ++i) {
            const Texture2D tex = maps[i].texture;
            if (tex.id != 0 && tex.id != rlGetTextureIdDefault() && tex.id != keepId &&
                std::find(freed.begin(), freed.end(), tex.id) == freed.end()) {
                freed.push_back(tex.id);
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

// Readies a loaded model's physically based materials: the loaders leave their maps point-sampled with no mips
// (fine for pixel art, shimmering on anything else), and a normal map needs tangents, which many files lack.
// Call after NormalizeMeshVertices(): tangents are taken in the space the vertices end up in.
void PreparePbrMaterials(Model& model) {
    const unsigned int defaultTex = rlGetTextureIdDefault();
    const int maps[] = { MATERIAL_MAP_ALBEDO, MATERIAL_MAP_METALNESS, MATERIAL_MAP_NORMAL, MATERIAL_MAP_ROUGHNESS,
                         MATERIAL_MAP_OCCLUSION, MATERIAL_MAP_EMISSION };
    std::vector<unsigned int> done;
    for (int i = 0; i < model.materialCount; ++i) {
        if (!model.materials[i].maps) continue;
        for (int slot : maps) {
            Texture2D& tex = model.materials[i].maps[slot].texture;
            if (tex.id == 0 || tex.id == defaultTex || std::find(done.begin(), done.end(), tex.id) != done.end()) continue;
            done.push_back(tex.id);
            if (tex.mipmaps <= 1) GenTextureMipmaps(&tex);
            SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
        }
    }
    for (int m = 0; m < model.meshCount; ++m) {
        Mesh& mesh = model.meshes[m];
        const int mat = model.meshMaterial ? model.meshMaterial[m] : 0;
        if (mat < 0 || mat >= model.materialCount || !model.materials[mat].maps) continue;
        if (model.materials[mat].maps[MATERIAL_MAP_NORMAL].texture.id == 0 || mesh.tangents) continue;
        if (!mesh.texcoords || !mesh.normals) continue;
        // GenMeshTangents would hand the result to rlgl's own buffer; this layer keeps one per attribute instead.
        unsigned int* vbo = mesh.vboId;
        mesh.vboId = nullptr;
        GenMeshTangents(&mesh);
        mesh.vboId = vbo;
        if (mesh.tangents) UpdateMeshBuffer(mesh, 4, mesh.tangents, mesh.vertexCount * 4 * (int)sizeof(float), 0);
    }
}

// One loaded+normalized copy of each model file, shared by every object that
// uses it (duplicates, copy/paste, repeated placements). Meshes and their GPU
// buffers live here once; each object only owns a private materials array so a
// per-object texture assignment doesn't leak onto its siblings.
struct SharedModelEntry {
    Model model{};
    Vector3 extent{ 1.0f, 1.0f, 1.0f };   // pre-normalization size of the mesh
    std::string diffuseFile;               // base-colour texture found beside the model
    ModelAnimation* anims = nullptr;       // skeletal clips (file space), owned
    int animCount = 0;
    Matrix norm{};                         // file space -> normalized mesh space
    std::unordered_map<int, std::shared_ptr<const pcoll::Collider>> colliders;   // by accuracy
    int refs = 0;
};
std::unordered_map<std::string, SharedModelEntry> g_sharedModels;

// FBX files parsed ahead of time on a worker thread (see PrefetchModel).
struct PrefetchResult {
    bool ok = false;
    FbxCpuData data;
};
std::unordered_map<std::string, std::shared_future<PrefetchResult>> g_prefetched;

std::string SharedModelKey(const std::string& path) {
    return U8Path(path).lexically_normal().generic_string();
}

BoundingBox ModelBounds(const Model& model) {
    BoundingBox box{};
    if (model.meshes == nullptr || model.meshCount <= 0) return box;
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

// Draws one shared shape model at an arbitrary position/size/rotation.
// wireframeOverlay draws a slightly enlarged wireframe pass (used for the
// selection outline) instead of a solid, lit, textured pass.
void DrawShapeModel(ShapeType shape, Vector3 pos, Vector3 size, Matrix rotMat,
                     Color tint, Texture2D texture, bool wireframeOverlay, const gfx::SurfaceMaterial& surface = {}) {
    Model& model = gfx::GetShapeModel(shape);
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = texture;
    gfx::ApplySurfaceMaterial(model.materials[0], surface);

    Vector3 drawSize = wireframeOverlay ? Vector3Scale(size, 1.02f) : size;

    model.transform = gfx::ComposeTRS(drawSize, rotMat, pos);

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


// The primitive part as CSG polygons: its shape mesh carried through `xf` (size, rotation, position) into the
// union's frame. Winding is made to agree with the mesh's own normals so a mesh with flipped triangles still
// yields a consistently wound solid.
csg::Polys PartPolygons(ScatteredObject& part) {
    csg::Polys out;
    const Model& shape = gfx::GetShapeModel(part.GetShapeType());
    const Vector3 size = *part.GetSizePtr();
    const Matrix rot = part.GetRotationMatrix();
    const Matrix xf = gfx::ComposeTRS(size, rot, *part.GetPosPtr());
    const Vector3 invSize = { size.x != 0.0f ? 1.0f / size.x : 0.0f, size.y != 0.0f ? 1.0f / size.y : 0.0f, size.z != 0.0f ? 1.0f / size.z : 0.0f };
    const Color c = *part.GetColorPtr();
    const uint32_t packed = (uint32_t)c.r | ((uint32_t)c.g << 8) | ((uint32_t)c.b << 16) | (255u << 24);
    for (int m = 0; m < shape.meshCount; ++m) {
        const Mesh& mesh = shape.meshes[m];
        if (!mesh.vertices) continue;
        const int triCount = mesh.indices ? mesh.triangleCount : mesh.vertexCount / 3;
        for (int t = 0; t < triCount; ++t) {
            int idx[3];
            for (int k = 0; k < 3; ++k) idx[k] = mesh.indices ? (int)mesh.indices[t * 3 + k] : t * 3 + k;
            if (idx[0] >= mesh.vertexCount || idx[1] >= mesh.vertexCount || idx[2] >= mesh.vertexCount) continue;
            csg::Vertex v[3];
            Vector3 wp[3];
            for (int k = 0; k < 3; ++k) {
                const float* p = &mesh.vertices[idx[k] * 3];
                wp[k] = Vector3Transform({ p[0], p[1], p[2] }, xf);
                v[k].pos = { wp[k].x, wp[k].y, wp[k].z };
                if (mesh.normals) {
                    const float* n = &mesh.normals[idx[k] * 3];
                    Vector3 wn = Vector3Normalize(Vector3Transform({ n[0] * invSize.x, n[1] * invSize.y, n[2] * invSize.z }, rot));
                    v[k].normal = { wn.x, wn.y, wn.z };
                }
            }
            const Vector3 geo = Vector3CrossProduct(Vector3Subtract(wp[1], wp[0]), Vector3Subtract(wp[2], wp[0]));
            if (Vector3Length(geo) < 1e-9f) continue;
            if (mesh.normals) {
                const csg::V3 an = { v[0].normal.x + v[1].normal.x + v[2].normal.x, v[0].normal.y + v[1].normal.y + v[2].normal.y,
                                     v[0].normal.z + v[1].normal.z + v[2].normal.z };
                if (an.x * geo.x + an.y * geo.y + an.z * geo.z < 0.0) std::swap(v[1], v[2]);
            }
            csg::Polygon poly;
            poly.verts = { v[0], v[1], v[2] };
            poly.color = packed;
            out.push_back(std::move(poly));
        }
    }
    return out;
}

// Euler angles (degrees) that MatrixRotateXYZ turns into `m`.
Vector3 EulerFromRotation(const Matrix& m) {
    const float sy = std::clamp(-m.m2, -1.0f, 1.0f);
    float x, y = asinf(sy), z;
    if (fabsf(sy) < 0.99999f) {
        x = atan2f(m.m6, m.m10);
        z = atan2f(m.m1, m.m0);
    } else {   // looking straight along an axis: x and z are one rotation, put it all on x
        x = atan2f(-m.m9, m.m5);
        z = 0.0f;
    }
    return { x * RAD2DEG, y * RAD2DEG, z * RAD2DEG };
}

} // namespace

static Vector3 s_lodCameraPos = {0.0f, 0.0f, 0.0f};

void SetLODCameraPos(Vector3 pos) {
    s_lodCameraPos = pos;
}

ScatteredObject::ScatteredObject(Vector3 pos, Vector3 size, Color color, ShapeType shape)
    : pos(pos), size(size), color(color), shape(shape) {}

ScatteredObject::~ScatteredObject() {
    ReleaseModel();
    // Texture is managed by TextureManager via refcount
    if (!texturePath.empty()) {
        textureManager::UnregisterTexture(texturePath);
    }
}

void ScatteredObject::Update(float dt) {
    // The animation clock runs on the engine's (time-scaled) step: it keeps time while culled and stops while the
    // simulation is paused. Skinning stays in Draw().
    if (animator) animator->Advance(std::min(dt, 0.1f));
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

void ScatteredObject::ReleaseModel() {
    if (!hasOwnModel) return;
    if (ownsProceduralMesh) {   // a union's mesh: private meshes and materials, nothing shared
        if (model.materials != nullptr) {
            for (int i = 0; i < model.materialCount; ++i) MemFree(model.materials[i].maps);
            MemFree(model.materials);
        }
        for (int i = 0; i < model.meshCount; ++i) UnloadMesh(model.meshes[i]);
        MemFree(model.meshes);
        MemFree(model.meshMaterial);
        model = Model{};
        unionVertexColors.clear();
        ownsProceduralMesh = false;
        hasOwnModel = false;
        return;
    }
    animator.reset();   // frees its private meshes; the clips below may go with the shared entry
    // Private materials array only; meshes belong to the shared entry.
    if (model.materials != nullptr) {
        for (int i = 0; i < model.materialCount; ++i) MemFree(model.materials[i].maps);
        MemFree(model.materials);
    }
    auto it = g_sharedModels.find(sharedModelKey);
    if (it != g_sharedModels.end() && --it->second.refs <= 0) {
        // Untrack before unloading: the tracker keys the model on its meshes
        // array, which UnloadModel is about to free.
        TechTools::MemoryTracker::Instance().UntrackModel(it->second.model);
        UnloadModelTextures(it->second.model);   // UnloadModel leaves the file's textures on the GPU
        UnloadModel(it->second.model);
        if (it->second.anims) UnloadModelAnimations(it->second.anims, it->second.animCount);
        g_sharedModels.erase(it);
    }
    model = Model{};
    sharedModelKey.clear();
    hasOwnModel = false;
}

void ScatteredObject::PrefetchModel(const std::string& path) {
    if (!IsFBXPath(path)) return;
    const std::string key = SharedModelKey(path);
    if (g_sharedModels.count(key) || g_prefetched.count(key)) return;
    g_prefetched.emplace(key, std::async(std::launch::async, [path]() {
        PrefetchResult r;
        r.ok = LoadFBXCpu(path, r.data);
        return r;
    }).share());
}

bool ScatteredObject::IsModelReady(const std::string& path) {
    const std::string key = SharedModelKey(path);
    auto it = g_prefetched.find(key);
    if (it == g_prefetched.end()) return true;   // cached, not FBX, or nothing started
    return it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

bool ScatteredObject::SetModel(const std::string& path) {
    const std::string key = SharedModelKey(path);
    auto it = g_sharedModels.find(key);
    if (it == g_sharedModels.end()) {
        Model loaded = { 0 };
        std::string diffuseFile;
        // FBX is not supported by raylib's built-in loaders, so route it through
        // the ufbx-backed loader. All other formats use raylib's LoadModel().
        if (IsFBXPath(path)) {
            bool ok = false;
            auto pre = g_prefetched.find(key);
            if (pre != g_prefetched.end()) {
                // Parsed on a worker thread already (blocks only if it isn't done).
                PrefetchResult r = pre->second.get();
                g_prefetched.erase(pre);
                if (r.ok) {
                    diffuseFile = r.data.diffuseFile;
                    ok = UploadFBXCpu(r.data, loaded);
                }
                FreeFbxCpu(r.data);
            } else {
                ok = LoadFBXIntoModel(path, loaded, &diffuseFile);
            }
            if (!ok) {
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

        // Normalize once, here: every user sees the same [-0.5, 0.5] mesh and
        // scales it through its own `size`.
        SharedModelEntry entry;
        const BoundingBox loadedBounds = ModelBounds(loaded);
        entry.extent = NormalizeMeshVertices(loaded, loadedBounds);
        PreparePbrMaterials(loaded);
        {
            // The same affine map NormalizeMeshVertices applied, for the skeleton.
            const Vector3 inv = { 1.0f / entry.extent.x, 1.0f / entry.extent.y, 1.0f / entry.extent.z };
            entry.norm = MatrixMultiply(MatrixScale(inv.x, inv.y, inv.z),
                MatrixTranslate(-loadedBounds.min.x * inv.x - 0.5f, -loadedBounds.min.y * inv.y - 0.5f,
                                -loadedBounds.min.z * inv.z - 0.5f));
        }
        if (!IsFBXPath(path) && loaded.skeleton.boneCount > 0 && loaded.boneMatrices) {
            entry.anims = LoadModelAnimations(path.c_str(), &entry.animCount);
            if (!entry.anims) entry.animCount = 0;
        }
        entry.model = loaded;
        entry.diffuseFile = diffuseFile;

        // Tagged with the model path so a leak report names the asset that is
        // holding the memory, not just "model, 2 MB".
        TechTools::MemoryTracker::Instance().TrackModel(entry.model, path);
        it = g_sharedModels.emplace(key, std::move(entry)).first;
    }

    // Take the new reference BEFORE dropping the old model: when this object already holds `key` (the same file set again)
    // the release would otherwise take the entry's count to zero, unload the model and erase `it` from under us.
    SharedModelEntry& shared = it->second;
    shared.refs++;
    ReleaseModel();   // drop the previous model only once the new one is ready
    sharedModelKey = key;
    modelExtent = shared.extent;
    modelDiffuseFile = shared.diffuseFile;

    model = shared.model;   // aliases shared meshes / meshMaterial
    model.materials = (Material*)MemAlloc((unsigned int)shared.model.materialCount * sizeof(Material));
    for (int i = 0; i < shared.model.materialCount; ++i) {
        model.materials[i] = shared.model.materials[i];
        model.materials[i].maps = (MaterialMap*)MemAlloc(kMaxMaterialMaps * sizeof(MaterialMap));
        memcpy(model.materials[i].maps, shared.model.materials[i].maps, kMaxMaterialMaps * sizeof(MaterialMap));
    }
    modelPath = path;
    hasOwnModel = true;
    geometryVersion++;

    animator.reset();
    if (shared.animCount > 0) {
        auto anim = std::make_unique<ModelAnimator>();
        if (anim->Init(shared.model, shared.anims, shared.animCount, shared.norm)) {
            model.meshes = anim->Meshes();
            animator = std::move(anim);
            ApplyAnimation();
        }
    }

    // The materials are fresh copies, so re-bind the object's texture. Draw()
    // does this lazily and asynchronously instead of decoding it here.
    if (!texturePath.empty()) textureNeedsBind = true;
    return true;
}

const std::string& ScatteredObject::GetModelPath() const { return modelPath; }

void ScatteredObject::ApplyAnimation() {
    if (!animator) return;
    int clip = animClip.empty() ? 0 : animator->FindClip(animClip.c_str());
    if (clip < 0) clip = 0;   // the saved clip is gone from the file: fall back to the first
    animator->Play(clip, animLoop, animSpeed);
    if (!animPlay) animator->Stop();
}

bool ScatteredObject::PlayAnimation(int clip, bool loop, float speed) {
    if (!animator || clip < 0 || clip >= animator->ClipCount()) return false;
    animClip = animator->ClipName(clip);
    animLoop = loop;
    animSpeed = speed;
    animPlay = true;
    ApplyAnimation();
    return true;
}

void ScatteredObject::StopAnimation() {
    animPlay = false;
    ApplyAnimation();
}

bool ScatteredObject::PlayAnimation(const std::string& name, bool loop, float speed) {
    return animator && PlayAnimation(animator->FindClip(name.c_str()), loop, speed);
}

bool ScatteredObject::HasModel() const { return hasOwnModel; }

BoundingBox ScatteredObject::GetModelBounds() const {
    if (!hasOwnModel) return BoundingBox{};
    return ModelBounds(model);
}

void ScatteredObject::SetSizeFromModel() {
    if (!hasOwnModel || model.meshes == nullptr) return;

    // The mesh is normalized to [-0.5, 0.5] when loaded, so its original
    // extent becomes the object's size and it renders inside GetBoundingBox()
    // (pos +/- size/2), exactly like a primitive. Used at import time, when
    // there is no saved size yet.
    size = modelExtent;
    geometryVersion++;
}

void ScatteredObject::NormalizeModelToUnitBox() {
    // Kept for callers that reload scenes: SetModel() already normalizes the
    // shared mesh once, and `size` stays whatever the save file said.
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
    copy->extraScripts = extraScripts;
    copy->nodes = nodes;
    copy->texturePath = texturePath;
    if (!texturePath.empty()) copy->textureNeedsBind = true;
    
    // The clone's destructor releases one reference, so it must take one. (RegisterTexture with an empty model
    // path returns early without counting, which left the destructor's release unmatched.)
    if (!texturePath.empty()) textureManager::AddTextureRef(texturePath);
    
    // The clone shares the source's mesh data (one copy in memory and on the
    // GPU); only its materials are private.
    if (hasOwnModel && !modelPath.empty()) {
        copy->animClip = animClip;
        copy->animLoop = animLoop;
        copy->animPlay = animPlay;
        copy->animSpeed = animSpeed;
        copy->SetModel(modelPath);
        *copy->GetSizePtr() = size;
    }
    copy->negate = negate;
    if (IsUnion()) {
        for (const UnionPart& p : unionParts) copy->unionParts.push_back({ p.obj->Clone(), p.mode });
        copy->useGlobalColor = useGlobalColor;
        copy->RebuildUnionMesh();
        copy->unionBaseSize = unionBaseSize;
    }
    copy->collisionAccuracy = collisionAccuracy;
    copy->canCollide = canCollide;
    copy->transparency = transparency;
    copy->surface = surface;
    copy->useModelMaterial = useModelMaterial;
    return copy;
}

std::unique_ptr<ScatteredObject> ScatteredObject::MakeUnion(std::vector<std::unique_ptr<ScatteredObject>> parts,
                                                             const std::vector<UnionMode>& modes) {
    if (parts.empty() || parts.size() != modes.size()) return nullptr;
    size_t first = parts.size();
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!parts[i] || parts[i]->HasModel()) return nullptr;   // imported meshes are not supported
        if (first == parts.size() && modes[i] != UnionMode::Negate) first = i;
    }
    if (first == parts.size()) return nullptr;   // nothing but cutters

    auto u = std::make_unique<ScatteredObject>(Vector3Zero(), Vector3One(), *parts[first]->GetColorPtr(), ShapeType::Cube);
    u->SetName("Union");
    u->anchored = parts[first]->anchored;
    u->canCollide = parts[first]->canCollide;
    u->surface = parts[first]->surface;
    u->useModelMaterial = false;   // a primitive's material, not a file's
    for (size_t i = 0; i < parts.size(); ++i) {
        parts[i]->parentModel = nullptr;
        u->unionParts.push_back({ std::move(parts[i]), modes[i] });
    }
    Vector3 center;
    if (!u->RebuildUnionMesh(&center)) return nullptr;
    // The parts were in world space: re-express them relative to the union's centre.
    for (UnionPart& p : u->unionParts) *p.obj->GetPosPtr() = Vector3Subtract(*p.obj->GetPosPtr(), center);
    u->pos = center;
    u->size = u->unionBaseSize;
    return u;
}

bool ScatteredObject::RebuildUnionMesh(Vector3* outCenter) {
    if (unionParts.empty()) return false;
    csg::Polys result;
    bool started = false;
    for (UnionPart& p : unionParts) {
        if (p.mode != UnionMode::Add) continue;
        csg::Polys polys = PartPolygons(*p.obj);
        result = started ? csg::Union(result, polys) : std::move(polys);
        started = true;
    }
    for (UnionPart& p : unionParts) {
        if (p.mode != UnionMode::Negate || !started) continue;
        result = csg::Subtract(result, PartPolygons(*p.obj));
    }
    for (UnionPart& p : unionParts) {
        if (p.mode != UnionMode::Intersect) continue;
        csg::Polys polys = PartPolygons(*p.obj);
        result = started ? csg::Intersect(result, polys) : std::move(polys);
        started = true;
    }
    const csg::Bounds bounds = csg::BoundsOf(result);
    const csg::TriSoup soup = csg::Triangulate(result);
    if (!bounds.valid || soup.TriangleCount() == 0) return false;

    const csg::V3 ext = { std::max(bounds.max.x - bounds.min.x, 1e-3), std::max(bounds.max.y - bounds.min.y, 1e-3),
                          std::max(bounds.max.z - bounds.min.z, 1e-3) };
    const csg::V3 mid = { (bounds.min.x + bounds.max.x) * 0.5, (bounds.min.y + bounds.max.y) * 0.5, (bounds.min.z + bounds.max.z) * 0.5 };

    ReleaseModel();   // the old mesh; ReleaseModel clears unionVertexColors too

    // raylib meshes index with 16 bits: split into pieces that fit.
    constexpr size_t kChunkVerts = 65535;   // a multiple of 3
    const size_t totalVerts = soup.color.size();
    const int meshCount = (int)((totalVerts + kChunkVerts - 1) / kChunkVerts);
    model = Model{};
    model.transform = MatrixIdentity();
    model.meshCount = meshCount;
    model.meshes = (Mesh*)MemAlloc((unsigned int)meshCount * sizeof(Mesh));
    model.meshMaterial = (int*)MemAlloc((unsigned int)meshCount * sizeof(int));
    unionVertexColors.assign((size_t)meshCount, {});
    for (int mi = 0; mi < meshCount; ++mi) {
        const size_t begin = (size_t)mi * kChunkVerts;
        const size_t count = std::min(kChunkVerts, totalVerts - begin);
        Mesh mesh = { 0 };
        mesh.vertexCount = (int)count;
        mesh.triangleCount = (int)(count / 3);
        mesh.vertices = (float*)MemAlloc((unsigned int)(count * 3 * sizeof(float)));
        mesh.normals = (float*)MemAlloc((unsigned int)(count * 3 * sizeof(float)));
        mesh.texcoords = (float*)MemAlloc((unsigned int)(count * 2 * sizeof(float)));
        mesh.colors = (unsigned char*)MemAlloc((unsigned int)(count * 4));
        mesh.indices = (unsigned short*)MemAlloc((unsigned int)(count * sizeof(unsigned short)));
        for (size_t i = 0; i < count; ++i) {
            const size_t s = begin + i;
            mesh.vertices[i * 3 + 0] = (float)((soup.pos[s * 3 + 0] - mid.x) / ext.x);
            mesh.vertices[i * 3 + 1] = (float)((soup.pos[s * 3 + 1] - mid.y) / ext.y);
            mesh.vertices[i * 3 + 2] = (float)((soup.pos[s * 3 + 2] - mid.z) / ext.z);
            // Unit-space normal of a surface that gets stretched by `ext`: n * ext.
            Vector3 n = Vector3Normalize({ (float)(soup.nrm[s * 3 + 0] * ext.x), (float)(soup.nrm[s * 3 + 1] * ext.y), (float)(soup.nrm[s * 3 + 2] * ext.z) });
            mesh.normals[i * 3 + 0] = n.x; mesh.normals[i * 3 + 1] = n.y; mesh.normals[i * 3 + 2] = n.z;
            const uint32_t col = soup.color[s];
            mesh.colors[i * 4 + 0] = (unsigned char)(col & 0xFF); mesh.colors[i * 4 + 1] = (unsigned char)((col >> 8) & 0xFF);
            mesh.colors[i * 4 + 2] = (unsigned char)((col >> 16) & 0xFF); mesh.colors[i * 4 + 3] = 255;
            mesh.indices[i] = (unsigned short)i;
        }
        // Texture coordinates: each triangle is projected along its dominant axis and spans 0..1 across the union,
        // like the primitives' faces do (so a texture or the default checker stretches with the object's size).
        for (size_t t = 0; t + 2 < count; t += 3) {
            const Vector3 a = { mesh.vertices[t * 3], mesh.vertices[t * 3 + 1], mesh.vertices[t * 3 + 2] };
            const Vector3 b = { mesh.vertices[(t + 1) * 3], mesh.vertices[(t + 1) * 3 + 1], mesh.vertices[(t + 1) * 3 + 2] };
            const Vector3 c = { mesh.vertices[(t + 2) * 3], mesh.vertices[(t + 2) * 3 + 1], mesh.vertices[(t + 2) * 3 + 2] };
            const Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
            const float ax = fabsf(n.x), ay = fabsf(n.y), az = fabsf(n.z);
            for (size_t k = 0; k < 3; ++k) {
                const float* v = &mesh.vertices[(t + k) * 3];   // unit space, -0.5..0.5
                float u, w;
                if (ax >= ay && ax >= az) { u = v[2]; w = v[1]; }
                else if (ay >= az)        { u = v[0]; w = v[2]; }
                else                      { u = v[0]; w = v[1]; }
                mesh.texcoords[(t + k) * 2 + 0] = u + 0.5f;
                mesh.texcoords[(t + k) * 2 + 1] = w + 0.5f;
            }
        }
        unionVertexColors[(size_t)mi].assign(mesh.colors, mesh.colors + count * 4);
        if (useGlobalColor) memset(mesh.colors, 255, count * 4);
        UploadMesh(&mesh, false);
        model.meshes[mi] = mesh;
        model.meshMaterial[mi] = 0;
    }
    // One material, the primitives' lit one.
    const Material& cubeMat = gfx::GetShapeModel(ShapeType::Cube).materials[0];
    model.materialCount = 1;
    model.materials = (Material*)MemAlloc(sizeof(Material));
    model.materials[0] = cubeMat;
    model.materials[0].maps = (MaterialMap*)MemAlloc(kMaxMaterialMaps * sizeof(MaterialMap));
    memcpy(model.materials[0].maps, cubeMat.maps, kMaxMaterialMaps * sizeof(MaterialMap));
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = gfx::GetDefaultTexture();

    hasOwnModel = true;
    ownsProceduralMesh = true;
    modelExtent = { (float)ext.x, (float)ext.y, (float)ext.z };
    unionBaseSize = modelExtent;
    geometryVersion++;
    if (!texturePath.empty()) textureNeedsBind = true;
    if (outCenter) *outCenter = { (float)mid.x, (float)mid.y, (float)mid.z };
    return true;
}

void ScatteredObject::ApplyUnionColors() {
    if (!ownsProceduralMesh) return;
    for (int mi = 0; mi < model.meshCount && mi < (int)unionVertexColors.size(); ++mi) {
        Mesh& mesh = model.meshes[mi];
        const std::vector<unsigned char>& src = unionVertexColors[(size_t)mi];
        if (useGlobalColor) memset(mesh.colors, 255, src.size());
        else memcpy(mesh.colors, src.data(), src.size());
        UpdateMeshBuffer(mesh, 3, mesh.colors, (int)src.size(), 0);
    }
}

void ScatteredObject::SetUseGlobalColor(bool on) {
    if (useGlobalColor == on) return;
    useGlobalColor = on;
    ApplyUnionColors();
}

std::vector<std::unique_ptr<ScatteredObject>> ScatteredObject::TakeUnionParts() {
    std::vector<std::unique_ptr<ScatteredObject>> out;
    const Vector3 scale = { unionBaseSize.x != 0.0f ? size.x / unionBaseSize.x : 1.0f, unionBaseSize.y != 0.0f ? size.y / unionBaseSize.y : 1.0f,
                            unionBaseSize.z != 0.0f ? size.z / unionBaseSize.z : 1.0f };
    const Matrix rot = GetRotationMatrix();
    for (UnionPart& p : unionParts) {
        ScatteredObject& o = *p.obj;
        Vector3 local = *o.GetPosPtr();
        local = { local.x * scale.x, local.y * scale.y, local.z * scale.z };
        *o.GetPosPtr() = Vector3Add(pos, Vector3Transform(local, rot));
        *o.GetSizePtr() = { o.size.x * scale.x, o.size.y * scale.y, o.size.z * scale.z };
        *o.GetRotationPtr() = EulerFromRotation(MatrixMultiply(o.GetRotationMatrix(), rot));
        o.negate = p.mode == UnionMode::Negate;
        o.canCollide = o.negate ? false : canCollide;
        o.anchored = anchored;
        out.push_back(std::move(p.obj));
    }
    unionParts.clear();
    ReleaseModel();
    return out;
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
    // Only when a texture is assigned: otherwise GetTexture() is the default
    // white texture and would wipe the model's own embedded textures.
    if (hasOwnModel && !texturePath.empty() && model.materials != nullptr) {
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
    if (IsHiddenEffective()) return;
    // A cutter is an editor aid: it never shows in play and casts no shadow.
    if (negate && (ui::IsPlayActive() || gfx::IsInShadowPass())) return;
    // When the shadow map is being reused, the pass is still entered and every
    // entity is still called -- Graphics.cpp sets a 0x0 scissor so the
    // fragments are all discarded. Everything below (LOD, frustum extraction,
    // the 8-corner bounding box, DrawModel) is therefore pure waste.
    // City::Draw() has always bailed out here; this brings the loose objects in
    // line. Must stay the very first statement: the skip has to happen before
    // UpdateLOD, which is not free either.
    if (gfx::IsInShadowPass() && gfx::IsShadowPassReused()) return;

    // Small, distant objects cast shadows too tiny to see: leave them out of the
    // shadow pass (they are still drawn normally in the main pass).
    if (gfx::IsInShadowPass()) {
        constexpr float kShadowSkipMinDistance = 50.0f;   // never skip anything closer than this (m)
        constexpr float kShadowSkipMinAngular = 0.03f;    // skip when radius / distance falls below this
        const float dist = Vector3Distance(s_lodCameraPos, pos);
        if (dist > kShadowSkipMinDistance) {
            const float radius = 0.5f * Vector3Length(size);
            if (radius / dist < kShadowSkipMinAngular) return;
        }
    }

    // A light's own host does not shadow it (a lamp inside its bulb would otherwise black out the room).
    if (gfx::IsInShadowPass() && gfx::NodeShadowSkipHost() == this) return;

    // Lazy texture bind: was deferred from SetTexturePath to avoid blocking
    if (textureNeedsBind && hasOwnModel && !texturePath.empty() && model.materials != nullptr) {
        bool pending = false;
        Texture2D gpuTex = textureManager::GetGPUTextureAsync(texturePath, pending);
        if (gpuTex.id != 0) {
            for (int i = 0; i < model.materialCount; ++i) {
                if (model.materials[i].maps != nullptr) {
                    model.materials[i].maps[MATERIAL_MAP_DIFFUSE].texture = gpuTex;
                }
            }
        }
        // Keep trying while the worker thread is still decoding.
        if (!pending) textureNeedsBind = false;
    }

    // Update LOD before drawing
    UpdateLOD();

    // Frustum culling: skip rendering if outside the view frustum
    // (Physics and collisions are still processed in Update())
    Frustum frustum = Frustum::ExtractCurrent();
    if (!frustum.Intersects(GetBoundingBox())) {
        gfx::IncrementCulledEntityCount(1);
        return;
    }
    // Hidden behind a big opaque box this frame (main view only; see Occlusion.hpp).
    if (!gfx::IsInShadowPass() && gfx::GetFrustumCullingEnabled() && occlusion::IsOccluded(GetBoundingBox())) {
        gfx::IncrementCulledEntityCount(1);
        return;
    }

    gfx::IncrementRenderedEntityCount(1);
    QueueNodeSurfaces();
    if (isSelected && !nodes.empty() && !ui::IsPlayActive() && !gfx::IsInShadowPass() && !gfx::IsInReflectionPass()) DrawNodeGizmos();

    // Apply transparency to alpha channel (0 = visible, 1 = invisible)
    Color drawColor = color;
    drawColor.a = (unsigned char)(color.a * (1.0f - transparency));
    // A union keeps its parts' colours in the mesh (vertex colours multiply the tint), unless it uses one global colour.
    if (ownsProceduralMesh && !useGlobalColor) drawColor = { 255, 255, 255, drawColor.a };
    if (negate) drawColor = { 255, 70, 70, 110 };

    if (hasOwnModel) {
        if (animator) {
            // Once per rendered frame, however many passes draw the object (a
            // vertex buffer takes one update per frame). The clock itself advances in Update().
            const unsigned long long frame = GetRenderFrameIndex();
            if (frame != animFrame) {
                animFrame = frame;
                animator->Skin();
            }
        }
        Matrix rotMat = useRenderRotation
            ? renderRotation
            : MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
        model.transform = gfx::ComposeTRS(size, rotMat, pos);
        // Feed the debug overlay's resource counters. DrawModel binds these
        // materials internally, so this is the last point we can see them; the
        // ids are deduplicated per frame by gfx, so a hundred objects sharing
        // one texture count as one.
        // The object's material over the file's (see ScatteredObject::surface). The shared entry keeps the
        // file's values, so turning useModelMaterial back on restores them.
        auto sharedIt = g_sharedModels.find(sharedModelKey);
        const Model* fileModel = sharedIt != g_sharedModels.end() ? &sharedIt->second.model : nullptr;
        for (int m = 0; m < model.materialCount; ++m) {
            Material& mat = model.materials[m];
            if (!mat.maps) continue;
            if (!useModelMaterial) {
                gfx::ApplySurfaceMaterial(mat, surface);
            } else if (fileModel && m < fileModel->materialCount && fileModel->materials[m].maps) {
                const MaterialMap* src = fileModel->materials[m].maps;
                mat.maps[MATERIAL_MAP_METALNESS].value = src[MATERIAL_MAP_METALNESS].value;
                mat.maps[MATERIAL_MAP_ROUGHNESS].value = src[MATERIAL_MAP_ROUGHNESS].value;
                mat.maps[MATERIAL_MAP_EMISSION].color = src[MATERIAL_MAP_EMISSION].color;
                mat.maps[MATERIAL_MAP_EMISSION].value = src[MATERIAL_MAP_EMISSION].value;
                if (surface.emissiveStrength > 0.0f) {
                    mat.maps[MATERIAL_MAP_EMISSION].color = surface.emissive;
                    mat.maps[MATERIAL_MAP_EMISSION].value = surface.emissiveStrength;
                }
            }
        }
        for (int m = 0; m < model.materialCount; ++m) {
            gfx::NoteMaterialUsed(model.materials[m].shader.id);
            if (model.materials[m].maps) {
                for (int k = 0; k < gfx::kMaterialMapCount; ++k) {
                    gfx::NoteTextureBound(model.materials[m].maps[k].texture.id);
                }
            }
        }
        DrawModel(model, Vector3Zero(), 1.0f, drawColor);
        gfx::IncrementDrawCallCount(1);
        gfx::AddMeshCount(1);
        gfx::AddModelTriangles(model);

        if (isSelected && !ui::IsPlayActive()) {
            // Selection outline: the same transform at 102% scale, drawn as
            // wireframe, then the solid transform is restored.
            const Vector3 outlineScale = { size.x * 1.02f, size.y * 1.02f, size.z * 1.02f };
            model.transform = gfx::ComposeTRS(outlineScale, rotMat, pos);
            rlEnableWireMode();
            DrawModel(model, Vector3Zero(), 1.0f, WHITE);
            rlDisableWireMode();
            model.transform = gfx::ComposeTRS(size, rotMat, pos);
        } else if (isHovered && !ui::IsPlayActive() && !gfx::IsInShadowPass() && gfx::Quality().hoverOutline) {
            gfx::DrawModelOutline(model, model.transform);
        }
        return;
    }

    Matrix rotMat = useRenderRotation
        ? renderRotation
        : MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
    // Opaque shapes are batched into one instanced draw per (shape, texture) while the engine's
    // object loops run; anything else (transparent, no batch active) is drawn right here.
    if (!gfx::QueueShape(shape, GetTexture(), gfx::ComposeTRS(size, rotMat, pos), drawColor, surface)) {
        DrawShapeModel(shape, pos, size, rotMat, drawColor, GetTexture(), false, surface);
        gfx::IncrementDrawCallCount(1);
        gfx::AddMeshCount(1);
        gfx::AddModelTriangles(gfx::GetShapeModel(shape));
    }

    if (isSelected && !ui::IsPlayActive()) {
        DrawShapeModel(shape, pos, size, rotMat, WHITE, GetTexture(), true);
    } else if (isHovered && !ui::IsPlayActive() && !gfx::IsInShadowPass() && gfx::Quality().hoverOutline) {
        const Matrix outlineXf = gfx::ComposeTRS(size, rotMat, pos);
        if (!gfx::QueueShapeOutline(shape, outlineXf)) gfx::DrawModelOutline(gfx::GetShapeModel(shape), outlineXf);
    }
}

void ScatteredObject::QueueNodeSurfaces() const {
    if (nodes.empty() || gfx::IsInShadowPass() || gfx::IsInReflectionPass()) return;
    const Model* mesh = hasOwnModel ? &model : &gfx::GetShapeModel(shape);
    if (mesh->meshCount <= 0) return;
    const Matrix rot = GetRotationMatrix();
    const Matrix xf = gfx::ComposeTRS(size, rot, pos);
    for (const ObjectNode& n : nodes) {
        if (!n.enabled || !n.IsSurfaceNode()) continue;
        gfx::NodeSurface s;
        s.model = mesh;
        s.transform = xf;
        s.size = size;
        float fn[3], fu[3], fv[3];
        NodeFaceAxes(n.face, fn, fu, fv);
        s.faceN = { fn[0], fn[1], fn[2] };
        s.faceU = { fu[0], fu[1], fu[2] };
        s.faceV = { fv[0], fv[1], fv[2] };
        s.tiled = n.type == ObjectNodeType::Texture;
        s.metresPerTile = { n.studsPerTile[0], n.studsPerTile[1] };
        s.offset = { n.offset[0], n.offset[1] };
        s.tint = { (unsigned char)(std::clamp(n.tint[0], 0.0f, 1.0f) * 255.0f + 0.5f), (unsigned char)(std::clamp(n.tint[1], 0.0f, 1.0f) * 255.0f + 0.5f),
                   (unsigned char)(std::clamp(n.tint[2], 0.0f, 1.0f) * 255.0f + 0.5f), (unsigned char)(std::clamp(1.0f - n.transparency, 0.0f, 1.0f) * 255.0f + 0.5f) };
        if (!n.texturePath.empty()) {
            bool pending = false;
            s.texture = textureManager::GetGPUTextureAsync(n.texturePath, pending);   // id 0 while it loads: the checker shows
        }
        gfx::QueueNodeSurface(s);
    }
}

void ScatteredObject::GetNodeEmitter(const ObjectNode& n, Vector3& outPos, Vector3& dir, Vector3& halfU, Vector3& halfV) const {
    const Matrix rot = GetRotationMatrix();
    dir = halfU = halfV = Vector3Zero();
    outPos = pos;
    if (n.type == ObjectNodeType::PointLight) return;
    float fn[3], fu[3], fv[3];
    NodeFaceAxes(n.face, fn, fu, fv);
    // The emitter sits on the chosen face, a hair out so the part itself is not lit edge-on.
    dir = Vector3Normalize(Vector3Transform({ fn[0], fn[1], fn[2] }, rot));
    outPos = Vector3Add(Vector3Transform({ fn[0] * 0.5f, fn[1] * 0.5f, fn[2] * 0.5f }, gfx::ComposeTRS(size, rot, pos)), Vector3Scale(dir, 0.02f));
    if (n.type == ObjectNodeType::SurfaceLight) {
        // Half-extent of the emitting rectangle across the face (a fraction of the face's size).
        halfU = Vector3Transform({ fu[0] * size.x * 0.5f * n.surfaceSize[0], fu[1] * size.y * 0.5f * n.surfaceSize[0], fu[2] * size.z * 0.5f * n.surfaceSize[0] }, rot);
        halfV = Vector3Transform({ fv[0] * size.x * 0.5f * n.surfaceSize[1], fv[1] * size.y * 0.5f * n.surfaceSize[1], fv[2] * size.z * 0.5f * n.surfaceSize[1] }, rot);
    }
}

void ScatteredObject::DrawNodeGizmos() const {
    for (const ObjectNode& n : nodes) {
        if (!n.IsLight() || !n.enabled) continue;
        Vector3 p, d, hu, hv;
        GetNodeEmitter(n, p, d, hu, hv);
        const Color col = { (unsigned char)(255 * n.color[0]), (unsigned char)(255 * n.color[1]), (unsigned char)(255 * n.color[2]), 200 };
        const Color c = { (unsigned char)(120 + col.r / 2), (unsigned char)(120 + col.g / 2), (unsigned char)(120 + col.b / 2), 220 };
        if (n.type == ObjectNodeType::PointLight) {
            DrawCircle3D(p, n.range, { 1, 0, 0 }, 90.0f, c);
            DrawCircle3D(p, n.range, { 0, 1, 0 }, 90.0f, c);
            DrawCircle3D(p, n.range, { 0, 0, 1 }, 90.0f, c);
            continue;
        }
        // Cone (or the surface light's spread): a ring at the far end, joined to the emitter by four lines.
        const float reach = n.range;
        const float radius = reach * tanf(DEG2RAD * fminf(n.angle, 85.0f));
        const Vector3 ref = fabsf(d.y) > 0.9f ? Vector3{ 1, 0, 0 } : Vector3{ 0, 1, 0 };
        const Vector3 right = Vector3Normalize(Vector3CrossProduct(d, ref));
        const Vector3 up = Vector3CrossProduct(right, d);
        const Vector3 end = Vector3Add(p, Vector3Scale(d, reach));
        Vector3 prev = Vector3Add(end, Vector3Scale(right, radius));
        for (int i = 1; i <= 32; ++i) {
            const float a = (float)i / 32.0f * 2.0f * PI;
            const Vector3 q = Vector3Add(end, Vector3Add(Vector3Scale(right, cosf(a) * radius), Vector3Scale(up, sinf(a) * radius)));
            DrawLine3D(prev, q, c);
            prev = q;
        }
        for (int i = 0; i < 4; ++i) {
            const float a = (float)i * 0.5f * PI;
            DrawLine3D(p, Vector3Add(end, Vector3Add(Vector3Scale(right, cosf(a) * radius), Vector3Scale(up, sinf(a) * radius))), c);
        }
        if (n.type == ObjectNodeType::SurfaceLight) {
            const Vector3 a = Vector3Add(p, Vector3Add(hu, hv)), b = Vector3Add(p, Vector3Subtract(hu, hv));
            const Vector3 e = Vector3Subtract(p, Vector3Add(hu, hv)), f = Vector3Add(p, Vector3Subtract(hv, hu));
            DrawLine3D(a, b, c); DrawLine3D(b, e, c); DrawLine3D(e, f, c); DrawLine3D(f, a, c);
        }
    }
}

Matrix ScatteredObject::GetRotationMatrix() const {
    return useRenderRotation
        ? renderRotation
        : MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
}

BoundingBox ScatteredObject::GetBoundingBox() const {
    // Called several times per object per frame (culling, shadow pass, picking), almost always with the
    // transform unchanged, so remember the last answer. pos/size/rotation are plain members edited through
    // pointers all over the editor, so compare the inputs instead of tracking edits.
    const Vector3 key[3] = { pos, size, rotation };
    if (boundsCacheValid && std::memcmp(key, boundsCacheKey, sizeof(key)) == 0) return boundsCache;

    const Matrix rot = MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
    const Vector3 half = { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
    // The box of the rotated cuboid's 8 corners is its centre +/- |R| * half-extents.
    const Vector3 ext = {
        fabsf(rot.m0) * half.x + fabsf(rot.m4) * half.y + fabsf(rot.m8)  * half.z,
        fabsf(rot.m1) * half.x + fabsf(rot.m5) * half.y + fabsf(rot.m9)  * half.z,
        fabsf(rot.m2) * half.x + fabsf(rot.m6) * half.y + fabsf(rot.m10) * half.z
    };

    BoundingBox box;
    box.min = { pos.x - ext.x, pos.y - ext.y, pos.z - ext.z };
    box.max = { pos.x + ext.x, pos.y + ext.y, pos.z + ext.z };

    std::memcpy(boundsCacheKey, key, sizeof(key));
    boundsCache = box;
    boundsCacheValid = true;
    return box;
}

bool ScatteredObject::GetOccluderCorners(Vector3 out[8], float& volume) const {
    // Only solid, fully opaque cuboids: a texture could have see-through parts, a mesh is not a solid box.
    if (hasOwnModel || shape != ShapeType::Cube || transparency > 0.0f || color.a < 255) return false;
    const float smallest = fminf(size.x, fminf(size.y, size.z));
    if (smallest < 1.5f) return false;   // thin or small things hide too little to be worth the cost

    const Matrix rot = useRenderRotation
        ? renderRotation
        : MatrixRotateXYZ({ DEG2RAD * rotation.x, DEG2RAD * rotation.y, DEG2RAD * rotation.z });
    const Vector3 half = { size.x * 0.5f, size.y * 0.5f, size.z * 0.5f };
    for (int i = 0; i < 8; ++i) {
        const Vector3 corner = { (i & 1) ? half.x : -half.x, (i & 2) ? half.y : -half.y, (i & 4) ? half.z : -half.z };
        out[i] = Vector3Add(pos, Vector3Transform(corner, rot));
    }
    volume = size.x * size.y * size.z;
    return true;
}

BoundingBox ModelGroup::GetBounds() const {
    BoundingBox box{};
    bool first = false;
    auto merge = [&](const BoundingBox& m) {
        if (!first) {
            box = m;
            first = true;
            return;
        }
        box.min.x = fminf(box.min.x, m.min.x);
        box.min.y = fminf(box.min.y, m.min.y);
        box.min.z = fminf(box.min.z, m.min.z);
        box.max.x = fmaxf(box.max.x, m.max.x);
        box.max.y = fmaxf(box.max.y, m.max.y);
        box.max.z = fmaxf(box.max.z, m.max.z);
    };
    for (const auto* member : members)
        if (member) merge(member->GetBoundingBox());
    for (const auto* child : children) {
        if (!child) continue;
        BoundingBox c = child->GetBounds();
        if (c.min.x == c.max.x && c.min.y == c.max.y && c.min.z == c.max.z &&
            child->members.empty() && child->children.empty()) continue;
        merge(c);
    }
    if (!first) {
        box.min = { 0.0f, 0.0f, 0.0f };
        box.max = { 0.0f, 0.0f, 0.0f };
    }
    return box;
}

bool ModelGroup::IsDescendantOf(const ModelGroup* other) const {
    for (const ModelGroup* g = parent; g; g = g->parent)
        if (g == other) return true;
    return false;
}

ModelGroup* ModelGroup::Root() {
    ModelGroup* g = this;
    while (g->parent) g = g->parent;
    return g;
}

void ModelGroup::CollectObjects(std::vector<ScatteredObject*>& out) const {
    for (auto* m : members) if (m) out.push_back(m);
    for (const auto* c : children) if (c) c->CollectObjects(out);
}

bool ModelGroup::IsHiddenEffective() const {
    for (const ModelGroup* g = this; g; g = g->parent)
        if (g->hidden) return true;
    return false;
}

bool ModelGroup::IsLockedEffective() const {
    for (const ModelGroup* g = this; g; g = g->parent)
        if (g->locked) return true;
    return false;
}

void ModelGroup::AddChild(ModelGroup* child) {
    if (!child || child == this) return;
    if (child->parent) child->parent->RemoveChild(child);
    child->parent = this;
    children.push_back(child);
}

void ModelGroup::RemoveChild(ModelGroup* child) {
    children.erase(std::remove(children.begin(), children.end(), child), children.end());
    if (child && child->parent == this) child->parent = nullptr;
}

std::vector<std::string> ScatteredObject::AllScripts() const {
    std::vector<std::string> out;
    if (!script.empty()) out.push_back(script);
    out.insert(out.end(), extraScripts.begin(), extraScripts.end());
    return out;
}

bool ScatteredObject::HasScript(const std::string& name) const {
    return script == name || std::find(extraScripts.begin(), extraScripts.end(), name) != extraScripts.end();
}

void ScatteredObject::AddScript(const std::string& name) {
    if (name.empty() || HasScript(name)) return;
    if (script.empty()) script = name;
    else extraScripts.push_back(name);
}

void ScatteredObject::RemoveScript(const std::string& name) {
    if (script == name) {
        script.clear();
        if (!extraScripts.empty()) {
            script = extraScripts.front();
            extraScripts.erase(extraScripts.begin());
        }
        return;
    }
    extraScripts.erase(std::remove(extraScripts.begin(), extraScripts.end(), name), extraScripts.end());
}

bool ScatteredObject::IsHiddenEffective() const {
    return hidden || (parentModel && parentModel->IsHiddenEffective());
}

bool ScatteredObject::IsLockedEffective() const {
    return locked || (parentModel && parentModel->IsLockedEffective());
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

float ScatteredObject::GetYaw() const {
    if (useRenderRotation) return atan2f(renderRotation.m2, renderRotation.m0);
    return -rotation.y * DEG2RAD;
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
    const int accuracy = (int)GetCollisionAccuracy();
    int key = accuracy * 100003 + (int)geometryVersion;
    if (colliderCacheKey == key && colliderCache) return *colliderCache;

    // One collider per (mesh or primitive, accuracy), shared by every object
    // using it: duplicates of a model don't each rebuild or store their own.
    static std::unordered_map<int, std::shared_ptr<const pcoll::Collider>> primitiveColliders;
    std::shared_ptr<const pcoll::Collider>* slot = nullptr;
    auto sharedIt = hasOwnModel ? g_sharedModels.find(sharedModelKey) : g_sharedModels.end();
    if (sharedIt != g_sharedModels.end()) slot = &sharedIt->second.colliders[accuracy];
    else if (!hasOwnModel) slot = &primitiveColliders[(int)shape * 1000 + accuracy];

    if (slot && *slot) {
        colliderCache = *slot;
    } else {
        std::vector<pcoll::Triangle> tris;
        if (hasOwnModel) {
            for (int i = 0; i < model.meshCount; ++i) ExtractMeshTriangles(model.meshes[i], tris);
        } else {
            Model& m = gfx::GetShapeModel(shape);
            for (int i = 0; i < m.meshCount; ++i) ExtractMeshTriangles(m.meshes[i], tris);
        }
        colliderCache = std::make_shared<const pcoll::Collider>(
            pcoll::BuildCollider(GetCollisionAccuracy(), tris));
        if (slot) *slot = colliderCache;
    }
    colliderCacheKey = key;
    return *colliderCache;
}

bool ScatteredObject::IsVisible(const Frustum& frustum) const {
    return frustum.Intersects(GetBoundingBox());
}