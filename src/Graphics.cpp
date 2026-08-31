#include "Graphics.hpp"
#include "ShaderCache.hpp"
#include "raymath.h"
#include "rlgl.h"
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

int shadowMapResolution = 1024;
int shadowQuality = 52;
constexpr int SHADOW_TEXTURE_SLOT = 10;

int QualityToResolution(int quality) {
    quality = Clamp(quality, 5, 100);
    float t = (float)(quality - 5) / 95.0f;
    int log2 = 8 + (int)(t * 4.0f + 0.5f);
    return 1 << log2;
}

const Vector3 kLightDir = Vector3Normalize({ -0.4f, -1.0f, -0.3f });
const Vector3 kAmbient = { 0.35f, 0.35f, 0.35f };
Vector3 ambient = kAmbient;
bool gridVisible = true;
bool wireframe = false;
bool inShadowPass = false;

Shader litShader{};
Texture2D defaultTexture{};
Model cubeModel{};
Model sphereModel{};
Model cylinderModel{};
Model wedgeModel{};
Model groundModel{};
Texture2D groundTexture{};
int lightDirLoc = -1;
int ambientLoc = -1;
int lightVPLoc = -1;
int shadowMapLoc = -1;
int shadowsEnabledLoc = -1;
// Underwater uniforms
int waterSurfaceYLoc = -1;
int waterAbsorptionLoc = -1;
int waterFogDensityLoc = -1;
int waterFogColorLoc = -1;
bool initialized = false;
bool shadowsEnabled = true;

RenderTexture2D shadowMap{};
Camera3D lightCamera{};
Matrix lightView = MatrixIdentity();
Matrix lightProj = MatrixIdentity();
Matrix lightViewProj = MatrixIdentity();

const char* kVertexShader = R"(
#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matNormal;
uniform mat4 matModel;
uniform mat4 lightVP;

out vec2 fragTexCoord;
out vec4 fragColor;
out vec3 fragNormal;
out vec4 fragShadowCoord;
out vec3 fragWorldPos;

void main()
{
    fragTexCoord = vertexTexCoord;
    fragColor = vertexColor;
    fragNormal = normalize(vec3(matNormal * vec4(vertexNormal, 0.0)));
    fragShadowCoord = lightVP * matModel * vec4(vertexPosition, 1.0);
    fragWorldPos = vec3(matModel * vec4(vertexPosition, 1.0));
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kFragmentShader = R"(
#version 330

in vec2 fragTexCoord;
in vec4 fragColor;
in vec3 fragNormal;
in vec4 fragShadowCoord;
in vec3 fragWorldPos;

uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 lightDir;
uniform vec3 ambient;
uniform sampler2D shadowMap;
uniform float shadowsEnabled;
uniform float waterSurfaceY;

out vec4 finalColor;

vec3 mod289(vec3 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec2 mod289v2(vec2 x) { return x - floor(x * (1.0 / 289.0)) * 289.0; }
vec3 permute(vec3 x) { return mod289(((x * 34.0) + 1.0) * x); }

float snoise(vec2 v) {
    const vec4 C = vec4(0.211324865405187, 0.366025403784439,
                        -0.577350269189626, 0.024390243902439);
    vec2 i  = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    vec2 i1;
    i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;
    i = mod289v2(i);
    vec3 p = permute(permute(i.y + vec3(0.0, i1.y, 1.0))
                             + i.x + vec3(0.0, i1.x, 1.0));
    vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy),
                             dot(x12.zw, x12.zw)), 0.0);
    m = m * m;
    m = m * m;
    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);
    vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

float ShadowCalculation(vec4 fragPosLightSpace, vec3 normal)
{
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;

    float currentDepth = projCoords.z;
    float shadow = 0.0;

    if (projCoords.x >= 0.0 && projCoords.x <= 1.0 &&
        projCoords.y >= 0.0 && projCoords.y <= 1.0 &&
        currentDepth <= 1.0)
    {
        float bias = max(0.002 * (1.0 - dot(normal, lightDir)), 0.002);
        vec2 texelSize = 1.0 / textureSize(shadowMap, 0);

        // 3x3 box PCF to soften the shadow edge
        for (int x = -1; x <= 1; ++x)
        {
            for (int y = -1; y <= 1; ++y)
            {
                float closestDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
                shadow += (currentDepth - bias) > closestDepth ? 1.0 : 0.0;
            }
        }
        shadow /= 9.0;
    }

    return shadow;
}

void main()
{
    vec3 normal = normalize(fragNormal);
    float diffuse = max(dot(normal, -lightDir), 0.0);
    float shadow = ShadowCalculation(fragShadowCoord, normal) * shadowsEnabled;

    vec4 texelColor = texture(texture0, fragTexCoord);
    vec3 lit = (ambient + (1.0 - shadow) * diffuse) * texelColor.rgb * colDiffuse.rgb * fragColor.rgb;

    float depthBelow = waterSurfaceY - fragWorldPos.y;
    if (depthBelow > 0.0) {
        float t = clamp(depthBelow * 0.3, 0.0, 1.0);
        vec3 waterTint = vec3(0.6, 0.75, 0.9);
        lit = mix(lit, lit * waterTint, t * 0.25);
        float luma = dot(lit, vec3(0.299, 0.587, 0.114));
        lit = mix(lit, vec3(luma), t * 0.1);
    }

    finalColor = vec4(lit, texelColor.a * colDiffuse.a);
}
)";

