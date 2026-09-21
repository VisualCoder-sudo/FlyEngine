#include "../../../include/Engine/Backend/FbxModel.hpp"

#include "ufbx.h"

#include <cstdlib>
#include <cstring>
#include <string>
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

bool LoadFBXIntoModel(const std::string& path, Model& out) {
    out = { 0 };

    ufbx_load_opts opts = { };
    opts.generate_missing_normals = true;
    opts.load_external_files = true;
    opts.ignore_missing_external_files = true;
    // Convert the (typically Z-up) FBX space into raylib's Y-up right-handed
    // coordinate system so meshes import with the "up" axis facing up.
    opts.target_axes = ufbx_axes_right_handed_y_up;

    ufbx_error error;
    ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
    if (!scene) {
        return false;
    }

    // Gather the unique materials referenced by meshes so we can build the
    // model's material table. Per-raylib convention, index 0 is the default
    // material if none is referenced.
    std::vector<bool> usedMat(scene->materials.count + 1, false);
    std::vector<int> ufbxToRL(scene->materials.count, -1); // ufbx material idx -> raylib mesh count slot

    // First pass: count sub-meshes and mark used materials.
    std::vector<int> meshPartCount(scene->materials.count, 0);
    size_t totalRLMeshes = 0;
    for (size_t i = 0; i < scene->meshes.count; ++i) {
        const ufbx_mesh* m = scene->meshes.data[i];
        if (!m || m->vertex_position.exists == false) continue;
        int parts = 0;
        if (m->material_parts.count > 0) {
            for (size_t p = 0; p < m->material_parts.count; ++p) {
                if (m->material_parts.data[p].num_faces == 0) continue;
                ++parts;
            }
        } else {
            parts = 1;
        }
        totalRLMeshes += (parts > 0) ? (size_t)parts : 0;
        if (parts == 0) continue;

        // Mark materials used by this mesh.
        if (m->material_parts.count > 0) {
            for (size_t p = 0; p < m->material_parts.count; ++p) {
                const ufbx_mesh_part& part = m->material_parts.data[p];
                if (part.num_faces == 0) continue;
                // material_parts index into mesh->materials; mesh->materials
                // indices correspond to scene->materials.
                for (size_t fi = 0; fi < part.face_indices.count; ++fi) {
                    uint32_t faceIdx = part.face_indices.data[fi];
                    if (faceIdx < m->face_material.count) {
                        uint32_t matIdx = m->face_material.data[faceIdx];
                        if (matIdx < scene->materials.count) usedMat[matIdx] = true;
                    }
                }
            }
        }
    }

    if (totalRLMeshes == 0) {
        ufbx_free_scene(scene);
        return false;
    }

    // Assign raylib material indices.
    std::vector<int> rlMaterialIndex(scene->materials.count, -1);
    int materialCount = 0;
    for (size_t i = 0; i < scene->materials.count; ++i) {
        if (usedMat[i]) rlMaterialIndex[i] = materialCount++;
    }
    if (materialCount == 0) materialCount = 1; // default material

    out.materialCount = materialCount;
    out.materials = (Material*)calloc(materialCount, sizeof(Material));
    for (int i = 0; i < materialCount; ++i) out.materials[i] = LoadMaterialDefault();

    // Fill material diffuse colors from FBX.
    for (size_t i = 0; i < scene->materials.count; ++i) {
        int rlIdx = rlMaterialIndex[i];
        if (rlIdx < 0) continue;
        const ufbx_material* mat = scene->materials.data[i];
        if (!mat) continue;
        Color c = { 255, 255, 255, 255 };
        const ufbx_material_map& dc = mat->fbx.diffuse_color;
        if (dc.has_value && dc.value_components >= 3) {
            c.r = (unsigned char)(dc.value_vec3.x * 255.0f);
            c.g = (unsigned char)(dc.value_vec3.y * 255.0f);
            c.b = (unsigned char)(dc.value_vec3.z * 255.0f);
        }
        if (out.materials[rlIdx].maps) {
            out.materials[rlIdx].maps[MATERIAL_MAP_DIFFUSE].color = c;
        }
    }

    // Second pass: build sub-meshes.
    std::vector<Mesh> meshes;
    std::vector<int> meshMaterial; // per sub-mesh material index

    for (size_t i = 0; i < scene->meshes.count; ++i) {
        const ufbx_mesh* m = scene->meshes.data[i];
        if (!m || m->vertex_position.exists == false || m->num_vertices == 0) continue;

        // Resolve the node transform that places this mesh in world space, and
        // the skin deformer (if any). Static meshes must be transformed by
        // `geometry_to_world`; skinned meshes by their per-vertex skin matrix
        // (which already folds in the node transform via the fallback). This
        // makes multi-part/scattered rigs import correctly instead of leaving
        // every part piled up in its own local space.
        const ufbx_node* node = (m->instances.count > 0) ? m->instances.data[0] : nullptr;
        const ufbx_matrix geometryToWorld = node ? node->geometry_to_world : ufbx_identity_matrix;
        const ufbx_matrix normalMatrix = ufbx_matrix_for_normals(&geometryToWorld);
        const ufbx_skin_deformer* skin =
            (m->skin_deformers.count > 0) ? m->skin_deformers.data[0] : nullptr;

        // Pre-transform one copy per control vertex, so skinned vertices (which
        // can differ per corner) and node placement are applied a single time.
        std::vector<ufbx_vec3> worldPos(m->num_vertices);
        std::vector<ufbx_vec3> worldNorm(m->num_vertices);
        if (skin) {
            for (size_t v = 0; v < m->num_vertices; ++v) {
                const ufbx_matrix mat = ufbx_get_skin_vertex_matrix(skin, v, &geometryToWorld);
                worldPos[v] = ufbx_transform_position(&mat, m->vertex_position.values.data[v]);
                if (m->vertex_normal.exists)
                    worldNorm[v] = ufbx_transform_direction(&ufbx_matrix_for_normals(&mat),
                                                            m->vertex_normal.values.data[v]);
            }
        } else {
            for (size_t v = 0; v < m->num_vertices; ++v) {
                worldPos[v] = ufbx_transform_position(&geometryToWorld,
                                                      m->vertex_position.values.data[v]);
                if (m->vertex_normal.exists)
                    worldNorm[v] = ufbx_transform_direction(&normalMatrix,
                                                            m->vertex_normal.values.data[v]);
            }
        }

        // If the mesh has multiple material parts, emit one raylib Mesh per
        // part; otherwise a single Mesh covering all faces.
        std::vector<const ufbx_mesh_part*> parts;
        if (m->material_parts.count > 0) {
            for (size_t p = 0; p < m->material_parts.count; ++p) {
                if (m->material_parts.data[p].num_faces == 0) continue;
                parts.push_back(&m->material_parts.data[p]);
            }
        } else {
            parts.push_back(nullptr);
        }

        for (const ufbx_mesh_part* part : parts) {
            // Determine the material for this part.
            int matRL = 0;
            if (part != nullptr && part->num_faces > 0) {
                // Find the material for the part's first face.
                uint32_t f0 = part->face_indices.data[0];
                uint32_t mi = (f0 < m->face_material.count) ? m->face_material.data[f0] : 0;
                if (mi < scene->materials.count && rlMaterialIndex[mi] >= 0)
                    matRL = rlMaterialIndex[mi];
            }

            // Count triangles for this part.
            size_t triCount = 0;
            if (part != nullptr) {
                for (size_t fi = 0; fi < part->face_indices.count; ++fi) {
                    uint32_t fidx = part->face_indices.data[fi];
                    if (fidx >= m->faces.count) continue;
                    triCount += (m->faces.data[fidx].num_indices >= 3) ? (m->faces.data[fidx].num_indices - 2) : 0;
                }
            } else {
                for (size_t f = 0; f < m->faces.count; ++f)
                    triCount += (m->faces.data[f].num_indices >= 3) ? (m->faces.data[f].num_indices - 2) : 0;
            }

            if (triCount == 0) continue;

            const int vertexCount = (int)(triCount * 3);
            Mesh mesh = { 0 };
            mesh.vertexCount = vertexCount;
            mesh.triangleCount = (int)triCount;

            float* verts = (float*)calloc((size_t)vertexCount * 3, sizeof(float));
            float* norms = (float*)calloc((size_t)vertexCount * 3, sizeof(float));
            float* uvs   = (float*)calloc((size_t)vertexCount * 2, sizeof(float));
            float* tans  = (float*)calloc((size_t)vertexCount * 4, sizeof(float));

            int vi = 0; // vertex index into output arrays
            auto emitTriangle = [&](const ufbx_face& face, uint32_t tri) {
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    // Combined corner index across the whole mesh.
                    uint32_t absCorner = (uint32_t)(face.index_begin + tri * 3 + corner);

                    if (m->vertex_position.exists && m->vertex_position.indices.count > absCorner) {
                        uint32_t idx = m->vertex_position.indices.data[absCorner];
                        const ufbx_vec3 p = (idx < worldPos.size()) ? worldPos[idx]
                                                                    : m->vertex_position.values.data[idx];
                        verts[vi * 3 + 0] = (float)p.x;
                        verts[vi * 3 + 1] = (float)p.y;
                        verts[vi * 3 + 2] = (float)p.z;
                    }

                    if (m->vertex_normal.exists && m->vertex_normal.indices.count > absCorner) {
                        uint32_t idx = m->vertex_normal.indices.data[absCorner];
                        const ufbx_vec3 n = (idx < worldNorm.size()) ? worldNorm[idx]
                                                                     : m->vertex_normal.values.data[idx];
                        norms[vi * 3 + 0] = (float)n.x;
                        norms[vi * 3 + 1] = (float)n.y;
                        norms[vi * 3 + 2] = (float)n.z;
                    } else {
                        norms[vi * 3 + 0] = 0.0f;
                        norms[vi * 3 + 1] = 1.0f;
                        norms[vi * 3 + 2] = 0.0f;
                    }

                    if (m->vertex_uv.exists && m->vertex_uv.indices.count > absCorner) {
                        uint32_t idx = m->vertex_uv.indices.data[absCorner];
                        const ufbx_vec2 uv = m->vertex_uv.values.data[idx];
                        uvs[vi * 2 + 0] = (float)uv.x;
                        uvs[vi * 2 + 1] = (float)uv.y;
                    }

                    if (m->vertex_tangent.exists && m->vertex_tangent.indices.count > absCorner) {
                        uint32_t idx = m->vertex_tangent.indices.data[absCorner];
                        const ufbx_vec3 t = ufbx_transform_direction(&normalMatrix,
                                                                     m->vertex_tangent.values.data[idx]);
                        tans[vi * 4 + 0] = (float)t.x;
                        tans[vi * 4 + 1] = (float)t.y;
                        tans[vi * 4 + 2] = (float)t.z;
                        tans[vi * 4 + 3] = 1.0f;
                    }

                    ++vi;
                }
            };

            if (part != nullptr) {
                for (size_t fi = 0; fi < part->face_indices.count; ++fi) {
                    uint32_t fidx = part->face_indices.data[fi];
                    if (fidx >= m->faces.count) continue;
                    const ufbx_face& face = m->faces.data[fidx];
                    if (face.num_indices < 3) continue;
                    uint32_t nt = (uint32_t)(face.num_indices - 2);
                    for (uint32_t tri = 0; tri < nt; ++tri) emitTriangle(face, tri);
                }
            } else {
                for (size_t f = 0; f < m->faces.count; ++f) {
                    const ufbx_face& face = m->faces.data[f];
                    if (face.num_indices < 3) continue;
                    uint32_t nt = (uint32_t)(face.num_indices - 2);
                    for (uint32_t tri = 0; tri < nt; ++tri) emitTriangle(face, tri);
                }
            }

            mesh.vertices = verts;
            mesh.normals = norms;
            mesh.texcoords = uvs;
            if (m->vertex_tangent.exists) mesh.tangents = tans;
            else free(tans);

            meshes.push_back(mesh);
            meshMaterial.push_back(matRL);
        }
    }

    ufbx_free_scene(scene);

    if (meshes.empty()) {
        if (out.materials) {
            for (int i = 0; i < out.materialCount; ++i) UnloadMaterial(out.materials[i]);
            free(out.materials);
        }
        out.materials = nullptr;
        out.materialCount = 0;
        return false;
    }

    out.meshCount = (int)meshes.size();
    out.meshes = (Mesh*)calloc((size_t)out.meshCount, sizeof(Mesh));
    out.meshMaterial = (int*)calloc((size_t)out.meshCount, sizeof(int));
    for (int i = 0; i < out.meshCount; ++i) {
        out.meshes[i] = meshes[i];
        out.meshMaterial[i] = meshMaterial[i];
        UploadMesh(&out.meshes[i], false);
    }

    return true;
}
