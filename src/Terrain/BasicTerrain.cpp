#include "../../include/Terrain/BasicTerrain.hpp"
#include "../../include/Engine/Frontend/ui.hpp"
#include "../../include/Engine/Platform/Platform.hpp"
#include "../../include/Engine/Graphics.hpp"
#include "raylib.h"
#include "raymath.h"
// Bind splatmap + albedo textures to explicit GL texture slots in Draw() using
// rlActiveTextureSlot/rlEnableTexture/rlSetUniform, so we are not limited by
// raylib's internal batch SetShaderValueTexture() cap of 4 textures.
#include "rlgl.h"
#include <algorithm>
#include <cmath>
#include <vector>
#include <filesystem>

// Forward decls for the file-local noise helpers (defined below); the Noise
// brush in ApplyBrush() needs them before their definitions.
static float TerrainValueNoise(float x, float y, int seed);
static float TerrainFbm(float x, float y, int seed, int octaves, float persistence, float lacunarity);

// Reports which of the engine's data roots was picked. This used to print the
// result of probing a dozen hand-written relative paths, which was the only
// way to diagnose "terrain textures missing" -- now that resolution is
// centralised, the useful line is the root that won and the ones that did not.
static void LogTerrainTextureDebug() {
    static bool logged = false;
    if (logged) return;
    logged = true;

    std::error_code ec;
    ui::Log("[TerrainPaint] CWD: %s", std::filesystem::current_path(ec).string().c_str());
    ui::Log("[TerrainPaint] EXE: %s", platform::ExecutablePath().c_str());
    for (const std::string& r : platform::DataSearchRoots()) {
        ui::Log("[TerrainPaint]  root %s", r.c_str());
    }
    const std::string dataRoot = platform::FoundDataRoot();
    ui::Log("[TerrainPaint]  using data root: %s",
            dataRoot.empty() ? "NONE FOUND (set FLYENGINE_DATA_DIR)" : dataRoot.c_str());
}

Camera3D* BasicTerrain::s_activeCamera = nullptr;
BasicTerrain* BasicTerrain::s_active = nullptr;
BasicTerrain::Brush BasicTerrain::s_brush{};
bool BasicTerrain::s_editorActive = false;
std::vector<BasicTerrain*> BasicTerrain::s_instances{};
Texture2D BasicTerrain::s_layerTextures[4] = {{0},{0},{0},{0}};
std::string BasicTerrain::s_layerNames[4] = {"Layer 0","Layer 1","Layer 2","Layer 3"};
int BasicTerrain::s_layerCount = 0;
Color BasicTerrain::s_layerColors[4] = {
    { 96, 168, 82, 255 },    // Layer 0 - grass green
    { 128, 96, 64, 255 },    // Layer 1 - dirt brown
    { 150, 138, 120, 255 },  // Layer 2 - light rock
    { 60, 60, 60, 255 }      // Layer 3 - dark rock
};
Shader BasicTerrain::s_terrainShader = {0};
int BasicTerrain::s_shaderSplatmapLoc = -1;
int BasicTerrain::s_shaderAlbedoLocs[4] = {-1, -1, -1, -1};
int BasicTerrain::s_shaderLayerCountLoc = -1;
int BasicTerrain::s_shaderLightDirLoc = -1;
int BasicTerrain::s_shaderLightColorLoc = -1;
int BasicTerrain::s_shaderAmbientColorLoc = -1;
int BasicTerrain::s_shaderTextureTilingLoc = -1;

BasicTerrain::BasicTerrain(int w, int d, float s, float maxH, float texTile)
    : width(w), depth(d), scale(s), maxHeight(maxH), textureTiling(texTile) {
    heightmap.assign((size_t)width * depth, 0.0f);
    meshDirty = true;

    // Splatmap resolution is supersampled relative to the heightmap grid so
    // painted blends look smooth instead of blocky.
    splatWidth = std::min(width * kSplatSupersample, kSplatMaxDim);
    splatDepth = std::min(depth * kSplatSupersample, kSplatMaxDim);

    // Initialize splatmap (layer 0 = full weight everywhere)
    splatmap.resize((size_t)splatWidth * splatDepth * 4, 0);
    for (size_t i = 0; i < (size_t)splatWidth * splatDepth; i++) {
        splatmap[i * 4 + 0] = 255;
        splatmap[i * 4 + 1] = 0;
        splatmap[i * 4 + 2] = 0;
        splatmap[i * 4 + 3] = 0;
    }

    // Load terrain paint shader (once, shared via static)
    static bool shaderLoaded = false;
    if (!shaderLoaded) {
        LogTerrainTextureDebug();
        // Compiled into the binary from shaders/terrain_paint.glsl (sokol-shdc).
        s_terrainShader = LoadShaderProgram("terrain_paint");
        ui::Log("[TerrainPaint] terrain_paint shader -> %s", IsShaderValid(s_terrainShader) ? "OK" : "FAILED");
        if (IsShaderValid(s_terrainShader)) {
            s_shaderSplatmapLoc = GetShaderLocation(s_terrainShader, "splatmap");
            s_shaderAlbedoLocs[0] = GetShaderLocation(s_terrainShader, "albedoTex0");
            s_shaderAlbedoLocs[1] = GetShaderLocation(s_terrainShader, "albedoTex1");
            s_shaderAlbedoLocs[2] = GetShaderLocation(s_terrainShader, "albedoTex2");
            s_shaderAlbedoLocs[3] = GetShaderLocation(s_terrainShader, "albedoTex3");
            s_shaderLayerCountLoc = GetShaderLocation(s_terrainShader, "layerCount");
            s_shaderLightDirLoc = GetShaderLocation(s_terrainShader, "lightDir");
            s_shaderLightColorLoc = GetShaderLocation(s_terrainShader, "lightColor");
            s_shaderAmbientColorLoc = GetShaderLocation(s_terrainShader, "ambientColor");
            s_shaderTextureTilingLoc = GetShaderLocation(s_terrainShader, "textureTiling");
        }
        shaderLoaded = true;
    }

    // Load terrain textures from TerrainTextures folder (once)
    static bool texturesLoaded = false;
    if (!texturesLoaded) { LoadTerrainTextures(); texturesLoaded = true; }

    // Default albedo for a fresh terrain. Leaving terrainTexture at {0} is a
    // valid state the renderer already handles (raylib falls back to its white
    // 1x1 default), so a missing preset is not fatal.
    const std::string texPath = platform::ResolveAsset("assets/PresetTextures/LeafyGrass.qoi");
    if (!texPath.empty()) {
        Image img = LoadImage(texPath.c_str());
        if (img.data) {
            terrainTexture = LoadTextureFromImage(img);
            SetTextureWrap(terrainTexture, TEXTURE_WRAP_REPEAT);
            UnloadImage(img);
        }
    } else {
        ui::Log("[TerrainPaint] Default albedo assets/PresetTextures/LeafyGrass.qoi not found; "
                "terrain will render unshaded-white until a texture is assigned");
    }

    s_instances.push_back(this);
}

BasicTerrain::~BasicTerrain() {
    s_instances.erase(std::remove(s_instances.begin(), s_instances.end(), this), s_instances.end());
    if (mesh.vertices) UnloadMesh(mesh);
    if (terrainTexture.id > 0) UnloadTexture(terrainTexture);
    if (splatmapTexture.id > 0) UnloadTexture(splatmapTexture);
    // The paint shader and layer textures are shared across all instances;
    // only free them when the last instance is destroyed.
    if (s_instances.empty()) {
        if (s_terrainShader.id > 0) { UnloadShader(s_terrainShader); s_terrainShader = {0}; }
        for (int i = 0; i < 4; i++) {
            if (s_layerTextures[i].id > 0) { UnloadTexture(s_layerTextures[i]); s_layerTextures[i] = {0}; }
        }
        s_layerCount = 0;
    }
    if (s_active == this) s_active = nullptr;
}

void BasicTerrain::GenerateFlat(float height) {
    std::fill(heightmap.begin(), heightmap.end(), height);
    // Reset splatmap to layer 0 full
    for (size_t i = 0; i < (size_t)splatWidth * splatDepth; i++) {
        splatmap[i * 4 + 0] = 255;
        splatmap[i * 4 + 1] = 0;
        splatmap[i * 4 + 2] = 0;
        splatmap[i * 4 + 3] = 0;
    }
    splatmapDirty = true;
    meshDirty = true;
}

bool BasicTerrain::LoadHeightmap(const std::string& path) {
    Image img = LoadImage(path.c_str());
    if (!img.data) return false;

    ImageResize(&img, width, depth);
    ImageColorGrayscale(&img);

    Color* pixels = LoadImageColors(img);
    if (!pixels) { UnloadImage(img); return false; }

    heightmap.resize((size_t)width * depth);
    float range = maxHeight - minHeight;

    for (int z = 0; z < depth; z++) {
        for (int x = 0; x < width; x++) {
            float h = pixels[z * width + x].r / 255.0f;
            heightmap[z * width + x] = minHeight + h * range;
        }
    }

    UnloadImageColors(pixels);
    UnloadImage(img);
    // Reset splatmap
    for (size_t i = 0; i < (size_t)splatWidth * splatDepth; i++) {
        splatmap[i * 4 + 0] = 255;
        splatmap[i * 4 + 1] = 0;
        splatmap[i * 4 + 2] = 0;
        splatmap[i * 4 + 3] = 0;
    }
    splatmapDirty = true;
    meshDirty = true;
    return true;
}

