#pragma once

#include "raylib.h"

#include <string>
#include <vector>

// Load an Autodesk FBX (.fbx) file into a raylib Model using the ufbx library.
// On success `out` is filled with the mesh/material data and uploaded to the
// GPU. Returns true on success; on failure `out` is left zeroed.
// `diffuseFile`, if given, receives the on-disk path of the base-colour texture
// (empty if none is found next to the file); no texture is loaded here.
bool LoadFBXIntoModel(const std::string& path, Model& out, std::string* diffuseFile = nullptr);

// True if the given path points to an .fbx file (case-insensitive).
bool IsFBXPath(const std::string& path);

// Two-stage loader so the slow part can run on a worker thread.
// LoadFBXCpu parses the file and builds indexed mesh arrays; it touches no
// GPU/raylib state and is safe off the main thread. UploadFBXCpu (main thread
// only) uploads the meshes, builds the materials and takes ownership of them.
struct FbxCpuData {
    std::vector<Mesh> meshes;
    std::vector<int> meshMaterial;
    std::vector<Color> materialColors;   // one per material (>= 1)
    std::string diffuseFile;             // base-colour texture found beside the file
};
bool LoadFBXCpu(const std::string& path, FbxCpuData& out);
bool UploadFBXCpu(FbxCpuData& data, Model& out);
void FreeFbxCpu(FbxCpuData& data);   // for data that was never uploaded
