#include "WaterBody.hpp"
#include "WaterNoise.hpp"
#include "ShaderCache.hpp"
#include "Engine.hpp"
#include "Graphics.hpp"
#include "ui.hpp"
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <fstream>
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
    // Only unload if we actually loaded custom files (never unload raylib's shared default shader)
    if (customShader && shaderLoaded) UnloadShader(shader);
    shader = { 0 };
    shaderLoaded = false;
    customShader = false;
}

void WaterBody::ReleaseGpuResources() {
    for (auto& [key, chunk] : chunks) {
        if (chunk.mesh.vertexCount > 0) {
            UnloadMesh(chunk.mesh);
        }
    }
    chunks.clear();
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
    if (chunk.mesh.vertexCount > 0) {
        UnloadMesh(chunk.mesh);
        chunk.mesh = { 0 };
    }
    chunk.model = { 0 };
    chunk.currentLod = -1;
}

void WaterBody::InitializeShader() {
    WaterNoise::Initialize();

    shader = LoadShader("water.vert", "water.frag");
    if (shader.id == 0 || shader.locs == nullptr) {
        shader = LoadShader(0, 0);
        customShader = false;
        shaderLoaded = (shader.id != 0);
    } else {
        customShader = true;
        shaderLoaded = true;
    }

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
    chunkFadeLoc = GetShaderLocation(shader, "chunkFade");
    objectCountLoc = GetShaderLocation(shader, "objectCount");
    objectPositionsLoc = GetShaderLocation(shader, "objectPositions");

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

void WaterBody::BuildChunkMesh(Chunk& chunk, int resolution, float worldMinX, float worldMaxX, float worldMinZ, float worldMaxZ) {
    int vertCount = (resolution + 1) * (resolution + 1);
    int triCount = resolution * resolution * 2;

    chunk.mesh = { 0 };
    chunk.mesh.vertexCount = vertCount;
    chunk.mesh.triangleCount = triCount;

    chunk.mesh.vertices = (float*)MemAlloc(vertCount * 3 * sizeof(float));
    chunk.mesh.texcoords = (float*)MemAlloc(vertCount * 2 * sizeof(float));
    chunk.mesh.normals = (float*)MemAlloc(vertCount * 3 * sizeof(float));
    chunk.mesh.indices = (unsigned short*)MemAlloc(triCount * 3 * sizeof(unsigned short));

    float chunkWorldX = chunk.gridX * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
    float chunkWorldZ = chunk.gridZ * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
    float halfChunk = CHUNK_SIZE * 0.5f;
    float stepX = CHUNK_SIZE / resolution;
    float stepZ = CHUNK_SIZE / resolution;

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

    Vector3 camForward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    float cfx = camForward.x;
    float cfz = camForward.z;

    // 1. Build set of chunk keys that should exist
    std::unordered_set<int64_t> activeKeys;

    for (int gz = gridMinZ; gz < gridMaxZ; gz++) {
        for (int gx = gridMinX; gx < gridMaxX; gx++) {
            float cx = gx * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
            float cz = gz * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
            float toChunkX = cx - camera.position.x;
            float toChunkZ = cz - camera.position.z;
            float dist = sqrtf(toChunkX * toChunkX + toChunkZ * toChunkZ);

            int newLod;
            if (dist <= MAX_RENDER_DISTANCE) {
                newLod = GetLodForDistance(dist);
            } else {
                float forwardDot = (toChunkX * cfx + toChunkZ * cfz);
                if (forwardDot < 0.0f || dist > HORIZON_DISTANCE) continue;
                newLod = 1;
            }
            if (newLod == 0) continue;

            int64_t key = ChunkKey(gx, gz);
            activeKeys.insert(key);

            auto it = chunks.find(key);
            if (it == chunks.end()) {
                // New chunk — create at fade = 0
                Chunk& chunk = chunks[key];
                chunk.gridX = gx;
                chunk.gridZ = gz;
                chunk.fade = 0.0f;
                BuildChunkMesh(chunk, LOD_RESOLUTIONS[newLod], worldMinX, worldMaxX, worldMinZ, worldMaxZ);
                chunk.currentLod = newLod;
            } else {
                // Existing chunk — update LOD if needed
                Chunk& chunk = it->second;
                if (newLod != chunk.currentLod) {
                    ReleaseChunkResources(chunk);
                    BuildChunkMesh(chunk, LOD_RESOLUTIONS[newLod], worldMinX, worldMaxX, worldMinZ, worldMaxZ);
                    chunk.currentLod = newLod;
                }
            }
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

    UpdateChunks(camera);

    if (chunks.empty()) return;

    BeginShaderMode(shader);
    UpdateShaderUniforms(camera, globalTime);

    // Gather nearby objects for proximity foam
    const int MAX_FOAM_OBJECTS = 16;
    float objData[MAX_FOAM_OBJECTS * 4];
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
            objCount++;
        }

        SetShaderValue(shader, objectCountLoc, &objCount, SHADER_UNIFORM_INT);
        SetShaderValueV(shader, objectPositionsLoc, objData, SHADER_UNIFORM_VEC4, objCount);
    }

    // Camera forward in XZ for behind-camera culling
    Vector3 camForward = Vector3Normalize(Vector3Subtract(camera.target, camera.position));
    float camForwardXZ_len = sqrtf(camForward.x * camForward.x + camForward.z * camForward.z);
    float cfx = camForwardXZ_len > 0.001f ? camForward.x / camForwardXZ_len : 0.0f;
    float cfz = camForwardXZ_len > 0.001f ? camForward.z / camForwardXZ_len : 0.0f;

    for (auto& [key, chunk] : chunks) {
        if (chunk.model.meshes == nullptr || chunk.model.materials == nullptr) continue;

        float cx = chunk.gridX * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
        float cz = chunk.gridZ * CHUNK_SIZE + CHUNK_SIZE * 0.5f;

        // Skip chunks behind the camera
        float toChunkX = cx - camera.position.x;
        float toChunkZ = cz - camera.position.z;
        if (toChunkX * cfx + toChunkZ * cfz < -CHUNK_SIZE) continue;

        Matrix modelMat = MatrixTranslate(cx, waterHeight, cz);
        SetShaderValueMatrix(shader, wModelLoc, modelMat);
        if (chunkFadeLoc >= 0) {
            SetShaderValue(shader, chunkFadeLoc, &chunk.fade, SHADER_UNIFORM_FLOAT);
        }

        DrawModel(chunk.model, { 0.0f, 0.0f, 0.0f }, 1.0f, WHITE);
    }

    EndShaderMode();
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
            float cx = chunk.gridX * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
            float cz = chunk.gridZ * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
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

float WaterBody::GetHeightAt(float x, float z) const {
    float lx = x - position.x;
    float lz = z - position.z;

    if (fabsf(lx) > size.x * 0.5f || fabsf(lz) > size.z * 0.5f) {
        return waterHeight;
    }

    // Match GPU vertex shader exactly: absolute world XZ (not entity-local —
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
    return waterHeight + n * noise.amplitude;
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