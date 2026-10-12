// ============================================================================
// gpu::IShaderHotReloader implementation using glslang for runtime GLSL -> SPIR-V
// ============================================================================

#include "Engine/Backend/IShaderHotReloader.hpp"
#include "Engine/Backend/GraphicsBackend.hpp"
#include "Engine/Backend/GraphicsBackendTypes.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <chrono>

#include <glslang/SPIRV/GlslangToSpv.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

namespace fs = std::filesystem;

namespace gpu {

// ============================================================================
// Internal helpers
// ============================================================================

static bool g_glslangInitialized = false;
static std::mutex g_glslangMutex;

static void EnsureGlslangInitialized() {
    std::lock_guard<std::mutex> lock(g_glslangMutex);
    if (!g_glslangInitialized) {
        glslang::InitializeProcess();
        g_glslangInitialized = true;
    }
}

static EShLanguage ShaderStageToGlslang(ShaderStage stage) {
    switch (stage) {
        case ShaderStage::Vertex:   return EShLangVertex;
        case ShaderStage::Fragment: return EShLangFragment;
        case ShaderStage::Compute:  return EShLangCompute;
        default: return EShLangVertex;
    }
}

static bool CompileGlslToSpirv(const std::string& source, ShaderStage stage, 
                               std::vector<uint32_t>& outSpirv, std::string& outError) {
    EnsureGlslangInitialized();

    glslang::TShader shader(ShaderStageToGlslang(stage));
    const char* src = source.c_str();
    shader.setStrings(&src, 1);
    
    // Target Vulkan SPIR-V
    shader.setEnvInput(glslang::EShSourceGlsl, ShaderStageToGlslang(stage), glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);

    EShMessages msgs = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    
    if (!shader.parse(GetDefaultResources(), 450, false, msgs)) {
        outError = "GLSL parse failed: " + std::string(shader.getInfoLog()) + 
                   "\n" + std::string(shader.getInfoDebugLog());
        return false;
    }

    glslang::TProgram program;
    program.addShader(&shader);
    
    if (!program.link(msgs)) {
        outError = "GLSL link failed: " + std::string(program.getInfoLog()) +
                   "\n" + std::string(program.getInfoDebugLog());
        return false;
    }

    std::vector<unsigned int> spirv;
    glslang::SpvOptions opts;
    spv::SpvBuildLogger logger;
    
    glslang::GlslangToSpv(*program.getIntermediate(ShaderStageToGlslang(stage)), 
                          spirv, &logger, &opts);

    if (spirv.empty()) {
        outError = "SPIR-V generation produced empty output";
        return false;
    }

    outSpirv.assign(spirv.begin(), spirv.end());
    return true;
}

// Parse a .shader file into vertex/fragment sources
static bool ParseShaderFile(const std::string& path, std::string& outVsSource, std::string& outFsSource, std::string& outError) {
    std::ifstream file(path);
    if (!file) {
        outError = "Failed to open shader file: " + path;
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    
    enum class Section { None, Vertex, Fragment } section = Section::None;
    std::stringstream vsStream, fsStream;
    
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("[vertex]") != std::string::npos) {
            section = Section::Vertex;
            continue;
        }
        if (line.find("[fragment]") != std::string::npos) {
            section = Section::Fragment;
            continue;
        }
        if (line.find("[compute]") != std::string::npos) {
            section = Section::None; // Not supported yet
            continue;
        }
        
        if (section == Section::Vertex) {
            vsStream << line << '\n';
        } else if (section == Section::Fragment) {
            fsStream << line << '\n';
        }
    }

    outVsSource = vsStream.str();
    outFsSource = fsStream.str();

    if (outVsSource.empty() || outFsSource.empty()) {
        outError = "Shader file missing [vertex] or [fragment] section: " + path;
        return false;
    }

    return true;
}

// ============================================================================
// ShaderHotReloader implementation
// ============================================================================

class ShaderHotReloaderImpl : public IShaderHotReloader {
public:
    struct RegisteredShader {
        std::string name;
        std::string sourcePath;
        std::string vsSource;
        std::string fsSource;
        std::vector<uint32_t> vsSpirv;
        std::vector<uint32_t> fsSpirv;
        ShaderHandle handle = INVALID_HANDLE;
        std::function<void(ShaderHandle)> onReload;
        fs::file_time_type lastWriteTime;
        bool wasReloadedThisFrame = false;
        std::string lastError;
    };

    ShaderHotReloaderImpl() = default;
    ~ShaderHotReloaderImpl() override = default;

    void RegisterShader(const std::string& name, const std::string& sourcePath,
                        std::function<void(ShaderHandle)> onReload) override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (shaders_.find(name) != shaders_.end()) {
            // Already registered
            return;
        }

        RegisteredShader reg;
        reg.name = name;
        reg.sourcePath = sourcePath;
        reg.onReload = std::move(onReload);

        // Initial load
        if (LoadAndCompile(reg)) {
            shaders_[name] = std::move(reg);
        }
    }

    void Update() override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (auto& [name, reg] : shaders_) {
            reg.wasReloadedThisFrame = false;
            
            if (!fs::exists(reg.sourcePath)) continue;
            
            auto currentTime = fs::last_write_time(reg.sourcePath);
            if (currentTime > reg.lastWriteTime) {
                reg.lastWriteTime = currentTime;
                if (LoadAndCompile(reg)) {
                    reg.wasReloadedThisFrame = true;
                    if (reg.onReload) {
                        reg.onReload(reg.handle);
                    }
                }
            }
        }
    }

    void ReloadAll() override {
        std::lock_guard<std::mutex> lock(mutex_);
        
        for (auto& [name, reg] : shaders_) {
            if (LoadAndCompile(reg)) {
                reg.wasReloadedThisFrame = true;
                if (reg.onReload) {
                    reg.onReload(reg.handle);
                }
            }
        }
    }

    ShaderHandle GetShader(const std::string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shaders_.find(name);
        if (it != shaders_.end()) {
            return it->second.handle;
        }
        return INVALID_HANDLE;
    }

    bool WasReloaded(const std::string& name) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = shaders_.find(name);
        if (it != shaders_.end()) {
            return it->second.wasReloadedThisFrame;
        }
        return false;
    }

private:
    std::unordered_map<std::string, RegisteredShader> shaders_;
    std::mutex mutex_;

    bool LoadAndCompile(RegisteredShader& reg) {
        std::string vsSource, fsSource, error;
        
        if (!ParseShaderFile(reg.sourcePath, vsSource, fsSource, error)) {
            reg.lastError = error;
            return false;
        }

        std::vector<uint32_t> vsSpirv, fsSpirv;
        
        if (!CompileGlslToSpirv(vsSource, ShaderStage::Vertex, vsSpirv, error)) {
            reg.lastError = "Vertex: " + error;
            return false;
        }
        
        if (!CompileGlslToSpirv(fsSource, ShaderStage::Fragment, fsSpirv, error)) {
            reg.lastError = "Fragment: " + error;
            return false;
        }

        // Store sources for potential re-use
        reg.vsSource = std::move(vsSource);
        reg.fsSource = std::move(fsSource);
        reg.vsSpirv = std::move(vsSpirv);
        reg.fsSpirv = std::move(fsSpirv);
        reg.lastError.clear();
        
        return true;
    }
};

std::unique_ptr<IShaderHotReloader> CreateShaderHotReloader() {
    return std::make_unique<ShaderHotReloaderImpl>();
}

} // namespace gpu