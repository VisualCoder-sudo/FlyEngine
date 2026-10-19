#include "../../include/Graphics/Graphics.hpp"
#include "../../include/Engine/CityMeshes.hpp"
#include "../../include/Graphics/Lighting.hpp"
#include "../Engine/ShadowsInternal.hpp"
#include "../Engine/GraphicsInternal.hpp"
#include "../../include/Graphics/Atmosphere.hpp"
#include "../../include/Graphics/Clouds.hpp"
#include "../../include/Graphics/PostFX.hpp"
#include "../../include/Engine/TechnicalTools.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "../../include/Engine/Frontend/ui.hpp"
#include "../../include/Engine/Platform/Platform.hpp"
#include <cmath>
#include <map>
#include <unordered_map>
#include <string>

namespace {

constexpr int CLOUD_SHADOW_TEXTURE_SLOT = 12;




bool gridVisible = true;
bool wireframe = false;
// Raise the camera near plane with altitude: depth precision grows ~1/near, so a
// higher camera can afford a much larger near plane and far-away layers stop
// z-fighting. rlSetClipPlanes()/rlGetCullDistanceFar() only exist from raylib
// 5.5 onward; on older raylib this is a no-op -- the road shader's bias still
// adapts on its own.
#if defined(RAYLIB_VERSION_MAJOR) && \
    (RAYLIB_VERSION_MAJOR > 5 || (RAYLIB_VERSION_MAJOR == 5 && RAYLIB_VERSION_MINOR >= 5))
void ApplyNearPlane(double nearP) {
    rlSetClipPlanes(nearP, rlGetCullDistanceFar());
}
#else
void ApplyNearPlane(double) {}
#endif



Shader litShader{};
Texture2D defaultTexture{};
Model cubeModel{};
Model sphereModel{};
Model cylinderModel{};
Model wedgeModel{};
// Uniforms the three lit programs share (shaders/lit.glsl); -1 where a program has no such uniform.
struct LitLocs {
    int sunDir = -1, sunColor = -1, ambientSky = -1, ambientGround = -1, skyColor = -1;
    int lightVP = -1, shadowMap = -1, waterSurfaceY = -1;
    int night = -1, lightCount = -1, lights = -1, weather = -1, camPos = -1;
    int cloudShadow = -1, cloudShadowTex = -1;
    int shadowVP = -1, shadowMapFar[3] = { -1, -1, -1 };
};
LitLocs litLocs, roadLocs, cityLocs, shapeLocs;
void FindLitLocs(Shader& sh, LitLocs& l) {
    l.sunDir = GetShaderLocation(sh, "sunDir");
    l.sunColor = GetShaderLocation(sh, "sunColor");
    l.ambientSky = GetShaderLocation(sh, "ambientSky");
    l.ambientGround = GetShaderLocation(sh, "ambientGround");
    l.skyColor = GetShaderLocation(sh, "skyColor");
    l.lightVP = GetShaderLocation(sh, "lightVP");
    l.shadowMap = GetShaderLocation(sh, "shadowMap");
    l.waterSurfaceY = GetShaderLocation(sh, "waterSurfaceY");
    l.night = GetShaderLocation(sh, "nightAmount");
    l.lightCount = GetShaderLocation(sh, "lightCount");
    l.lights = GetShaderLocation(sh, "nightLights");
    l.weather = GetShaderLocation(sh, "weather");
    l.camPos = GetShaderLocation(sh, "camPos");
    l.cloudShadow = GetShaderLocation(sh, "cloudShadow");
    l.cloudShadowTex = GetShaderLocation(sh, "cloudShadowTex");
    l.shadowVP = GetShaderLocation(sh, "shadowVP");
    l.shadowMapFar[0] = GetShaderLocation(sh, "shadowMap1");
    l.shadowMapFar[1] = GetShaderLocation(sh, "shadowMap2");
    l.shadowMapFar[2] = GetShaderLocation(sh, "shadowMap3");
    int shadowSlot = gfx::shadow::kTextureSlot;
    if (l.shadowMap != -1) SetShaderValue(sh, l.shadowMap, &shadowSlot, SHADER_UNIFORM_INT);
    // A texture unit nothing else binds: with no clouds the sampler reads the default white texture.
    int cloudSlot = CLOUD_SHADOW_TEXTURE_SLOT;
    if (l.cloudShadowTex != -1) SetShaderValue(sh, l.cloudShadowTex, &cloudSlot, SHADER_UNIFORM_INT);
}
// Instanced-lit shader + material used for city buildings.
Shader cityInstancedShader{};
Material cityInstancedMaterial{};
bool cityInstancedReady = false;
float nightLightData[32 * 4] = {};
// Road shader: lit fragment shader with a depth-biased vertex stage, used only
// for the flat road/pad/parks mesh so it wins the depth test against the
// near-coplanar ground plane at altitude.
Shader roadShader{};
// Instanced variant of the plain lit shader for loose objects (ShapeBatch.cpp).
Shader shapeInstancedShader{};
Material shapeInstancedMaterial{};
bool shapeInstancedReady = false;
bool initialized = false;

// The lit, road and instanced shaders live in shaders/lit.glsl and are compiled
// into the binary by sokol-shdc (programs "lit", "lit_road", "lit_instanced").

}

