#version 330

// Terrain Fragment Shader
// 4-layer splatmap blending with triplanar mapping support

in vec3 worldPos;
in vec3 worldNormal;
in vec3 viewDir;
in vec2 texCoord[4];
in vec3 tangent;
in vec3 bitangent;
in float heightNormalized;

uniform int layerCount;
uniform sampler2D albedoTex[4];
uniform sampler2D normalTex[4];
uniform sampler2D roughnessTex[4];
uniform sampler2D splatmap; // RGBA = 4 layer weights
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 ambientColor;
uniform float fogDensity;
uniform vec3 fogColor;
uniform bool useTriplanar;

// Also declared in terrain.vert. GLSL uniforms are not shared between stages
// the way varyings are: each stage has to declare what it uses, or the
// identifier is simply undefined. (Varyings must match *by name* across
// stages; uniforms are per-stage globals and only the name has to agree so the
// program linker can match their storage.)
uniform vec3 cameraPos;
uniform float tileSize[4];

out vec4 fragColor;

// PI is a preprocessor macro in rlgl.h, not a GLSL builtin, and LoadShader
// does not inject rlgl's defines into a shader loaded from a file. Declaring
// it here is what the other shaders in this project do.
const float PI = 3.14159265358979323846;

// Triplanar mapping helper
vec3 TriplanarSample(sampler2D tex, vec3 worldPos, vec3 normal, float scale) {
    vec3 absNormal = abs(normal);
    vec3 weights = absNormal / (absNormal.x + absNormal.y + absNormal.z);
    
    vec2 uvX = worldPos.zy * scale;
    vec2 uvY = worldPos.xz * scale;
    vec2 uvZ = worldPos.xy * scale;
    
    vec3 colX = texture(tex, uvX).rgb;
    vec3 colY = texture(tex, uvY).rgb;
    vec3 colZ = texture(tex, uvZ).rgb;
    
    return colX * weights.x + colY * weights.y + colZ * weights.z;
}

vec3 TriplanarNormal(sampler2D tex, vec3 worldPos, vec3 normal, float scale) {
    vec3 absNormal = abs(normal);
    vec3 weights = absNormal / (absNormal.x + absNormal.y + absNormal.z);
    
    vec2 uvX = worldPos.zy * scale;
    vec2 uvY = worldPos.xz * scale;
    vec2 uvZ = worldPos.xy * scale;
    
    vec3 nX = texture(tex, uvX).rgb * 2.0 - 1.0;
    vec3 nY = texture(tex, uvY).rgb * 2.0 - 1.0;
    vec3 nZ = texture(tex, uvZ).rgb * 2.0 - 1.0;
    
    // Transform normals to world space
    nX = vec3(-nX.z, nX.x, nX.y); // YZ plane
    nY = vec3(nY.y, -nY.z, nY.x); // XZ plane
    // nZ is already in XY plane
    
    return normalize(nX * weights.x + nY * weights.y + nZ * weights.z);
}

// TBN normal mapping
vec3 ApplyNormalMap(sampler2D normalMap, vec2 uv, vec3 worldNormal, vec3 tangent, vec3 bitangent) {
    vec3 normalMapSample = texture(normalMap, uv).rgb * 2.0 - 1.0;
    mat3 tbn = mat3(tangent, bitangent, worldNormal);
    return normalize(tbn * normalMapSample);
}

// Blends one layer into the accumulators.
//
// GLSL 3.30 forbids indexing a sampler array with anything other than a
// constant expression, so the samplers are taken as parameters -- passing
// albedoTex[0] as a sampler2D is legal, reading it as albedoTex[i] inside a
// loop is not. `idx` is still used to index texCoord and tileSize, which are
// ordinary (non-opaque) arrays and may be indexed by a runtime value.
void BlendLayer(sampler2D albedoMap, sampler2D normalMap, sampler2D roughMap,
                int idx, float w,
                inout vec3 albedo, inout vec3 normal, inout float roughness) {
    vec3 layerAlbedo, layerNormal;
    float layerRoughness;

    if (useTriplanar && (abs(worldNormal.y) < 0.7)) {
        // Triplanar for steep slopes, so cliffs do not smear.
        float invTile = 1.0 / tileSize[idx];
        layerAlbedo = TriplanarSample(albedoMap, worldPos, worldNormal, invTile);
        layerNormal = TriplanarNormal(normalMap, worldPos, worldNormal, invTile);
        layerRoughness = TriplanarSample(roughMap, worldPos, worldNormal, invTile).r;
    } else {
        layerAlbedo = texture(albedoMap, texCoord[idx]).rgb;
        layerNormal = ApplyNormalMap(normalMap, texCoord[idx], worldNormal, tangent, bitangent);
        layerRoughness = texture(roughMap, texCoord[idx]).r;
    }

    albedo    += layerAlbedo * w;
    normal     = normalize(normal + (layerNormal - worldNormal) * w);
    roughness += layerRoughness * w;
}

void main() {
    // Sample splatmap weights
    vec4 weights = texture(splatmap, texCoord[0]).rgba;
    
    // Normalize weights
    float weightSum = weights.r + weights.g + weights.b + weights.a;
    if (weightSum > 0.0) {
        weights /= weightSum;
    } else {
        weights = vec4(1.0, 0.0, 0.0, 0.0);
    }
    
    // Blend layers
    vec3 albedo = vec3(0.0);
    vec3 normal = worldNormal;
    float roughness = 0.5;
    
    // Unrolled rather than looped: see BlendLayer() for why the samplers cannot
    // be indexed by a loop variable. The layerCount and weight guards are what
    // the loop's "if (i >= layerCount) break;" and "if (weights[i] <= 0.0)
    // continue;" did.
    if (0 < layerCount && weights[0] > 0.0)
        BlendLayer(albedoTex[0], normalTex[0], roughnessTex[0], 0, weights[0], albedo, normal, roughness);
    if (1 < layerCount && weights[1] > 0.0)
        BlendLayer(albedoTex[1], normalTex[1], roughnessTex[1], 1, weights[1], albedo, normal, roughness);
    if (2 < layerCount && weights[2] > 0.0)
        BlendLayer(albedoTex[2], normalTex[2], roughnessTex[2], 2, weights[2], albedo, normal, roughness);
    if (3 < layerCount && weights[3] > 0.0)
        BlendLayer(albedoTex[3], normalTex[3], roughnessTex[3], 3, weights[3], albedo, normal, roughness);
    
    // Lighting (simple PBR)
    vec3 N = normalize(normal);
    vec3 V = normalize(viewDir);
    vec3 L = normalize(-lightDir);
    vec3 H = normalize(V + L);
    
    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    
    // GGX BRDF (simplified)
    float a = roughness * roughness;
    float a2 = a * a;
    float f = (NdotH * (a - 1.0) + 1.0);
    float D = a2 / (PI * f * f);
    
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float VdotH = max(dot(V, H), 0.0);
    float G = VdotH / (VdotH * (1.0 - k) + k);
    
    vec3 F0 = vec3(0.04); // Non-metallic
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    
    vec3 specular = D * G * F * lightColor * NdotL;
    vec3 diffuse = albedo / PI * lightColor * NdotL;
    
    vec3 color = diffuse + specular + albedo * ambientColor;
    
    // Fog
    float dist = length(worldPos - cameraPos);
    float fog = 1.0 - exp(-fogDensity * dist);
    fog = clamp(fog, 0.0, 1.0);
    color = mix(color, fogColor, fog);
    
    // Gamma correction
    color = pow(color, vec3(1.0/2.2));
    
    fragColor = vec4(color, 1.0);
}