bool BasicTerrain::SaveHeightmap(const std::string& path) const {
    Image src = GenImageColor(width, depth, BLACK);
    Color* px = LoadImageColors(src);
    UnloadImage(src);
    if (!px) return false;

    float range = maxHeight - minHeight;
    if (range <= 0.0f) range = 1.0f;

    for (int i = 0; i < width * depth; i++) {
        float h = (heightmap[i] - minHeight) / range;
        h = std::clamp(h, 0.0f, 1.0f);
        unsigned char v = (unsigned char)(h * 255.0f);
        px[i] = Color{v, v, v, 255};
    }

    Image out = { px, width, depth, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    bool ok = ExportImage(out, path.c_str());
    UnloadImageColors(px);
    return ok;
}

float BasicTerrain::HeightLocal(float lx, float lz) const {
    lx = std::clamp(lx, 0.0f, (float)(width - 1));
    lz = std::clamp(lz, 0.0f, (float)(depth - 1));

    int x0 = (int)lx;
    int z0 = (int)lz;
    int x1 = std::min(x0 + 1, width - 1);
    int z1 = std::min(z0 + 1, depth - 1);
    float fx = lx - x0;
    float fz = lz - z0;

    float h00 = heightmap[(size_t)z0 * width + x0];
    float h10 = heightmap[(size_t)z0 * width + x1];
    float h01 = heightmap[(size_t)z1 * width + x0];
    float h11 = heightmap[(size_t)z1 * width + x1];

    float h0 = h00 + (h10 - h00) * fx;
    float h1 = h01 + (h11 - h01) * fx;
    return h0 + (h1 - h0) * fz;
}

float BasicTerrain::GetHeightAt(float x, float z) const {
    float lx = (x - position.x) / scale + width * 0.5f;
    float lz = (z - position.z) / scale + depth * 0.5f;
    if (lx < -1 || lz < -1 || lx > width || lz > depth) return minHeight;
    return HeightLocal(lx, lz);
}

Vector3 BasicTerrain::GetNormalAt(float x, float z) const {
    float eps = scale * 0.5f;
    float hL = GetHeightAt(x - eps, z);
    float hR = GetHeightAt(x + eps, z);
    float hD = GetHeightAt(x, z - eps);
    float hU = GetHeightAt(x, z + eps);
    Vector3 n = { hL - hR, 2.0f * eps, hD - hU };
    return Vector3Normalize(n);
}

bool BasicTerrain::Raycast(const Ray& ray, float* outDistance, Vector3* outHitPoint, Vector3* outNormal) const {
    Vector3 ro = ray.position;
    Vector3 rd = Vector3Normalize(ray.direction);

    // Robust ray vs heightfield. Works for flat AND raised terrain at ANY
    // position/scale. We clamp the ray to the terrain's world AABB (slab test),
    // then march interior points and detect the first ray-below-surface crossing.
    // The real surface height at world (x,z) is position.y + GetHeightAt(x,z).
    const float halfW = width  * scale * 0.5f;
    const float halfD = depth  * scale * 0.5f;
    const float x0 = position.x - halfW, x1 = position.x + halfW;
    const float z0 = position.z - halfD, z1 = position.z + halfD;
    const float yBot = position.y + minHeight - 1.0f;
    const float yTop = position.y + maxHeight + 1.0f;

    // Slab (AABB) intersection to get the ray's t-range through the volume.
    float tmin = -1e30f, tmax = 1e30f;
    auto slab = [&](float ori, float dir, float a, float b) -> bool {
        if (fabsf(dir) < 1e-9f) return (ori >= a && ori <= b);
        float ta = (a - ori) / dir, tb = (b - ori) / dir;
        if (ta > tb) { float tmp = ta; ta = tb; tb = tmp; }
        tmin = fmaxf(tmin, ta); tmax = fminf(tmax, tb);
        return tmin <= tmax;
    };
    if (!slab(ro.x, rd.x, x0, x1)) return false;
    if (!slab(ro.z, rd.z, z0, z1)) return false;
    if (!slab(ro.y, rd.y, yBot, yTop)) return false;
    if (tmax < 0.0f) return false;
    if (tmin < 0.0f) tmin = 0.0f;
    if (tmin > tmax) return false;

    float step = scale * 0.125f;
    float t = tmin;
    while (t <= tmax) {
        float ppx = ro.x + rd.x * t;
        float ppz = ro.z + rd.z * t;
        float py  = ro.y + rd.y * t;
        float surfY = position.y + GetHeightAt(ppx, ppz);
        if (py <= surfY) {
            // Bisect the crossing between the previous sample and t.
            float lo = t - step; if (lo < tmin) lo = tmin;
            for (int it = 0; it < 8; it++) {
                float mid = (lo + t) * 0.5f;
                float mx = ro.x + rd.x * mid;
                float mz = ro.z + rd.z * mid;
                float my = ro.y + rd.y * mid;
                if (my <= position.y + GetHeightAt(mx, mz)) t = mid; else lo = mid;
            }
            float hx = ro.x + rd.x * t;
            float hz = ro.z + rd.z * t;
            Vector3 hp = { hx, position.y + GetHeightAt(hx, hz), hz };
            if (outDistance) *outDistance = t;
            if (outHitPoint) *outHitPoint = hp;
            if (outNormal) *outNormal = GetNormalAt(hx, hz);
            return true;
        }
        t += step;
    }
    return false;
}

void BasicTerrain::ApplyBrush(const Brush& b, Vector2 worldPos, float dt) {
    float cx = (worldPos.x - position.x) / scale + width * 0.5f;
    float cz = (worldPos.y - position.z) / scale + depth * 0.5f;
    float pixelRadius = b.radius / scale;
    if (pixelRadius < 1.0f) pixelRadius = 1.0f;

    int minX = std::max(0, (int)std::floor(cx - pixelRadius));
    int maxX = std::min(width - 1, (int)std::ceil(cx + pixelRadius));
    int minZ = std::max(0, (int)std::floor(cz - pixelRadius));
    int maxZ = std::min(depth - 1, (int)std::ceil(cz + pixelRadius));

    // Erosion is a neighborhood operation, so it can't run inside the per-cell
    // loop: run the passes over a padded snapshot of the brush footprint, then
    // blend the resulting height deltas back in by the brush falloff, so the
    // erosion is shaped like every other tool (smoothstep + hardness).
    if (b.tool == Tool::Erode) {
        const int pad = 1;
        const int ex0 = std::max(0, minX - pad), ex1 = std::min(width - 1, maxX + pad);
        const int ez0 = std::max(0, minZ - pad), ez1 = std::min(depth - 1, maxZ + pad);
        const int rw = ex1 - ex0 + 1, rd = ez1 - ez0 + 1;
        if (rw >= 3 && rd >= 3) {
            std::vector<float> buf((size_t)rw * rd);
            for (int z = ez0; z <= ez1; z++)
                for (int x = ex0; x <= ex1; x++)
                    buf[(size_t)(z - ez0) * rw + (x - ex0)] = heightmap[(size_t)z * width + x];

            // Strength scales the per-frame pass count so dragging erodes at a
            // rate rather than a single fixed blast (frame-rate independent).
            ErosionSettings es;
            es.iterations = std::clamp((int)(b.strength * 0.05f * dt * 60.0f + 0.5f), 1, 5);
            RunErosion(buf.data(), rw, rd, scale, es, es.iterations);

            for (int z = ez0; z <= ez1; z++) {
                for (int x = ex0; x <= ex1; x++) {
                    const float ddx = x - cx, ddz = z - cz;
                    float n;
                    if (b.shape == Shape::Square) n = std::max(std::fabs(ddx), std::fabs(ddz)) / pixelRadius;
                    else                          n = sqrtf(ddx * ddx + ddz * ddz) / pixelRadius;
                    if (n >= 1.0f) continue;
                    float weight = 1.0f - n * n * (3.0f - 2.0f * n);
                    weight = powf(weight, 1.0f + b.hardness * 4.0f);
                    if (weight <= 0.0f) continue;

                    const float before = heightmap[(size_t)z * width + x];
                    const float after  = buf[(size_t)(z - ez0) * rw + (x - ex0)];
                    heightmap[(size_t)z * width + x] =
                        std::clamp(before + (after - before) * weight, minHeight, maxHeight);
                }
            }
            meshDirty = true;
        }
        return;
    }

    for (int z = minZ; z <= maxZ; z++) {
        for (int x = minX; x <= maxX; x++) {
            float dx = x - cx;
            float dz = z - cz;

            // Normalized distance to brush edge: circle = Euclidean, square = Chebyshev
            float n;
            if (b.shape == Shape::Square) n = std::max(std::fabs(dx), std::fabs(dz)) / pixelRadius;
            else                          n = sqrtf(dx * dx + dz * dz) / pixelRadius;
            if (n >= 1.0f) continue;

            float weight = 1.0f - n * n * (3.0f - 2.0f * n);   // smoothstep falloff
            weight = powf(weight, 1.0f + b.hardness * 4.0f);
            if (weight <= 0.0f) continue;

            float& h = heightmap[(size_t)z * width + x];

            switch (b.tool) {
                // Raise/Lower are rate-based so painting looks identical at any
                // frame rate; strength slider maps to a gentle 0.25x u/s.
                case Tool::Raise:   h += b.strength * dt * 0.25f * weight; break;
                case Tool::Lower:   h -= b.strength * dt * 0.25f * weight; break;
                case Tool::Smooth: {
                    float sum = 0; int count = 0;
                    if (x > 0)        { sum += heightmap[(size_t)z * width + x - 1]; count++; }
                    if (x < width-1)  { sum += heightmap[(size_t)z * width + x + 1]; count++; }
                    if (z > 0)        { sum += heightmap[(size_t)(z-1) * width + x]; count++; }
                    if (z < depth-1)  { sum += heightmap[(size_t)(z+1) * width + x]; count++; }
                    if (count > 0) h += (sum / count - h) * std::clamp(b.strength * 0.05f * dt, 0.0f, 1.0f) * weight;
                    break;
                }
                case Tool::Flatten: h += (b.targetHeight - h) * std::clamp(b.strength * 0.05f * dt, 0.0f, 1.0f) * weight; break;
                case Tool::Noise: {
                    // Deterministic FBM displacement; strength scales the amount,
                    // noiseScale sets the feature size in meters.
                    const float u = (x + 0.5f) * scale / b.noiseScale;
                    const float v = (z + 0.5f) * scale / b.noiseScale;
                    const float n = TerrainFbm(u, v, b.noiseSeed, 3, 0.5f, 2.0f) * 2.0f - 1.0f;
                    h += n * b.strength * dt * 0.06f * weight;
                    break;
                }
                // Erode is handled above (neighborhood op). Ramp is a one-shot
                // click-drag in Update(). None is the "no tool" idle state.
                case Tool::Erode:
                case Tool::Ramp:
                case Tool::None:
                    break;
            }
            h = std::clamp(h, minHeight, maxHeight);
        }
    }
    meshDirty = true;
}

void BasicTerrain::StampBrush(const Brush& b, Vector2 worldPos) {
    switch (b.tool) {
        case Tool::Raise:
        case Tool::Lower: {
            // Gentle single-click stamp: a small visible bump/dent, not a spike
            // (ApplyBrush multiplies by 0.25, so 0.4x strength lands ~4 units)
            Brush sb = b;
            sb.strength = std::max(1.0f, b.strength * 0.4f);
            ApplyBrush(sb, worldPos);
            break;
        }
        case Tool::Smooth: {
            // One click smooths the area noticeably
            Brush sb = b;
            sb.strength = std::max(b.strength, 50.0f);
            for (int i = 0; i < 8; i++) ApplyBrush(sb, worldPos);
            break;
        }
        case Tool::Flatten: {
            // One click snaps the footprint to the target height
            Brush fb = b;
            fb.strength = 100.0f;
            ApplyBrush(fb, worldPos);
            break;
        }
        case Tool::Erode: {
            // One click = a short burst of erosion passes
            Brush eb = b;
            eb.strength = std::max(b.strength, 60.0f);
            ApplyBrush(eb, worldPos);   // dt=1 -> 5 passes at strength >= 60
            break;
        }
        case Tool::Noise: {
            // One click = a few FBM displacement passes
            Brush nb = b;
            nb.strength = std::max(b.strength, 50.0f);
            for (int i = 0; i < 3; i++) ApplyBrush(nb, worldPos);
            break;
        }
        case Tool::Paint: {
            Brush pb = b;
            pb.strength = std::max(b.strength, 40.0f);
            for (int i = 0; i < 4; i++) ApplyPaintBrush(pb, worldPos);
            break;
        }
        // Ramp is drag-based: Update() captures rampStart on press and
        // commits on release, so there is nothing to stamp here.
        case Tool::Ramp:
        case Tool::None:
            break;
    }
}

void BasicTerrain::ApplyRamp(Vector2 start, Vector2 end, float strength, float bandWidth) {
    // World -> grid coordinates
    const float caX = (start.x - position.x) / scale + width * 0.5f;
    const float caZ = (start.y - position.z) / scale + depth * 0.5f;
    const float cbX = (end.x   - position.x) / scale + width * 0.5f;
    const float cbZ = (end.y   - position.z) / scale + depth * 0.5f;

    const float dx = cbX - caX, dz = cbZ - caZ;
    const float len2 = dx * dx + dz * dz;
    const float halfW = std::max(0.5f, bandWidth * 0.5f / scale);

    // Captured *before* any edits so the grade is a single linear ramp.
    const float hA = GetHeightAt(start.x, start.y);
    const float hB = GetHeightAt(end.x, end.y);

    const int minX = std::max(0, (int)std::floor(std::min(caX, cbX) - halfW));
    const int maxX = std::min(width - 1, (int)std::ceil(std::max(caX, cbX) + halfW));
    const int minZ = std::max(0, (int)std::floor(std::min(caZ, cbZ) - halfW));
    const int maxZ = std::min(depth - 1, (int)std::ceil(std::max(caZ, cbZ) + halfW));

    // Strength slider 2..200 -> pull factor 0.35..1.0 toward the target grade.
    const float pull = std::clamp(0.35f + strength / 200.0f * 0.65f, 0.0f, 1.0f);

    for (int z = minZ; z <= maxZ; z++) {
        for (int x = minX; x <= maxX; x++) {
            float t = 0.0f, dist;
            if (len2 > 1e-6f) {
                t = ((x - caX) * dx + (z - caZ) * dz) / len2;
                t = std::clamp(t, 0.0f, 1.0f);
                const float px = caX + dx * t, pz = caZ + dz * t;
                dist = std::sqrt((x - px) * (x - px) + (z - pz) * (z - pz));
            } else {
                // Degenerate drag (start == end): stamp a small mound/pit.
                dist = std::sqrt((x - caX) * (x - caX) + (z - caZ) * (z - caZ));
            }
            if (dist > halfW) continue;

            // Smooth edge falloff inside the band, like the brush tools
            const float e = std::clamp(dist / halfW, 0.0f, 1.0f);
            const float fall = 1.0f - e * e * (3.0f - 2.0f * e);

            const float target = hA + (hB - hA) * t;
            float& h = heightmap[(size_t)z * width + x];
            h = std::clamp(h + (target - h) * pull * fall, minHeight, maxHeight);
        }
    }
    meshDirty = true;
}

void BasicTerrain::ApplyPaintBrush(const Brush& b, Vector2 worldPos, float dt) {
    // World units covered by one splatmap texel. Independent of the heightmap
    // vertex spacing now that the splatmap is supersampled for smoother blends.
    float splatScaleX = (width * scale) / (float)splatWidth;
    float splatScaleZ = (depth * scale) / (float)splatDepth;

    float cx = (worldPos.x - position.x) / splatScaleX + splatWidth * 0.5f;
    float cz = (worldPos.y - position.z) / splatScaleZ + splatDepth * 0.5f;
    float pixelRadius = b.radius / std::min(splatScaleX, splatScaleZ);
    if (pixelRadius < 1.0f) pixelRadius = 1.0f;

    int minX = std::max(0, (int)std::floor(cx - pixelRadius));
    int maxX = std::min(splatWidth - 1, (int)std::ceil(cx + pixelRadius));
    int minZ = std::max(0, (int)std::floor(cz - pixelRadius));
    int maxZ = std::min(splatDepth - 1, (int)std::ceil(cz + pixelRadius));

    float rate = b.strength * dt * 2.0f;

    for (int z = minZ; z <= maxZ; z++) {
        for (int x = minX; x <= maxX; x++) {
            float dx = x - cx;
            float dz = z - cz;
            float n;
            if (b.shape == Shape::Square)
                n = std::max(std::fabs(dx), std::fabs(dz)) / pixelRadius;
            else
                n = sqrtf(dx * dx + dz * dz) / pixelRadius;
            if (n >= 1.0f) continue;

            float weight = 1.0f - n * n * (3.0f - 2.0f * n);
            weight = powf(weight, 1.0f + b.hardness * 4.0f);
            if (weight <= 0.0f) continue;

            float w = std::clamp(rate * weight, 0.0f, 1.0f);
            size_t idx = (size_t)z * splatWidth + x;
            uint8_t* splat = &splatmap[idx * 4];

            if (b.paintErase) {
                int sel = b.paintLayer;
                uint8_t selVal = splat[sel];
                uint8_t dec = (uint8_t)std::clamp(w * 255.0f, 0.0f, (float)selVal);
                splat[sel] -= dec;
                int others = 0;
                for (int i = 0; i < 4; i++) if (i != sel) others += splat[i];
                if (others > 0) {
                    for (int i = 0; i < 4; i++) {
                        if (i != sel) splat[i] = (uint8_t)std::clamp(splat[i] + (int)((float)dec * splat[i] / others), 0, 255);
                    }
                } else {
                    splat[0] = 255;
                }
            } else {
                int sel = b.paintLayer;
                uint8_t add = (uint8_t)std::clamp(w * 255.0f, 0.0f, 255.0f - splat[sel]);
                splat[sel] += add;
                int others = 0;
                for (int i = 0; i < 4; i++) if (i != sel) others += splat[i];
                if (others > 0 && add > 0) {
                    for (int i = 0; i < 4; i++) {
                        if (i != sel) {
                            int reduction = (int)((float)add * splat[i] / others);
                            splat[i] = (uint8_t)std::max(0, (int)splat[i] - reduction);
                        }
                    }
                }
            }

            // Normalize to sum=255
            int sum = splat[0] + splat[1] + splat[2] + splat[3];
            if (sum != 255 && sum > 0) {
                float scl = 255.0f / sum;
                int s0 = (int)std::round(splat[0] * scl);
                int s1 = (int)std::round(splat[1] * scl);
                int s2 = (int)std::round(splat[2] * scl);
                int s3 = 255 - s0 - s1 - s2;
                splat[0] = (uint8_t)std::clamp(s0, 0, 255);
                splat[1] = (uint8_t)std::clamp(s1, 0, 255);
                splat[2] = (uint8_t)std::clamp(s2, 0, 255);
                splat[3] = (uint8_t)std::clamp(s3, 0, 255);
            }
        }
    }
    splatmapDirty = true;
}

void BasicTerrain::LoadTerrainTextures() {
    for (int i = 0; i < 4; i++) {
        if (s_layerTextures[i].id > 0) { UnloadTexture(s_layerTextures[i]); s_layerTextures[i] = {0}; }
        s_layerNames[i] = "Layer " + std::to_string(i);
    }
    s_layerCount = 0;

    // ResolveAsset only returns files, so ask each data root for the directory
    // itself. The first root that has it is the one the files came from.
    std::string dirPath;
    for (const std::string& r : platform::DataSearchRoots()) {
        const std::string candidate =
            (std::filesystem::u8path(r) / "assets" / "Textures" / "TerrainTextures").string();
        std::error_code ec;
        if (std::filesystem::is_directory(candidate, ec)) { dirPath = candidate; break; }
    }
    ui::Log("[TerrainPaint] LoadTerrainTextures: resolved dir = %s", dirPath.empty() ? "EMPTY" : dirPath.c_str());
    if (dirPath.empty()) return;

    // Collect texture files
    std::vector<std::string> texFiles;
    std::error_code ec;
    auto dirIt = std::filesystem::directory_iterator(dirPath, ec);
    if (ec) { ui::Log("[TerrainPaint] directory_iterator failed: %s", ec.message().c_str()); return; }
    for (const auto& entry : dirIt) {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".qoi" || ext == ".png") {
            texFiles.push_back(entry.path().string());
        }
    }
    std::sort(texFiles.begin(), texFiles.end());

    ui::Log("[TerrainPaint] Found %d texture files", (int)texFiles.size());

    for (const auto& p : texFiles) {
        if (s_layerCount >= 4) break;
        Image img = LoadImage(p.c_str());
        ui::Log("[TerrainPaint]   LoadImage('%s') -> %s", p.c_str(), img.data ? "OK" : "FAILED");
        if (!img.data) continue;
        s_layerTextures[s_layerCount] = LoadTextureFromImage(img);
        SetTextureWrap(s_layerTextures[s_layerCount], TEXTURE_WRAP_REPEAT);
        GenTextureMipmaps(&s_layerTextures[s_layerCount]);
        SetTextureFilter(s_layerTextures[s_layerCount], TEXTURE_FILTER_TRILINEAR);
        UnloadImage(img);

        std::string fname = std::filesystem::path(p).stem().string();
        s_layerNames[s_layerCount] = fname;
        ui::Log("[TerrainPaint]   Layer %d = '%s'", s_layerCount, fname.c_str());
        s_layerCount++;
    }

    // Fallback: if the directory scan produced nothing, try loading the known
    // preset files directly from several candidate paths.
    if (s_layerCount == 0) {
        const char* defaultFiles[] = { "LeafyGrass.qoi", "rockygrass.qoi" };
        const char* basePaths[] = {
            "assets/Textures/TerrainTextures/",
            "../assets/Textures/TerrainTextures/",
            "../../assets/Textures/TerrainTextures/",
            "../../../assets/Textures/TerrainTextures/",
            "assets/PresetTextures/", "../../../assets/PresetTextures/",
            "../../../../assets/PresetTextures/", "../../assets/PresetTextures/",
            "../assets/PresetTextures/"
        };
        for (const char* base : basePaths) {
            for (const char* f : defaultFiles) {
                if (s_layerCount >= 4) break;
                std::string path = std::string(base) + f;
                if (!FileExists(path.c_str())) continue;
                Image img = LoadImage(path.c_str());
                if (!img.data) continue;
                s_layerTextures[s_layerCount] = LoadTextureFromImage(img);
                SetTextureWrap(s_layerTextures[s_layerCount], TEXTURE_WRAP_REPEAT);
                GenTextureMipmaps(&s_layerTextures[s_layerCount]);
                SetTextureFilter(s_layerTextures[s_layerCount], TEXTURE_FILTER_TRILINEAR);
                UnloadImage(img);
                s_layerNames[s_layerCount] = f;
                ui::Log("[TerrainPaint]   Fallback Layer %d = '%s'", s_layerCount, f);
                s_layerCount++;
            }
            if (s_layerCount >= 4) break;
        }
    }
    ui::Log("[TerrainPaint] Total layers loaded: %d", s_layerCount);
}