namespace gfx {

Matrix ComposeTRS(Vector3 scale, Matrix rotation, Vector3 position) {
    const Matrix scaleMat = MatrixScale(scale.x, scale.y, scale.z);
    const Matrix transMat = MatrixTranslate(position.x, position.y, position.z);
    return MatrixMultiply(MatrixMultiply(scaleMat, rotation), transMat);
}

void Init() {
    if (initialized) return;

    // Shaders are compiled into the binary (shaders/lit.glsl via sokol-shdc).
    litShader = LoadShaderProgram("lit");
    FindLitLocs(litShader, litLocs);

    // Road shader: same lighting as litShader but with a depth bias so the flat
    // road mesh always wins against the near-coplanar ground plane at altitude.
    roadShader = LoadShaderProgram("lit_road");
    // A failed link falls back to the default shader and silently loses the depth bias (roads re-fight the ground).
    if (!IsShaderValid(roadShader))
        TraceLog(LOG_ERROR, "ROAD SHADER LINK FAILED (id=0): depth bias inactive, roads will z-fight the ground at altitude");
    else
        TraceLog(LOG_INFO, "Road shader linked OK (id=%i): depth bias active.", roadShader.id);
    // DrawMesh uploads matProjection to shaders whose projection loc is registered.
    if (roadShader.id != 0 && roadShader.locs)
        roadShader.locs[SHADER_LOC_MATRIX_PROJECTION] = GetShaderLocation(roadShader, "matProjection");
    FindLitLocs(roadShader, roadLocs);

    // Instanced lit shader (city buildings). Own uniforms mirror litShader's.
    cityInstancedShader = LoadShaderProgram("lit_instanced");
    if (cityInstancedShader.id != 0) FindLitLocs(cityInstancedShader, cityLocs);

    // Load the shared default texture BEFORE any material grabs it, otherwise
    // materials keep texture id 0 and the shaders sample an unbound unit (black).
    Image checker = GenImageChecked(64, 64, 8, 8, LIGHTGRAY, GRAY);
    defaultTexture = LoadTextureFromImage(checker);
    UnloadImage(checker);

    shapeInstancedShader = LoadShaderProgram("lit_shape_instanced");
    if (shapeInstancedShader.id != 0) FindLitLocs(shapeInstancedShader, shapeLocs);

    cityInstancedMaterial = LoadMaterialDefault();
    cityInstancedMaterial.shader = cityInstancedShader;
    cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].texture = defaultTexture;
    cityInstancedReady = cityInstancedShader.id != 0;
    shapeInstancedMaterial = LoadMaterialDefault();
    shapeInstancedMaterial.shader = shapeInstancedShader;
    shapeInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].texture = defaultTexture;
    shapeInstancedReady = shapeInstancedShader.id != 0;

    cubeModel = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
    sphereModel = LoadModelFromMesh(GenMeshSphere(0.5f, 16, 16));
    cylinderModel = LoadModelFromMesh(GenMeshCylinder(0.5f, 1.0f, 16));
    wedgeModel = LoadModelFromMesh(GenerateWedgeShapeMesh());
    InitCityMeshes();

    Model* models[] = { &cubeModel, &sphereModel, &cylinderModel, &wedgeModel };
    for (Model* model : models) {
        model->materials[0].shader = litShader;
        model->materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = defaultTexture;
    }

    shadow::Init();


    InitReflections();

    InitPostFX();

    initialized = true;
}

