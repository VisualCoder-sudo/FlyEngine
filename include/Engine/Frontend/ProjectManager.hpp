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

// Saves current scene to a temporary file for playtesting (doesn't overwrite main .flyproj).
// Returns the path to the temp file, or empty string on failure.
std::string SaveProjectFileTemp(const std::string& projectFolder, const std::vector<ScatteredObject*>& objects,
                                const std::vector<std::unique_ptr<ModelGroup>>& models,
                                terrain::Terrain* terrain = nullptr);

// Reads a project header (Name/Template) without touching an engine.
bool ReadProjectHeader(const std::string& projectFolder, Info& outInfo);

// Reads a project header from a specific .flyproj file path.
bool ReadProjectHeaderFromPath(const std::string& projectFilePath, Info& outInfo);

// Parses a project and loads its embedded scene into the engine, filling
// `objects` with the newly created entities and rebuilding model groups.
bool OpenProjectFile(const std::string& projectFolder, Engine& engine,
                     std::vector<ScatteredObject*>& objects,
                     std::vector<std::unique_ptr<ModelGroup>>& models, Info& outInfo,
                     phys::Simulation* physicsSim = nullptr,
                     terrain::Terrain** outTerrain = nullptr);

// Opens a specific .flyproj file (not the default <folder>/<folder>.flyproj)
// Useful for loading temp files like <project>_temp.flyproj
bool OpenProjectFileFromPath(const std::string& projectFilePath, Engine& engine,
                             std::vector<ScatteredObject*>& objects,
                             std::vector<std::unique_ptr<ModelGroup>>& models, Info& outInfo,
                             phys::Simulation* physicsSim = nullptr,
                             terrain::Terrain** outTerrain = nullptr);

// The project the editor is currently working on (empty until one is opened).
void SetCurrentProject(const Info& info);
const Info& GetCurrentProject();

// Creates a new project with the given name and template.
// Returns true on success. The project is created in the current working directory.
bool CreateProject(const std::string& name, const std::string& templateName = "Empty");

// ---- locations (cross-platform) ---------------------------------------------
// Per-user application data folder where the recent-projects list is stored.
std::string GetAppDataDirectory();
std::string GetRecentFilePath();
// Folder that holds every project created by the project manager.
std::string GetProjectsDirectory();
std::string GetProjectFolder(const std::string& name);
std::string GetProjectFilePath(const std::string& name);
std::string GetAssetsFolder(const std::string& name);

// Rejects names that are empty or contain characters Windows forbids in paths.
bool IsValidProjectName(const std::string& name);

// ---- recent projects list ---------------------------------------------------
std::vector<std::string> LoadRecentPaths();
void SaveRecentPaths(const std::vector<std::string>& paths);
void PushRecent(const std::string& path, std::vector<std::string>& recent);

} // namespace project