// ---------------- Procedural generation ----------------
// Self-contained seeded value noise (no external noise library): a hash
// gives deterministic per-lattice-point randomness, smoothstep-interpolated
// between lattice points, then combined over several octaves (fbm) for
// natural-looking hills, or "ridged" (folded) for sharper mountain peaks.

static uint32_t TerrainHash(int x, int y, int seed) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263) ^ (uint32_t)(seed * 2654435761u);
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= (h >> 16);
    return h;
}

static float TerrainValueNoise(float x, float y, int seed) {
    int ix = (int)std::floor(x);
    int iy = (int)std::floor(y);
    float fx = x - ix;
    float fy = y - iy;

    float v00 = (TerrainHash(ix,     iy,     seed) & 0xFFFFFF) / (float)0xFFFFFF;
    float v10 = (TerrainHash(ix + 1, iy,     seed) & 0xFFFFFF) / (float)0xFFFFFF;
    float v01 = (TerrainHash(ix,     iy + 1, seed) & 0xFFFFFF) / (float)0xFFFFFF;
    float v11 = (TerrainHash(ix + 1, iy + 1, seed) & 0xFFFFFF) / (float)0xFFFFFF;

    float sx = fx * fx * (3.0f - 2.0f * fx);
    float sy = fy * fy * (3.0f - 2.0f * fy);
    float top = v00 + (v10 - v00) * sx;
    float bot = v01 + (v11 - v01) * sx;
    return top + (bot - top) * sy; // [0,1)
}