void Shutdown() {
    if (!initialized) return;

    ShutdownPostFX();
    UnloadModel(cubeModel);
    UnloadModel(sphereModel);
    UnloadModel(cylinderModel);
    UnloadModel(wedgeModel);
    ShutdownCityMeshes();
    UnloadTexture(defaultTexture);
    shadow::Shutdown();
    ShutdownReflections();
    if (cityInstancedShader.id != 0) UnloadShader(cityInstancedShader);
    if (roadShader.id != 0) UnloadShader(roadShader);
    cityInstancedReady = false;
    cityInstancedMaterial = {};
    if (shapeInstancedShader.id != 0) UnloadShader(shapeInstancedShader);
    shapeInstancedReady = false;
    shapeInstancedMaterial = {};
    UnloadShader(litShader);

    initialized = false;
}

Shader& GetLitShader() { return litShader; }

Shader& GetRoadShader() { return roadShader; }

Model& GetShapeModel(ShapeType type) {
    switch (type) {
        case ShapeType::Sphere:   return sphereModel;
        case ShapeType::Cylinder: return cylinderModel;
        case ShapeType::Wedge:    return wedgeModel;
        default:                  return cubeModel;
    }
}

Texture2D GetDefaultTexture() { return defaultTexture; }

// Unit building meshes used by the city instancer. Shape 0 is a plain box.
Mesh GetCityShapeMesh(int shape) {
    const Mesh* mesh = CityShapeMesh(shape);
    return mesh ? *mesh : cubeModel.meshes[0];
}


void SetGridVisible(bool visible) { gridVisible = visible; }

bool IsGridVisible() { return gridVisible; }

void SetWireframe(bool enabled) {
    wireframe = enabled;
    if (wireframe) rlEnableWireMode();
    else rlDisableWireMode();
}

bool IsWireframe() { return wireframe; }

void DrawModelOutline(const Model& model, const Matrix& transform) {
    static Shader outlineShader = {};
    static Material outlineMaterial = {};
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        outlineShader = LoadShaderProgram("outline");
        outlineMaterial = LoadMaterialDefault();
        outlineMaterial.shader = outlineShader;
    }
    if (outlineShader.id == 0 || model.meshCount <= 0) return;

    const RenderQuality& q = Quality();
    const float invViewport[2] = { 2.0f / (float)GetScreenWidth(), 2.0f / (float)GetScreenHeight() };
    SetShaderValue(outlineShader, GetShaderLocation(outlineShader, "invViewport"), invViewport, SHADER_UNIFORM_VEC2);
    SetShaderValue(outlineShader, GetShaderLocation(outlineShader, "thickness"), &q.outlineThickness, SHADER_UNIFORM_FLOAT);
    outlineMaterial.maps[MATERIAL_MAP_DIFFUSE].color = {
        (unsigned char)(q.outlineColor[0] * 255.0f + 0.5f), (unsigned char)(q.outlineColor[1] * 255.0f + 0.5f),
        (unsigned char)(q.outlineColor[2] * 255.0f + 0.5f), 255 };

    rlEnableFrontCull();
    for (int i = 0; i < model.meshCount; ++i) DrawMesh(model.meshes[i], outlineMaterial, transform);
    rlDisableFrontCull();
}



