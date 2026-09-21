#pragma once

#include "../Backend/ScatteredObject.hpp"
#include <memory>
#include <string>
#include <vector>

class Engine;
namespace phys { class Simulation; }
namespace terrain { class Terrain; }

namespace project {

// A Flyengine project: a folder containing project metadata and scene.
// The folder name is the project name, and inside is a metadata file and
// the embedded scene data.
struct Info {
    std::string name;
    std::string path;        // full path to the project folder
    std::string templateName; // "Blank" or "Sample"
};

// Applies the Flyengine logo as the active window's icon/taskbar icon.
// Safe to call from the splash, project manager, or editor window.
void ApplyWindowIcon();

// Runs the project-manager window (its own raylib window). Returns the
// project the user chose to open, or an empty Info if they quit.
Info ShowProjectManager();

// Writes the given scene back into an existing project (preserving its
// Name/Template/Created header fields). Returns false on I/O failure.
bool SaveProjectFile(const std::string& projectFolder, const std::vector<ScatteredObject*>& objects,
                     const std::vector<std::unique_ptr<ModelGroup>>& models,
                     terrain::Terrain* terrain = nullptr);

// Reads a project header (Name/Template) without touching an engine.
bool ReadProjectHeader(const std::string& projectFolder, Info& outInfo);

// Parses a project and loads its embedded scene into the engine, filling
// `objects` with the newly created entities and rebuilding model groups.
bool OpenProjectFile(const std::string& projectFolder, Engine& engine,
                     std::vector<ScatteredObject*>& objects,
                     std::vector<std::unique_ptr<ModelGroup>>& models, Info& outInfo,
                     phys::Simulation* physicsSim = nullptr,
                     terrain::Terrain** outTerrain = nullptr);

// The project the editor is currently working on (empty until one is opened).
void SetCurrentProject(const Info& info);
const Info& GetCurrentProject();

} // namespace project
