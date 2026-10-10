#include "../../../include/Engine/Backend/FbxModel.hpp"

#include "ufbx.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

// Convert an Autodesk FBX file into a raylib Model using the ufbx library.
//
// ufbx (https://github.com/ufbx/ufbx) is MIT-licensed and handles both the
// legacy binary and newer text FBX formats. We extract triangle geometry
// (positions, normals, UVs, tangents) and per-part materials, then hand the
// result to raylib as a multi-material Model.

// True if the given path points to an .fbx file (case-insensitive).
bool IsFBXPath(const std::string& path) {
    if (path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".fbx";
}


// Finds the on-disk file behind an FBX texture: next to the .fbx (the importer
// extracts embedded textures there) by relative path or bare file name, then
// the stored absolute path. Returns an empty string if nothing exists.
static std::string FindFbxTextureFile(const ufbx_texture* tex, const std::filesystem::path& fbxDir) {
    namespace fs = std::filesystem;
    if (!tex) return std::string();
    if (tex->type != UFBX_TEXTURE_FILE && tex->file_textures.count > 0) tex = tex->file_textures.data[0];
    std::vector<fs::path> candidates;
    if (tex->relative_filename.length) {
        fs::path rel = fs::u8path(std::string(tex->relative_filename.data, tex->relative_filename.length));
        candidates.push_back(fbxDir / rel);
        candidates.push_back(fbxDir / rel.filename());
    }
    if (tex->filename.length) {
        fs::path abs = fs::u8path(std::string(tex->filename.data, tex->filename.length));
        candidates.push_back(fbxDir / abs.filename());
        candidates.push_back(abs);
    }
    for (const fs::path& c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec)) return c.generic_string();
    }
    return std::string();
}

namespace {

// One output vertex: position, normal, uv, tangent. Identical corners (same
// vertex referenced by several faces) are merged so meshes are indexed rather
// than three loose vertices per triangle.
struct VertexKey {
    float v[12];
    bool operator==(const VertexKey& o) const { return std::memcmp(v, o.v, sizeof(v)) == 0; }
};
struct VertexKeyHash {
    size_t operator()(const VertexKey& k) const {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(k.v);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(k.v); ++i) { h ^= b[i]; h *= 1099511628211ull; }
        return (size_t)h;
    }
};

// raylib mesh indices are unsigned short, so one mesh holds at most 65535 vertices.
constexpr size_t kMaxChunkVertices = 65535;

struct MeshChunk {
    std::vector<float> pos, nor, uv, tan;
    std::vector<unsigned short> idx;
    std::unordered_map<VertexKey, unsigned short, VertexKeyHash> lookup;
    size_t vertexCount() const { return pos.size() / 3; }
    void clear() { pos.clear(); nor.clear(); uv.clear(); tan.clear(); idx.clear(); lookup.clear(); }
};

template <typename T>
T* CopyToCalloc(const std::vector<T>& src) {
    T* p = (T*)calloc(src.size() ? src.size() : 1, sizeof(T));
    if (!src.empty()) std::memcpy(p, src.data(), src.size() * sizeof(T));
    return p;
}

Mesh ChunkToMesh(const MeshChunk& c, bool withTangents) {
    Mesh mesh = { 0 };
    mesh.vertexCount = (int)c.vertexCount();
    mesh.triangleCount = (int)(c.idx.size() / 3);
    mesh.vertices = CopyToCalloc(c.pos);
    mesh.normals = CopyToCalloc(c.nor);
    mesh.texcoords = CopyToCalloc(c.uv);
    mesh.indices = CopyToCalloc(c.idx);
    if (withTangents) mesh.tangents = CopyToCalloc(c.tan);
    return mesh;
}

} // namespace

void FreeFbxCpu(FbxCpuData& data) {
    for (Mesh& m : data.meshes) {
        free(m.vertices); free(m.normals); free(m.texcoords); free(m.tangents); free(m.indices);
    }
    data.meshes.clear();
    data.meshMaterial.clear();
}

