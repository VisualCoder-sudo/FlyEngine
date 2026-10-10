#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../include/Terrain/Water/WaterNoise.hpp"
#include "../../../include/Engine/Backend/ShaderCache.hpp"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <fstream>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <unordered_set>

Camera3D* WaterBody::s_activeCamera = nullptr;
Engine* WaterBody::s_activeEngine = nullptr;
std::vector<WaterBody*> WaterBody::s_instances;

WaterBody::WaterBody(Vector3 pos, Vector3 sz, float height, Color color)
    : position(pos), size(sz), waterHeight(height), baseColor(color) {
    TraceLog(LOG_INFO, "[WaterBody] ctor pos=(%.2f,%.2f,%.2f) size=(%.2f,%.2f,%.2f) instances=%d",
             pos.x, pos.y, pos.z, sz.x, sz.y, sz.z, (int)s_instances.size() + 1);
    s_instances.push_back(this);
    InitializeShader();
    RebuildMesh();
}

WaterBody::~WaterBody() {
    s_instances.erase(std::remove(s_instances.begin(), s_instances.end(), this), s_instances.end());
    ReleaseGpuResources();
    if (rippleTex.id != 0) { UnloadTexture(rippleTex); rippleTex = { 0 }; }
    if (s_instances.empty() && s_spraySprite.id != 0) { UnloadTexture(s_spraySprite); s_spraySprite = { 0 }; }
    // Only unload if we actually loaded custom files (never unload raylib's shared default shader)
    if (customShader && shaderLoaded) UnloadShader(shader);
    shader = { 0 };
    shaderLoaded = false;
    customShader = false;
}

void WaterBody::ReleaseGpuResources() {
    for (auto& [key, chunk] : chunks) {
        ReleaseChunkResources(chunk);
    }
    chunks.clear();

    if (farShellModel.meshes != nullptr) {
        UnloadModel(farShellModel);
    }
    farShellModel = { 0 };
    farShellMesh = { 0 };
    farShellBuilt = false;
}

int64_t WaterBody::ChunkKey(int gx, int gz) {
    return ((int64_t)gx << 32) | ((uint32_t)gz);
}

int WaterBody::GetLodForDistance(float dist) const {
    for (int i = 0; i < LOD_COUNT; i++) {
        if (dist >= LOD_DISTANCES[i]) return i;
    }
    return LOD_COUNT - 1;
}

void WaterBody::ReleaseChunkResources(Chunk& chunk) {
    // The GPU buffers belong to the model's copy of the mesh (UploadMesh ran on
    // chunk.model.meshes[0]); chunk.mesh shares its CPU arrays. Unloading the
    // model frees both. Unloading only chunk.mesh -- whose vaoId was never set --
    // leaked every chunk's vertex/index buffers and the model's arrays.
    if (chunk.model.meshes != nullptr) {
        UnloadModel(chunk.model);
    } else if (chunk.mesh.vertexCount > 0) {
        UnloadMesh(chunk.mesh);
    }
    chunk.mesh = { 0 };
    chunk.model = { 0 };
    chunk.currentLod = -1;
}

void WaterBody::InitializeShader() {
    WaterNoise::Initialize();

    // Compiled into the binary from shaders/water.glsl (sokol-shdc).
    shader = LoadShaderProgram("water");
    customShader = IsShaderValid(shader);
    shaderLoaded = (shader.id != 0);

    wModelLoc = GetShaderLocation(shader, "wModel");
    wViewLoc = GetShaderLocation(shader, "wView");
    wProjLoc = GetShaderLocation(shader, "wProj");
    cameraPosLoc = GetShaderLocation(shader, "cameraPos");
    globalTimeLoc = GetShaderLocation(shader, "globalTime");
    permLoc = GetShaderLocation(shader, "perm");

    waterPosLoc = GetShaderLocation(shader, "waterBodyPosition");
    waterHeightLoc = GetShaderLocation(shader, "waterBodyHeight");
    waterSizeLoc = GetShaderLocation(shader, "waterBodySize");
    baseColorLoc = GetShaderLocation(shader, "waterBodyBaseColor");
    noiseParams1Loc = GetShaderLocation(shader, "waterBodyNoiseParams1");
    noiseParams2Loc = GetShaderLocation(shader, "waterBodyNoiseParams2");
    noiseDirectionLoc = GetShaderLocation(shader, "waterBodyNoiseDirection");
    foamParamsLoc = GetShaderLocation(shader, "waterBodyFoamParams");
    foamColorLoc = GetShaderLocation(shader, "waterBodyFoamColor");
    detailParamsLoc = GetShaderLocation(shader, "waterBodyDetailParams");
    reflVPLoc = GetShaderLocation(shader, "reflViewProj");
    reflTexLoc = GetShaderLocation(shader, "reflectionTex");
    reflParamsLoc = GetShaderLocation(shader, "reflParams");
    chunkFadeLoc = GetShaderLocation(shader, "chunkFade");
    sceneALoc = GetShaderLocation(shader, "sceneA");
    sceneBLoc = GetShaderLocation(shader, "sceneB");
    sceneCLoc = GetShaderLocation(shader, "sceneC");
    objectCountLoc = GetShaderLocation(shader, "objectCount");
    objectPositionsLoc = GetShaderLocation(shader, "objectPositions");
    objectShapesLoc = GetShaderLocation(shader, "objectShapes");
    farRimLoc = GetShaderLocation(shader, "farRimParams");
    rippleParamsLoc = GetShaderLocation(shader, "rippleParams");
    rippleTexLoc = GetShaderLocation(shader, "rippleTexFS");
    rippleTexVSLoc = GetShaderLocation(shader, "rippleTexVS");

    if (reflTexLoc >= 0) {
        int slot = gfx::GetReflectionTextureSlot();
        SetShaderValue(shader, reflTexLoc, &slot, SHADER_UNIFORM_INT);
    }

    // Upload the shared permutation table to the shader once.
    // 512 ints → vertex shader uniform int perm[512].
    if (permLoc >= 0) {
        const unsigned char* raw = WaterNoise::GetPermutationTable();
        int permArray[512];
        for (int i = 0; i < 512; i++) permArray[i] = (int)raw[i];
        SetShaderValueV(shader, permLoc, permArray, SHADER_UNIFORM_INT, 512);
    }
}

