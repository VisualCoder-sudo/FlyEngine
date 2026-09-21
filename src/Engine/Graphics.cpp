#include "../../include/Engine/Graphics.hpp"
#include "../../include/Engine/Backend/ShaderCache.hpp"
#include "raymath.h"
#include "rlgl.h"
#include "../../include/Engine/Frontend/ui.hpp"
#include <atomic>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

int shadowMapResolution = 2048;
int shadowQuality = 52;
constexpr int SHADOW_TEXTURE_SLOT = 10;

int reflectionResolution = 768;
int reflectionQuality = 50;
constexpr int REFLECTION_TEXTURE_SLOT = 11;
bool reflectionEnabled = true;
bool inReflectionPass = false;
RenderTexture2D reflectionTarget{};
Matrix reflView = MatrixIdentity();
Matrix reflProj = MatrixIdentity();
Matrix reflViewProj = MatrixIdentity();

int QualityToResolution(int quality) {
    // Shadow acne fix (3): raise the texel budget for a given quality slider.
    // Base shifted 8 -> 9 (log2), so default 52 now yields 2048 (4x texels of
    // the old 1024) — the generated shadow map is sharper and each acne stripe
    // is a few texels narrower, cutting the visible flicker. Low 512 barely
    // differs; High reaches 8192 for crisp silhouettes from up high.
    quality = Clamp(quality, 5, 100);
    float t = (float)(quality - 5) / 95.0f;
    int log2 = 9 + (int)(t * 4.0f + 0.5f);
    return 1 << log2;
}

int ReflectionQualityToResolution(int quality) {
    quality = Clamp(quality, 5, 100);
    float t = (float)(quality - 5) / 95.0f;
    return 360 + (int)(t * 1080.0f + 0.5f); // 360..1440
}

const Vector3 kLightDir = Vector3Normalize({ -0.4f, -1.0f, -0.3f });
const Vector3 kAmbient = { 0.35f, 0.35f, 0.35f };
Vector3 ambient = kAmbient;
bool gridVisible = true;
bool wireframe = false;
bool inShadowPass = false;
// Shadow-map reuse state.
bool shadowReuseEnabled = true;
std::atomic<bool> shadowsDirty{ true };
bool shadowPassReused = false;
bool shadowHaveRendered = false;
Vector3 shadowLastCenter{};
float shadowLastHalf = 0.0f;
int shadowLastRes = 0;
int shadowFramesSinceRender = 0;
int shadowInputHold = 0;
constexpr int kShadowMaxAge = 90;   // force a refresh at least this often (frames)
constexpr int kShadowInputHold = 3; // keep re-rendering this many frames after input

// Any input that could edit the scene (or move the camera) this frame.
bool SceneInputActivity() {
    for (int b = 0; b < 7; b++)
        if (IsMouseButtonDown(b) || IsMouseButtonPressed(b) || IsMouseButtonReleased(b)) return true;
    if (GetMouseWheelMove() != 0.0f) return true;
    for (int k = 32; k <= 348; k++)
        if (IsKeyDown(k) || IsKeyPressed(k) || IsKeyReleased(k)) return true;
    return false;
}

// Persistent instance buffers.
bool instanceBuffersEnabled = true;

// rlSetVertexAttribute() takes an int offset in newer raylib and a const void*
// pointer in older ones; this picks whichever the linked rlgl.h declares.
template <typename T = int>
auto SetInstAttr(unsigned int idx, T offset, int)
    -> decltype(rlSetVertexAttribute(idx, 4, RL_FLOAT, false, 64, offset), void()) {
    rlSetVertexAttribute(idx, 4, RL_FLOAT, false, 64, offset);
}
template <typename T = int>
auto SetInstAttr(unsigned int idx, T offset, long)
    -> decltype(rlSetVertexAttribute(idx, 4, RL_FLOAT, false, 64, (const void*)(size_t)offset), void()) {
    rlSetVertexAttribute(idx, 4, RL_FLOAT, false, 64, (const void*)(size_t)offset);
}
Camera3D shadowViewCamera{};   // last camera seen by UpdateLighting (the shadow frustum follows it)
bool haveShadowViewCamera = false;
constexpr float kShadowNear = 1.0f;
constexpr float kShadowFar = 430.0f;
constexpr float kShadowLightDist = 330.0f; // light eye distance from the frustum centre
constexpr float kShadowMinHalf = 40.0f;
constexpr float kShadowMaxHalf = 150.0f;

Mesh cityWedgeMesh{}; // unit corner wedge for angled parcels
Mesh citySlantMesh{}; // unit sheared slab for silhouette variety

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
// Instanced-lit shader + material used for city buildings.
Shader cityInstancedShader{};
Material cityInstancedMaterial{};
bool cityInstancedReady = false;
int cityLightDirLoc = -1;
int cityAmbientLoc = -1;
int cityLightVPLoc = -1;
int cityShadowMapLoc = -1;
int cityShadowsEnabledLoc = -1;
int cityWaterSurfaceYLoc = -1;
// Road shader: lit fragment shader with a depth-biased vertex stage, used only
// for the flat road/pad/parks mesh so it wins the depth test against the
// near-coplanar ground plane at altitude.
Shader roadShader{};
int roadLightDirLoc = -1;
int roadAmbientLoc = -1;
int roadLightVPLoc = -1;
int roadShadowMapLoc = -1;
int roadShadowsEnabledLoc = -1;
int roadWaterSurfaceYLoc = -1;
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