static float TerrainFbm(float x, float y, int seed, int octaves, float persistence, float lacunarity) {
    float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int o = 0; o < octaves; o++) {
        sum += TerrainValueNoise(x * freq, y * freq, seed + o * 101) * amp;
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f; // [0,1)
}

// Folds the noise into a ridge (sharp near 1, valleys near 0) for
// mountain-like peaks instead of smooth rolling hills.
static float TerrainRidged(float x, float y, int seed, int octaves, float persistence, float lacunarity) {
    float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int o = 0; o < octaves; o++) {
        float n = TerrainValueNoise(x * freq, y * freq, seed + o * 191);
        n = 1.0f - std::fabs(2.0f * n - 1.0f);
        sum += n * n * amp;
        norm += amp;
        amp *= persistence;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f; // [0,1)
}

void BasicTerrain::GenerateTerrainInBox() {
    float minWX = genBoxPos.x - genBoxSize.x * 0.5f;
    float maxWX = genBoxPos.x + genBoxSize.x * 0.5f;
    float minWZ = genBoxPos.z - genBoxSize.z * 0.5f;
    float maxWZ = genBoxPos.z + genBoxSize.z * 0.5f;

    // Blend the generated region smoothly into whatever terrain surrounds it
    // instead of a hard edge, over a band ~15% of the box's footprint.
    float falloffX = std::max(scale, genBoxSize.x * 0.15f);
    float falloffZ = std::max(scale, genBoxSize.z * 0.15f);

    float baseHeight = genBoxPos.y - genBoxSize.y * 0.5f;
    float hillAmp = genHills ? std::max(1.0f, genBoxSize.y * 0.35f) : 0.0f;
    float mountainAmp = genMountains ? std::max(1.0f, genBoxSize.y * 0.65f) : 0.0f;

    float noiseScale = 1.0f / std::max(8.0f, std::min(genBoxSize.x, genBoxSize.z) * 0.08f);

    for (int z = 0; z < depth; z++) {
        float wz = position.z + (z - depth * 0.5f) * scale;
        for (int x = 0; x < width; x++) {
            float wx = position.x + (x - width * 0.5f) * scale;
            if (wx < minWX - falloffX || wx > maxWX + falloffX ||
                wz < minWZ - falloffZ || wz > maxWZ + falloffZ) continue;

            // Falloff mask: 1.0 inside the box, smoothly fading to 0.0 over
            // the falloff band outside it.
            float mx = 1.0f, mz = 1.0f;
            if (wx < minWX) mx = std::clamp(1.0f - (minWX - wx) / falloffX, 0.0f, 1.0f);
            else if (wx > maxWX) mx = std::clamp(1.0f - (wx - maxWX) / falloffX, 0.0f, 1.0f);
            if (wz < minWZ) mz = std::clamp(1.0f - (minWZ - wz) / falloffZ, 0.0f, 1.0f);
            else if (wz > maxWZ) mz = std::clamp(1.0f - (wz - maxWZ) / falloffZ, 0.0f, 1.0f);
            float mask = mx * mz;
            mask = mask * mask * (3.0f - 2.0f * mask);
            if (mask <= 0.0f) continue;

            float nx = wx * noiseScale;
            float nz = wz * noiseScale;

            float generated = baseHeight;
            if (hillAmp > 0.0f) {
                generated += TerrainFbm(nx, nz, genSeed, 4, 0.5f, 2.0f) * hillAmp;
            }
            if (mountainAmp > 0.0f) {
                // A separate, lower-frequency fbm decides *where* mountains
                // rise, so peaks cluster into ranges instead of covering the
                // whole region uniformly.
                float cluster = TerrainFbm(nx * 0.35f, nz * 0.35f, genSeed + 9001, 3, 0.5f, 2.0f);
                cluster = std::clamp((cluster - 0.45f) / 0.35f, 0.0f, 1.0f);
                generated += TerrainRidged(nx * 2.2f, nz * 2.2f, genSeed + 4242, 5, 0.5f, 2.1f) * mountainAmp * cluster;
            }

            size_t vi = (size_t)z * width + x;
            float existing = heightmap[vi];
            float h = existing + (generated - existing) * mask;
            heightmap[vi] = std::clamp(h, minHeight, maxHeight);
        }
    }

    if (genTextures) {
        AutoPaintByHeightSlope(minWX - falloffX, maxWX + falloffX, minWZ - falloffZ, maxWZ + falloffZ);
    }

    meshDirty = true;
}

