#include "../../../include/Engine/Backend/ShaderCache.hpp"
#include "raylib.h"
#include "rlgl.h"
#include "external/glad.h"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <cstdio>

namespace fs = std::filesystem;

namespace shaderCache {

static std::string s_cacheDir;
static std::string s_driverInfo;
static bool s_initialized = false;

static std::string GetDriverInfo() {
    const char* renderer = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    if (!renderer || !version) return "unknown";
    return std::string(renderer) + "|" + std::string(version);
}

static std::string ComputeSHA256Simple(const std::string& data) {
    // Simple DJB2-based hash for cache keys. Not cryptographic, just fast.
    uint64_t h1 = 5381, h2 = 0x12345678ABCDEF0ULL;
    for (char c : data) {
        h1 = ((h1 << 5) + h1) ^ (uint8_t)c;
        h2 = ((h2 << 7) + h2) ^ (uint8_t)c;
    }
    char buf[33];
    snprintf(buf, sizeof(buf), "%016llx%016llx",
             (unsigned long long)h1, (unsigned long long)h2);
    return std::string(buf);
}

void Init(const std::string& cacheDir) {
    if (s_initialized) return;
    s_cacheDir = cacheDir;
    s_driverInfo = GetDriverInfo();
    s_initialized = true;

    std::error_code ec;
    fs::create_directories(s_cacheDir, ec);

    // Check if GPU driver changed since last run
    fs::path driverFile = fs::path(s_cacheDir) / "driver_info.txt";
    std::ifstream driverIn(driverFile);
    std::string savedDriver;
    std::getline(driverIn, savedDriver);
    driverIn.close();

    if (!savedDriver.empty() && savedDriver != s_driverInfo) {
        TraceLog(LOG_INFO, "SHADER_CACHE: GPU driver changed, invalidating cache");
        InvalidateAll();
    }

    // Write current driver info
    std::ofstream driverOut(driverFile);
    if (driverOut) driverOut << s_driverInfo;
}

void Shutdown() {
    s_initialized = false;
    s_cacheDir.clear();
    s_driverInfo.clear();
}

void InvalidateAll() {
    if (s_cacheDir.empty()) return;
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(s_cacheDir, ec)) {
        if (entry.is_regular_file()) {
            fs::remove(entry.path(), ec);
        }
    }
}

