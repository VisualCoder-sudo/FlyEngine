#pragma once
#include <string>
#include <vector>

#include "raylib.h"

class ScatteredObject;

namespace textureManager {

void Init(const std::string& projectDir);
void Shutdown();

// Register a texture for use by a model. Returns the project-relative path
// where the texture was stored (model folder or shared). Handles deduplication
// by content hash (SHA256).
std::string RegisterTexture(const std::string& sourcePath, const std::string& modelPath);

// Unregister a texture (object no longer uses it). Decrements refcount,
// deletes file if count reaches 0, handles shared->model migration.
void UnregisterTexture(const std::string& texturePath);

// Called on project load to rebuild registry from all objects' texturePath.
// Verifies files exist, deduplicates by hash, cleans up orphans.
void VerifyAndRebuild(const std::vector<ScatteredObject*>& objects);

// Delete the model's folder (assets/3D/[ModelFolder]/) if no object still
// references a model or texture file inside it. Safe no-op when the folder is
// still in use or when modelPath is empty.
void RemoveModelDirectory(const std::string& modelPath, const std::vector<ScatteredObject*>& objects);

// Imported model folders are temporary until the project is saved.
// MarkPendingImport stamps + journals a freshly imported folder (under
// assets/3D). CommitAssets (called by every save) moves textures used by 2+
// models into assets/shared, deletes managed folders/textures no object uses,
// and makes whatever is left permanent. DiscardPendingImports deletes imports
// that were never committed (editor close, opening another scene, next launch).
void MarkPendingImport(const std::string& absDir);
void CommitAssets(const std::vector<ScatteredObject*>& objects);
void DiscardPendingImports();

// Utility: get the model-specific texture directory (assets/3D/[ModelName]/)
std::string GetModelTextureDir(const std::string& modelPath);

// Utility: get the shared texture directory (assets/shared/)
std::string GetSharedTextureDir();

// Utility: compute SHA256 hash of a file
std::string ComputeSHA256(const std::string& filePath);

// Returns vector of project-relative paths: "assets/PresetTextures/wood.jpg", etc.
// Scans projectDir/assets/PresetTextures/ for supported image formats.
// Logs "Could not load preset textures" if folder missing/empty; returns empty vector.
std::vector<std::string> GetPresetTextures(const std::string& projectDir);

// Copies a preset texture to the project's assets/PresetTextures/ folder.
// Returns the project-relative path (e.g., "assets/PresetTextures/wood.png").
// If the texture is already in the project's PresetTextures folder, returns the existing path.
std::string EnsurePresetTextureInProject(const std::string& presetPath, const std::string& projectDir);

// Get GPU texture handle for a registered texture path.
// Returns invalid texture (id=0) if not found.
Texture2D GetGPUTexture(const std::string& relPath);

// Non-blocking variant: decodes on a worker thread. Returns id=0 with
// `pending` = true until the texture is ready (call again next frame);
// id=0 with `pending` = false means it failed to load.
Texture2D GetGPUTextureAsync(const std::string& relPath, bool& pending);

// LOD (Level of Detail) texture system.
// Checks if a texture path is a preset texture (assets/PresetTextures/*).
bool IsPresetTexture(const std::string& relPath);

// Returns the LOD texture path for a given preset path and level.
// level 0 = full-res (original), 1 = med (_med), 2 = low (_low).
// Returns empty string if the LOD file doesn't exist on disk.
// For level 0, returns the original presetPath.
std::string GetLODPath(const std::string& presetPath, int level);

// Returns LOD level based on camera-to-object distance.
// 0 = full-res (<15), 1 = medium (<40), 2 = low (>=40).
int GetLODLevel(float distance);

} // namespace textureManager