void BasicTerrain::AutoPaintByHeightSlope(float minWX, float maxWX, float minWZ, float maxWZ) {
    int layerCount = BasicTerrain::GetLayerCount();
    if (layerCount <= 0) return;

    float splatScaleX = (width * scale) / (float)splatWidth;
    float splatScaleZ = (depth * scale) / (float)splatDepth;

    int sxMin = std::max(0, (int)std::floor((minWX - position.x) / splatScaleX + splatWidth * 0.5f));
    int sxMax = std::min(splatWidth - 1, (int)std::ceil((maxWX - position.x) / splatScaleX + splatWidth * 0.5f));
    int szMin = std::max(0, (int)std::floor((minWZ - position.z) / splatScaleZ + splatDepth * 0.5f));
    int szMax = std::min(splatDepth - 1, (int)std::ceil((maxWZ - position.z) / splatScaleZ + splatDepth * 0.5f));

    for (int z = szMin; z <= szMax; z++) {
        for (int x = sxMin; x <= sxMax; x++) {
            float wx = position.x + (x - splatWidth * 0.5f) * splatScaleX;
            float wz = position.z + (z - splatDepth * 0.5f) * splatScaleZ;

            float h = GetHeightAt(wx, wz);
            float t = (maxHeight > minHeight) ? std::clamp((h - minHeight) / (maxHeight - minHeight), 0.0f, 1.0f) : 0.0f;
            Vector3 n = GetNormalAt(wx, wz);
            float slope = 1.0f - std::clamp(n.y, 0.0f, 1.0f); // 0 = flat, ~1 = vertical

            // Up to 4 layers: 0=base/grass (low ground), 1=slope/rock,
            // 2=higher ground, 3=snow near peaks. Layers beyond what's
            // actually loaded (layerCount) are simply left at zero.
            float w[4] = { 0, 0, 0, 0 };
            w[0] = std::clamp(1.0f - t * 1.6f, 0.0f, 1.0f);
            if (layerCount > 1) w[1] = std::clamp(slope * 2.2f, 0.0f, 1.0f);
            if (layerCount > 2) w[2] = std::clamp((t - 0.35f) * 2.0f, 0.0f, 1.0f) * (1.0f - w[1]);
            if (layerCount > 3) w[3] = std::clamp((t - 0.75f) * 4.0f, 0.0f, 1.0f);

            float sum = w[0] + w[1] + w[2] + w[3];
            if (sum <= 0.0001f) { w[0] = 1.0f; sum = 1.0f; }

            size_t idx = (size_t)z * splatWidth + x;
            uint8_t* splat = &splatmap[idx * 4];
            for (int i = 0; i < 4; i++) {
                splat[i] = (i < layerCount) ? (uint8_t)std::clamp((w[i] / sum) * 255.0f, 0.0f, 255.0f) : 0;
            }
            // Renormalize so the 4 channels sum exactly to 255, same as the
            // paint brush does, so the shader's weight normalization is a no-op.
            int isum = splat[0] + splat[1] + splat[2] + splat[3];
            if (isum == 0) {
                splat[0] = 255;
            } else if (isum != 255) {
                int s0 = splat[0], s1 = splat[1], s2 = splat[2];
                splat[3] = (uint8_t)std::clamp(255 - s0 - s1 - s2, 0, 255);
            }
        }
    }
    splatmapDirty = true;
}

void BasicTerrain::RebuildMesh() {
    // Try the adaptive path first: it collapses locally-flat regions of the
    // heightmap into single quads (2 triangles) instead of their full
    // per-vertex grid, so e.g. a freshly-flattened terrain renders as just 2
    // triangles while hills/bumps keep full per-vertex detail. It can bail
    // out (return false) if the terrain is rough enough everywhere that
    // nothing collapses, or in the unlikely case that would still overflow
    // the 16-bit index buffer - BuildDenseMesh() is the always-correct,
    // always-safe fallback for that.
    if (!BuildAdaptiveMesh()) {
        BuildDenseMesh();
    }

    UploadMesh(&mesh, false);

    // NOTE: LoadModelFromMesh() stores a shallow copy of `mesh`, so the model's
    // mesh shares the same CPU buffers and GPU VBO ids. We must NOT call
    // UnloadModel()/UnloadMesh() on the model afterwards (that would double-free
    // the aliased buffers); UnloadMesh(mesh) at the top of BuildAdaptiveMesh()/
    // BuildDenseMesh() already frees them on the next rebuild. Detach the old
    // model's arrays so the shared mesh buffers stay alive and owned by the
    // local `mesh`.
    if (model.meshes != NULL) { RL_FREE(model.meshes); model.meshes = NULL; }
    if (model.materials != NULL)
    {
        // Free the material's map array but NOT the maps themselves (they are
        // the default texture/shader, shared and owned by raylib).
        if (model.materials[0].maps != NULL) { RL_FREE(model.materials[0].maps); model.materials[0].maps = NULL; }
        RL_FREE(model.materials); model.materials = NULL;
    }
    if (model.meshMaterial != NULL) { RL_FREE(model.meshMaterial); model.meshMaterial = NULL; }
    model.meshCount = 0;
    model.materialCount = 0;

    model = LoadModelFromMesh(mesh);

    // Remember raylib's default material shader (assigned by LoadModelFromMesh)
    // so Draw() can switch back to it for the flat-color fallback path when the
    // real splat shader/layer textures aren't available yet.
    if (model.materialCount > 0) {
        defaultMaterialShader = model.materials[0].shader;
        // Set a legacy fallback albedo so the mesh is never invisible on frame 1.
        if (terrainTexture.id > 0) {
            model.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = terrainTexture;
        }
    }

    if (splatmapDirty) UpdateSplatmapTexture();
    meshDirty = false;
}

// Central-difference surface normal at a single heightmap grid point. Shared
// by both the dense and adaptive mesh paths so that two quads meeting at the
// same grid index (whether from adjacent adaptive blocks or the dense grid)
// always agree on lighting there - no shading seam at merge boundaries.
Vector3 BasicTerrain::ComputeGridNormal(int x, int z) const {
    size_t vi = (size_t)(z * width + x);
    float h = heightmap[vi];
    float hL = (x > 0) ? heightmap[vi - 1] : h;
    float hR = (x < width - 1) ? heightmap[vi + 1] : h;
    float hD = (z > 0) ? heightmap[vi - width] : h;
    float hU = (z < depth - 1) ? heightmap[vi + width] : h;

    Vector3 n = { hL - hR, 2.0f * scale, hD - hU };
    return Vector3Normalize(n);
}

// The original always-full-resolution mesh generator: one vertex per
// heightmap sample, always (width-1)*(depth-1)*2 triangles. Kept as the
// guaranteed-correct fallback for BuildAdaptiveMesh().
void BasicTerrain::BuildDenseMesh() {
    if (mesh.vertices) UnloadMesh(mesh);
    mesh = { 0 };

    int vertCount = width * depth;
    int triCount = (width - 1) * (depth - 1) * 2;

    mesh.vertexCount = vertCount;
    mesh.triangleCount = triCount;

    mesh.vertices  = (float*)MemAlloc((size_t)vertCount * 3 * sizeof(float));
    mesh.normals   = (float*)MemAlloc((size_t)vertCount * 3 * sizeof(float));
    mesh.texcoords = (float*)MemAlloc((size_t)vertCount * 2 * sizeof(float));
    mesh.indices   = (unsigned short*)MemAlloc((size_t)triCount * 3 * sizeof(unsigned short));

    float halfW = width * scale * 0.5f;
    float halfD = depth * scale * 0.5f;

    for (int z = 0; z < depth; z++) {
        for (int x = 0; x < width; x++) {
            size_t vi = (size_t)(z * width + x);
            mesh.vertices[vi * 3 + 0] = x * scale - halfW;
            mesh.vertices[vi * 3 + 1] = heightmap[vi];
            mesh.vertices[vi * 3 + 2] = z * scale - halfD;

            mesh.texcoords[vi * 2 + 0] = ((float)x / (width - 1)) * textureTiling;
            mesh.texcoords[vi * 2 + 1] = ((float)z / (depth - 1)) * textureTiling;
        }
    }

    for (int z = 0; z < depth; z++) {
        for (int x = 0; x < width; x++) {
            size_t vi = (size_t)(z * width + x);
            Vector3 n = ComputeGridNormal(x, z);
            mesh.normals[vi * 3 + 0] = n.x;
            mesh.normals[vi * 3 + 1] = n.y;
            mesh.normals[vi * 3 + 2] = n.z;
        }
    }

    size_t ii = 0;
    for (int z = 0; z < depth - 1; z++) {
        for (int x = 0; x < width - 1; x++) {
            unsigned short a = (unsigned short)(z * width + x);
            unsigned short b = (unsigned short)(a + 1);
            unsigned short c = (unsigned short)(a + width);
            unsigned short d = (unsigned short)(c + 1);
            mesh.indices[ii++] = a; mesh.indices[ii++] = c; mesh.indices[ii++] = b;
            mesh.indices[ii++] = b; mesh.indices[ii++] = c; mesh.indices[ii++] = d;
        }
    }
}

// Tolerance, in world-space height units, for how far a heightmap sample may
// deviate from the flat plane implied by its block's 4 corners before that
// block is considered "not flat" and gets subdivided further. Small enough
// that any seam left behind at a merge boundary (bounded by this same value,
// see IsBlockFlat) is sub-visible.
static constexpr float kAdaptiveFlatEpsilon = 0.12f;