// Road variant of the lit vertex shader: identical output, but gl_Position.z is
// biased by a constant window-depth offset (1.0/1048576.0 of clip w) so roads --
// which lie ~0.01-0.03 above the ground plane -- win the depth test against it at
// altitude, where perspective depth precision is much coarser than that gap.
const char* kVertexShaderRoad = R"(
#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;

uniform mat4 mvp;
uniform mat4 matNormal;
uniform mat4 matModel;
uniform mat4 lightVP;
uniform mat4 matProjection; // set by raylib when the shader's projection loc is registered

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
    // World-space depth bias: pull the vertex toward the camera by a fixed number
    // of METRES (independent of viewing distance, so the city never seems to
    // rise or sink as the camera moves). The base amount beats the ground
    // plane; the height term (y above the 0.04 road elevation) orders the
    // stacked road layers against each other.
    float layer = max(vertexPosition.y - 0.04, 0.0);
    float dz = 0.02 + layer * 2.0;
    if (matProjection[3][3] > 0.5) {
        gl_Position.z += matProjection[2][2] * dz;                    // orthographic
    } else {
        gl_Position.z += matProjection[3][2] * dz / gl_Position.w;    // perspective
    }
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

// Shadow acne fixes (shared by every lit mesh).
// 1) clip-space light VP is needed in the fragment stage for normal-based bias.
uniform mat4 lightVP;

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