RenderTexture2D LoadShadowmapRenderTexture(int width, int height) {
    RenderTexture2D target = { 0 };

    target.id = rlLoadFramebuffer();
    target.texture.width = width;
    target.texture.height = height;

    if (target.id > 0) {
        rlEnableFramebuffer(target.id);

        target.texture.id = rlLoadTexture(NULL, width, height, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, 1);
        target.texture.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        target.texture.mipmaps = 1;
        rlFramebufferAttach(target.id, target.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);

        target.depth.id = rlLoadTextureDepth(width, height, false);
        target.depth.width = width;
        target.depth.height = height;
        target.depth.format = 19;
        target.depth.mipmaps = 1;

        rlFramebufferAttach(target.id, target.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);

        if (rlFramebufferComplete(target.id)) {
            TraceLog(LOG_INFO, "FBO: [ID %i] Framebuffer object created successfully", target.id);
        } else {
            TraceLog(LOG_WARNING, "FBO: [ID %i] Framebuffer object is not complete", target.id);
        }

        rlDisableFramebuffer();
    } else {
        TraceLog(LOG_WARNING, "FBO: Framebuffer object can not be created");
    }

    return target;
}

Texture2D GenerateGridTexture() {
    const int texSize = 400;
    const int lineSpacing = 10;

    Image img = GenImageColor(texSize, texSize, Color{ 32, 34, 40, 255 });
    const Color lineColor = { 56, 60, 70, 255 };
    for (int i = 0; i <= texSize; i += lineSpacing) {
        ImageDrawLine(&img, i, 0, i, texSize - 1, lineColor);
        ImageDrawLine(&img, 0, i, texSize - 1, i, lineColor);
    }

    Texture2D texture = LoadTextureFromImage(img);
    UnloadImage(img);

    GenTextureMipmaps(&texture);
    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);

    return texture;
}

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

}