// Safety cap on the adaptive mesh's vertex count. Each merged block emits its
// own 4 corner vertices (not shared with neighbors), so a terrain that is
// rough at every scale and never collapses could in the worst case approach
// ~4x the dense mesh's vertex count. raylib's index buffer here is 16-bit, so
// bail out to the always-safe BuildDenseMesh() rather than risk overflowing
// it. In practice this only triggers on maximally noisy heightmaps (e.g. a
// raw imported noise texture) - any terrain with meaningful flat/planar
// regions stays well under this.
static constexpr size_t kAdaptiveMaxVertices = 60000;

bool BasicTerrain::IsBlockFlat(int x0, int z0, int x1, int z1) const {
    float h00 = heightmap[(size_t)z0 * width + x0];
    float h10 = heightmap[(size_t)z0 * width + x1];
    float h01 = heightmap[(size_t)z1 * width + x0];
    float h11 = heightmap[(size_t)z1 * width + x1];

    float invW = 1.0f / (float)(x1 - x0);
    float invD = 1.0f / (float)(z1 - z0);

    // Every interior sample (not just the corners) must lie close to the
    // bilinear plane the corners describe. This also correctly allows a
    // tilted-but-planar ramp to collapse (not just perfectly horizontal
    // ground), since deviation from the plane is ~0 for any true plane
    // regardless of slope.
    for (int z = z0; z <= z1; z++) {
        float v = (float)(z - z0) * invD;
        float hLo = h00 + (h01 - h00) * v;
        float hHi = h10 + (h11 - h10) * v;
        for (int x = x0; x <= x1; x++) {
            float u = (float)(x - x0) * invW;
            float expected = hLo + (hHi - hLo) * u;
            float actual = heightmap[(size_t)z * width + x];
            if (std::fabs(actual - expected) > kAdaptiveFlatEpsilon) return false;
        }
    }
    return true;
}

// Recursively collapses the [x0,z0]-[x1,z1] block (inclusive vertex-index
// range) into either a single flat quad, or two half-size sub-blocks split
// along whichever axis is currently longer (so long thin non-flat strips,
// e.g. a ridge, subdivide along their length rather than ballooning into a
// full grid). Splitting on plain index ranges (not requiring power-of-two
// dimensions) means this works for any width/depth, not just power-of-two
// terrain sizes.
void BasicTerrain::CollectAdaptiveBlock(int x0, int z0, int x1, int z1,
                                         std::vector<float>& verts, std::vector<float>& normals,
                                         std::vector<float>& uvs, std::vector<unsigned short>& indices) const {
    int cellsX = x1 - x0;
    int cellsZ = z1 - z0;

    // A single cell has no interior samples to deviate, so it's always
    // trivially "flat" here - it's already the smallest unit, identical to
    // what the dense grid would produce for that cell.
    bool flat = (cellsX <= 1 && cellsZ <= 1) || IsBlockFlat(x0, z0, x1, z1);

    if (!flat) {
        if (cellsX >= cellsZ) {
            int xm = x0 + cellsX / 2;
            CollectAdaptiveBlock(x0, z0, xm, z1, verts, normals, uvs, indices);
            CollectAdaptiveBlock(xm, z0, x1, z1, verts, normals, uvs, indices);
        } else {
            int zm = z0 + cellsZ / 2;
            CollectAdaptiveBlock(x0, z0, x1, zm, verts, normals, uvs, indices);
            CollectAdaptiveBlock(x0, zm, x1, z1, verts, normals, uvs, indices);
        }
        return;
    }

    // Emit a single quad (2 triangles) for this block, using the *real*
    // heightmap values at its 4 corners (not the interpolated plane), so a
    // merged quad's corners always land exactly on true terrain samples.
    float halfW = width * scale * 0.5f;
    float halfD = depth * scale * 0.5f;

    unsigned short base = (unsigned short)(verts.size() / 3);
    const int cx[4] = { x0, x1, x0, x1 };
    const int cz[4] = { z0, z0, z1, z1 };
    for (int i = 0; i < 4; i++) {
        int x = cx[i], z = cz[i];
        float h = heightmap[(size_t)z * width + x];
        verts.push_back(x * scale - halfW);
        verts.push_back(h);
        verts.push_back(z * scale - halfD);

        Vector3 n = ComputeGridNormal(x, z);
        normals.push_back(n.x); normals.push_back(n.y); normals.push_back(n.z);

        uvs.push_back(((float)x / (width - 1)) * textureTiling);
        uvs.push_back(((float)z / (depth - 1)) * textureTiling);
    }
    // Corner order: 0=TL(x0,z0) 1=TR(x1,z0) 2=BL(x0,z1) 3=BR(x1,z1); same
    // winding as the dense grid's (a,c,b)/(b,c,d) triangles.
    indices.push_back(base + 0); indices.push_back(base + 2); indices.push_back(base + 1);
    indices.push_back(base + 1); indices.push_back(base + 2); indices.push_back(base + 3);
}

// Adaptive mesh builder: quadtree-collapses locally-flat regions into single
// quads. Returns false (leaving `mesh` untouched) if the result wouldn't
// actually be safe/useful, so the caller can fall back to BuildDenseMesh().
bool BasicTerrain::BuildAdaptiveMesh() {
    std::vector<float> verts, normals, uvs;
    std::vector<unsigned short> indices;
    verts.reserve(1024); normals.reserve(1024); uvs.reserve(512); indices.reserve(1024);

    CollectAdaptiveBlock(0, 0, width - 1, depth - 1, verts, normals, uvs, indices);

    size_t vertCount = verts.size() / 3;
    if (vertCount == 0 || vertCount > kAdaptiveMaxVertices) return false;

    if (mesh.vertices) UnloadMesh(mesh);
    mesh = { 0 };

    mesh.vertexCount = (int)vertCount;
    mesh.triangleCount = (int)(indices.size() / 3);

    mesh.vertices  = (float*)MemAlloc(verts.size() * sizeof(float));
    mesh.normals   = (float*)MemAlloc(normals.size() * sizeof(float));
    mesh.texcoords = (float*)MemAlloc(uvs.size() * sizeof(float));
    mesh.indices   = (unsigned short*)MemAlloc(indices.size() * sizeof(unsigned short));

    std::copy(verts.begin(), verts.end(), mesh.vertices);
    std::copy(normals.begin(), normals.end(), mesh.normals);
    std::copy(uvs.begin(), uvs.end(), mesh.texcoords);
    std::copy(indices.begin(), indices.end(), mesh.indices);

    return true;
}


// (Re)upload the splatmap CPU buffer to the GPU texture. Cheap enough to call
// per paint frame; independent of the full mesh rebuild.
void BasicTerrain::UpdateSplatmapTexture() {
    if (splatmapTexture.id > 0 && splatmapTexture.width == splatWidth && splatmapTexture.height == splatDepth) {
        // Same size: update the existing texture in place. Recreating a 2048x2048
        // texture on every paint frame costs a GPU allocation (~16 MB) each time.
        UpdateTexture(splatmapTexture, splatmap.data());
    } else {
        if (splatmapTexture.id > 0) UnloadTexture(splatmapTexture);
        Image splatImg = { 0 };
        splatImg.data = splatmap.data();
        splatImg.width = splatWidth;
        splatImg.height = splatDepth;
        splatImg.mipmaps = 1;
        splatImg.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        splatmapTexture = LoadTextureFromImage(splatImg);
        SetTextureWrap(splatmapTexture, TEXTURE_WRAP_CLAMP);
        // Bilinear (not point) filtering is what actually makes layer transitions
        // look like a smooth blend instead of hard, blocky steps between texels.
        SetTextureFilter(splatmapTexture, TEXTURE_FILTER_BILINEAR);
    }
    splatmapDirty = false;
    paintColorDirty = true;
}

// Bake the splatmap weight data into a single solid-color image (one color per
// layer) and upload it for display via the DEFAULT raylib shader. This is a
// guaranteed-visible basic paint tool that does not depend on a custom shader
// or multi-texture binding.
void BasicTerrain::UpdatePaintColorTexture() {
    if (splatmap.empty()) return;

    std::vector<unsigned char> colorData((size_t)splatWidth * splatDepth * 4, 0);
    for (size_t i = 0; i < (size_t)splatWidth * splatDepth; i++) {
        uint8_t* s = &splatmap[i * 4];
        long r = 0, g = 0, b = 0;
        int layer = 4;
        unsigned char maxW = 0;
        // Choose the dominant layer for a clean, readable paint result.
        for (int L = 0; L < 4; L++) {
            if (s[L] > maxW) { maxW = s[L]; layer = L; }
        }
        if (layer < 4 && s[layer] > 0) {
            r = s_layerColors[layer].r;
            g = s_layerColors[layer].g;
            b = s_layerColors[layer].b;
        } else {
            r = s_layerColors[0].r; g = s_layerColors[0].g; b = s_layerColors[0].b;
        }
        colorData[i * 4 + 0] = (unsigned char)r;
        colorData[i * 4 + 1] = (unsigned char)g;
        colorData[i * 4 + 2] = (unsigned char)b;
        colorData[i * 4 + 3] = 255;
    }

    if (paintColorTexture.id > 0 && paintColorTexture.width == splatWidth && paintColorTexture.height == splatDepth) {
        UpdateTexture(paintColorTexture, colorData.data());
    } else {
        if (paintColorTexture.id > 0) UnloadTexture(paintColorTexture);
        Image img = { 0 };
        img.data = colorData.data();
        img.width = splatWidth;
        img.height = splatDepth;
        img.mipmaps = 1;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        paintColorTexture = LoadTextureFromImage(img);
        SetTextureWrap(paintColorTexture, TEXTURE_WRAP_CLAMP);
        SetTextureFilter(paintColorTexture, TEXTURE_FILTER_BILINEAR);
    }
    paintColorDirty = false;
}

