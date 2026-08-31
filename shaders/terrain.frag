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

out vec4 fragColor;

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
    float metallic = 0.0;
    
    for (int i = 0; i < 4; i++) {
        if (i >= layerCount) break;
        if (weights[i] <= 0.0) continue;
        
        float w = weights[i];
        
        vec3 layerAlbedo, layerNormal;
        float layerRoughness;
        
        if (useTriplanar && (abs(worldNormal.y) < 0.7)) {
            // Use triplanar for steep slopes
            layerAlbedo = TriplanarSample(albedoTex[i], worldPos, worldNormal, 1.0 / tileSize[i]);
            layerNormal = TriplanarNormal(normalTex[i], worldPos, worldNormal, 1.0 / tileSize[i]);
            layerRoughness = TriplanarSample(roughnessTex[i], worldPos, worldNormal, 1.0 / tileSize[i]).r;
        } else {
            // Standard UV mapping
            layerAlbedo = texture(albedoTex[i], texCoord[i]).rgb;
            layerNormal = ApplyNormalMap(normalTex[i], texCoord[i], worldNormal, tangent, bitangent);
            layerRoughness = texture(roughnessTex[i], texCoord[i]).r;
        }
        
        albedo += layerAlbedo * w;
        normal = normalize(normal + (layerNormal - worldNormal) * w);
        roughness += layerRoughness * w;
    }
    
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