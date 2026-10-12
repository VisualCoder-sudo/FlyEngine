// Chunked-terrain shader (terrain::Terrain): 4-layer splatmap blending with
// normal/roughness maps, optional triplanar mapping and simple GGX lighting.
//
// Differences from the GLSL 330 original, forced by sokol-shdc:
//   * sampler arrays (albedoTex[4], ...) become numbered textures; the engine's
//     "albedoTex[i]" lookups resolve to them by name.
//   * the 12 layer textures share one sampler (sokol allows at most 12).
//   * per-layer UVs are computed in the fragment stage instead of passing a
//     varying array; tileSize is a vec4 ("tileSize[i]" addresses a component).
@module terrain
@include fly_common.glsl

@vs vs
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 model;
    mat4 viewProj;
    vec3 cameraPos;
    float minHeight;
    float maxHeight;
};
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexTangent;
out vec3 worldPos;
out vec3 worldNormal;
out vec3 viewDir;
out vec2 baseTexCoord;
out vec3 tangent;
out vec3 bitangent;
out float heightNormalized;
void main() {
    vec4 worldPos4 = model * vec4(vertexPosition, 1.0);
    worldPos = worldPos4.xyz;
    mat3 normalMatrix = mat3(transpose(inverse(model)));
    worldNormal = normalize(normalMatrix * vertexNormal);
    viewDir = normalize(cameraPos - worldPos);
    baseTexCoord = vertexTexCoord;
    tangent = normalize(normalMatrix * vertexTangent.xyz);
    bitangent = cross(worldNormal, tangent) * vertexTangent.w;
    heightNormalized = clamp((worldPos.y - minHeight) / (maxHeight - minHeight), 0.0, 1.0);
    gl_Position = fly_clip(viewProj * worldPos4);
}
@end

@fs fs
@include_block fly_color
layout(binding=1) uniform fs_params {
    vec4 tileSize;
    vec3 lightDir;
    int layerCount;
    vec3 lightColor;        // linear sunlight on a surface that faces it, times pi
    float _pad1;
    vec3 ambientColor;      // linear light from the sky
    int useTriplanar;
    vec3 cameraPos;
};
layout(binding=0) uniform texture2D albedoTex0;
layout(binding=1) uniform texture2D albedoTex1;
layout(binding=2) uniform texture2D albedoTex2;
layout(binding=3) uniform texture2D albedoTex3;
layout(binding=4) uniform texture2D normalTex0;
layout(binding=5) uniform texture2D normalTex1;
layout(binding=6) uniform texture2D normalTex2;
layout(binding=7) uniform texture2D normalTex3;
layout(binding=8) uniform texture2D roughnessTex0;
layout(binding=9) uniform texture2D roughnessTex1;
layout(binding=10) uniform texture2D roughnessTex2;
layout(binding=11) uniform texture2D roughnessTex3;
layout(binding=12) uniform texture2D splatmap;
layout(binding=0) uniform sampler layer_smp;
layout(binding=1) uniform sampler splatmap_smp;

in vec3 worldPos;
in vec3 worldNormal;
in vec3 viewDir;
in vec2 baseTexCoord;
in vec3 tangent;
in vec3 bitangent;
in float heightNormalized;
out vec4 fragColor;

const float PI = 3.14159265358979323846;

vec3 TriplanarSample(texture2D tex, vec3 p, vec3 normal, float scale) {
    vec3 an = abs(normal);
    vec3 w = an / (an.x + an.y + an.z);
    vec3 colX = texture(sampler2D(tex, layer_smp), p.zy * scale).rgb;
    vec3 colY = texture(sampler2D(tex, layer_smp), p.xz * scale).rgb;
    vec3 colZ = texture(sampler2D(tex, layer_smp), p.xy * scale).rgb;
    return colX * w.x + colY * w.y + colZ * w.z;
}