void BasicTerrain::Update(float dt) {
    if (meshDirty) RebuildMesh();
    else if (splatmapDirty) UpdateSplatmapTexture();
    // The flat-colour paint texture is only shown by the fallback (no layer
    // textures) path, and Draw() bakes it on demand. Baking it here as well cost
    // a full 2048x2048 CPU pass on every paint frame even with the shader path.

    brushValid = false;

    Camera3D* cam = s_activeCamera;
    if (!cam) return;

    bool overUI = ui::IsMouseOverUI();

    // Skip input handling if TerrainEditor is actively editing a terrain
    if (s_editorActive) {
        // Still update brush preview for visual feedback, but don't handle input
        // Track the brush across the WHOLE terrain...
    }
    
    Ray ray = GetMouseRay(GetMousePosition(), *cam);
    Vector3 hitPoint{};
    bool hit = Raycast(ray, nullptr, &hitPoint, nullptr);

    // Track the brush across the WHOLE terrain. If the heightfield raycast
    // misses for any reason, fall back to a guaranteed analytic ground-plane
    // intersection so the brush/paint always works everywhere on the footprint.
    Vector3 pick;
    bool pickValid = hit;
    if (pickValid) {
        pick = hitPoint;
    } else if (!overUI && fabsf(ray.direction.y) > 1e-5f) {
        float t = (position.y - ray.position.y) / Vector3Normalize(ray.direction).y;
        if (t < 0.0f) t = 0.0f;
        Vector3 p = Vector3Add(ray.position, Vector3Scale(Vector3Normalize(ray.direction), t));
        float lx = (p.x - position.x) / scale + width * 0.5f;
        float lz = (p.z - position.z) / scale + depth * 0.5f;
        if (lx >= 0.0f && lz >= 0.0f && lx <= (float)width && lz <= (float)depth) {
            pick = p;
            pick.y = GetHeightAt(p.x, p.z);
            pickValid = true;
        }
    }

    if (!overUI && pickValid) {
        brushWorldPos = { pick.x, pick.z };
        brushValid = true;
    }

    // Click on terrain -> select, activate the tool panel & stamp once.
    // A Ramp press starts a drag instead of a stamp (committed on release).
    // Skip if TerrainEditor is handling input for this terrain
    bool editorHandling = s_editorActive && s_active == this;
    bool pressedThisFrame = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    if (s_brush.tool != Tool::Ramp) rampDragging = false;   // cancel dangling drags

    if (!editorHandling && !overUI && pressedThisFrame && !ui::WasUIClickConsumed()) {
        if (brushValid && s_brush.tool == Tool::Ramp) {
            editActive = true;
            s_active = this;
            if (!isSelected) ui::SetSelectedTerrain(this);
            ui::MarkUIClickConsumed();
            rampStart = brushWorldPos;
            rampDragging = true;
        } else if (brushValid && s_brush.tool != Tool::None) {
            editActive = true;
            s_active = this;
            if (!isSelected) ui::SetSelectedTerrain(this);
            if (s_brush.tool != Tool::None) ui::MarkUIClickConsumed();
            Brush b = s_brush;
            if (b.tool == Tool::Paint) {
                // Paint tool: Ctrl toggles erase mode
                if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
                    b.paintErase = !b.paintErase;
                }
            } else {
                // Sculpt tools: Ctrl inverts raise<->lower
                bool invert = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
                if (invert) {
                    if (b.tool == Tool::Raise) b.tool = Tool::Lower;
                    else if (b.tool == Tool::Lower) b.tool = Tool::Raise;
                }
            }
            StampBrush(b, brushWorldPos);   // small instant stamp
        } else {
            editActive = false;
        }
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        if (rampDragging) {
            // Commit the ramp from the capture point to the current cursor.
            // brushWorldPos may be slightly stale if the cursor left the
            // footprint mid-drag; ApplyRamp clamps all grid coords anyway.
            ApplyRamp(rampStart, brushWorldPos, s_brush.strength, s_brush.radius * 2.0f);
            rampDragging = false;
        }
        editActive = false;
    }

    // If this terrain is not the active/selected one, suppress editing
    if (s_active != this) {
        editActive = false;
    }

    // Skip drag handling if TerrainEditor is handling input for this terrain
    if (editActive && !overUI && IsMouseButtonDown(MOUSE_BUTTON_LEFT) && brushValid &&
        s_brush.tool != Tool::None && s_brush.tool != Tool::Ramp && !editorHandling) {
        // Drag continues painting gradually (rate-based, frame-rate independent).
        Brush b = s_brush;
        if (b.tool == Tool::Paint) {
            // Paint tool: Ctrl toggles erase, Shift = extra strength
            if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
                b.paintErase = !b.paintErase;
            }
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                b.strength *= 2.5f;
            }
            ApplyPaintBrush(b, brushWorldPos, dt);
        } else {
            // Sculpt tools: Ctrl inverts raise<->lower, Shift = extra strength
            bool invert = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
            if (invert) {
                if (b.tool == Tool::Raise) b.tool = Tool::Lower;
                else if (b.tool == Tool::Lower) b.tool = Tool::Raise;
            }
            if (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) {
                b.strength *= 2.5f;
            }
            ApplyBrush(b, brushWorldPos, dt);
        }
    }

    // CS2-style hotkeys: [ and ] shrink/grow the brush
    if (s_active == this && !ui::IsEditingText()) {
        if (IsKeyPressed(KEY_LEFT_BRACKET))  s_brush.radius = std::clamp(s_brush.radius - 4.0f, 4.0f, 200.0f);
        if (IsKeyPressed(KEY_RIGHT_BRACKET)) s_brush.radius = std::clamp(s_brush.radius + 4.0f, 4.0f, 200.0f);
    }
}

void BasicTerrain::Draw() {
    if (model.meshCount == 0 || model.materialCount == 0) return;

    // Frustum culling for the entire terrain
    Frustum frustum = Frustum::ExtractCurrent();
    BoundingBox bounds = GetCullBounds();
    if (!frustum.Intersects(bounds)) {
        gfx::IncrementCulledEntityCount(1);
        return;
    }
    gfx::IncrementRenderedEntityCount(1);

    Material& mat = model.materials[0];

    // Real splat-blended path requires: a compiled terrain shader, at least one
    // layer texture loaded, an uploaded splatmap texture, and valid uniform
    // locations for the two most essential uniforms.
    bool useShader = IsShaderValid(s_terrainShader) && s_layerCount > 0 &&
                      splatmapTexture.id > 0 && s_shaderSplatmapLoc >= 0 &&
                      s_shaderLayerCountLoc >= 0;

    if (!useShader) {
        // Fallback: no layer textures / shader yet. Bake the dominant splat
        // layer into a flat-color texture and draw through raylib's default
        // material shader so the terrain is never invisible.
        if (paintColorTexture.id == 0 || paintColorDirty) UpdatePaintColorTexture();

        mat.shader = defaultMaterialShader;
        mat.maps[MATERIAL_MAP_ALBEDO].texture =
            (paintColorTexture.id > 0) ? paintColorTexture : terrainTexture;
        mat.maps[MATERIAL_MAP_ALBEDO].color = WHITE;
        // Clear any layer/splatmap textures a previous shaded frame may have
        // left bound in these slots, so they don't get redundantly rebound.
        for (int i = 1; i <= 4; i++) mat.maps[i].texture = Texture2D{0};

        DrawModel(model, position, 1.0f, WHITE);
        gfx::IncrementDrawCallCount(1);
        gfx::AddMeshCount(1);
        return;
    }

    // --- Real multi-texture splat blend, through terrain_paint.vert/frag ---
    //
    // raylib's DrawMesh() walks every material map slot (0..MAX_MATERIAL_MAPS)
    // and, for each one with a bound texture, activates that GL texture unit
    // and writes the unit index into material.shader.locs[SHADER_LOC_MAP_ALBEDO
    // + slot]. That's how we get more than raylib's usual handful of
    // auto-bound PBR maps: we park our 4 layer textures + the splatmap in maps
    // 0-4 and point each slot's shader.locs entry at the *actual* uniform
    // location for "albedoTex0".."albedoTex3"/"splatmap" (found once via
    // GetShaderLocation when the shader was loaded), since those names don't
    // match raylib's default map naming and would otherwise resolve to -1.
    mat.shader = s_terrainShader;

    for (int i = 0; i < 4; i++) {
        mat.maps[i].texture = (i < s_layerCount) ? s_layerTextures[i] : s_layerTextures[0];
        if (s_shaderAlbedoLocs[i] >= 0) {
            mat.shader.locs[SHADER_LOC_MAP_ALBEDO + i] = s_shaderAlbedoLocs[i];
        }
    }
    mat.maps[4].texture = splatmapTexture;
    mat.shader.locs[SHADER_LOC_MAP_ALBEDO + 4] = s_shaderSplatmapLoc;

    int layerCount = s_layerCount;
    SetShaderValue(s_terrainShader, s_shaderLayerCountLoc, &layerCount, SHADER_UNIFORM_INT);
    if (s_shaderTextureTilingLoc >= 0) {
        SetShaderValue(s_terrainShader, s_shaderTextureTilingLoc, &textureTiling, SHADER_UNIFORM_FLOAT);
    }

    // TODO: these are a fixed daylight approximation. Graphics.hpp doesn't
    // currently expose the live sun direction/color/ambient that gfx::
    // UpdateLighting() feeds into GetLitShader(), so this shader can't match
    // the rest of the scene's lighting yet. Add getters to gfx (e.g.
    // gfx::GetLightDir()/GetLightColor()/GetAmbientColor()) and swap these in
    // if you want the terrain to relight consistently with everything else.
    Vector3 lightDir = Vector3Normalize(Vector3{ -0.35f, -0.85f, -0.35f });
    Vector3 lightColor = { 1.0f, 0.97f, 0.90f };
    Vector3 ambientColor = { 0.35f, 0.38f, 0.42f };
    if (s_shaderLightDirLoc >= 0) SetShaderValue(s_terrainShader, s_shaderLightDirLoc, &lightDir, SHADER_UNIFORM_VEC3);
    if (s_shaderLightColorLoc >= 0) SetShaderValue(s_terrainShader, s_shaderLightColorLoc, &lightColor, SHADER_UNIFORM_VEC3);
    if (s_shaderAmbientColorLoc >= 0) SetShaderValue(s_terrainShader, s_shaderAmbientColorLoc, &ambientColor, SHADER_UNIFORM_VEC3);

    DrawModel(model, position, 1.0f, WHITE);
    gfx::IncrementDrawCallCount(1);
    gfx::AddMeshCount(1);
}