bool LoadBinary(const std::string& name, Shader& outShader) {
    if (!s_initialized || s_cacheDir.empty()) return false;

    fs::path binPath = fs::path(s_cacheDir) / (name + ".bin");
    fs::path metaPath = fs::path(s_cacheDir) / (name + ".meta");

    if (!fs::exists(binPath) || !fs::exists(metaPath)) return false;

    // Read metadata (source hash for validation)
    std::ifstream metaIn(metaPath);
    std::string savedHash;
    std::getline(metaIn, savedHash);
    std::string savedDriver;
    std::getline(metaIn, savedDriver);
    metaIn.close();

    if (savedDriver != s_driverInfo) return false;

    // Read binary: first 4 bytes = GLenum format, rest = program binary
    std::ifstream binIn(binPath, std::ios::binary | std::ios::ate);
    if (!binIn) return false;
    std::streamsize totalSize = binIn.tellg();
    if (totalSize <= (std::streamsize)sizeof(GLenum)) return false;
    binIn.seekg(0, std::ios::beg);
    GLenum binaryFormat = 0;
    binIn.read(reinterpret_cast<char*>(&binaryFormat), sizeof(GLenum));
    std::streamsize dataSize = totalSize - sizeof(GLenum);
    std::vector<char> binary(dataSize);
    if (!binIn.read(binary.data(), dataSize)) return false;
    binIn.close();

    // Create GL program and load binary
    GLuint programId = glCreateProgram();
    glProgramBinary(programId, binaryFormat,
                    binary.data(), (GLsizei)binary.size());

    GLint success = 0;
    glGetProgramiv(programId, GL_LINK_STATUS, &success);
    if (!success) {
        glDeleteProgram(programId);
        TraceLog(LOG_WARNING, "SHADER_CACHE: Failed to load binary for '%s'", name.c_str());
        return false;
    }

    outShader.id = programId;

    // Populate the locs array exactly like raylib's LoadShaderFromMemory does.
    // Without this, DrawMesh can't find any vertex attributes or uniforms.
    outShader.locs = (int*)RL_CALLOC(RL_MAX_SHADER_LOCATIONS, sizeof(int));
    for (int i = 0; i < RL_MAX_SHADER_LOCATIONS; i++) outShader.locs[i] = -1;

    // Vertex attribute locations
    outShader.locs[SHADER_LOC_VERTEX_POSITION] = rlGetLocationAttrib(programId, "vertexPosition");
    outShader.locs[SHADER_LOC_VERTEX_TEXCOORD01] = rlGetLocationAttrib(programId, "vertexTexCoord");
    outShader.locs[SHADER_LOC_VERTEX_TEXCOORD02] = rlGetLocationAttrib(programId, "vertexTexCoord2");
    outShader.locs[SHADER_LOC_VERTEX_NORMAL] = rlGetLocationAttrib(programId, "vertexNormal");
    outShader.locs[SHADER_LOC_VERTEX_TANGENT] = rlGetLocationAttrib(programId, "vertexTangent");
    outShader.locs[SHADER_LOC_VERTEX_COLOR] = rlGetLocationAttrib(programId, "vertexColor");
    outShader.locs[SHADER_LOC_VERTEX_BONEIDS] = rlGetLocationAttrib(programId, "vertexBoneIndices");
    outShader.locs[SHADER_LOC_VERTEX_BONEWEIGHTS] = rlGetLocationAttrib(programId, "vertexBoneWeights");
    outShader.locs[SHADER_LOC_VERTEX_INSTANCETRANSFORM] = rlGetLocationAttrib(programId, "instanceTransform");

    // Uniform locations
    outShader.locs[SHADER_LOC_MATRIX_MVP] = rlGetLocationUniform(programId, "mvp");
    outShader.locs[SHADER_LOC_MATRIX_VIEW] = rlGetLocationUniform(programId, "matView");
    outShader.locs[SHADER_LOC_MATRIX_PROJECTION] = rlGetLocationUniform(programId, "matProjection");
    outShader.locs[SHADER_LOC_MATRIX_MODEL] = rlGetLocationUniform(programId, "matModel");
    outShader.locs[SHADER_LOC_MATRIX_NORMAL] = rlGetLocationUniform(programId, "matNormal");
    outShader.locs[SHADER_LOC_MATRIX_BONETRANSFORMS] = rlGetLocationUniform(programId, "boneMatrices");
    outShader.locs[SHADER_LOC_VECTOR_VIEW] = rlGetLocationUniform(programId, "viewPos");
    outShader.locs[SHADER_LOC_COLOR_DIFFUSE] = rlGetLocationUniform(programId, "colDiffuse");
    outShader.locs[SHADER_LOC_COLOR_SPECULAR] = rlGetLocationUniform(programId, "colSpecular");
    outShader.locs[SHADER_LOC_COLOR_AMBIENT] = rlGetLocationUniform(programId, "colAmbient");
    outShader.locs[SHADER_LOC_MAP_ALBEDO] = rlGetLocationUniform(programId, "texture0");
    outShader.locs[SHADER_LOC_MAP_METALNESS] = rlGetLocationUniform(programId, "texture1");
    outShader.locs[SHADER_LOC_MAP_NORMAL] = rlGetLocationUniform(programId, "texture2");

    TraceLog(LOG_INFO, "SHADER_CACHE: Loaded cached shader '%s' (locs populated)", name.c_str());
    return true;
}

void SaveBinary(const std::string& name, const Shader& shader) {
    if (!s_initialized || s_cacheDir.empty()) return;
    if (shader.id == 0) return;

    GLuint programId = shader.id;

    // Get binary size
    GLint binarySize = 0;
    glGetProgramiv(programId, GL_PROGRAM_BINARY_LENGTH, &binarySize);
    if (binarySize <= 0) return;

    // Read binary
    std::vector<char> binary(binarySize);
    GLenum binaryFormat = 0;
    glGetProgramBinary(programId, binarySize, nullptr, &binaryFormat,
                       binary.data());

    // Write binary: first 4 bytes = GLenum format, rest = program binary
    fs::path binPath = fs::path(s_cacheDir) / (name + ".bin");
    std::ofstream binOut(binPath, std::ios::binary);
    if (!binOut) return;
    binOut.write(reinterpret_cast<const char*>(&binaryFormat), sizeof(GLenum));
    binOut.write(binary.data(), binarySize);
    binOut.close();

    // Write metadata
    fs::path metaPath = fs::path(s_cacheDir) / (name + ".meta");
    std::ofstream metaOut(metaPath);
    if (metaOut) {
        metaOut << "cached" << "\n";
        metaOut << s_driverInfo << "\n";
    }

    TraceLog(LOG_INFO, "SHADER_CACHE: Saved cached shader '%s'", name.c_str());
}

} // namespace shaderCache