vec3 TriplanarNormal(texture2D tex, vec3 p, vec3 normal, float scale) {
    vec3 an = abs(normal);
    vec3 w = an / (an.x + an.y + an.z);
    vec3 nX = texture(sampler2D(tex, layer_smp), p.zy * scale).rgb * 2.0 - 1.0;
    vec3 nY = texture(sampler2D(tex, layer_smp), p.xz * scale).rgb * 2.0 - 1.0;
    vec3 nZ = texture(sampler2D(tex, layer_smp), p.xy * scale).rgb * 2.0 - 1.0;
    nX = vec3(-nX.z, nX.x, nX.y);
    nY = vec3(nY.y, -nY.z, nY.x);
    return normalize(nX * w.x + nY * w.y + nZ * w.z);
}

vec3 ApplyNormalMap(texture2D normalMap, vec2 uv) {
    vec3 s = texture(sampler2D(normalMap, layer_smp), uv).rgb * 2.0 - 1.0;
    mat3 tbn = mat3(tangent, bitangent, worldNormal);
    return normalize(tbn * s);
}

void BlendLayer(texture2D albedoMap, texture2D normalMap, texture2D roughMap,
                float tile, float w, inout vec3 albedo, inout vec3 normal, inout float roughness) {
    vec3 layerAlbedo, layerNormal;
    float layerRoughness;
    vec2 uv = baseTexCoord * tile;
    if (useTriplanar != 0 && abs(worldNormal.y) < 0.7) {
        float invTile = 1.0 / tile;
        layerAlbedo = TriplanarSample(albedoMap, worldPos, worldNormal, invTile);
        layerNormal = TriplanarNormal(normalMap, worldPos, worldNormal, invTile);
        layerRoughness = TriplanarSample(roughMap, worldPos, worldNormal, invTile).r;
    } else {
        layerAlbedo = texture(sampler2D(albedoMap, layer_smp), uv).rgb;
        layerNormal = ApplyNormalMap(normalMap, uv);
        layerRoughness = texture(sampler2D(roughMap, layer_smp), uv).r;
    }
    albedo += layerAlbedo * w;
    normal = normalize(normal + (layerNormal - worldNormal) * w);
    roughness += layerRoughness * w;
}

void main() {
    vec4 weights = texture(sampler2D(splatmap, splatmap_smp), baseTexCoord * tileSize.x).rgba;
    float weightSum = weights.r + weights.g + weights.b + weights.a;
    if (weightSum > 0.0) weights /= weightSum;
    else weights = vec4(1.0, 0.0, 0.0, 0.0);

    vec3 albedo = vec3(0.0);
    vec3 normal = worldNormal;
    float roughness = 0.5;
    if (0 < layerCount && weights[0] > 0.0) BlendLayer(albedoTex0, normalTex0, roughnessTex0, tileSize.x, weights[0], albedo, normal, roughness);
    if (1 < layerCount && weights[1] > 0.0) BlendLayer(albedoTex1, normalTex1, roughnessTex1, tileSize.y, weights[1], albedo, normal, roughness);
    if (2 < layerCount && weights[2] > 0.0) BlendLayer(albedoTex2, normalTex2, roughnessTex2, tileSize.z, weights[2], albedo, normal, roughness);
    if (3 < layerCount && weights[3] > 0.0) BlendLayer(albedoTex3, normalTex3, roughnessTex3, tileSize.w, weights[3], albedo, normal, roughness);

    albedo = fly_srgb_to_linear(albedo);
    vec3 N = normalize(normal);
    vec3 V = normalize(viewDir);
    vec3 L = normalize(-lightDir);
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float a = roughness * roughness;
    float a2 = a * a;
    float f = (NdotH * (a - 1.0) + 1.0);
    float D = a2 / (PI * f * f);
    float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float VdotH = max(dot(V, H), 0.0);
    float G = VdotH / (VdotH * (1.0 - k) + k);
    vec3 F0 = vec3(0.04);
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    vec3 specular = D * G * F * lightColor * NdotL;
    vec3 diffuse = albedo / PI * lightColor * NdotL;
    // Linear light; fog and the sRGB encoding come later, in the post passes.
    vec3 color = diffuse + specular + albedo * ambientColor;
    fragColor = vec4(color, 1.0);
}
@end

@program terrain vs fs