float ShadowCalculation(vec4 fragPosLightSpace, vec3 normal) {
    // Combined anti-acne scheme (shared by every lit mesh).
    //
    // (A) NORMAL-BASED BIAS — shift the shadow comparison point along the
    //     surface normal in WORLD space before projecting to light space. This
    //     is a fixed world-units offset, so it stays correct at every camera
    //     altitude (the acne never returns no matter how high the flyer is).
    //     World offset ~1 light-space texel (ortho 70 wide / 1024..4096).
    vec4 biasedLightPos = lightVP * vec4(fragWorldPos + normalize(normal) * 0.02, 1.0);
    vec3 projCoords = biasedLightPos.xyz / biasedLightPos.w;
    projCoords = projCoords * 0.5 + 0.5;

    float currentDepth = projCoords.z;
    float shadow = 0.0;

    if (projCoords.x >= 0.0 && projCoords.x <= 1.0 &&
        projCoords.y >= 0.0 && projCoords.y <= 1.0 &&
        currentDepth <= 1.0)
    {
        // (B) SLOPE-SCALE DEPTH BIAS — bias grows as the surface tilts away
        //     from the light: 1.0 - dot(normal, -lightDir) is 0 for a surface
        //     facing the light dead-on and reaches 1.0 at grazing incidence.
        //     Acne is worst exactly at grazing, so acne-prone surfaces get the
        //     largest bias while flat-lit faces stay acne-free and acne across
        //     a shallow sun / low-elevation directional light is suppressed.
        float facing = 1.0 - dot(normal, normalize(-lightDir));
        float bias = max(0.0037 * facing * facing, 0.00093); // tuned for the 1..430 shadow depth range
        vec2 texelSize = 1.0 / textureSize(shadowMap, 0);

        // 3x3 box PCF to soften the shadow edge
        for (int x = -1; x <= 1; ++x) {
            for (int y = -1; y <= 1; ++y) {
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

// Instanced variant of the lit vertex shader: the model matrix comes from the
// per-instance `instanceTransform` attribute (DrawMeshInstanced binds location
// 9+), while mvp/matNormal/lightVP are still supplied by raylib/our pass.
const char* kVertexShaderInstanced = R"(
#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
in mat4 instanceTransform;

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
    // Per-instance tint rides in the (otherwise unused) bottom row of the
    // transform: m3,m7,m11 == instanceTransform[0..2][3]. That lets every colour of
    // a shape share one draw call. Zeroed again so the matrix stays affine; an
    // all-zero tint (e.g. editor markers) means "no per-instance tint".
    mat4 model = instanceTransform;
    vec3 instTint = vec3(model[0][3], model[1][3], model[2][3]);
    model[0][3] = 0.0;
    model[1][3] = 0.0;
    model[2][3] = 0.0;
    if (dot(instTint, instTint) < 1e-6) instTint = vec3(1.0);

    vec4 worldPos = model * vec4(vertexPosition, 1.0);
    fragTexCoord = vertexTexCoord;
    fragColor = vec4(vertexColor.rgb * instTint, vertexColor.a);
    fragNormal = normalize(mat3(model) * vertexNormal);
    fragShadowCoord = lightVP * worldPos;
    fragWorldPos = worldPos.xyz;
    gl_Position = mvp * worldPos;
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

// Corner wedge: a right-isoceles triangular prism (unit cube footprint, apex at
// (0.5, 0.5)). Fills the triangular parcel where two angled streets meet.
Mesh GenerateCityWedgeMesh() {
    Mesh mesh = { 0 };

    const float invSqrt2 = 0.70710678f;
    const int vertexCount = 18;
    const int indexCount = 24;

    float vertices[vertexCount * 3] = {
        // bottom face (A,C,B) y=-0.5
         0.5f, -0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,     0.5f, -0.5f, -0.5f,
        // top face (a,b,c) y=+0.5
         0.5f,  0.5f,  0.5f,     0.5f,  0.5f, -0.5f,    -0.5f,  0.5f,  0.5f,
        // +x leg (A,B,b,a)
         0.5f, -0.5f,  0.5f,     0.5f, -0.5f, -0.5f,
         0.5f,  0.5f, -0.5f,     0.5f,  0.5f,  0.5f,
        // +z leg (A,a,c,C)
         0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,
        // hypotenuse (B,C,c,b)
         0.5f, -0.5f, -0.5f,    -0.5f, -0.5f,  0.5f,
        -0.5f,  0.5f,  0.5f,     0.5f,  0.5f, -0.5f,
    };
    float normals[vertexCount * 3] = {
        0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,
        0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f,
        -invSqrt2, 0.0f, -invSqrt2,   -invSqrt2, 0.0f, -invSqrt2,
        -invSqrt2, 0.0f, -invSqrt2,   -invSqrt2, 0.0f, -invSqrt2,
    };
    float texcoords[vertexCount * 2] = {
        0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
        0.0f, 0.0f,   1.0f, 0.0f,   1.0f, 1.0f,   0.0f, 1.0f,
    };
    unsigned short indices[indexCount] = {
        0, 1, 2,
        3, 5, 4,
        6, 8, 7,  6, 9, 8,
        10, 12, 11,  10, 13, 12,
        14, 16, 15,  14, 17, 16,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    for (int i = 0; i < vertexCount * 4; i++) mesh.colors[i] = 255;
    mesh.indices = (unsigned short*)RL_MALLOC(indexCount * sizeof(unsigned short));
    memcpy(mesh.indices, indices, indexCount * sizeof(unsigned short));

    UploadMesh(&mesh, false);

    return mesh;
}

// Sheared slab: unit footprint with its top pushed +z by 0.35 (a parallelogram
// cross-section), giving buildings a slanted rather than boxy silhouette.
Mesh GenerateCitySlantMesh() {
    Mesh mesh = { 0 };

    const float sh = 0.35f;
    const int vertexCount = 24;
    const int indexCount = 36;

    float vertices[vertexCount * 3] = {
        // bottom (B0,B1,B2,B3)
        -0.5f, -0.5f, -0.5f,     0.5f, -0.5f, -0.5f,     0.5f, -0.5f,  0.5f,    -0.5f, -0.5f,  0.5f,
        // top (T3,T2,T1,T0)
        -0.5f,  0.5f,  0.5f + sh,  0.5f,  0.5f,  0.5f + sh,  0.5f,  0.5f, -0.5f + sh,   -0.5f,  0.5f, -0.5f + sh,
        // +x (B1,B2,T2,T1)
         0.5f, -0.5f, -0.5f,     0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f + sh,    0.5f,  0.5f, -0.5f + sh,
        // -x (B3,B0,T0,T3)
        -0.5f, -0.5f,  0.5f,    -0.5f, -0.5f, -0.5f,    -0.5f,  0.5f, -0.5f + sh,   -0.5f,  0.5f,  0.5f + sh,
        // +z ramp (B2,T2,T3,B3)
         0.5f, -0.5f,  0.5f,     0.5f,  0.5f,  0.5f + sh,  -0.5f,  0.5f,  0.5f + sh,   -0.5f, -0.5f,  0.5f,
        // -z ramp (B0,T0,T1,B1)
        -0.5f, -0.5f, -0.5f,    -0.5f,  0.5f, -0.5f + sh,   0.5f,  0.5f, -0.5f + sh,    0.5f, -0.5f, -0.5f,
    };
    float normals[vertexCount * 3] = {
        0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,   0.0f, -1.0f, 0.0f,
        0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,   0.0f,  1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,   1.0f, 0.0f, 0.0f,
        -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,   -1.0f, 0.0f, 0.0f,
        0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,     0.0f, -sh, 1.0f,
        0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,    0.0f,  sh, -1.0f,
    };
    float texcoords[vertexCount * 2] = {
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
        0.0f, 0.0f,  1.0f, 0.0f,  1.0f, 1.0f,  0.0f, 1.0f,
    };
    unsigned short indices[indexCount] = {
        0, 2, 1,  0, 3, 2,
        4, 6, 5,  4, 7, 6,
        8, 10, 9,  8, 11, 10,
        12, 14, 13,  12, 15, 14,
        16, 18, 17,  16, 19, 18,
        20, 22, 21,  20, 23, 22,
    };

    mesh.vertexCount = vertexCount;
    mesh.triangleCount = indexCount / 3;
    mesh.vertices = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.vertices, vertices, vertexCount * 3 * sizeof(float));
    mesh.texcoords = (float*)RL_MALLOC(vertexCount * 2 * sizeof(float));
    memcpy(mesh.texcoords, texcoords, vertexCount * 2 * sizeof(float));
    mesh.normals = (float*)RL_MALLOC(vertexCount * 3 * sizeof(float));
    memcpy(mesh.normals, normals, vertexCount * 3 * sizeof(float));
    mesh.colors = (unsigned char*)RL_MALLOC(vertexCount * 4 * sizeof(unsigned char));
    for (int i = 0; i < vertexCount * 4; i++) mesh.colors[i] = 255;
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
    if (!shaderCache::LoadBinary("lit_shader_v2", litShader)) {
        litShader = LoadShaderFromMemory(kVertexShader, kFragmentShader);
        if (litShader.id != 0) {
            shaderCache::SaveBinary("lit_shader_v2", litShader);
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

    // Road shader: same lighting as litShader but with a depth bias so the flat
    // road mesh always wins against the near-coplanar ground plane at altitude.
    if (!shaderCache::LoadBinary("lit_shader_road_v4", roadShader)) {
        roadShader = LoadShaderFromMemory(kVertexShaderRoad, kFragmentShader);
        if (roadShader.id != 0) {
            shaderCache::SaveBinary("lit_shader_road_v4", roadShader);
        }
    }
    // Loud diagnostic: this is the ONLY place a real-GPU link failure can silently
    // hide (raylib returns id==0 and falls back to the default shader, losing the
    // depth bias -> coplanar roads re-fight the ground at altitude). The headless
    // harness stubs GetRoadShader() so it can never see this; the file + loud log
    // let the real application surface it.
    if (roadShader.id == 0) {
        FILE* dzD = nullptr;
        fopen_s(&dzD, "road_shader_status.txt", "w");
        if (dzD) {
            fputs("ROAD_SHADER_LINK_FAILED - road depth bias INACTIVE, expect z-fighting.\n", dzD);
            fclose(dzD);
        }
        TraceLog(LOG_ERROR, "ROAD SHADER LINK FAILED (id=0): depth bias inactive, "
                            "roads will z-fight the ground at altitude. Check road_shader_status.txt");
    } else {
        FILE* dzD = nullptr;
        fopen_s(&dzD, "road_shader_status.txt", "w");
        if (dzD) {
            fputs("ROAD_SHADER_LINK_OK - depth bias active (constant world-space offset).\n", dzD);
            fclose(dzD);
        }
        TraceLog(LOG_INFO, "Road shader linked OK (id=%i): depth bias active.", roadShader.id);
    }
    // raylib only uploads matProjection to shaders whose projection loc is registered.
    if (roadShader.id != 0 && roadShader.locs)
        roadShader.locs[SHADER_LOC_MATRIX_PROJECTION] = GetShaderLocation(roadShader, "matProjection");
    roadLightDirLoc = GetShaderLocation(roadShader, "lightDir");
    roadAmbientLoc = GetShaderLocation(roadShader, "ambient");
    roadLightVPLoc = GetShaderLocation(roadShader, "lightVP");
    roadShadowMapLoc = GetShaderLocation(roadShader, "shadowMap");
    roadShadowsEnabledLoc = GetShaderLocation(roadShader, "shadowsEnabled");
    roadWaterSurfaceYLoc = GetShaderLocation(roadShader, "waterSurfaceY");
    if (roadShadowMapLoc != -1) SetShaderValue(roadShader, roadShadowMapLoc, &shadowSlot, SHADER_UNIFORM_INT);

    // Instanced lit shader (city buildings). Own uniforms mirror litShader's.
    if (!shaderCache::LoadBinary("instanced_lit_shader_v3", cityInstancedShader)) {
        cityInstancedShader = LoadShaderFromMemory(kVertexShaderInstanced, kFragmentShader);
        if (cityInstancedShader.id != 0) {
            shaderCache::SaveBinary("instanced_lit_shader_v3", cityInstancedShader);
        }
    }
    if (cityInstancedShader.id != 0) {
        cityLightDirLoc = GetShaderLocation(cityInstancedShader, "lightDir");
        cityAmbientLoc = GetShaderLocation(cityInstancedShader, "ambient");
        cityLightVPLoc = GetShaderLocation(cityInstancedShader, "lightVP");
        cityShadowMapLoc = GetShaderLocation(cityInstancedShader, "shadowMap");
        cityShadowsEnabledLoc = GetShaderLocation(cityInstancedShader, "shadowsEnabled");
        cityWaterSurfaceYLoc = GetShaderLocation(cityInstancedShader, "waterSurfaceY");
        if (cityShadowMapLoc != -1) SetShaderValue(cityInstancedShader, cityShadowMapLoc, &shadowSlot, SHADER_UNIFORM_INT);
    }

    // Load the shared default texture BEFORE any material grabs it, otherwise
    // materials keep texture id 0 and the shaders sample an unbound unit (black).
    Image checker = GenImageChecked(64, 64, 8, 8, LIGHTGRAY, GRAY);
    defaultTexture = LoadTextureFromImage(checker);
    UnloadImage(checker);

    cityInstancedMaterial = LoadMaterialDefault();
    cityInstancedMaterial.shader = cityInstancedShader;
    cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].texture = defaultTexture;
    cityInstancedReady = cityInstancedShader.id != 0;

    cubeModel = LoadModelFromMesh(GenMeshCube(1.0f, 1.0f, 1.0f));
    sphereModel = LoadModelFromMesh(GenMeshSphere(0.5f, 16, 16));
    cylinderModel = LoadModelFromMesh(GenMeshCylinder(0.5f, 1.0f, 16));
    wedgeModel = LoadModelFromMesh(GenerateWedgeMesh());
    cityWedgeMesh = GenerateCityWedgeMesh();
    citySlantMesh = GenerateCitySlantMesh();

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

    reflectionTarget = LoadRenderTexture(reflectionResolution, reflectionResolution);
    SetTextureFilter(reflectionTarget.texture, TEXTURE_FILTER_BILINEAR);

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
    UnloadMesh(cityWedgeMesh);
    UnloadMesh(citySlantMesh);
    UnloadModel(groundModel);
    UnloadTexture(defaultTexture);
    UnloadTexture(groundTexture);
    if (shadowMap.id > 0) rlUnloadFramebuffer(shadowMap.id);
    if (reflectionTarget.id > 0) UnloadRenderTexture(reflectionTarget);
    if (cityInstancedShader.id != 0) UnloadShader(cityInstancedShader);
    if (roadShader.id != 0) UnloadShader(roadShader);
    cityInstancedReady = false;
    cityInstancedMaterial = {};
    UnloadShader(litShader);

    shaderCache::Shutdown();
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
    switch (shape) {
        case 1: return cityWedgeMesh;
        case 2: return citySlantMesh;
        default: return cubeModel.meshes[0];
    }
}

void SetShadowsEnabled(bool enabled) { shadowsEnabled = enabled; shadowsDirty = true; }

bool IsShadowsEnabled() { return shadowsEnabled; }

void SetShadowQuality(int quality) {
    quality = Clamp(quality, 5, 100);
    if (quality == shadowQuality && shadowMap.id > 0) return;
    shadowQuality = quality;

    int resolution = QualityToResolution(quality);
    if (resolution == shadowMapResolution && shadowMap.id > 0) return;
    shadowMapResolution = resolution;
    shadowsDirty = true;

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

    // The shadow frustum follows the viewer. It used to be a fixed 70 m box at the
    // world origin, so anything further away (most of a city) got no shadows and
    // tall buildings were clipped by the near plane of a light only 40 m up.
    Vector3 focus = { 0.0f, 0.0f, 0.0f };
    float half = kShadowMinHalf;
    if (haveShadowViewCamera) {
        const Camera3D& cam = shadowViewCamera;
        const Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        if (cam.position.y > 0.0f && fwd.y < -0.05f) {
            const float t = fminf(-cam.position.y / fwd.y, 800.0f);
            focus = Vector3Add(cam.position, Vector3Scale(fwd, t));
            focus.y = 0.0f;
        } else {
            focus = Vector3Add(cam.position, Vector3Scale(fwd, 60.0f));
            focus.y = 0.0f;
        }
        const float dist = Vector3Distance(cam.position, focus);
        half = fminf(fmaxf(0.7f * dist + 30.0f, kShadowMinHalf), kShadowMaxHalf);
        half = ceilf(half / 10.0f) * 10.0f; // quantize so the map scale doesn't swim
    }

    // Snap the frustum centre to whole shadow texels in light space (no shimmering
    // while the camera moves). Basis matches raylib's lookAt for up = (0,1,0).
    const Vector3 lightRight = Vector3Normalize(Vector3CrossProduct(kLightDir, { 0.0f, 1.0f, 0.0f }));
    const Vector3 lightUp = Vector3CrossProduct(lightRight, kLightDir);
    const float texel = (2.0f * half) / (float)shadowMapResolution;
    const float cx = Vector3DotProduct(focus, lightRight);
    const float cy = Vector3DotProduct(focus, lightUp);
    const float sx = floorf(cx / texel) * texel;
    const float sy = floorf(cy / texel) * texel;
    focus = Vector3Add(focus, Vector3Add(Vector3Scale(lightRight, sx - cx), Vector3Scale(lightUp, sy - cy)));

    lightCamera.target = focus;
    lightCamera.position = Vector3Subtract(focus, Vector3Scale(kLightDir, kShadowLightDist));
    lightCamera.up = { 0.0f, 1.0f, 0.0f };
    lightCamera.projection = CAMERA_ORTHOGRAPHIC;

    // Reuse the previous depth map when nothing that could change it happened.
    if (SceneInputActivity()) shadowInputHold = kShadowInputHold;
    const bool frustumSame = shadowHaveRendered && shadowLastRes == shadowMapResolution &&
                             shadowLastHalf == half && Vector3Equals(shadowLastCenter, focus);
    shadowPassReused = shadowReuseEnabled && frustumSame && !shadowsDirty.load() &&
                       shadowInputHold == 0 && shadowFramesSinceRender < kShadowMaxAge &&
                       !ui::IsPlayActive();
    if (shadowInputHold > 0) shadowInputHold--;

    BeginTextureMode(shadowMap);
    if (shadowPassReused) {
        // Keep the depth map as is: entities still "draw" between Begin/EndShadowPass,
        // but an empty scissor rect discards every fragment.
        rlEnableScissorTest();
        rlScissor(0, 0, 0, 0);
        shadowFramesSinceRender++;
    } else {
        ClearBackground(WHITE);
        shadowsDirty = false;
        shadowHaveRendered = true;
        shadowLastCenter = focus;
        shadowLastHalf = half;
        shadowLastRes = shadowMapResolution;
        shadowFramesSinceRender = 0;
    }

    BeginMode3D(lightCamera);
    rlSetMatrixProjection(MatrixOrtho(-half, half, -half, half, kShadowNear, kShadowFar));

    lightView = rlGetMatrixModelview();
    lightProj = rlGetMatrixProjection();
}

void EndShadowPass() {
    if (!shadowsEnabled || shadowMap.id == 0) return;

    EndMode3D();
    EndTextureMode();
    if (shadowPassReused) rlDisableScissorTest();
    shadowPassReused = false;

    inShadowPass = false;
    lightViewProj = MatrixMultiply(lightView, lightProj);

    // The frustum moves every frame, so hand the fresh matrix to the receiving
    // shaders now instead of relying on UpdateLighting() running after this pass.
    if (litShader.id != 0 && lightVPLoc != -1) SetShaderValueMatrix(litShader, lightVPLoc, lightViewProj);
    if (roadShader.id != 0 && roadLightVPLoc != -1) SetShaderValueMatrix(roadShader, roadLightVPLoc, lightViewProj);
    if (cityInstancedReady && cityLightVPLoc != -1) SetShaderValueMatrix(cityInstancedShader, cityLightVPLoc, lightViewProj);
}

bool IsInShadowPass() {
    return inShadowPass;
}

void SetReflectionsEnabled(bool enabled) { reflectionEnabled = enabled; }

bool IsReflectionsEnabled() { return reflectionEnabled; }

void SetReflectionQuality(int quality) {
    quality = Clamp(quality, 5, 100);
    if (quality == reflectionQuality && reflectionTarget.id > 0) return;
    reflectionQuality = quality;

    int resolution = ReflectionQualityToResolution(quality);
    if (resolution == reflectionResolution && reflectionTarget.id > 0) return;
    reflectionResolution = resolution;

    if (reflectionTarget.id > 0) UnloadRenderTexture(reflectionTarget);
    reflectionTarget = LoadRenderTexture(reflectionResolution, reflectionResolution);
    SetTextureFilter(reflectionTarget.texture, TEXTURE_FILTER_BILINEAR);
}

int GetReflectionQuality() { return reflectionQuality; }

Camera3D BeginReflectionPass(float planeHeight, const Camera3D& worldCamera, Color clearColor) {
    if (!reflectionEnabled || reflectionTarget.id == 0) {
        inReflectionPass = false;
        return worldCamera;
    }

    inReflectionPass = true;

    // Mirror the camera about the horizontal plane y = planeHeight. Objects above
    // the water appear below the mirrored camera, producing a true planar mirror.
    Camera3D mirrored = worldCamera;
    float planeY = planeHeight;
    mirrored.position.y = 2.0f * planeY - mirrored.position.y;
    mirrored.target.y = 2.0f * planeY - mirrored.target.y;
    mirrored.up.y = -mirrored.up.y;

    // Guard against a degenerate basis when looking straight up/down (the mirrored
    // up vector becomes parallel to the view direction).
    Vector3 fwd = Vector3Normalize(Vector3Subtract(mirrored.target, mirrored.position));
    if (fabsf(Vector3DotProduct(fwd, mirrored.up)) > 0.999f) {
        mirrored.up = { 0.0f, 0.0f, 1.0f };
    }

    BeginTextureMode(reflectionTarget);
    ClearBackground(clearColor);

    return mirrored;
}

void EndReflectionPass() {
    if (!inReflectionPass || reflectionTarget.id == 0) return;

    reflView = rlGetMatrixModelview();
    reflProj = rlGetMatrixProjection();

    rlDrawRenderBatchActive();
    EndMode3D();
    EndTextureMode();

    inReflectionPass = false;
    reflViewProj = MatrixMultiply(reflView, reflProj);
}

bool IsInReflectionPass() {
    return inReflectionPass;
}

Matrix GetReflectionViewProj() { return reflViewProj; }

RenderTexture2D GetReflectionTarget() { return reflectionTarget; }

int GetReflectionTextureSlot() { return REFLECTION_TEXTURE_SLOT; }

void UpdateLighting(const Camera3D& camera) {
    shadowViewCamera = camera;
    haveShadowViewCamera = true;

    SetShaderValue(litShader, lightDirLoc, &kLightDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(litShader, ambientLoc, &ambient, SHADER_UNIFORM_VEC3);
    SetShaderValueMatrix(litShader, lightVPLoc, lightViewProj);

    float enabled = shadowsEnabled ? 1.0f : 0.0f;
    SetShaderValue(litShader, shadowsEnabledLoc, &enabled, SHADER_UNIFORM_FLOAT);

    if (roadShader.id != 0) {
        SetShaderValue(roadShader, roadLightDirLoc, &kLightDir, SHADER_UNIFORM_VEC3);
        SetShaderValue(roadShader, roadAmbientLoc, &ambient, SHADER_UNIFORM_VEC3);
        SetShaderValueMatrix(roadShader, roadLightVPLoc, lightViewProj);
        SetShaderValue(roadShader, roadShadowsEnabledLoc, &enabled, SHADER_UNIFORM_FLOAT);
    }

    if (shadowsEnabled && shadowMap.depth.id > 0) {
        rlActiveTextureSlot(SHADOW_TEXTURE_SLOT);
        rlEnableTexture(shadowMap.depth.id);
    }
}

void SetupInstancedLighting() {
    if (!cityInstancedReady) return;
    SetShaderValue(cityInstancedShader, cityLightDirLoc, &kLightDir, SHADER_UNIFORM_VEC3);
    SetShaderValue(cityInstancedShader, cityAmbientLoc, &ambient, SHADER_UNIFORM_VEC3);
    SetShaderValueMatrix(cityInstancedShader, cityLightVPLoc, lightViewProj);

    float enabled = shadowsEnabled ? 1.0f : 0.0f;
    SetShaderValue(cityInstancedShader, cityShadowsEnabledLoc, &enabled, SHADER_UNIFORM_FLOAT);

    if (shadowsEnabled && shadowMap.depth.id > 0) {
        rlActiveTextureSlot(SHADOW_TEXTURE_SLOT);
        rlEnableTexture(shadowMap.depth.id);
    }
}

void DrawCityInstances(Mesh mesh, const std::vector<Matrix>& transforms, int start, int count, Color tint) {
    if (!cityInstancedReady || count <= 0) return;
    if (start < 0 || (size_t)(start + count) > transforms.size()) return;
    if (!inShadowPass) SetupInstancedLighting();
    cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].color = tint;
    DrawMeshInstanced(mesh, cityInstancedMaterial, transforms.data() + start, count);
}

unsigned int CreateInstanceBuffer(const std::vector<Matrix>& transforms) {
    if (transforms.empty()) return 0;
    // GL wants column-major float16 (m0,m1,m2,...), NOT the Matrix struct's memory order.
    std::vector<float16> data(transforms.size());
    for (size_t i = 0; i < transforms.size(); i++) data[i] = MatrixToFloatV(transforms[i]);
    return rlLoadVertexBuffer(data.data(), (int)(data.size() * sizeof(float16)), false);
}

void DestroyInstanceBuffer(unsigned int id) {
    if (id != 0) rlUnloadVertexBuffer(id);
}

void SetInstanceBuffersEnabled(bool enabled) { instanceBuffersEnabled = enabled; }
bool GetInstanceBuffersEnabled() { return instanceBuffersEnabled; }

bool InstanceBuffersActive() {
    if (!instanceBuffersEnabled || !cityInstancedReady) return false;
    const int ver = rlGetVersion();
    if (ver != RL_OPENGL_33 && ver != RL_OPENGL_43 && ver != RL_OPENGL_ES_30) return false;
    return cityInstancedShader.locs != nullptr &&
           cityInstancedShader.locs[SHADER_LOC_MATRIX_MODEL] != -1 &&
           cityInstancedShader.locs[SHADER_LOC_MATRIX_MVP] != -1;
}

// Same state setup as raylib's DrawMeshInstanced(), but the per-instance
// transforms come from a persistent VBO instead of being uploaded every call.
void DrawCityInstancesBuffered(Mesh mesh, unsigned int instanceVbo, int count, Color tint) {
    if (!InstanceBuffersActive() || count <= 0 || instanceVbo == 0 || mesh.vaoId == 0) return;
    if (!inShadowPass) SetupInstancedLighting();

    const Shader& sh = cityInstancedShader;
    rlEnableShader(sh.id);

    if (sh.locs[SHADER_LOC_COLOR_DIFFUSE] != -1) {
        const float v[4] = { tint.r / 255.0f, tint.g / 255.0f, tint.b / 255.0f, tint.a / 255.0f };
        rlSetUniform(sh.locs[SHADER_LOC_COLOR_DIFFUSE], v, SHADER_UNIFORM_VEC4, 1);
    }
    if (sh.locs[SHADER_LOC_COLOR_SPECULAR] != -1) {
        const Color sc = cityInstancedMaterial.maps[MATERIAL_MAP_SPECULAR].color;
        const float v[4] = { sc.r / 255.0f, sc.g / 255.0f, sc.b / 255.0f, sc.a / 255.0f };
        rlSetUniform(sh.locs[SHADER_LOC_COLOR_SPECULAR], v, SHADER_UNIFORM_VEC4, 1);
    }

    const Matrix matView = rlGetMatrixModelview();
    const Matrix matProjection = rlGetMatrixProjection();
    if (sh.locs[SHADER_LOC_MATRIX_VIEW] != -1) rlSetUniformMatrix(sh.locs[SHADER_LOC_MATRIX_VIEW], matView);
    if (sh.locs[SHADER_LOC_MATRIX_PROJECTION] != -1) rlSetUniformMatrix(sh.locs[SHADER_LOC_MATRIX_PROJECTION], matProjection);

    // Point the mesh VAO's instance attributes at this tile's buffer.
    const int locModel = sh.locs[SHADER_LOC_MATRIX_MODEL];
    rlEnableVertexArray(mesh.vaoId);
    rlEnableVertexBuffer(instanceVbo);
    for (unsigned int i = 0; i < 4; i++) {
        rlEnableVertexAttribute((unsigned int)locModel + i);
        SetInstAttr((unsigned int)locModel + i, (int)(i * sizeof(Vector4)), 0);
        rlSetVertexAttributeDivisor((unsigned int)locModel + i, 1);
    }
    rlDisableVertexBuffer();
    rlDisableVertexArray();

    const Matrix matModel = MatrixIdentity();
    const Matrix matModelView = MatrixMultiply(rlGetMatrixTransform(), matView);
    if (sh.locs[SHADER_LOC_MATRIX_NORMAL] != -1)
        rlSetUniformMatrix(sh.locs[SHADER_LOC_MATRIX_NORMAL], MatrixTranspose(MatrixInvert(matModel)));

    // Diffuse map (the only map the city material uses).
    const Texture2D& tex = cityInstancedMaterial.maps[MATERIAL_MAP_DIFFUSE].texture;
    if (tex.id > 0) {
        rlActiveTextureSlot(MATERIAL_MAP_DIFFUSE);
        rlEnableTexture(tex.id);
        const int slot = MATERIAL_MAP_DIFFUSE;
        if (sh.locs[SHADER_LOC_MAP_DIFFUSE] != -1)
            rlSetUniform(sh.locs[SHADER_LOC_MAP_DIFFUSE], &slot, SHADER_UNIFORM_INT, 1);
    }

    rlEnableVertexArray(mesh.vaoId);
    rlSetUniformMatrix(sh.locs[SHADER_LOC_MATRIX_MVP], MatrixMultiply(matModelView, matProjection));
    if (mesh.indices != nullptr) rlDrawVertexArrayElementsInstanced(0, mesh.triangleCount * 3, 0, count);
    else rlDrawVertexArrayInstanced(0, mesh.vertexCount, count);

    if (tex.id > 0) {
        rlActiveTextureSlot(MATERIAL_MAP_DIFFUSE);
        rlDisableTexture();
    }
    rlDisableVertexArray();
    rlDisableVertexBuffer();
    rlDisableVertexBufferElement();
    rlDisableShader();
}

void SetShadowReuseEnabled(bool enabled) { shadowReuseEnabled = enabled; shadowsDirty = true; }
bool GetShadowReuseEnabled() { return shadowReuseEnabled; }
void MarkShadowsDirty() { shadowsDirty = true; }
bool IsShadowPassReused() { return inShadowPass && shadowPassReused; }

void SetUnderwaterParams(float waterY, Vector3, float, Vector3) {
    if (waterSurfaceYLoc != -1) SetShaderValue(litShader, waterSurfaceYLoc, &waterY, SHADER_UNIFORM_FLOAT);
    if (cityWaterSurfaceYLoc != -1 && cityInstancedShader.id != 0)
        SetShaderValue(cityInstancedShader, cityWaterSurfaceYLoc, &waterY, SHADER_UNIFORM_FLOAT);
    if (roadWaterSurfaceYLoc != -1 && roadShader.id != 0)
        SetShaderValue(roadShader, roadWaterSurfaceYLoc, &waterY, SHADER_UNIFORM_FLOAT);
}

void DrawGround() {
    if (!gridVisible) return;
    groundModel.transform = MatrixIdentity();
    DrawModel(groundModel, Vector3Zero(), 1.0f, WHITE);
}

}