void WaterBody::RebuildMesh() {
    ReleaseGpuResources();
    meshDirty = true;
}

void WaterBody::BuildChunkMesh(Chunk& chunk, int resolution, float cellSize, float worldMinX, float worldMaxX, float worldMinZ, float worldMaxZ) {
    int vertCount = (resolution + 1) * (resolution + 1);
    int triCount = resolution * resolution * 2;

    chunk.mesh = { 0 };
    chunk.mesh.vertexCount = vertCount;
    chunk.mesh.triangleCount = triCount;

    chunk.mesh.vertices = (float*)MemAlloc(vertCount * 3 * sizeof(float));
    chunk.mesh.texcoords = (float*)MemAlloc(vertCount * 2 * sizeof(float));
    chunk.mesh.normals = (float*)MemAlloc(vertCount * 3 * sizeof(float));
    chunk.mesh.indices = (unsigned short*)MemAlloc(triCount * 3 * sizeof(unsigned short));

    float chunkWorldX = chunk.gridX * cellSize + cellSize * 0.5f;
    float chunkWorldZ = chunk.gridZ * cellSize + cellSize * 0.5f;
    float halfChunk = cellSize * 0.5f;
    float stepX = cellSize / resolution;
    float stepZ = cellSize / resolution;

    int idx = 0;
    for (int z = 0; z <= resolution; z++) {
        for (int x = 0; x <= resolution; x++) {
            float lx = -halfChunk + x * stepX;
            float lz = -halfChunk + z * stepZ;

            // Clamp to water body bounds
            float worldX = chunkWorldX + lx;
            float worldZ = chunkWorldZ + lz;
            worldX = Clamp(worldX, worldMinX, worldMaxX);
            worldZ = Clamp(worldZ, worldMinZ, worldMaxZ);

            // Store as chunk-local (model matrix translates to chunkWorldX/chunkWorldZ)
            chunk.mesh.vertices[idx * 3 + 0] = worldX - chunkWorldX;
            chunk.mesh.vertices[idx * 3 + 1] = 0.0f;
            chunk.mesh.vertices[idx * 3 + 2] = worldZ - chunkWorldZ;

            // UVs: map to actual water body bounds for texture scaling
            float uvX = (worldX - worldMinX) / (worldMaxX - worldMinX);
            float uvZ = (worldZ - worldMinZ) / (worldMaxZ - worldMinZ);
            chunk.mesh.texcoords[idx * 2 + 0] = Clamp(uvX, 0.0f, 1.0f);
            chunk.mesh.texcoords[idx * 2 + 1] = Clamp(uvZ, 0.0f, 1.0f);

            chunk.mesh.normals[idx * 3 + 0] = 0.0f;
            chunk.mesh.normals[idx * 3 + 1] = 1.0f;
            chunk.mesh.normals[idx * 3 + 2] = 0.0f;
            idx++;
        }
    }

    idx = 0;
    for (int gz = 0; gz < resolution; gz++) {
        for (int gx = 0; gx < resolution; gx++) {
            int a = gz * (resolution + 1) + gx;
            int b = a + 1;
            int c = a + (resolution + 1);
            int d = c + 1;
            chunk.mesh.indices[idx++] = a;
            chunk.mesh.indices[idx++] = c;
            chunk.mesh.indices[idx++] = b;
            chunk.mesh.indices[idx++] = b;
            chunk.mesh.indices[idx++] = c;
            chunk.mesh.indices[idx++] = d;
        }
    }

    chunk.model = LoadModelFromMesh(chunk.mesh);
    ::UploadMesh(&chunk.model.meshes[0], false);
    if (chunk.model.materials != nullptr && shaderLoaded) {
        chunk.model.materials[0].shader = shader;
    }
}

WaterBody::DebugStats WaterBody::GetDebugStats() const {
    DebugStats s;
    s.totalChunks = (int)chunks.size();
    for (auto& [key, chunk] : chunks) {
        if (chunk.currentLod >= 0 && chunk.currentLod < LOD_COUNT) s.lodCounts[chunk.currentLod]++;
    }
    s.farShellBuilt = farShellBuilt && farShellModel.meshes != nullptr;
    s.farShellVertCount = farShellBuilt ? (int)farShellMesh.vertexCount : 0;
    if (s.farShellVertCount > 0) {
        s.farShellResolution = (int)sqrtf((float)s.farShellVertCount) - 1;
    }
    s.horizonDistance = HORIZON_DISTANCE;
    return s;
}