bool LoadFBXCpu(const std::string& path, FbxCpuData& result) {
    result = FbxCpuData{};

    ufbx_load_opts opts = { };
    opts.generate_missing_normals = true;
    opts.load_external_files = true;
    opts.ignore_missing_external_files = true;
    // Convert the (typically Z-up) FBX space into raylib's Y-up right-handed
    // coordinate system so meshes import with the "up" axis facing up.
    opts.target_axes = ufbx_axes_right_handed_y_up;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) return false;

    // Materials actually referenced by faces get compact indices; index 0 is a
    // default material if none are.
    std::vector<bool> usedMat(scene->materials.count + 1, false);
    bool anyGeometry = false;
    for (size_t i = 0; i < scene->meshes.count; ++i) {
        const ufbx_mesh* m = scene->meshes.data[i];
        if (!m || !m->vertex_position.exists || m->num_vertices == 0) continue;
        anyGeometry = true;
        for (size_t p = 0; p < m->material_parts.count; ++p) {
            const ufbx_mesh_part& part = m->material_parts.data[p];
            for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                uint32_t faceIdx = part.face_indices.data[fi];
                if (faceIdx < m->face_material.count) {
                    uint32_t matIdx = m->face_material.data[faceIdx];
                    if (matIdx < scene->materials.count) usedMat[matIdx] = true;
                }
            }
        }
    }
    if (!anyGeometry) { ufbx_free_scene(scene); return false; }

    std::vector<int> rlMaterialIndex(scene->materials.count, -1);
    int materialCount = 0;
    for (size_t i = 0; i < scene->materials.count; ++i) {
        if (usedMat[i]) rlMaterialIndex[i] = materialCount++;
    }
    if (materialCount == 0) materialCount = 1;
    result.materialColors.assign((size_t)materialCount, WHITE);

    const std::filesystem::path fbxDir = std::filesystem::u8path(path).parent_path();
    for (size_t i = 0; i < scene->materials.count; ++i) {
        const int rlIdx = rlMaterialIndex[i];
        if (rlIdx < 0) continue;
        const ufbx_material* mat = scene->materials.data[i];
        if (!mat) continue;
        Color c = WHITE;
        const ufbx_material_map& dc = mat->fbx.diffuse_color;
        if (dc.has_value && dc.value_components >= 3) {
            c.r = (unsigned char)(dc.value_vec3.x * 255.0f);
            c.g = (unsigned char)(dc.value_vec3.y * 255.0f);
            c.b = (unsigned char)(dc.value_vec3.z * 255.0f);
        }
        const ufbx_texture* tex = mat->pbr.base_color.texture;
        if (!tex) tex = mat->fbx.diffuse_color.texture;
        // A textured material must not be tinted by its flat diffuse colour.
        if (tex) c = WHITE;
        result.materialColors[(size_t)rlIdx] = c;
        // Remember the base-colour texture's file (first one found) so the
        // caller can bind it through TextureManager like a user-picked one.
        if (tex && result.diffuseFile.empty()) result.diffuseFile = FindFbxTextureFile(tex, fbxDir);
    }

    for (size_t i = 0; i < scene->meshes.count; ++i) {
        const ufbx_mesh* m = scene->meshes.data[i];
        if (!m || !m->vertex_position.exists || m->num_vertices == 0) continue;

        // Static meshes are placed by `geometry_to_world`; skinned meshes by
        // their per-vertex skin matrix, so multi-part rigs import in place
        // instead of piling up in their own local space.
        const ufbx_node* node = (m->instances.count > 0) ? m->instances.data[0] : nullptr;
        const ufbx_matrix geometryToWorld = node ? node->geometry_to_world : ufbx_identity_matrix;
        const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&geometryToWorld);
        const ufbx_skin_deformer* skin =
            (m->skin_deformers.count > 0) ? m->skin_deformers.data[0] : nullptr;

        std::vector<ufbx_vec3> worldPos(m->num_vertices);
        std::vector<ufbx_vec3> worldNorm(m->num_vertices);
        for (size_t v = 0; v < m->num_vertices; ++v) {
            if (skin) {
                const ufbx_matrix mat = ufbx_get_skin_vertex_matrix(skin, v, &geometryToWorld);
                worldPos[v] = ufbx_transform_position(&mat, m->vertex_position.values.data[v]);
                if (m->vertex_normal.exists) {
                    // Not a temporary: taking the address of a function return
                    // value is an MSVC extension that GCC/Clang reject.
                    const ufbx_matrix normalMat = ufbx_matrix_for_normals(&mat);
                    worldNorm[v] = ufbx_transform_direction(&normalMat, m->vertex_normal.values.data[v]);
                }
            } else {
                worldPos[v] = ufbx_transform_position(&geometryToWorld, m->vertex_position.values.data[v]);
                if (m->vertex_normal.exists)
                    worldNorm[v] = ufbx_transform_direction(&normalMatrix, m->vertex_normal.values.data[v]);
            }
        }

        std::vector<const ufbx_mesh_part*> parts;
        if (m->material_parts.count > 0) {
            for (size_t p = 0; p < m->material_parts.count; ++p) {
                if (m->material_parts.data[p].num_faces != 0) parts.push_back(&m->material_parts.data[p]);
            }
        } else {
            parts.push_back(nullptr);
        }

        const bool hasTangents = m->vertex_tangent.exists;

        auto makeKey = [&](uint32_t absCorner) {
            VertexKey k;
            std::memset(&k, 0, sizeof(k));
            if (m->vertex_position.indices.count > absCorner) {
                const uint32_t idx = m->vertex_position.indices.data[absCorner];
                const ufbx_vec3 p = (idx < worldPos.size()) ? worldPos[idx] : m->vertex_position.values.data[idx];
                k.v[0] = (float)p.x; k.v[1] = (float)p.y; k.v[2] = (float)p.z;
            }
            if (m->vertex_normal.exists && m->vertex_normal.indices.count > absCorner) {
                const uint32_t idx = m->vertex_normal.indices.data[absCorner];
                const ufbx_vec3 n = (idx < worldNorm.size()) ? worldNorm[idx] : m->vertex_normal.values.data[idx];
                k.v[3] = (float)n.x; k.v[4] = (float)n.y; k.v[5] = (float)n.z;
            } else {
                k.v[4] = 1.0f;
            }
            if (m->vertex_uv.exists && m->vertex_uv.indices.count > absCorner) {
                const ufbx_vec2 uv = m->vertex_uv.values.data[m->vertex_uv.indices.data[absCorner]];
                k.v[6] = (float)uv.x;
                k.v[7] = 1.0f - (float)uv.y;   // FBX UV origin is bottom-left
            }
            if (hasTangents && m->vertex_tangent.indices.count > absCorner) {
                const ufbx_vec3 t = ufbx_transform_direction(&normalMatrix,
                    m->vertex_tangent.values.data[m->vertex_tangent.indices.data[absCorner]]);
                k.v[8] = (float)t.x; k.v[9] = (float)t.y; k.v[10] = (float)t.z; k.v[11] = 1.0f;
            }
            return k;
        };

        std::vector<uint32_t> triCorners;
        for (const ufbx_mesh_part* part : parts) {
            int matRL = 0;
            if (part != nullptr) {
                const uint32_t f0 = part->face_indices.data[0];
                const uint32_t mi = (f0 < m->face_material.count) ? m->face_material.data[f0] : 0;
                if (mi < scene->materials.count && rlMaterialIndex[mi] >= 0) matRL = rlMaterialIndex[mi];
            }

            MeshChunk chunk;
            auto flush = [&]() {
                if (chunk.idx.empty()) return;
                result.meshes.push_back(ChunkToMesh(chunk, hasTangents));
                result.meshMaterial.push_back(matRL);
                chunk.clear();
            };
            auto addFace = [&](const ufbx_face& face) {
                if (face.num_indices < 3) return;
                // ufbx handles quads/n-gons, including concave ones; the result
                // is mesh-wide corner indices.
                triCorners.resize((size_t)(face.num_indices - 2) * 3);
                ufbx_triangulate_face(triCorners.data(), triCorners.size(), m, face);
                for (size_t t = 0; t < triCorners.size(); t += 3) {
                    if (chunk.vertexCount() + 3 > kMaxChunkVertices) flush();
                    for (size_t c = 0; c < 3; ++c) {
                        const VertexKey key = makeKey(triCorners[t + c]);
                        auto it = chunk.lookup.find(key);
                        unsigned short index;
                        if (it != chunk.lookup.end()) {
                            index = it->second;
                        } else {
                            index = (unsigned short)chunk.vertexCount();
                            chunk.lookup.emplace(key, index);
                            chunk.pos.insert(chunk.pos.end(), key.v, key.v + 3);
                            chunk.nor.insert(chunk.nor.end(), key.v + 3, key.v + 6);
                            chunk.uv.insert(chunk.uv.end(), key.v + 6, key.v + 8);
                            chunk.tan.insert(chunk.tan.end(), key.v + 8, key.v + 12);
                        }
                        chunk.idx.push_back(index);
                    }
                }
            };

            if (part != nullptr) {
                for (size_t fi = 0; fi < part->face_indices.count; ++fi) {
                    const uint32_t fidx = part->face_indices.data[fi];
                    if (fidx < m->faces.count) addFace(m->faces.data[fidx]);
                }
            } else {
                for (size_t f = 0; f < m->faces.count; ++f) addFace(m->faces.data[f]);
            }
            flush();
        }
    }

    ufbx_free_scene(scene);
    if (result.meshes.empty()) return false;
    return true;
}

