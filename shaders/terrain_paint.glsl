// BasicTerrain splat-painting shader: four albedo layers blended by an RGBA
// splatmap, simple directional lighting, gamma-encoded output.
@module terrain_paint
@include fly_common.glsl

@vs vs
@include_block fly_clip
layout(binding=0) uniform vs_params {
    mat4 mvp;
    mat4 matModel;
};
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
out vec3 worldPos;
out vec3 worldNormal;
out vec2 texCoord;
void main() {
    vec4 worldPos4 = matModel * vec4(vertexPosition, 1.0);
    worldPos = worldPos4.xyz;
    mat3 normalMatrix = mat3(transpose(inverse(matModel)));
    worldNormal = normalize(normalMatrix * vertexNormal);
    texCoord = vertexTexCoord;
    gl_Position = fly_clip(mvp * vec4(vertexPosition, 1.0));
}
@end

@fs fs
layout(binding=1) uniform fs_params {
    vec3 lightDir;
    int layerCount;
    vec3 lightColor;
    float textureTiling;
    vec3 ambientColor;
};
layout(binding=0) uniform texture2D splatmap;
layout(binding=0) uniform sampler splatmap_smp;
layout(binding=1) uniform texture2D albedoTex0;
layout(binding=1) uniform sampler albedoTex0_smp;
layout(binding=2) uniform texture2D albedoTex1;
layout(binding=2) uniform sampler albedoTex1_smp;
layout(binding=3) uniform texture2D albedoTex2;
layout(binding=3) uniform sampler albedoTex2_smp;
layout(binding=4) uniform texture2D albedoTex3;
layout(binding=4) uniform sampler albedoTex3_smp;
in vec3 worldPos;
in vec3 worldNormal;
in vec2 texCoord;
out vec4 fragColor;
void main() {
    vec2 splatUV = texCoord / max(textureTiling, 0.001);
    vec4 weights = texture(sampler2D(splatmap, splatmap_smp), splatUV).rgba;
    float weightSum = weights.r + weights.g + weights.b + weights.a;
    if (weightSum > 0.001) weights /= weightSum;
    else weights = vec4(1.0, 0.0, 0.0, 0.0);

    vec3 albedo = vec3(0.0);
    if (layerCount > 0 && weights.r > 0.001) albedo += texture(sampler2D(albedoTex0, albedoTex0_smp), texCoord).rgb * weights.r;
    if (layerCount > 1 && weights.g > 0.001) albedo += texture(sampler2D(albedoTex1, albedoTex1_smp), texCoord).rgb * weights.g;
    if (layerCount > 2 && weights.b > 0.001) albedo += texture(sampler2D(albedoTex2, albedoTex2_smp), texCoord).rgb * weights.b;
    if (layerCount > 3 && weights.a > 0.001) albedo += texture(sampler2D(albedoTex3, albedoTex3_smp), texCoord).rgb * weights.a;

    vec3 N = normalize(worldNormal);
    vec3 L = normalize(-lightDir);
    float NdotL = max(dot(N, L), 0.0);
    vec3 color = albedo * (ambientColor + lightColor * NdotL);
    color = pow(color, vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
@end

@program terrain_paint vs fs