void WaterBody::WriteCoverageMap(const Camera3D& camera, const char* path, int cellsPerAxis) const {
    std::ofstream f(path);
    if (!f) return;

    Vector3 fwd = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    float fwdLen = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
    float cfx = fwdLen > 0.001f ? fwd.x / fwdLen : 0.0f;
    float cfz = fwdLen > 0.001f ? fwd.z / fwdLen : 0.0f;

    const float shellRim = MAX_RENDER_DISTANCE + CHUNK_SIZE * 0.25f;
    const float halfW = size.x * 0.5f;
    const float halfD = size.z * 0.5f;

    f << "# cam=" << camera.position.x << "," << camera.position.y << "," << camera.position.z
      << " fwdXZ=" << cfx << "," << cfz << " cells=" << cellsPerAxis << "\n";
    f << "# '.':void  'v':chunk-behind-cam-culled  'C':chunk-drawn  'S':shell-only  '+':chunk+shell  'x':outside-body\n";
    for (int iz = cellsPerAxis - 1; iz >= 0; iz--) {
        for (int ix = 0; ix < cellsPerAxis; ix++) {
            float wx = position.x - halfW + (ix + 0.5f) * (size.x / (float)cellsPerAxis);
            float wz = position.z - halfD + (iz + 0.5f) * (size.z / (float)cellsPerAxis);
            if (wx < position.x - halfW || wx > position.x + halfW ||
                wz < position.z - halfD || wz > position.z + halfD) {
                f << 'x';
                continue;
            }
            int gx = (int)floorf(wx / CHUNK_SIZE);
            int gz = (int)floorf(wz / CHUNK_SIZE);
            bool hasChunk = chunks.count(ChunkKey(gx, gz)) > 0;
            float dx = wx - camera.position.x;
            float dz = wz - camera.position.z;
            float d2 = dx * dx + dz * dz;
            bool shellCovers = d2 > shellRim * shellRim;
            // Mirrors the fixed Draw() cull: a chunk is skipped ONLY when it is
            // behind the camera AND the far shell still covers it (outside the
            // rim) - near-camera cells must never be left unbacked.
            bool behindCam = dx * cfx + dz * cfz < -CHUNK_SIZE;
            bool culled = hasChunk && behindCam && shellCovers;
            char c;
            if (!hasChunk && !shellCovers) c = '.';
            else if (culled && !shellCovers) c = 'v';
            else if (hasChunk && !culled && !shellCovers) c = 'C';
            else if (hasChunk && !culled && shellCovers) c = '+';
            else if (!hasChunk && shellCovers) c = 'S';
            else c = '?';
            f << c;
        }
        f << "\n";
    }
}

void WaterBody::BuildFarShell() {
    if (farShellModel.meshes != nullptr) {
        UnloadModel(farShellModel);
        farShellModel = { 0 };
    }
    farShellMesh = { 0 };

    // ~50 m cells, 16..192 per side so a 9 km ocean stays a single cheap draw
    // without ballooning vertex count (192² = 37k verts is nothing next to the
    // ~2.8k chunk meshes near the camera).
    float maxDim = fmaxf(size.x, size.z);
    int resolution = (int)ceilf(maxDim / 50.0f);
    resolution = Clamp(resolution, 16, 192);

    int vertCount = (resolution + 1) * (resolution + 1);
    int triCount = resolution * resolution * 2;

    farShellMesh.vertexCount = vertCount;
    farShellMesh.triangleCount = triCount;
    farShellMesh.vertices = (float*)MemAlloc(vertCount * 3 * sizeof(float));
    farShellMesh.texcoords = (float*)MemAlloc(vertCount * 2 * sizeof(float));
    farShellMesh.normals = (float*)MemAlloc(vertCount * 3 * sizeof(float));
    farShellMesh.indices = (unsigned short*)MemAlloc(triCount * 3 * sizeof(unsigned short));

    float halfW = size.x * 0.5f;
    float halfD = size.z * 0.5f;
    float stepX = size.x / resolution;
    float stepZ = size.z / resolution;

    // Vertices are WORLD space (y = 0; the vertex shader adds waves and translates
    // by the water height), so the shell is drawn with wModel = identity.
    int idx = 0;
    for (int z = 0; z <= resolution; z++) {
        for (int x = 0; x <= resolution; x++) {
            float wx = position.x - halfW + x * stepX;
            float wz = position.z - halfD + z * stepZ;
            farShellMesh.vertices[idx * 3 + 0] = wx;
            farShellMesh.vertices[idx * 3 + 1] = 0.0f;
            farShellMesh.vertices[idx * 3 + 2] = wz;
            farShellMesh.texcoords[idx * 2 + 0] = (float)x / resolution;
            farShellMesh.texcoords[idx * 2 + 1] = (float)z / resolution;
            farShellMesh.normals[idx * 3 + 0] = 0.0f;
            farShellMesh.normals[idx * 3 + 1] = 1.0f;
            farShellMesh.normals[idx * 3 + 2] = 0.0f;
            idx++;
        }
    }

    idx = 0;
    for (int gz = 0; gz < resolution; gz++) {
        for (int gx = 0; gx < resolution; gx++) {
            int a = gz * (resolution + 1) + gx;
            int b = a + 1;
            int c = a + (resolution + 1);
            int d = c + 1;
            farShellMesh.indices[idx++] = a;
            farShellMesh.indices[idx++] = c;
            farShellMesh.indices[idx++] = b;
            farShellMesh.indices[idx++] = b;
            farShellMesh.indices[idx++] = c;
            farShellMesh.indices[idx++] = d;
        }
    }

    farShellModel = LoadModelFromMesh(farShellMesh);
    ::UploadMesh(&farShellModel.meshes[0], false);
    if (farShellModel.materials != nullptr && shaderLoaded) {
        farShellModel.materials[0].shader = shader;
    }
    farShellBuilt = true;
    lastShellSize = size;
    lastShellPos = position;
}

