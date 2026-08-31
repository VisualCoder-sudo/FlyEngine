#version 330

// Terrain Vertex Shader
// Supports: 4-layer splatmap blending, LOD geomorphing, tangent space normal mapping

in vec3 vertexPosition;
in vec3 vertexNormal;
in vec2 vertexTexCoord;
in vec4 vertexTangent;

uniform mat4 model;
uniform mat4 viewProj;
uniform vec3 cameraPos;
uniform float minHeight;
uniform float maxHeight;
uniform int layerCount;
uniform float tileSize[4];
uniform float morphFactor; // For geomorphing

out vec3 worldPos;
out vec3 worldNormal;
out vec3 viewDir;
out vec2 texCoord[4];
out vec3 tangent;
out vec3 bitangent;
out float heightNormalized;

void main() {
    // Apply model transform
    vec4 worldPos4 = model * vec4(vertexPosition, 1.0);
    worldPos = worldPos4.xyz;
    
    // Normal transform
    mat3 normalMatrix = mat3(transpose(inverse(model)));
    worldNormal = normalize(normalMatrix * vertexNormal);
    
    // View direction
    viewDir = normalize(cameraPos - worldPos);
    
    // Texture coordinates for each layer
    for (int i = 0; i < 4; i++) {
        texCoord[i] = vertexTexCoord * tileSize[i];
    }
    
    // Tangent space
    tangent = normalize(normalMatrix * vertexTangent.xyz);
    bitangent = cross(worldNormal, tangent) * vertexTangent.w;
    
    // Height normalized for blending
    heightNormalized = (worldPos.y - minHeight) / (maxHeight - minHeight);
    heightNormalized = clamp(heightNormalized, 0.0, 1.0);
    
    // Geomorphing (vertex morphing toward lower LOD)
    // This requires the lower LOD vertex position as an attribute
    // For now, pass through
    
    gl_Position = viewProj * worldPos4;
}