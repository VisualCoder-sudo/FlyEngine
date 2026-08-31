#pragma once

#include "ScatteredObject.hpp"
#include <iosfwd>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

class Engine;
namespace terrain { class Terrain; }
namespace phys { class Simulation; }

bool SaveSceneToFile(const std::vector<ScatteredObject*>& objects,
                     const std::vector<std::unique_ptr<ModelGroup>>& models,
                     const std::string& path,
                     terrain::Terrain* terrain = nullptr);
bool LoadSceneFromFile(Engine& engine, std::vector<ScatteredObject*>& objects,
                       std::vector<std::unique_ptr<ModelGroup>>& models,
                       const std::string& path,
                       phys::Simulation* physicsSim = nullptr,
                       terrain::Terrain** outTerrain = nullptr);
std::string ChooseSceneSavePath();
std::string ChooseSceneOpenPath();

// Stream variants so a scene can be embedded inside another file (a .flyproj
// project). The stream is positioned exactly at the "SIMPLE_ENGINE_BUILD"
// signature on save; on load it must be positioned just before it.
// `baseDir` (the .flyproj's folder, empty otherwise) lets imported-model paths
// be stored relative to the project so whole folders stay portable.
bool SaveSceneToStream(std::ostream& out, const std::vector<ScatteredObject*>& objects,
                       const std::vector<std::unique_ptr<ModelGroup>>& models,
                       const std::string& baseDir = "",
                       terrain::Terrain* terrain = nullptr);
bool LoadSceneFromStream(std::istream& in, Engine& engine, std::vector<ScatteredObject*>& objects,
                         std::vector<std::unique_ptr<ModelGroup>>& models,
                         const std::string& baseDir = "",
                         phys::Simulation* physicsSim = nullptr,
                         terrain::Terrain** outTerrain = nullptr);

// Undo/redo snapshots. These capture ONLY the scene objects + model groups into
// an in-memory byte string (no standalone scripts, terrain, or water), so an
// undo step can rebuild the object list without side effects on those systems.
// `baseDir` resolves project-relative model/texture paths against, and is empty
// when the stored paths are already absolute or project-relative to the current
// project.
bool SnapshotSceneToMemory(const std::vector<ScatteredObject*>& objects,
                           const std::vector<std::unique_ptr<ModelGroup>>& models,
                           const std::string& baseDir,
                           std::string& out);
// Rebuilds `objects` + `models` from a snapshot. Caller is responsible for first
// removing existing objects/models from the engine (see ObjectInteractionManager).
bool RestoreSceneFromMemory(std::istringstream& in, Engine& engine,
                            std::vector<ScatteredObject*>& objects,
                            std::vector<std::unique_ptr<ModelGroup>>& models,
                            const std::string& baseDir);