bool FlyMeshBuffersValid(Mesh mesh); // rl_models.cpp

void WaterBody::UpdateChunks(const Camera3D& camera) {
    float dt = GetFrameTime();
    if (dt <= 0.0f || dt > 0.1f) dt = 1.0f / 60.0f;

    float halfW = size.x * 0.5f;
    float halfD = size.z * 0.5f;
    int gridMinX = (int)floorf((position.x - halfW) / CHUNK_SIZE);
    int gridMaxX = (int)ceilf((position.x + halfW) / CHUNK_SIZE);
    int gridMinZ = (int)floorf((position.z - halfD) / CHUNK_SIZE);
    int gridMaxZ = (int)ceilf((position.z + halfD) / CHUNK_SIZE);

    // Water body world bounds for clamping chunk meshes
    float worldMinX = position.x - halfW;
    float worldMaxX = position.x + halfW;
    float worldMinZ = position.z - halfD;
    float worldMaxZ = position.z + halfD;

    // 1. Build set of chunk keys that should exist
    std::unordered_set<int64_t> activeKeys;

    // Build meshes gradually (max per frame) so a fast camera pan can't spawn
    // thousands of meshes in one frame and hitch. NO direction gating: the near
    // disk (20 m cells) is a full ring around the camera. Everything beyond it is
    // covered by the full-body far shell (see BuildFarShell - a single always-
    // present mesh from the disk edge out to the water body boundary), so there is
    // exactly one seam (disk → shell) and no band of water can ever be absent: the
    // shell is tied to the body, not to chunk streaming.
    constexpr int MAX_BUILDS_PER_FRAME = 96;
    constexpr int MAX_ACTIVE_CHUNKS = 3500;

    // Prewarm: a teleport (large camera displacement) or a freshly-cleared map
    // (body move / rebuild / load) re-covers the near disk in a couple of frames.
    // New chunks are created fully opaque, so coverage never waits on the fade.
    float camMoveX = camera.position.x - lastChunkCamXZ.x;
    float camMoveZ = camera.position.z - lastChunkCamXZ.y;
    bool prewarm = chunks.empty() || lastChunkCamXZ.x == 1e30f ||
                   (camMoveX * camMoveX + camMoveZ * camMoveZ) > 200.0f * 200.0f;
    int buildBudget = prewarm ? 600 : MAX_BUILDS_PER_FRAME;
    int buildsThisFrame = 0;

    // Near disk scan: fine 20 m grid out to MAX_RENDER_DISTANCE. Scanning the
    // entire grid of a huge water body every frame (9100×9100 → 456×456 ≈ 208k
    // cells) dominates the frame; the far shell owns everything beyond the disk.
    float nearScan = MAX_RENDER_DISTANCE + CHUNK_SIZE;
    int scanMinX = (int)floorf((camera.position.x - nearScan) / CHUNK_SIZE);
    int scanMaxX = (int)ceilf((camera.position.x + nearScan) / CHUNK_SIZE);
    int scanMinZ = (int)floorf((camera.position.z - nearScan) / CHUNK_SIZE);
    int scanMaxZ = (int)ceilf((camera.position.z + nearScan) / CHUNK_SIZE);
    if (scanMinX < gridMinX) scanMinX = gridMinX;
    if (scanMaxX > gridMaxX) scanMaxX = gridMaxX;
    if (scanMinZ < gridMinZ) scanMinZ = gridMinZ;
    if (scanMaxZ > gridMaxZ) scanMaxZ = gridMaxZ;

    for (int gz = scanMinZ; gz < scanMaxZ; gz++) {
        for (int gx = scanMinX; gx < scanMaxX; gx++) {
            float cx = gx * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
            float cz = gz * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
            float toChunkX = cx - camera.position.x;
            float toChunkZ = cz - camera.position.z;
            float dist = sqrtf(toChunkX * toChunkX + toChunkZ * toChunkZ);
            // Build one CHUNK_SIZE past the nominal radius so the disk overlaps the
            // far-shell's rim-fade band (r 350..370) instead of leaving a hollow
            // seam of cells the shell hasn't clipped in yet.
            if (dist > MAX_RENDER_DISTANCE + CHUNK_SIZE) continue;

            int newLod = GetLodForDistance(dist);
            // A cell whose center lands exactly on the 350 m boundary returns LOD 0
            // ("too far"); clamping to 1 keeps it rendered, or a gap opens where the
            // disk edge meets the far shell.
            if (newLod == 0) newLod = 1;

            int64_t key = ChunkKey(gx, gz);
            activeKeys.insert(key);

            auto it = chunks.find(key);
            if (it == chunks.end()) {
                // New chunk - created fully opaque so coverage never lags invisibly.
                if (buildsThisFrame >= buildBudget || (int)chunks.size() >= MAX_ACTIVE_CHUNKS) continue;
                Chunk& chunk = chunks[key];
                chunk.gridX = gx;
                chunk.gridZ = gz;
                chunk.cellSize = CHUNK_SIZE;
                chunk.fade = 1.0f;
                chunk.lodDist = dist;
                BuildChunkMesh(chunk, LOD_RESOLUTIONS[newLod], CHUNK_SIZE, worldMinX, worldMaxX, worldMinZ, worldMaxZ);
                chunk.currentLod = newLod;
                buildsThisFrame++;
            } else {
                // Existing chunk - update LOD if the camera has clearly crossed a
                // LOD boundary. The hysteresis margin (24 m) stops panning along a
                // boundary from rebuilding the same mesh every frame, which is what
                // made the old all-directions version feel laggy.
                Chunk& chunk = it->second;
                if (newLod != chunk.currentLod) {
                    const float margin = 24.0f;
                    bool crossed = (dist < chunk.lodDist - margin) || (dist > chunk.lodDist + margin);
                    if (crossed && buildsThisFrame < buildBudget) {
                        ReleaseChunkResources(chunk);
                        BuildChunkMesh(chunk, LOD_RESOLUTIONS[newLod], CHUNK_SIZE, worldMinX, worldMaxX, worldMinZ, worldMaxZ);
                        chunk.currentLod = newLod;
                        chunk.lodDist = dist;
                        buildsThisFrame++;
                    }
                }
            }
        }
    }

    lastChunkCamXZ = { camera.position.x, camera.position.z };

    // Self-heal: a chunk whose GPU buffers failed to create is never drawn (it
    // shows as an unrendered patch). Rebuild those, a few per frame.
    {
        int healed = 0;
        for (auto& [key, chunk] : chunks) {
            if (healed >= 8) break;
            if (chunk.model.meshes == nullptr || chunk.currentLod < 0) continue;
            if (FlyMeshBuffersValid(chunk.model.meshes[0])) continue;
            TraceLog(LOG_WARNING, "[WaterBody] chunk (%d,%d) had invalid GPU buffers, rebuilding", chunk.gridX, chunk.gridZ);
            const int lod = std::max(1, std::min(chunk.currentLod, LOD_COUNT - 1));
            ReleaseChunkResources(chunk);
            BuildChunkMesh(chunk, LOD_RESOLUTIONS[lod], CHUNK_SIZE, worldMinX, worldMaxX, worldMinZ, worldMaxZ);
            chunk.currentLod = lod;
            ++healed;
        }
    }

    // 2. Fade active chunks up, inactive chunks down
    for (auto& [key, chunk] : chunks) {
        if (activeKeys.count(key)) {
            chunk.fade = fminf(chunk.fade + dt * FADE_SPEED, 1.0f);
        } else {
            chunk.fade = fmaxf(chunk.fade - dt * FADE_SPEED, 0.0f);
        }
    }

    // 3. Erase chunks fully faded out
    for (auto it = chunks.begin(); it != chunks.end(); ) {
        if (it->second.fade <= 0.0f) {
            ReleaseChunkResources(it->second);
            it = chunks.erase(it);
        } else {
            ++it;
        }
    }
}