namespace gfx {

void Init() {
    if (initialized) return;

    // Initialize shader cache
    {
        fs::path exeDir = fs::current_path();
        shaderCache::Init((exeDir / ".shader_cache").generic_string());
    }

    // Try loading lit shader from binary cache
    if (!shaderCache::LoadBinary("lit_shader", litShader)) {
        litShader = LoadShaderFromMemory(kVertexShader, kFragmentShader);
        if (litShader.id != 0) {
            shaderCache::SaveBinary("lit_shader", litShader);
        }
    }
    lightDirLoc = GetShaderLocation(litShader, "lightDir");
    ambientLoc = GetShaderLocation(litShader, "ambient");
    lightVPLoc = GetShaderLocation(litShader, "lightVP");
    shadowMapLoc = GetShaderLocation(litShader, "shadowMap");
    shadowsEnabledLoc = GetShaderLocation(litShader, "shadowsEnabled");
    waterSurfaceYLoc = GetShaderLocation(litShader, "waterSurfaceY");
    waterAbsorptionLoc = GetShaderLocation(litShader, "waterAbsorption");
    waterFogDensityLoc = GetShaderLocation(litShader, "waterFogDensity");
    waterFogColorLoc = GetShaderLocation(litShader, "waterFogColor");

    int shadowSlot = SHADOW_TEXTURE_SLOT;
    SetShaderValue(litShader, shadowMapLoc, &shadowSlot, SHADER_UNIFORM_INT);

    Image checker = GenImageChecked(64, 64, 8, 8, LIGHTGRAY, GRAY);
    defaultTexture = LoadTextureFromImage(checker);
    UnloadImage(checker);

    cubeModel = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
    sphereModel = LoadModelFromMesh(GenMeshSphere(0.5f, 16, 16));
    cylinderModel = LoadModelFromMesh(GenMeshCylinder(0.5f, 1.0f, 16));
    wedgeModel = LoadModelFromMesh(GenerateWedgeMesh());

    Model* models[] = { &cubeModel, &sphereModel, &cylinderModel, &wedgeModel };
    for (Model* model : models) {
        model->materials[0].shader = litShader;
        model->materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = defaultTexture;
    }

    groundModel = LoadModelFromMesh(GenMeshPlane(40.0f, 40.0f, 1, 1));
    groundTexture = GenerateGridTexture();
    groundModel.materials[0].shader = litShader;
    groundModel.materials[0].maps[MATERIAL_MAP_DIFFUSE].texture = groundTexture;

    shadowMap = LoadShadowmapRenderTexture(shadowMapResolution, shadowMapResolution);
    if (shadowMap.depth.id > 0) {
        SetTextureFilter(shadowMap.depth, TEXTURE_FILTER_BILINEAR);
    }

    lightCamera.position = Vector3Scale(kLightDir, -40.0f);
    lightCamera.target = { 0.0f, 0.0f, 0.0f };
    lightCamera.up = { 0.0f, 1.0f, 0.0f };
    lightCamera.fovy = 45.0f;
    lightCamera.projection = CAMERA_ORTHOGRAPHIC;

    initialized = true;
}

void Shutdown() {
    if (!initialized) return;

    UnloadModel(cubeModel);
    UnloadModel(sphereModel);
    UnloadModel(cylinderModel);
    UnloadModel(wedgeModel);
    UnloadModel(groundModel);
    UnloadTexture(defaultTexture);
    UnloadTexture(groundTexture);
    if (shadowMap.id > 0) rlUnloadFramebuffer(shadowMap.id);
    UnloadShader(litShader);

    shaderCache::Shutdown();
    initialized = false;
}

Shader& GetLitShader() { return litShader; }

Model& GetShapeModel(ShapeType type) {
    switch (type) {
        case ShapeType::Sphere:   return sphereModel;
        case ShapeType::Cylinder: return cylinderModel;
        case ShapeType::Wedge:    return wedgeModel;
        default:                  return cubeModel;
    }
}

Texture2D GetDefaultTexture() { return defaultTexture; }

void SetShadowsEnabled(bool enabled) { shadowsEnabled = enabled; }

bool IsShadowsEnabled() { return shadowsEnabled; }

void SetShadowQuality(int quality) {
    quality = Clamp(quality, 5, 100);
    if (quality == shadowQuality && shadowMap.id > 0) return;
    shadowQuality = quality;

    int resolution = QualityToResolution(quality);
    if (resolution == shadowMapResolution && shadowMap.id > 0) return;
    shadowMapResolution = resolution;

    if (shadowMap.id > 0) {
        UnloadTexture(shadowMap.texture);
        UnloadTexture(shadowMap.depth);
        rlUnloadFramebuffer(shadowMap.id);
        shadowMap = { 0 };
    }
    shadowMap = LoadShadowmapRenderTexture(shadowMapResolution, shadowMapResolution);
    if (shadowMap.depth.id > 0) {
        SetTextureFilter(shadowMap.depth, TEXTURE_FILTER_BILINEAR);
    }
}

int GetShadowQuality() { return shadowQuality; }

void SetAmbientIntensity(float intensity) {
    intensity = fmaxf(0.0f, fminf(intensity, 2.0f));
    ambient = Vector3Scale(kAmbient, intensity);
}

float GetAmbientIntensity() {
    return (kAmbient.x > 0.0f) ? ambient.x / kAmbient.x : 1.0f;
}

void SetGridVisible(bool visible) { gridVisible = visible; }

bool IsGridVisible() { return gridVisible; }

void SetWireframe(bool enabled) {
    wireframe = enabled;
    if (wireframe) rlEnableWireMode();
    else rlDisableWireMode();
}

bool IsWireframe() { return wireframe; }

void BeginShadowPass() {
    if (!shadowsEnabled || shadowMap.id == 0) return;

    inShadowPass = true;

    BeginTextureMode(shadowMap);
    ClearBackground(WHITE);

    BeginMode3D(lightCamera);
    rlSetMatrixProjection(MatrixOrtho(-35.0f, 35.0f, -35.0f, 35.0f, 0.1f, 200.0f));

    lightView = rlGetMatrixModelview();
    lightProj = rlGetMatrixProjection();
}

void EndShadowPass() {
    if (!shadowsEnabled || shadowMap.id == 0) return;

    EndMode3D();
    EndTextureMode();

    inShadowPass = false;
    lightViewProj = MatrixMultiply(lightView, lightProj);
}

bool IsInShadowPass() {
    return inShadowPass;
}

void UpdateLighting(const Camera3D& camera) {
    (void)camera;

    SetShaderValue(litShader, lightDirLoc, &kLightDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(litShader, ambientLoc, &ambient, SHADER_UNIFORM_VEC3);
    SetShaderValueMatrix(litShader, lightVPLoc, lightViewProj);

    float enabled = shadowsEnabled ? 1.0f : 0.0f;
    SetShaderValue(litShader, shadowsEnabledLoc, &enabled, SHADER_UNIFORM_FLOAT);

    if (shadowsEnabled && shadowMap.depth.id > 0) {
        rlActiveTextureSlot(SHADOW_TEXTURE_SLOT);
        rlEnableTexture(shadowMap.depth.id);
    }
}

void SetUnderwaterParams(float waterY, Vector3, float, Vector3) {
    if (waterSurfaceYLoc != -1) SetShaderValue(litShader, waterSurfaceYLoc, &waterY, SHADER_UNIFORM_FLOAT);
}

void DrawGround() {
    if (!gridVisible) return;
    groundModel.transform = MatrixIdentity();
    DrawModel(groundModel, Vector3Zero(), 1.0f, WHITE);
}

}