bool BasicTerrain::IsVisible(const Frustum& frustum) const {
    return frustum.Intersects(GetCullBounds());
}

BoundingBox BasicTerrain::GetCullBounds() const {
    float hw = width * scale * 0.5f;
    float hd = depth * scale * 0.5f;
    BoundingBox bounds;
    bounds.min = { position.x - hw, minHeight, position.z - hd };
    bounds.max = { position.x + hw, maxHeight, position.z + hd };
    return bounds;
}

void BasicTerrain::DrawOverlay3D() {
    if (!editActive && !isSelected) return;

    // Yellow outline around the terrain footprint so selection is obvious
    float hw = width * scale * 0.5f;
    float hd = depth * scale * 0.5f;
    float y = position.y + 0.3f;
    Vector3 c[4] = {
        { position.x - hw, y, position.z - hd },
        { position.x + hw, y, position.z - hd },
        { position.x + hw, y, position.z + hd },
        { position.x - hw, y, position.z + hd },
    };
    for (int i = 0; i < 4; i++) DrawLine3D(c[i], c[(i + 1) % 4], YELLOW);

    // Procedural generation region preview: a movable/resizable box, shown
    // only while the Generate tab is open (see ui.cpp's DrawTerrainToolPanel).
    if (showGenBox) {
        BoundingBox gb = {
            { genBoxPos.x - genBoxSize.x * 0.5f, genBoxPos.y - genBoxSize.y * 0.5f, genBoxPos.z - genBoxSize.z * 0.5f },
            { genBoxPos.x + genBoxSize.x * 0.5f, genBoxPos.y + genBoxSize.y * 0.5f, genBoxPos.z + genBoxSize.z * 0.5f }
        };
        DrawBoundingBox(gb, Color{ 80, 220, 120, 255 });
    }

    // Show Wireframe draws the *true* full-resolution heightmap grid directly
    // from `heightmap`, not the current render mesh. The render mesh is now
    // adaptive (RebuildMesh() collapses flat regions into single quads), so
    // DrawModelWires(model, ...) would only show whatever's left of that -
    // e.g. a couple of diagonal lines across a flat terrain. Drawing the grid
    // straight from the heightmap keeps this a reliable per-vertex reference
    // view regardless of how aggressively the render mesh has been collapsed.
    if (showWireframe) {
        float halfW = width * scale * 0.5f;
        float halfD = depth * scale * 0.5f;

        rlBegin(RL_LINES);
        rlColor4ub(255, 0, 0, 255);

        // Lines running along X, one per row (constant z)
        for (int z = 0; z < depth; z++) {
            for (int x = 0; x < width - 1; x++) {
                float h0 = heightmap[(size_t)z * width + x];
                float h1 = heightmap[(size_t)z * width + x + 1];
                rlVertex3f(position.x + x * scale - halfW,       position.y + h0, position.z + z * scale - halfD);
                rlVertex3f(position.x + (x + 1) * scale - halfW, position.y + h1, position.z + z * scale - halfD);
            }
        }
        // Lines running along Z, one per column (constant x)
        for (int x = 0; x < width; x++) {
            for (int z = 0; z < depth - 1; z++) {
                float h0 = heightmap[(size_t)z * width + x];
                float h1 = heightmap[(size_t)(z + 1) * width + x];
                rlVertex3f(position.x + x * scale - halfW, position.y + h0, position.z + z * scale - halfD);
                rlVertex3f(position.x + x * scale - halfW, position.y + h1, position.z + (z + 1) * scale - halfD);
            }
        }

        rlEnd();
    }

    // Brush outline that follows the terrain surface (shape-aware).
    // The Ramp tool gets its own band preview instead of the ring.
    if (editActive && brushValid && s_brush.tool != Tool::Ramp) {
        Color ring = Color{ 60, 140, 255, 220 };
        if (s_brush.shape == Shape::Square) {
            const int steps = 48;
            Vector3 prev{}; bool has = false;
            for (int i = 0; i <= steps; i++) {
                float s = (float)i / steps * 4.0f;
                int side = (int)s; float f = s - side;
                float u, v;
                switch (side) {
                    case 0: u = -1.0f + f * 2.0f; v = -1.0f; break;
                    case 1: u = 1.0f;             v = -1.0f + f * 2.0f; break;
                    case 2: u = 1.0f - f * 2.0f;  v = 1.0f; break;
                    default: u = -1.0f;           v = 1.0f - f * 2.0f; break;
                }
                float px = brushWorldPos.x + u * s_brush.radius;
                float pz = brushWorldPos.y + v * s_brush.radius;
                Vector3 p = { px, GetHeightAt(px, pz) + 0.4f, pz };
                if (has) DrawLine3D(prev, p, ring);
                prev = p; has = true;
            }
        } else {
            const int seg = 40;
            Vector3 prev{};
            for (int i = 0; i <= seg; i++) {
                float ang = (float)i / seg * 2.0f * PI;
                float px = brushWorldPos.x + cosf(ang) * s_brush.radius;
                float pz = brushWorldPos.y + sinf(ang) * s_brush.radius;
                Vector3 p = { px, GetHeightAt(px, pz) + 0.4f, pz };
                if (i > 0) DrawLine3D(prev, p, ring);
                prev = p;
            }
        }
    } else if (editActive && brushValid && s_brush.tool == Tool::Ramp) {
        // Ramp preview: a live band from the capture point to the cursor.
        // Before the drag starts the capture point is the cursor itself, so
        // the preview collapses to a small marker near the brush.
        const Vector2 a = rampDragging ? rampStart : brushWorldPos;
        const float hA = GetHeightAt(a.x, a.y);
        const float hB = GetHeightAt(brushWorldPos.x, brushWorldPos.y);
        const Vector3 pa = { a.x, hA + 0.4f, a.y };
        const Vector3 pb = { brushWorldPos.x, hB + 0.4f, brushWorldPos.y };

        const Color rampCol = Color{ 255, 170, 60, 230 };
        const float hw = s_brush.radius;

        // Direction + perpendicular across the band (world XZ)
        Vector2 d = { brushWorldPos.x - a.x, brushWorldPos.y - a.y };
        const float len = sqrtf(d.x * d.x + d.y * d.y);
        Vector2 perp{ 0.0f, 0.0f };
        if (len > 1e-4f) { perp.x = -d.y / len; perp.y = d.x / len; }

        Vector3 q00 = { a.x + perp.x * hw, GetHeightAt(a.x + perp.x * hw, a.y + perp.y * hw) + 0.4f, a.y + perp.y * hw };
        Vector3 q01 = { a.x - perp.x * hw, GetHeightAt(a.x - perp.x * hw, a.y - perp.y * hw) + 0.4f, a.y - perp.y * hw };
        Vector3 q10 = { brushWorldPos.x + perp.x * hw, GetHeightAt(brushWorldPos.x + perp.x * hw, brushWorldPos.y + perp.y * hw) + 0.4f, brushWorldPos.y + perp.y * hw };
        Vector3 q11 = { brushWorldPos.x - perp.x * hw, GetHeightAt(brushWorldPos.x - perp.x * hw, brushWorldPos.y - perp.y * hw) + 0.4f, brushWorldPos.y - perp.y * hw };

        DrawLine3D(q00, q10, rampCol);
        DrawLine3D(q10, q11, rampCol);
        DrawLine3D(q11, q01, rampCol);
        DrawLine3D(q01, q00, rampCol);
        if (rampDragging) {
            DrawLine3D(pa, pb, rampCol);
            // Start marker: small cross on the surface
            DrawLine3D({ pa.x - 1.5f, pa.y, pa.z }, { pa.x + 1.5f, pa.y, pa.z }, rampCol);
            DrawLine3D({ pa.x, pa.y, pa.z - 1.5f }, { pa.x, pa.y, pa.z + 1.5f }, rampCol);
        } else {
            // Hovering with no drag yet: show the press-start marker
            DrawLine3D({ pa.x - 1.5f, pa.y + 1.5f, pa.z }, { pa.x + 1.5f, pa.y - 1.5f, pa.z }, rampCol);
            DrawLine3D({ pa.x - 1.5f, pa.y - 1.5f, pa.z }, { pa.x + 1.5f, pa.y + 1.5f, pa.z }, rampCol);
        }
    }
}