bool UploadFBXCpu(FbxCpuData& data, Model& out) {
    out = { 0 };
    if (data.meshes.empty()) return false;

    out.materialCount = (int)data.materialColors.size();
    out.materials = (Material*)calloc((size_t)out.materialCount, sizeof(Material));
    for (int i = 0; i < out.materialCount; ++i) {
        out.materials[i] = LoadMaterialDefault();
        if (out.materials[i].maps) out.materials[i].maps[MATERIAL_MAP_DIFFUSE].color = data.materialColors[(size_t)i];
    }

    out.meshCount = (int)data.meshes.size();
    out.meshes = (Mesh*)calloc((size_t)out.meshCount, sizeof(Mesh));
    out.meshMaterial = (int*)calloc((size_t)out.meshCount, sizeof(int));
    for (int i = 0; i < out.meshCount; ++i) {
        out.meshes[i] = data.meshes[(size_t)i];
        out.meshMaterial[i] = data.meshMaterial[(size_t)i];
        UploadMesh(&out.meshes[i], false);
    }
    data.meshes.clear();   // ownership moved into `out`
    data.meshMaterial.clear();
    return true;
}

bool LoadFBXIntoModel(const std::string& path, Model& out, std::string* diffuseFile) {
    if (diffuseFile) diffuseFile->clear();
    out = { 0 };
    FbxCpuData data;
    if (!LoadFBXCpu(path, data)) return false;
    if (diffuseFile) *diffuseFile = data.diffuseFile;
    return UploadFBXCpu(data, out);
}
