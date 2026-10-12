#pragma once
#include "raylib.h"
#include <string>

namespace shaderCache {

void Init(const std::string& cacheDir);
void Shutdown();

bool LoadBinary(const std::string& name, Shader& outShader);
void SaveBinary(const std::string& name, const Shader& shader);
void InvalidateAll();

} // namespace shaderCache
