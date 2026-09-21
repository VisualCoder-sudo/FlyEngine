#pragma once

#include <string>

// Result of copying a model file into the active project. `storedPath` is the
// path to persist in the scene file: relative to the project directory (with
// forward slashes) when the model was copied in, absolute otherwise.
struct ImportResult {
    bool ok = false;
    std::string storedPath;
    std::string targetDir;  // absolute path to the directory created for this import
    std::string error;   // fatal: the model was not imported
    std::string warning; // non-fatal: imported, but some dependencies were missing
};

// Opens a 3D model file picker (obj/gltf/glb/iqm/vox/m3d). Empty on cancel.
std::string ChooseModelOpenPath();

// Opens an image file picker (png/jpg/bmp/tga/webp). Empty on cancel.
std::string ChooseTexturePath();

// Copies `sourcePath` plus its textures (OBJ .mtl + map_* textures, GLTF
// external buffers/images) into <projectFolder>/assets/3D/<name>[_N]/
// so the project folder stays self-contained. `projectName` is the project
// folder name. On success result.storedPath is relative to projectDir and
// the copied files have had their references rewritten to
// basenames (raylib resolves those relative to the copied model's directory).
// `targetParentDir`, if non-empty, is used as the parent folder for the
// per-model subfolder instead of the default <projectFolder>/assets/3D
// (e.g. so a caller can import into whatever folder the user currently has
// open in an asset browser).
ImportResult ImportModel(const std::string& sourcePath,
                         const std::string& projectDir,
                         const std::string& projectName,
                         const std::string& targetParentDir = std::string());

// Turns a stored scene path back into a loadable path: relative paths are
// joined onto projectDir, absolute ones pass through unchanged.
std::string ResolveStoredAssetPath(const std::string& stored, const std::string& projectDir);

// True when `p` is an absolute Windows path (drive letter or UNC share).
bool IsAbsolutePath(const std::string& p);

// Converts `p` to a forward-slash relative path under `base`. If `p` does not
// live inside `base` (or either path is empty), `p` is returned unchanged.
std::string PathRelativeTo(const std::string& p, const std::string& base);