void WaterBody::Update(float dt) {
    (void)dt;
}

void WaterBody::UpdateShaderUniforms(const Camera3D& camera, float globalTime) {
    if (!shaderLoaded) return;

    // wModel is set per-chunk in Draw(). Only set view/proj once.
    if (wViewLoc >= 0) {
        SetShaderValueMatrix(shader, wViewLoc, rlGetMatrixModelview());
    }
    if (wProjLoc >= 0) {
        SetShaderValueMatrix(shader, wProjLoc, rlGetMatrixProjection());
    }
    SetShaderValue(shader, cameraPosLoc, &camera.position, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, globalTimeLoc, &globalTime, SHADER_UNIFORM_FLOAT);

    Vector3 waterPos = position;
    SetShaderValue(shader, waterPosLoc, &waterPos, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader, waterHeightLoc, &waterHeight, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader, waterSizeLoc, &size, SHADER_UNIFORM_VEC3);

    Vector4 baseColorNorm = { baseColor.r / 255.0f, baseColor.g / 255.0f, baseColor.b / 255.0f, baseColor.a / 255.0f };
    SetShaderValue(shader, baseColorLoc, &baseColorNorm, SHADER_UNIFORM_VEC4);

    Vector4 noiseParams1 = { noise.amplitude, noise.frequency, noise.speed, (float)noise.octaves };
    SetShaderValue(shader, noiseParams1Loc, &noiseParams1, SHADER_UNIFORM_VEC4);

    Vector4 noiseParams2 = { noise.persistence, noise.lacunarity, (float)noise.seed, globalTime };
    SetShaderValue(shader, noiseParams2Loc, &noiseParams2, SHADER_UNIFORM_VEC4);

    SetShaderValue(shader, noiseDirectionLoc, &noise.direction, SHADER_UNIFORM_VEC2);

    Vector4 foamParams = { foam.intensity, foam.scale, foam.threshold, 0.0f };
    SetShaderValue(shader, foamParamsLoc, &foamParams, SHADER_UNIFORM_VEC4);

    Vector3 foamColorNorm = { foam.color.r / 255.0f, foam.color.g / 255.0f, foam.color.b / 255.0f };
    SetShaderValue(shader, foamColorLoc, &foamColorNorm, SHADER_UNIFORM_VEC3);

    Vector4 detailParams = { detail.intensity, detail.scale, detail.speed, 0.0f };
    SetShaderValue(shader, detailParamsLoc, &detailParams, SHADER_UNIFORM_VEC4);

    // The scene's lighting (Explorer > Lighting, time of day, weather): the sun, the ambient light, the sky and the fog.
    {
        const Vector3 toSun = Vector3Negate(gfx::SunDirection());
        const Vector3 sun = gfx::SunRadianceShaded();
        const Vector3 a3 = gfx::AmbientSky();
        const Vector3 sky = gfx::WaterSky();
        const Vector4 sceneA = { toSun.x, toSun.y, toSun.z, 0.2126f * a3.x + 0.7152f * a3.y + 0.0722f * a3.z };
        const Vector4 sceneB = { sun.x, sun.y, sun.z, 0.0f };
        const Vector4 sceneC = { sky.x, sky.y, sky.z, 0.0f };
        if (sceneALoc >= 0) SetShaderValue(shader, sceneALoc, &sceneA, SHADER_UNIFORM_VEC4);
        if (sceneBLoc >= 0) SetShaderValue(shader, sceneBLoc, &sceneB, SHADER_UNIFORM_VEC4);
        if (sceneCLoc >= 0) SetShaderValue(shader, sceneCLoc, &sceneC, SHADER_UNIFORM_VEC4);
    }

    // Planar reflection: mirrored camera view-proj, strength/distortion params, and
    // the reflection texture (bound to a fixed slot so DrawModel can't overwrite it).
    if (reflVPLoc >= 0) {
        SetShaderValueMatrix(shader, reflVPLoc, gfx::GetReflectionViewProj());
    }
    if (reflParamsLoc >= 0) {
        float enabled = gfx::IsReflectionsEnabled() ? 1.0f : 0.0f;
        // distanceFade is a 0..1 knob; scale so the planar reflection falls off
        // symmetrically with fragment distance (fragDist, meters) - full fade at
        // ~125 m for the default 0.4. The short radius keeps mirrored detail only
        // on water near the camera; the far ocean settles to one uniform ambient,
        // so the reflection can't form a camera-following center-vs-sides band.
        Vector4 reflParams = { reflection.strength, reflection.distortion, reflection.distanceFade * 0.02f, enabled };
        SetShaderValue(shader, reflParamsLoc, &reflParams, SHADER_UNIFORM_VEC4);
    }
    BindRippleTexture();
    if (reflTexLoc >= 0 && gfx::IsReflectionsEnabled()) {
        Texture2D reflTex = gfx::GetReflectionTarget().texture;
        if (reflTex.id > 0) {
            rlActiveTextureSlot(gfx::GetReflectionTextureSlot());
            rlEnableTexture(reflTex.id);
        }
    }
}