namespace {
uint32_t RainHash(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
}

// Rain: streaks that fall through a box around the camera. Each drop's place is a function of its index and the
// time only, wrapped around the camera, so the rain stays put in the world while you move through it.
void DrawWeather(const Camera3D& camera) {
    const float rain = RainNow();
    if (rain < 0.02f || IsInShadowPass() || IsInReflectionPass()) return;
    const int count = (int)(300.0f + 2300.0f * rain);
    const float box = 44.0f, top = 14.0f, depth = 30.0f;
    const float t = (float)GetTime();
    const float speed = 15.0f + 6.0f * rain;
    const Color sky = CurrentSky();
    const Color col = { (unsigned char)(sky.r + (255 - sky.r) * 0.55f), (unsigned char)(sky.g + (255 - sky.g) * 0.55f),
                        (unsigned char)(sky.b + (255 - sky.b) * 0.55f), (unsigned char)(46.0f + 40.0f * rain) };   // (blended in linear light, where a pale streak counts for more)
    const Vector3 slant = { 0.10f, -0.9f, 0.04f };
    for (int i = 0; i < count; i++) {
        const uint32_t h = RainHash((uint32_t)i * 2654435761U + 17U);
        const float hx = (float)(h & 0xffff) / 65535.0f * box;
        const float hz = (float)((h >> 16) & 0xffff) / 65535.0f * box;
        const float phase = (float)(RainHash(h) & 0xffff) / 65535.0f * depth;
        float dx = fmodf(hx - camera.position.x, box); if (dx < 0.0f) dx += box;
        float dz = fmodf(hz - camera.position.z, box); if (dz < 0.0f) dz += box;
        const float fall = fmodf(t * speed + phase, depth);
        const Vector3 a = { camera.position.x + dx - box * 0.5f, camera.position.y + top - fall, camera.position.z + dz - box * 0.5f };
        const Vector3 b = Vector3Add(a, Vector3Scale(slant, 0.9f + 0.5f * rain));
        DrawLine3D(a, b, col);
    }
}
void SetNightLights(const Vector4* lights, int count) {
    count = Clamp(count, 0, 32);
    for (int i = 0; i < count; i++) {
        nightLightData[i * 4] = lights[i].x; nightLightData[i * 4 + 1] = lights[i].y;
        nightLightData[i * 4 + 2] = lights[i].z; nightLightData[i * 4 + 3] = lights[i].w;
    }
    const float night = GetNightAmount();
    const float c = (float)count;
    if (roadShader.id != 0 && roadLocs.lights != -1) {
        SetShaderValue(roadShader, roadLocs.night, &night, SHADER_UNIFORM_FLOAT);
        SetShaderValue(roadShader, roadLocs.lightCount, &c, SHADER_UNIFORM_FLOAT);
        if (count > 0) SetShaderValueV(roadShader, roadLocs.lights, nightLightData, SHADER_UNIFORM_VEC4, count);
    }
    if (cityInstancedReady && cityLocs.lights != -1) {
        SetShaderValue(cityInstancedShader, cityLocs.lightCount, &c, SHADER_UNIFORM_FLOAT);
        if (count > 0) SetShaderValueV(cityInstancedShader, cityLocs.lights, nightLightData, SHADER_UNIFORM_VEC4, count);
    }
}

namespace {
Vector3 lightingCamPos{};   // the camera UpdateLighting() last saw
bool cityEnvironmentStale = true;   // the instanced city shader has not been given this frame's light yet

// The scene's light for one of the lit programs (see the uniform list in shaders/lit.glsl).
void UploadLitEnvironment(Shader& sh, const LitLocs& l) {
    if (sh.id == 0) return;
    const Vector3 dir = SunDirection(), sun = SunRadiance(), sky = AmbientSky(), ground = AmbientGround(), refl = SkyRadiance();
    float cascades[16 * shadow::kMaxCascades];
    const int cascadeCount = shadow::CascadeData(cascades);
    if (l.shadowVP != -1) SetShaderValueV(sh, l.shadowVP, cascades, SHADER_UNIFORM_VEC4, 4 * shadow::kMaxCascades);
    for (int i = 1; i < shadow::kMaxCascades; i++)
        if (l.shadowMapFar[i - 1] != -1) SetShaderValueTexture(sh, l.shadowMapFar[i - 1], i < cascadeCount ? shadow::FarMapDepth(i - 1) : Texture2D{});
    const Vector4 sunDir = { dir.x, dir.y, dir.z, (float)cascadeCount };
    const Vector4 sunColor = { sun.x, sun.y, sun.z, 1.0f };
    const Vector4 ambientSky = { sky.x, sky.y, sky.z, 1.0f };
    const Vector4 ambientGround = { ground.x, ground.y, ground.z, 1.0f };
    const Vector4 skyColor = { refl.x, refl.y, refl.z, 1.0f };
    if (l.sunDir != -1) SetShaderValue(sh, l.sunDir, &sunDir, SHADER_UNIFORM_VEC4);
    if (l.sunColor != -1) SetShaderValue(sh, l.sunColor, &sunColor, SHADER_UNIFORM_VEC4);
    if (l.ambientSky != -1) SetShaderValue(sh, l.ambientSky, &ambientSky, SHADER_UNIFORM_VEC4);
    if (l.ambientGround != -1) SetShaderValue(sh, l.ambientGround, &ambientGround, SHADER_UNIFORM_VEC4);
    if (l.skyColor != -1) SetShaderValue(sh, l.skyColor, &skyColor, SHADER_UNIFORM_VEC4);
    if (l.weather != -1) {
        const Vector4 w = { WetnessNow(), (float)GetTime(), 0.0f, 0.0f };
        SetShaderValue(sh, l.weather, &w, SHADER_UNIFORM_VEC4);
    }
    if (l.camPos != -1) {
        const Vector4 cp = { lightingCamPos.x, lightingCamPos.y, lightingCamPos.z, 1.0f };
        SetShaderValue(sh, l.camPos, &cp, SHADER_UNIFORM_VEC4);
    }
    if (l.lightVP != -1) SetShaderValueMatrix(sh, l.lightVP, GetLightViewProj());
    if (l.cloudShadow != -1) {
        const Vector4 cs = GetCloudShadowParams();
        SetShaderValue(sh, l.cloudShadow, &cs, SHADER_UNIFORM_VEC4);
        if (l.cloudShadowTex != -1) SetShaderValueTexture(sh, l.cloudShadowTex, GetCloudShadowTexture());
    }
}
}

namespace detail {
Shader& CityInstancedShader() { return cityInstancedShader; }
Material& CityInstancedMaterial() { return cityInstancedMaterial; }
bool CityInstancedReady() { return cityInstancedReady; }
Material& ShapeInstancedMaterial() { return shapeInstancedMaterial; }
bool ShapeInstancedReady() { return shapeInstancedReady; }
void SetupShapeInstancedLighting() { if (shapeInstancedReady) shadow::BindMap(); }

void UploadLightViewProj(const Matrix& lightViewProj) {
    if (litShader.id != 0 && litLocs.lightVP != -1) SetShaderValueMatrix(litShader, litLocs.lightVP, lightViewProj);
    if (roadShader.id != 0 && roadLocs.lightVP != -1) SetShaderValueMatrix(roadShader, roadLocs.lightVP, lightViewProj);
    if (cityInstancedReady && cityLocs.lightVP != -1) SetShaderValueMatrix(cityInstancedShader, cityLocs.lightVP, lightViewProj);
    if (shapeInstancedReady && shapeLocs.lightVP != -1) SetShaderValueMatrix(shapeInstancedShader, shapeLocs.lightVP, lightViewProj);
}
}

void UploadWaterShadows(Shader& shader) {
    LitLocs locs;
    FindLitLocs(shader, locs);
    UploadLitEnvironment(shader, locs);
}

void UpdateLighting(const Camera3D& camera) {
    shadow::SetViewCamera(camera);
    lightingCamPos = camera.position;

    if (camera.projection == CAMERA_PERSPECTIVE) {
        const double h = fabs((double)camera.position.y);
        ApplyNearPlane(fmin(fmax(h * 0.03, 0.05), 2.0));
    }

    UpdateAtmosphere(camera);
    UpdateClouds(GetFrameTime());
    UpdateCloudShadow(camera);
    UploadLitEnvironment(litShader, litLocs);
    UploadLitEnvironment(roadShader, roadLocs);
    if (shapeInstancedReady) UploadLitEnvironment(shapeInstancedShader, shapeLocs);
    cityEnvironmentStale = true;

    shadow::BindMap();
}

// Called before every instanced city draw (there are hundreds a frame). The scene's light is the same
// for all of them, and a shader keeps the uniforms it was given, so it is handed over once a frame.
namespace detail {
void SetupInstancedLighting() {
    if (!cityInstancedReady) return;
    if (cityEnvironmentStale) {
        cityEnvironmentStale = false;
        UploadLitEnvironment(cityInstancedShader, cityLocs);
        if (cityLocs.night != -1) {
            const float night = GetNightAmount();
            SetShaderValue(cityInstancedShader, cityLocs.night, &night, SHADER_UNIFORM_FLOAT);
        }
    }

    shadow::BindMap();
}
}

void SetUnderwaterParams(float waterY, Vector3, float, Vector3) {
    if (litLocs.waterSurfaceY != -1) SetShaderValue(litShader, litLocs.waterSurfaceY, &waterY, SHADER_UNIFORM_FLOAT);
    if (cityLocs.waterSurfaceY != -1 && cityInstancedShader.id != 0)
        SetShaderValue(cityInstancedShader, cityLocs.waterSurfaceY, &waterY, SHADER_UNIFORM_FLOAT);
    if (shapeLocs.waterSurfaceY != -1 && shapeInstancedReady)
        SetShaderValue(shapeInstancedShader, shapeLocs.waterSurfaceY, &waterY, SHADER_UNIFORM_FLOAT);
    if (roadLocs.waterSurfaceY != -1 && roadShader.id != 0)
        SetShaderValue(roadShader, roadLocs.waterSurfaceY, &waterY, SHADER_UNIFORM_FLOAT);
}

namespace {
struct LoadingStatus { std::string text; float fraction = 0.0f; };
std::map<const void*, LoadingStatus> loadingStatuses;
}
void SetLoadingStatus(const void* owner, const std::string& text, float fraction) { loadingStatuses[owner] = { text, fraction }; }
void ClearLoadingStatus(const void* owner) { loadingStatuses.erase(owner); }
int LoadingStatusCount() { return (int)loadingStatuses.size(); }
std::string LoadingStatusText(int index) {
    int i = 0;
    for (const auto& kv : loadingStatuses) if (i++ == index) return kv.second.text;
    return "";
}
// The smooth UI font (the same file the editor and the player use), loaded on first use at a size that stays sharp;
// raylib's blocky built-in font only if no font file can be found.
Font LoadingFont() {
    static Font font{};
    static bool tried = false;
    if (!tried) {
        tried = true;
        const std::string& path = platform::ResolveFontPath();
        if (!path.empty()) {
            Font f = LoadFontEx(path.c_str(), 52, nullptr, 0);
            if (f.texture.id != 0) { font = f; SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR); }
        }
    }
    return font;
}

