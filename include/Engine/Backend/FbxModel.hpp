#pragma once

#include "raylib.h"

#include <string>

// Load an Autodesk FBX (.fbx) file into a raylib Model using the ufbx library.
// On success `out` is filled with the mesh/material data and uploaded to the
// GPU. Returns true on success; on failure `out` is left zeroed.
bool LoadFBXIntoModel(const std::string& path, Model& out);

// True if the given path points to an .fbx file (case-insensitive).
bool IsFBXPath(const std::string& path);