void WaterBody::Draw() {
    if (gfx::IsInShadowPass()) return;

    if (!ui::IsPlayActive()) {
        DrawPlane(
            { position.x, waterHeight, position.z },
            { size.x, size.z },
            baseColor
        );
        return;
    }

    if (!shaderLoaded) return;

    Camera3D camera = s_activeCamera ? *s_activeCamera : Camera3D{};
    float globalTime = (float)GetTime();

    // Dynamic ripples (wakes/splashes): follow the camera, advance, upload.
    RecentreRipples(camera.position.x, camera.position.z);
    StepRipples(GetFrameTime());
    UploadRippleTexture();

    UpdateChunks(camera);

    // Rebuild the far shell if the water body moved or resized since last frame
    if (!farShellBuilt || lastShellSize.x != size.x || lastShellSize.z != size.z ||
        lastShellPos.x != position.x || lastShellPos.z != position.z) {
        BuildFarShell();
    }

    if (chunks.empty()) return;

    // Extract frustum for culling
    Frustum frustum = Frustum::ExtractCurrent();

    gfx::IncrementRenderedEntityCount(0);
    gfx::IncrementCulledEntityCount(0);

    BeginShaderMode(shader);
    UpdateShaderUniforms(camera, globalTime);

    // Gather nearby objects for proximity foam
    const int MAX_FOAM_OBJECTS = 16;
    float objData[MAX_FOAM_OBJECTS * 4];
    float objShape[MAX_FOAM_OBJECTS * 4]; // half X, half Z, yaw, speed factor
    int objCount = 0;

    if (s_activeEngine && objectCountLoc >= 0 && objectPositionsLoc >= 0) {
        float halfW = size.x * 0.5f + 2.0f;
        float halfD = size.z * 0.5f + 2.0f;

        for (auto& entity : s_activeEngine->GetEntities()) {
            if (objCount >= MAX_FOAM_OBJECTS) break;
            ScatteredObject* obj = dynamic_cast<ScatteredObject*>(entity.get());
            if (!obj) continue;

            Vector3 op = *obj->GetPosPtr();
            Vector3 os = *obj->GetSizePtr();

            if (op.x + os.x * 0.5f < position.x - halfW ||
                op.x - os.x * 0.5f > position.x + halfW ||
                op.z + os.z * 0.5f < position.z - halfD ||
                op.z - os.z * 0.5f > position.z + halfD) continue;

            objData[objCount * 4 + 0] = op.x;
            objData[objCount * 4 + 1] = op.y - os.y * 0.5f; // bottom of the object's
                                                              // bounding box, so contact
                                                              // detection works for any
                                                              // height/shape, not just
                                                              // objects centered at the
                                                              // water surface.
            objData[objCount * 4 + 2] = op.z;
            objData[objCount * 4 + 3] = fmaxf(os.x, os.z) * 0.5f;
            {
                // Outline for the foam ring: the real footprint, not a circle round the centre.
                const Vector3 ov = obj->GetVelocity();
                const float sp = sqrtf(ov.x * ov.x + ov.z * ov.z);
                const float t = fminf(fmaxf((sp - 0.5f) / 3.5f, 0.0f), 1.0f);
                objShape[objCount * 4 + 0] = os.x * 0.5f;
                objShape[objCount * 4 + 1] = os.z * 0.5f;
                objShape[objCount * 4 + 2] = obj->GetYaw();
                objShape[objCount * 4 + 3] = 0.12f + 0.88f * t * t * (3.0f - 2.0f * t); // calm bodies: faint contact foam only
            }
            objCount++;
        }

        SetShaderValue(shader, objectCountLoc, &objCount, SHADER_UNIFORM_INT);
        SetShaderValueV(shader, objectPositionsLoc, objData, SHADER_UNIFORM_VEC4, objCount);
        if (objectShapesLoc >= 0) SetShaderValueV(shader, objectShapesLoc, objShape, SHADER_UNIFORM_VEC4, objCount);
    }

    // Far shell: covers the water body from the edge of the detailed chunk disk
    // all the way to the body boundary, closing the "unloaded" gap on huge oceans.
    // Vertices are world XZ at y = 0, so the model matrix lifts them to the actual
    // water surface (y = 0 would bury the shell below the chunks - the bug that
    // made the far ocean read as an unloaded void); the rim clip (farRimParams)
    // keeps it off the near disk where chunks already render, so the two layers
    // never double-blend.
    if (farShellBuilt && farShellModel.meshes != nullptr && frustum.Intersects(GetBoundingBox())) {
        gfx::IncrementRenderedEntityCount(1);
        SetShaderValueMatrix(shader, wModelLoc, MatrixTranslate(0.0f, waterHeight, 0.0f));
        if (chunkFadeLoc >= 0) {
            float one = 1.0f;
            SetShaderValue(shader, chunkFadeLoc, &one, SHADER_UNIFORM_FLOAT);
        }
        if (farRimLoc >= 0) {
            // Shell starts just inside the disk edge (MAX_RENDER_DISTANCE) and owns
            // everything beyond, so the disk → shell transition is never a gap.
            Vector2 rim = { MAX_RENDER_DISTANCE + CHUNK_SIZE * 0.25f, CHUNK_SIZE * 0.25f };
            SetShaderValue(shader, farRimLoc, &rim, SHADER_UNIFORM_VEC2);
        }
        DrawModel(farShellModel, { 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
        gfx::IncrementDrawCallCount(1);
        gfx::AddMeshCount(1);
    }

    // Chunks render inside the disk - disable the shell's rim clip for them.
    if (farRimLoc >= 0) {
        Vector2 rimOff = { 0.0f, 0.0f };
        SetShaderValue(shader, farRimLoc, &rimOff, SHADER_UNIFORM_VEC2);
    }

    // Camera forward in XZ for behind-camera culling
    Vector3 camForward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    float camForwardXZ_len = sqrtf(camForward.x * camForward.x + camForward.z * camForward.z);
    float cfx = camForwardXZ_len > 0.001f ? camForward.x / camForwardXZ_len : 0.0f;
    float cfz = camForwardXZ_len > 0.001f ? camForward.z / camForwardXZ_len : 0.0f;

    for (auto& [key, chunk] : chunks) {
        if (chunk.model.meshes == nullptr || chunk.model.materials == nullptr) continue;

        float cellHalf = chunk.cellSize * 0.5f;
        float cx = chunk.gridX * chunk.cellSize + cellHalf;
        float cz = chunk.gridZ * chunk.cellSize + cellHalf;
        float toChunkX = cx - camera.position.x;
        float toChunkZ = cz - camera.position.z;

        // Frustum culling for chunks
        BoundingBox chunkBounds;
        chunkBounds.min = { cx - cellHalf, waterHeight - 10.0f, cz - cellHalf };
        chunkBounds.max = { cx + cellHalf, waterHeight + 10.0f, cz + cellHalf };
        if (!frustum.Intersects(chunkBounds)) {
            gfx::IncrementCulledEntityCount(1);
            continue;
        }
        gfx::IncrementRenderedEntityCount(1);

        // Cull chunks behind the camera - BUT only where the far shell already
        // covers them (outside its rim, r > MAX_RENDER_DISTANCE + CHUNK_SIZE*0.25).
        // Culling near cells the shell clips would leave a void behind the camera
        // (the pixelated "unloaded square" around the body center): within the
        // rim circle NOTHING else renders there.
        float rimCover = MAX_RENDER_DISTANCE + CHUNK_SIZE * 0.25f;
        float toChunkD2 = toChunkX * toChunkX + toChunkZ * toChunkZ;
        if (toChunkX * cfx + toChunkZ * cfz < -chunk.cellSize && toChunkD2 > rimCover * rimCover) continue;

        Matrix modelMat = MatrixTranslate(cx, waterHeight, cz);
        SetShaderValueMatrix(shader, wModelLoc, modelMat);
        if (chunkFadeLoc >= 0) {
            SetShaderValue(shader, chunkFadeLoc, &chunk.fade, SHADER_UNIFORM_FLOAT);
        }

        DrawModel(chunk.model, { 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
        gfx::IncrementDrawCallCount(1);
        gfx::AddMeshCount(1);
    }

    EndShaderMode();

    // Spray droplets from splashes and bows, drawn over the water.
    StepSpray(GetFrameTime());
    DrawSpray(camera);
}

void WaterBody::DrawOverlay3D() {
    if (!isSelected) return;

    if (showWireframe) {
        BoundingBox box = GetBoundingBox();
        DrawBoundingBox(box, GREEN);
    }

    if (showGrid) {
        for (auto& [key, chunk] : chunks) {
            if (chunk.mesh.vertices == nullptr) continue;
            float cellHalf = chunk.cellSize * 0.5f;
            float cx = chunk.gridX * chunk.cellSize + cellHalf;
            float cz = chunk.gridZ * chunk.cellSize + cellHalf;
            for (int i = 0; i < chunk.mesh.triangleCount * 3; i += 3) {
                unsigned short a = chunk.mesh.indices[i];
                unsigned short b = chunk.mesh.indices[i + 1];
                unsigned short c = chunk.mesh.indices[i + 2];
                Vector3 v1 = { chunk.mesh.vertices[a * 3] + cx, waterHeight, chunk.mesh.vertices[a * 3 + 2] + cz };
                Vector3 v2 = { chunk.mesh.vertices[b * 3] + cx, waterHeight, chunk.mesh.vertices[b * 3 + 2] + cz };
                Vector3 v3 = { chunk.mesh.vertices[c * 3] + cx, waterHeight, chunk.mesh.vertices[c * 3 + 2] + cz };
                DrawTriangle3D(v1, v2, v3, Fade(BLUE, 0.1f));
                DrawLine3D(v1, v2, Fade(BLUE, 0.3f));
                DrawLine3D(v2, v3, Fade(BLUE, 0.3f));
                DrawLine3D(v3, v1, Fade(BLUE, 0.3f));
            }
        }
    }
}

float WaterBody::GetHeightAt(float x, float z, float rippleWeight) const {
    float lx = x - position.x;
    float lz = z - position.z;

    if (fabsf(lx) > size.x * 0.5f || fabsf(lz) > size.z * 0.5f) {
        return waterHeight;
    }

    // Match GPU vertex shader exactly: absolute world XZ (not entity-local -
    // the GPU samples worldPos post-wModel, which is already in world space),
    // time as the Y dimension for organic evolution, plus directional flow
    // drift (noise.direction) so buoyancy stays in sync with the rendered
    // wave surface.
    float time = GetTime() * noise.speed;
    float timeY = time * 0.5f;
    float flowX = noise.direction.x * time;
    float flowZ = noise.direction.y * time;

    float nx = (x - flowX) * noise.frequency;
    float ny = timeY;
    float nz = (z - flowZ) * noise.frequency;

    float n = WaterNoise::FBM3D(nx, ny, nz,
                                 noise.octaves, noise.persistence, noise.lacunarity,
                                 noise.seed);
    return waterHeight + n * noise.amplitude + (rippleWeight > 0.0f ? rippleWeight * GetRippleHeightAt(x, z) : 0.0f);
}

BoundingBox WaterBody::GetBoundingBox() const {
    float halfW = size.x * 0.5f;
    float halfD = size.z * 0.5f;
    float maxWave = noise.amplitude * 2.0f;
    return {
        { position.x - halfW, waterHeight - maxWave, position.z - halfD },
        { position.x + halfW, waterHeight + maxWave, position.z + halfD }
    };
}

bool WaterBody::IntersectsXZ(const BoundingBox& box) const {
    float halfW = size.x * 0.5f;
    float halfD = size.z * 0.5f;
    return !(box.max.x < position.x - halfW || box.min.x > position.x + halfW ||
             box.max.z < position.z - halfD || box.min.z > position.z + halfD);
}

bool WaterBody::IsVisible(const Frustum& frustum) const {
    // WaterBody is visible if its bounds intersect the frustum
    return frustum.Intersects(GetBoundingBox());
}

bool WaterBody::SaveToFile(const std::string& path) const {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;

    auto writeVec3 = [&](Vector3 v) { file.write((char*)&v, sizeof(Vector3)); };
    auto writeColor = [&](Color c) { file.write((char*)&c, sizeof(Color)); };
    auto writeStr = [&](const std::string& s) {
        uint32_t len = s.size();
        file.write((char*)&len, sizeof(len));
        file.write(s.data(), len);
    };

    writeVec3(position);
    writeVec3(size);
    writeVec3(rotation);
    writeVec3(origin);
    file.write((char*)&waterHeight, sizeof(float));
    writeColor(baseColor);
    file.write((char*)&transparency, sizeof(float));
    writeStr(name);

    file.write((char*)&noise, sizeof(NoiseParams));
    file.write((char*)&foam, sizeof(FoamParams));
    file.write((char*)&grid, sizeof(GridParams));

    return file.good();
}

bool WaterBody::LoadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    auto readVec3 = [&](Vector3& v) { file.read((char*)&v, sizeof(Vector3)); };
    auto readColor = [&](Color& c) { file.read((char*)&c, sizeof(Color)); };
    auto readStr = [&](std::string& s) {
        uint32_t len;
        file.read((char*)&len, sizeof(len));
        s.resize(len);
        file.read(&s[0], len);
    };

    readVec3(position);
    readVec3(size);
    readVec3(rotation);
    readVec3(origin);
    file.read((char*)&waterHeight, sizeof(float));
    readColor(baseColor);
    file.read((char*)&transparency, sizeof(float));
    readStr(name);

    file.read((char*)&noise, sizeof(NoiseParams));
    file.read((char*)&foam, sizeof(FoamParams));
    file.read((char*)&grid, sizeof(GridParams));

    MarkMeshDirty();
    return file.good();
}