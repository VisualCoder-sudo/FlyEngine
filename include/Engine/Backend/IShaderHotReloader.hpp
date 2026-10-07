#pragma once

#include "GraphicsBackendTypes.hpp"
#include <functional>
#include <string>
#include <unordered_map>

namespace gpu {

class IShaderHotReloader {
public:
    virtual ~IShaderHotReloader() = default;
    
    // Register a shader for hot-reload
    // sourcePath: path to common shader format (.shader file)
    // onReload: callback when recompilation succeeds
    virtual void RegisterShader(const std::string& name, const std::string& sourcePath, 
                                std::function<void(ShaderHandle newShader)> onReload) = 0;
    
    // Poll for file changes, recompile if needed
    virtual void Update() = 0;
    
    // Force recompile all registered shaders
    virtual void ReloadAll() = 0;
    
    // Get current shader handle
    virtual ShaderHandle GetShader(const std::string& name) = 0;
    
    // Check if shader was reloaded this frame
    virtual bool WasReloaded(const std::string& name) = 0;
};

} // namespace gpu

// Factory function
namespace gpu {
std::unique_ptr<IShaderHotReloader> CreateShaderHotReloader();  // defined in ShaderHotReloader.cpp
}

// ============================================================================
// Common Shader Format (.shader file)
// ============================================================================
//
// ---
// [vertex]
// #version 450
// layout(set=0, binding=0) uniform VSParams {
//     mat4 mvp;
//     mat4 matNormal;
//     mat4 matModel;
//     mat4 lightVP;
// };
//
// layout(location = 0) in vec3 vertexPosition;
// layout(location = 1) in vec2 vertexTexCoord;
// layout(location = 2) in vec3 vertexNormal;
// layout(location = 3) in vec4 vertexColor;
//
// layout(location = 0) out vec2 fragTexCoord;
// layout(location = 1) out vec4 fragColor;
// layout(location = 2) out vec3 fragNormal;
// layout(location = 3) out vec4 fragShadowCoord;
// layout(location = 4) out vec3 fragWorldPos;
//
// void main() {
//     fragTexCoord = vertexTexCoord;
//     fragColor = vertexColor;
//     fragNormal = normalize(mat3(matNormal) * vertexNormal);
//     vec4 worldPos = matModel * vec4(vertexPosition, 1.0);
//     fragShadowCoord = lightVP * worldPos;
//     fragWorldPos = worldPos.xyz;
//     gl_Position = mvp * vec4(vertexPosition, 1.0);
// }
//
// [fragment]
// #version 450
// layout(std140, set=0, binding=1) uniform FSParams {
//     vec4 colDiffuse;
//     vec3 lightDir;
//     vec3 ambient;
//     float shadowsEnabled;
//     float waterSurfaceY;
//     mat4 lightVP;
// };
//
// layout(set=1, binding=0) uniform sampler2D texture0;
// layout(set=1, binding=1) uniform sampler2D shadowMap;
//
// layout(location = 0) in vec2 fragTexCoord;
// layout(location = 1) in vec4 fragColor;
// layout(location = 2) in vec3 fragNormal;
// layout(location = 3) in vec4 fragShadowCoord;
// layout(location = 4) in vec3 fragWorldPos;
//
// layout(location = 0) out vec4 finalColor;
//
// // ... fragment shader code
// ---