void DrawLoadingStatus(Rectangle area) {
    if (loadingStatuses.empty()) return;
    const Font font = LoadingFont();
    const bool smooth = font.texture.id != 0;
    const float fontSize = 26.0f;
    const int pad = 14, barH = 6;
    float y = area.y + 16.0f;
    for (const auto& kv : loadingStatuses) {
        const int tw = smooth ? (int)(MeasureTextEx(font, kv.second.text.c_str(), fontSize, 0.5f).x + 0.5f) : MeasureText(kv.second.text.c_str(), (int)fontSize);
        const float bw = (float)tw + pad * 2.0f, bh = fontSize + (float)(pad * 2 + barH + 4);
        const float x = area.x + (area.width - bw) * 0.5f;
        DrawRectangleRounded({ x, y, bw, bh }, 0.18f, 8, Color{ 18, 20, 26, 225 });
        DrawRectangleRoundedLines({ x, y, bw, bh }, 0.18f, 8, Color{ 0, 190, 200, 255 });
        if (smooth) DrawTextEx(font, kv.second.text.c_str(), { x + pad, y + pad }, fontSize, 0.5f, Color{ 235, 240, 245, 255 });
        else DrawText(kv.second.text.c_str(), (int)x + pad, (int)y + pad, (int)fontSize, Color{ 235, 240, 245, 255 });
        const float bx = x + pad, by = y + pad + fontSize + 6.0f, full = (float)tw;
        DrawRectangle((int)bx, (int)by, (int)full, barH, Color{ 50, 56, 66, 255 });
        DrawRectangle((int)bx, (int)by, (int)(full * Clamp(kv.second.fraction, 0.0f, 1.0f)), barH, Color{ 0, 190, 200, 255 });
        y += bh + 8.0f;
    }
}

} // namespace
