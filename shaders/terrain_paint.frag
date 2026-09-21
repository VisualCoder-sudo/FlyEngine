#version 330

in vec3 worldPos;
in vec3 worldNormal;
in vec2 texCoord;

uniform sampler2D splatmap;
uniform sampler2D albedoTex0;
uniform sampler2D albedoTex1;
uniform sampler2D albedoTex2;
uniform sampler2D albedoTex3;
uniform int layerCount;
uniform float textureTiling;
uniform vec3 lightDir;
uniform vec3 lightColor;
uniform vec3 ambientColor;

out vec4 fragColor;

void main() {
    vec2 splatUV = texCoord / max(textureTiling, 0.001);
    vec4 weights = texture(splatmap, splatUV).rgba;
    float weightSum = weights.r + weights.g + weights.b + weights.a;
    if (weightSum > 0.001) weights /= weightSum;
    else weights = vec4(1.0, 0.0, 0.0, 0.0);

    vec3 albedo = vec3(0.0);
    if (layerCount > 0 && weights.r > 0.001) albedo += texture(albedoTex0, texCoord).rgb * weights.r;
    if (layerCount > 1 && weights.g > 0.001) albedo += texture(albedoTex1, texCoord).rgb * weights.g;
    if (layerCount > 2 && weights.b > 0.001) albedo += texture(albedoTex2, texCoord).rgb * weights.b;
    if (layerCount > 3 && weights.a > 0.001) albedo += texture(albedoTex3, texCoord).rgb * weights.a;

    vec3 N = normalize(worldNormal);
    vec3 L = normalize(-lightDir);
    float NdotL = max(dot(N, L), 0.0);
    vec3 color = albedo * (ambientColor + lightColor * NdotL);
    color = pow(color, vec3(1.0 / 2.2));
    fragColor = vec4(color, 1.0);
}
