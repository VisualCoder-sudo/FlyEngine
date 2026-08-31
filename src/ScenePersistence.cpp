// Win32 and raylib have several overlapping API names. Rename the Win32
// declarations while importing the common-dialog API, then remove its A/W
// aliases before raylib is included through the engine headers.
#define WIN32_LEAN_AND_MEAN
#define CloseWindow Win32CloseWindow
#define ShowCursor Win32ShowCursor
#define Rectangle Win32Rectangle
#include <windows.h>
#include <commdlg.h>
#undef CloseWindow
#undef ShowCursor
#undef Rectangle
#undef LoadImage
#undef DrawText
#undef DrawTextEx
#undef PlaySound

#include "ScenePersistence.hpp"
#include "Engine.hpp"
#include "Flyscript.hpp"
#include "ModelImport.hpp"
#include "PhysicsCollision.hpp"
#include "Terrain.hpp"
#include "TerrainRegistry.hpp"
#include "WaterBody.hpp"
#include "ui.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>

namespace {

std::string ChoosePath(bool saving) {
    char path[MAX_PATH] = "Untitled.simplebuild";
    OPENFILENAMEA dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrFilter = "Simple Engine Builds (*.simplebuild)\0*.simplebuild\0All Files\0*.*\0";
    dialog.lpstrDefExt = "simplebuild";
    dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (saving ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return (saving ? GetSaveFileNameA(&dialog) : GetOpenFileNameA(&dialog)) ? std::string(path) : std::string();
}

} // namespace

std::string ChooseSceneSavePath() { return ChoosePath(true); }
std::string ChooseSceneOpenPath() { return ChoosePath(false); }

bool SaveSceneToStream(std::ostream& file, const std::vector<ScatteredObject*>& objects,
                       const std::vector<std::unique_ptr<ModelGroup>>& models,
                       const std::string& baseDir,
                       terrain::Terrain* terrain) {
    file << "SIMPLE_ENGINE_BUILD 16\n" << objects.size() << "\n" << std::setprecision(9);
    for (auto* object : objects) {
        if (!object) continue;
        const Vector3& pos = *object->GetPosPtr();
        const Vector3& size = *object->GetSizePtr();
        const Vector3& rotation = *object->GetRotationPtr();
        const Vector3& origin = *object->GetOriginPtr();
        const Color& color = *object->GetColorPtr();
        const Vector3& velocity = object->GetVelocity();
        const Vector3& angularVelocity = object->GetAngularVelocity();
        file << static_cast<int>(object->GetShapeType()) << ' ' << std::quoted(object->GetName()) << ' '
             << pos.x << ' ' << pos.y << ' ' << pos.z << ' ' << size.x << ' ' << size.y << ' ' << size.z << ' '
             << rotation.x << ' ' << rotation.y << ' ' << rotation.z << ' '
             << origin.x << ' ' << origin.y << ' ' << origin.z << ' '
             << static_cast<int>(color.r) << ' ' << static_cast<int>(color.g) << ' ' << static_cast<int>(color.b) << ' ' << static_cast<int>(color.a) << ' '
             << (int)object->anchored << ' '
             << velocity.x << ' ' << velocity.y << ' ' << velocity.z << ' '
             << angularVelocity.x << ' ' << angularVelocity.y << ' ' << angularVelocity.z << ' '
             << object->GetStoredMass() << ' '
             << (int)object->runOnPlay << ' ' << object->script.size() << '\n';
        if (!object->script.empty()) {
            file.write(object->script.data(), (std::streamsize)object->script.size());
        }
        file << '\n';

        // v8: imported mesh, stored relative to the project folder (or
        // absolute when the scene was never saved inside one).
        file << std::quoted(PathRelativeTo(object->GetModelPath(), baseDir)) << '\n';

        // v9: collision settings.
        // v10: transparency (0 = visible, 1 = invisible).
        // v11: texture path (project-relative).
        file << (int)object->canCollide << ' ' << static_cast<int>(object->GetCollisionAccuracy()) << ' ' << object->GetTransparency() << ' ' << std::quoted(object->GetTexturePath()) << '\n';
    }

    // Standalone scripts (the explorer "Flyscripts" group).
    if (flyscript::IsRuntimeReady()) {
        const auto& scripts = flyscript::GetRuntime().scripts;
        file << scripts.size() << "\n";
        for (const auto& s : scripts) {
            file << (int)s.runOnPlay << ' ' << s.name.size() << '\n';
            if (!s.name.empty()) file.write(s.name.data(), (std::streamsize)s.name.size());
            file << '\n' << s.source.size() << '\n';
            if (!s.source.empty()) file.write(s.source.data(), (std::streamsize)s.source.size());
            file << '\n';
        }
    } else {
        file << "0\n";
    }

    // Model groups (v7). Member indices refer to positions in the object list
    // written above, so loading reconstructs the pointer links.
    file << models.size() << "\n";
    for (const auto& model : models) {
        if (!model) {
            file << std::quoted("Model") << " 0\n\n";
            continue;
        }
        file << std::quoted(model->name) << ' ' << model->members.size() << '\n';
        for (auto* member : model->members) {
            auto it = std::find(objects.begin(), objects.end(), member);
            size_t index = (it != objects.end()) ? static_cast<size_t>(it - objects.begin()) : 0;
            file << index << ' ';
        }
        file << '\n';
    }

    // v12: ocean surfaces - removed
    file << "0\n";

    // v16: terrain (all terrains stored in one terrain.terrain file next to the
    // scene/project; the .flyproj only records how many there are so the loader
    // knows to restore them from that file).
    {
        auto& reg = terrain::GetTerrainRegistry();
        file << reg.Count() << "\n";
        if (reg.Count() > 0 && !baseDir.empty()) {
            std::string terrainFile = baseDir + "/terrain.terrain";
            if (!reg.WriteFile(terrainFile)) {
                ui::LogAlways("[terrain] failed to write %s", terrainFile.c_str());
            }
        }
    }

    // v15: water bodies (live registry = every WaterBody currently in the scene)
    {
        const std::vector<WaterBody*>& waters = WaterBody::GetInstances();
        file << waters.size() << "\n";
        for (WaterBody* water : waters) {
            if (!water) continue;
            const Vector3& pos = *water->GetPosPtr();
            const Vector3& wsize = *water->GetSizePtr();
            const Color& col = water->GetBaseColor();
            const auto& noise = water->GetNoiseParams();
            const auto& foam = water->GetFoamParams();
            const auto& grid = water->GetGridParams();
            file << std::quoted(water->GetName()) << ' '
                 << pos.x << ' ' << pos.y << ' ' << pos.z << ' '
                 << wsize.x << ' ' << wsize.y << ' ' << wsize.z << ' '
                 << water->GetWaterHeight() << ' '
                 << (int)col.r << ' ' << (int)col.g << ' ' << (int)col.b << ' ' << (int)col.a << ' '
                 << water->GetTransparency() << '\n';
            file << noise.amplitude << ' ' << noise.frequency << ' ' << noise.speed << ' '
                 << noise.direction.x << ' ' << noise.direction.y << ' '
                 << noise.octaves << ' ' << noise.persistence << ' ' << noise.lacunarity << ' ' << noise.seed << '\n';
            file << foam.intensity << ' ' << foam.scale << ' ' << foam.threshold << ' '
                 << (int)foam.color.r << ' ' << (int)foam.color.g << ' ' << (int)foam.color.b << '\n';
            file << grid.baseResolution << ' ' << grid.maxResolution << ' ' << grid.densityThreshold << ' '
                 << (grid.adaptive ? 1 : 0) << '\n';
        }
    }

    return file.good();
}

bool SaveSceneToFile(const std::vector<ScatteredObject*>& objects,
                     const std::vector<std::unique_ptr<ModelGroup>>& models,
                     const std::string& path,
                     terrain::Terrain* terrain) {
    std::ofstream file(path, std::ios::trunc);
    if (!file) return false;
    std::error_code ec;
    std::string baseDir = std::filesystem::path(path).parent_path().string();
    if (ec || baseDir.empty()) baseDir.clear();
    return SaveSceneToStream(file, objects, models, baseDir, terrain);
}

bool LoadSceneFromStream(std::istream& file, Engine& engine, std::vector<ScatteredObject*>& objects,
                         std::vector<std::unique_ptr<ModelGroup>>& models,
                         const std::string& baseDir,
                         phys::Simulation* physicsSim,
                         terrain::Terrain** outTerrain) {
    std::string signature;
    int version = 0;
    size_t count = 0;
    if (!(file >> signature >> version >> count) || signature != "SIMPLE_ENGINE_BUILD" || version < 1 || version > 16) return false;

    models.clear(); // loading a scene rebuilds model containers from scratch

    std::vector<std::unique_ptr<ScatteredObject>> loaded;
    loaded.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        int shapeValue, red, green, blue, alpha;
        std::string name;
        Vector3 pos, size, rotation;
        if (!(file >> shapeValue >> std::quoted(name) >> pos.x >> pos.y >> pos.z >> size.x >> size.y >> size.z
              >> rotation.x >> rotation.y >> rotation.z)) return false;
        if (shapeValue < static_cast<int>(ShapeType::Cube) || shapeValue > static_cast<int>(ShapeType::Wedge)) return false;

        // v5 added the Origin (pivot) offset; earlier files pivot from the center.
        Vector3 origin = { 0.0f, 0.0f, 0.0f };
        if (version >= 5) {
            if (!(file >> origin.x >> origin.y >> origin.z)) return false;
        }

        if (!(file >> red >> green >> blue >> alpha)) return false;
        auto object = std::make_unique<ScatteredObject>(pos, size, Color{(unsigned char)red, (unsigned char)green, (unsigned char)blue, (unsigned char)alpha}, static_cast<ShapeType>(shapeValue));
        object->SetName(name);
        *object->GetRotationPtr() = rotation;
        *object->GetOriginPtr() = origin;

        // v1/v2 files predate the Anchored flag, so objects load unanchored
        // (the physics behavior those scenes were saved with).
        if (version >= 3) {
            int anchored = 0;
            if (!(file >> anchored)) return false;
            object->anchored = (anchored != 0);
        } else {
            object->anchored = false;
        }

        if (version >= 4) {
            Vector3 velocity, angularVelocity;
            if (!(file >> velocity.x >> velocity.y >> velocity.z
                  >> angularVelocity.x >> angularVelocity.y >> angularVelocity.z)) return false;
            object->SetVelocity(velocity);
            object->SetAngularVelocity(angularVelocity);
        }

        // v6 added the Mass override (0 = auto, derived from size).
        if (version >= 6) {
            float mass = 0.0f;
            if (!(file >> mass)) return false;
            if (mass > 0.0f) object->SetMass(mass);
        }

        if (version >= 2) {
            int runOnPlay = 1;
            size_t scriptLen = 0;
            if (!(file >> runOnPlay >> scriptLen)) return false;
            object->runOnPlay = (runOnPlay != 0);
            file.ignore(); // newline after the length
            std::string script;
            if (scriptLen > 0) {
                script.resize(scriptLen);
                file.read(&script[0], (std::streamsize)scriptLen);
                if (!file) return false;
            }
            object->script = std::move(script);
        }

        // v8 added the imported-mesh path (a quoted token after the script
        // block). Old scenes simply have no such token and load as primitives.
        if (version >= 8) {
            std::string storedModel;
            if (!(file >> std::quoted(storedModel))) return false;
            if (!storedModel.empty()) {
                std::string resolved = ResolveStoredAssetPath(storedModel, baseDir);
                if (!object->SetModel(resolved)) {
                    ui::LogAlways("Could not load mesh for '%s': %s", object->GetName().c_str(), resolved.c_str());
                } else {
                    // Mesh vertices are raw at this point (real-world scale).
                    // Draw() scales by the saved `size`, so the mesh must be
                    // normalized into the unit box first, same as at import
                    // time, or it gets scaled twice and comes out stretched.
                    object->NormalizeModelToUnitBox();
                }
            }
        }

        // v9 added the collision settings (canCollide + accuracy). Older files
        // default to full collisions with the Default accuracy.
        if (version >= 9) {
            int canCollide = 1, accuracy = static_cast<int>(pcoll::CollisionAccuracy::Default);
            if (!(file >> canCollide >> accuracy)) return false;
            if (canCollide < 0 || canCollide > 1) return false;
            if (accuracy < static_cast<int>(pcoll::CollisionAccuracy::Box) ||
                accuracy > static_cast<int>(pcoll::CollisionAccuracy::Precise)) return false;
            object->canCollide = (canCollide != 0);
            object->SetCollisionAccuracy(static_cast<pcoll::CollisionAccuracy>(accuracy));
        }

        // v10 added transparency (0 = visible, 1 = invisible). Default 0 for older scenes.
        if (version >= 10) {
            float transparency = 0.0f;
            if (!(file >> transparency)) return false;
            if (transparency < 0.0f) transparency = 0.0f;
            if (transparency > 1.0f) transparency = 1.0f;
            object->SetTransparency(transparency);
        }

        // v11 added texture path (project-relative). Default empty for older scenes.
        if (version >= 11) {
            std::string texturePath;
            if (!(file >> std::quoted(texturePath))) return false;
            if (!texturePath.empty()) {
                object->SetTexturePath(texturePath, baseDir);
            }
        }

        loaded.push_back(std::move(object));
    }

    // Standalone scripts (v2 only).
    if (version >= 2 && flyscript::IsRuntimeReady()) {
        size_t scriptCount = 0;
        if (file >> scriptCount) {
            auto& scripts = flyscript::GetRuntime().scripts;
            scripts.clear();
            scripts.reserve(scriptCount);
            for (size_t i = 0; i < scriptCount; ++i) {
                int runOnPlay = 1;
                size_t nameLen = 0, sourceLen = 0;
                if (!(file >> runOnPlay >> nameLen)) return false;
                file.ignore(); // newline after the length
                std::string name;
                if (nameLen > 0) {
                    name.resize(nameLen);
                    file.read(&name[0], (std::streamsize)nameLen);
                    if (!file) return false;
                }
                if (!(file >> sourceLen)) return false;
                file.ignore(); // newline after the length
                std::string source;
                if (sourceLen > 0) {
                    source.resize(sourceLen);
                    file.read(&source[0], (std::streamsize)sourceLen);
                    if (!file) return false;
                }
                flyscript::Script s;
                s.name = std::move(name);
                s.source = std::move(source);
                s.runOnPlay = (runOnPlay != 0);
                scripts.push_back(std::move(s));
            }
        }
    }

    // Model groups (v7). Member indices are relative to the object block just
    // loaded (0..count-1); older files simply have no models section.
    if (version >= 7) {
        size_t modelCount = 0;
        if (!(file >> modelCount)) return false;
        models.reserve(modelCount);
        for (size_t i = 0; i < modelCount; ++i) {
            std::string modelName;
            size_t memberCount = 0;
            if (!(file >> std::quoted(modelName) >> memberCount)) return false;
            auto model = std::make_unique<ModelGroup>();
            model->name = modelName;
            model->members.reserve(memberCount);
            for (size_t m = 0; m < memberCount; ++m) {
                size_t index = 0;
                if (!(file >> index)) return false;
                if (index < loaded.size()) {
                    ScatteredObject* member = loaded[index].get();
                    member->parentModel = model.get();
                    model->members.push_back(member);
                }
            }
            models.push_back(std::move(model));
        }
    }

    // v12: ocean surfaces - removed, but still consume the data for backward compatibility
    if (version >= 12) {
        size_t oceanCount = 0;
        if (!(file >> oceanCount)) return false;
        for (size_t i = 0; i < oceanCount; ++i) {
            std::string oceanName;
            Vector3 pos, size, rot;
            float windSpeed, waveH, foam;
            float wDirX, wDirY;
            int res;
            if (!(file >> std::quoted(oceanName) >> pos.x >> pos.y >> pos.z
                  >> size.x >> size.y >> size.z
                  >> rot.x >> rot.y >> rot.z
                  >> windSpeed >> wDirX >> wDirY >> waveH >> foam >> res)) return false;

            // v13: colors, rendering, resolution properties
            if (version >= 13) {
                float sr, sg, sb, dr, dg, db, fr, fg, fb;
                float fresnelPow, fogDens, specPow;
                int refrEn, autoRes, autoWaves;
                float refrStr, waveSpacing;
                int baseRes;
                if (!(file >> sr >> sg >> sb >> dr >> dg >> db >> fr >> fg >> fb
                      >> fresnelPow >> fogDens >> specPow
                      >> refrEn >> refrStr >> autoRes >> baseRes >> autoWaves >> waveSpacing)) return false;
            }

            size_t waveCount = 0;
            if (!(file >> waveCount)) return false;
            for (size_t w = 0; w < waveCount; ++w) {
                float amp, steep, wl, dx, dy;
                if (!(file >> amp >> steep >> wl >> dx >> dy)) return false;
            }
        }
    }

    // v14: terrain. v16+: all terrains live in one terrain.terrain file next to
    // the scene/project and are restored through the TerrainRegistry.
    if (version >= 14) {
        if (version >= 16) {
            int terrainCount = 0;
            if (!(file >> terrainCount)) return false;
            if (terrainCount > 0 && !baseDir.empty()) {
                std::string terrainFile = baseDir + "/terrain.terrain";
                int loaded = terrain::GetTerrainRegistry().ReadFile(terrainFile, [&]() -> terrain::Terrain* {
                    Vector3 center = {0, 0, 0};
                    auto t = std::make_unique<terrain::Terrain>(center, 1000.0f, 1000.0f, 256, 65);
                    terrain::Terrain* raw = t.get();
                    if (physicsSim) t->SetPhysicsSimulation(physicsSim);
                    engine.AddEntity(std::move(t));
                    return raw;
                });
                if (loaded == 0) {
                    ui::LogAlways("Failed to load terrain data from: %s", terrainFile.c_str());
                } else {
                    // Re-resolve + load the layer material textures (project-relative).
                    for (auto* t : terrain::GetTerrainRegistry().GetTerrains()) {
                        if (t) t->ReloadMaterialTextures(baseDir);
                    }
                }
            }
        } else if (outTerrain) {
            // Legacy v14/v15: single terrain with a <name>.terrain sidecar.
            int hasTerrain = 0;
            if (!(file >> hasTerrain)) hasTerrain = 0;
            if (hasTerrain) {
                std::string terrainName;
                float width, depth;
                int chunkSize, chunkRes;
                float minH, maxH;
                int physicsMode;
                int layerCount;

                std::getline(file, terrainName); // consume newline
                std::getline(file, terrainName);
                if (!(file >> width >> depth)) return false;
                if (!(file >> chunkSize >> chunkRes)) return false;
                if (!(file >> minH >> maxH)) return false;
                if (!(file >> physicsMode)) return false;
                if (!(file >> layerCount)) return false;

                for (int i = 0; i < layerCount; i++) {
                    std::string layerName;
                    float tileSize, blendRange;
                    std::getline(file, layerName); // consume newline
                    std::getline(file, layerName);
                    file >> tileSize >> blendRange;
                }

                Vector3 center = {0, 0, 0};
                auto terrain = std::make_unique<terrain::Terrain>(center, width, depth, chunkSize, chunkRes);
                terrain->SetName(terrainName);
                terrain->SetPhysicsMode(static_cast<terrain::PhysicsMode>(physicsMode));

                std::string terrainPath = baseDir + "/" + terrainName + ".terrain";
                if (!terrain->LoadFromFile(terrainPath)) {
                    ui::LogAlways("Failed to load terrain data from: %s", terrainPath.c_str());
                }

                if (physicsSim) terrain->SetPhysicsSimulation(physicsSim);

                *outTerrain = terrain.get();
                terrain::GetTerrainRegistry().Register(terrain.get());
                engine.AddEntity(std::move(terrain));
            }
        }
    }

    // v15: water bodies
    if (version >= 15) {
        size_t waterCount = 0;
        if (!(file >> waterCount)) return false;
        for (size_t i = 0; i < waterCount; ++i) {
            std::string wname;
            Vector3 pos{}, wsize{};
            float height = 0.0f;
            int r = 0, g = 0, b = 0, a = 0;
            float transparency = 0.3f;
            float amp, freq, speed, dirX, dirY, pers, lac;
            int octaves, seed;
            float foamI, foamS, foamT;
            int foamR, foamG, foamB;
            int baseRes, maxRes, adaptive;
            float densityThreshold;

            if (!(file >> std::quoted(wname)
                  >> pos.x >> pos.y >> pos.z >> wsize.x >> wsize.y >> wsize.z
                  >> height >> r >> g >> b >> a >> transparency)) return false;
            if (!(file >> amp >> freq >> speed >> dirX >> dirY >> octaves >> pers >> lac >> seed)) return false;
            if (!(file >> foamI >> foamS >> foamT >> foamR >> foamG >> foamB)) return false;
            if (!(file >> baseRes >> maxRes >> densityThreshold >> adaptive)) return false;

            auto water = std::make_unique<WaterBody>(pos, wsize, height,
                Color{ (unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a });
            water->SetName(wname);
            water->SetTransparency(transparency);

            WaterBody::NoiseParams np;
            np.amplitude = amp; np.frequency = freq; np.speed = speed;
            np.direction = { dirX, dirY };
            np.octaves = octaves; np.persistence = pers; np.lacunarity = lac; np.seed = seed;
            water->SetNoiseParams(np);

            WaterBody::FoamParams fp;
            fp.intensity = foamI; fp.scale = foamS; fp.threshold = foamT;
            fp.color = Color{ (unsigned char)foamR, (unsigned char)foamG, (unsigned char)foamB, 255 };
            water->SetFoamParams(fp);

            WaterBody::GridParams gp;
            gp.baseResolution = baseRes; gp.maxResolution = maxRes;
            gp.densityThreshold = densityThreshold; gp.adaptive = (adaptive != 0);
            water->SetGridParams(gp);

            engine.AddEntity(std::move(water));
        }
    }

    for (auto& object : loaded) {
        objects.push_back(object.get());
        engine.AddEntity(std::move(object));
    }
    return true;
}

bool LoadSceneFromFile(Engine& engine, std::vector<ScatteredObject*>& objects,
                       std::vector<std::unique_ptr<ModelGroup>>& models,
                       const std::string& path,
                       phys::Simulation* physicsSim,
                       terrain::Terrain** outTerrain) {
    std::ifstream file(path);
    if (!file) return false;
    std::error_code ec;
    std::string baseDir = std::filesystem::path(path).parent_path().string();
    if (ec || baseDir.empty()) baseDir.clear();
    return LoadSceneFromStream(file, engine, objects, models, baseDir, physicsSim, outTerrain);
}

bool SnapshotSceneToMemory(const std::vector<ScatteredObject*>& objects,
                           const std::vector<std::unique_ptr<ModelGroup>>& models,
                           const std::string& baseDir,
                           std::string& out) {
    std::ostringstream file;
    file << "SIMPLE_ENGINE_BUILD 15\n" << objects.size() << "\n" << std::setprecision(9);
    for (auto* object : objects) {
        if (!object) continue;
        const Vector3& pos = *object->GetPosPtr();
        const Vector3& size = *object->GetSizePtr();
        const Vector3& rotation = *object->GetRotationPtr();
        const Vector3& origin = *object->GetOriginPtr();
        const Color& color = *object->GetColorPtr();
        const Vector3& velocity = object->GetVelocity();
        const Vector3& angularVelocity = object->GetAngularVelocity();
        file << static_cast<int>(object->GetShapeType()) << ' ' << std::quoted(object->GetName()) << ' '
             << pos.x << ' ' << pos.y << ' ' << pos.z << ' ' << size.x << ' ' << size.y << ' ' << size.z << ' '
             << rotation.x << ' ' << rotation.y << ' ' << rotation.z << ' '
             << origin.x << ' ' << origin.y << ' ' << origin.z << ' '
             << static_cast<int>(color.r) << ' ' << static_cast<int>(color.g) << ' ' << static_cast<int>(color.b) << ' ' << static_cast<int>(color.a) << ' '
             << (int)object->anchored << ' '
             << velocity.x << ' ' << velocity.y << ' ' << velocity.z << ' '
             << angularVelocity.x << ' ' << angularVelocity.y << ' ' << angularVelocity.z << ' '
             << object->GetStoredMass() << ' '
             << (int)object->runOnPlay << ' ' << object->script.size() << '\n';
        if (!object->script.empty()) {
            file.write(object->script.data(), (std::streamsize)object->script.size());
        }
        file << '\n';
        file << std::quoted(PathRelativeTo(object->GetModelPath(), baseDir)) << '\n';
        file << (int)object->canCollide << ' ' << static_cast<int>(object->GetCollisionAccuracy()) << ' ' << object->GetTransparency() << ' ' << std::quoted(object->GetTexturePath()) << '\n';
    }

    // Model groups, same member-index convention as the scene file.
    file << models.size() << "\n";
    for (const auto& model : models) {
        if (!model) {
            file << std::quoted("Model") << " 0\n\n";
            continue;
        }
        file << std::quoted(model->name) << ' ' << model->members.size() << '\n';
        for (auto* member : model->members) {
            auto it = std::find(objects.begin(), objects.end(), member);
            size_t index = (it != objects.end()) ? static_cast<size_t>(it - objects.begin()) : 0;
            file << index << ' ';
        }
        file << '\n';
    }

    if (!file) return false;
    out = file.str();
    return true;
}

bool RestoreSceneFromMemory(std::istringstream& file, Engine& engine,
                            std::vector<ScatteredObject*>& objects,
                            std::vector<std::unique_ptr<ModelGroup>>& models,
                            const std::string& baseDir) {
    std::string signature;
    int version = 0;
    size_t count = 0;
    if (!(file >> signature >> version >> count) || signature != "SIMPLE_ENGINE_BUILD" || version < 1 || version > 15) return false;

    models.clear();

    std::vector<std::unique_ptr<ScatteredObject>> loaded;
    loaded.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        int shapeValue, red, green, blue, alpha;
        std::string name;
        Vector3 pos, size, rotation;
        if (!(file >> shapeValue >> std::quoted(name) >> pos.x >> pos.y >> pos.z >> size.x >> size.y >> size.z
              >> rotation.x >> rotation.y >> rotation.z)) return false;
        if (shapeValue < static_cast<int>(ShapeType::Cube) || shapeValue > static_cast<int>(ShapeType::Wedge)) return false;

        Vector3 origin = { 0.0f, 0.0f, 0.0f };
        if (version >= 5) {
            if (!(file >> origin.x >> origin.y >> origin.z)) return false;
        }

        if (!(file >> red >> green >> blue >> alpha)) return false;
        auto object = std::make_unique<ScatteredObject>(pos, size, Color{(unsigned char)red, (unsigned char)green, (unsigned char)blue, (unsigned char)alpha}, static_cast<ShapeType>(shapeValue));
        object->SetName(name);
        *object->GetRotationPtr() = rotation;
        *object->GetOriginPtr() = origin;

        if (version >= 3) {
            int anchored = 0;
            if (!(file >> anchored)) return false;
            object->anchored = (anchored != 0);
        } else {
            object->anchored = false;
        }

        if (version >= 4) {
            Vector3 velocity, angularVelocity;
            if (!(file >> velocity.x >> velocity.y >> velocity.z
                  >> angularVelocity.x >> angularVelocity.y >> angularVelocity.z)) return false;
            object->SetVelocity(velocity);
            object->SetAngularVelocity(angularVelocity);
        }

        if (version >= 6) {
            float mass = 0.0f;
            if (!(file >> mass)) return false;
            if (mass > 0.0f) object->SetMass(mass);
        }

        if (version >= 2) {
            int runOnPlay = 1;
            size_t scriptLen = 0;
            if (!(file >> runOnPlay >> scriptLen)) return false;
            object->runOnPlay = (runOnPlay != 0);
            file.ignore();
            std::string script;
            if (scriptLen > 0) {
                script.resize(scriptLen);
                file.read(&script[0], (std::streamsize)scriptLen);
                if (!file) return false;
            }
            object->script = std::move(script);
        }

        if (version >= 8) {
            std::string storedModel;
            if (!(file >> std::quoted(storedModel))) return false;
            if (!storedModel.empty()) {
                std::string resolved = ResolveStoredAssetPath(storedModel, baseDir);
                if (!object->SetModel(resolved)) {
                    ui::LogAlways("Could not load mesh for '%s': %s", object->GetName().c_str(), resolved.c_str());
                } else {
                    object->NormalizeModelToUnitBox();
                }
            }
        }

        if (version >= 9) {
            int canCollide = 1, accuracy = static_cast<int>(pcoll::CollisionAccuracy::Default);
            if (!(file >> canCollide >> accuracy)) return false;
            if (canCollide < 0 || canCollide > 1) return false;
            if (accuracy < static_cast<int>(pcoll::CollisionAccuracy::Box) ||
                accuracy > static_cast<int>(pcoll::CollisionAccuracy::Precise)) return false;
            object->canCollide = (canCollide != 0);
            object->SetCollisionAccuracy(static_cast<pcoll::CollisionAccuracy>(accuracy));
        }

        if (version >= 10) {
            float transparency = 0.0f;
            if (!(file >> transparency)) return false;
            if (transparency < 0.0f) transparency = 0.0f;
            if (transparency > 1.0f) transparency = 1.0f;
            object->SetTransparency(transparency);
        }

        if (version >= 11) {
            std::string texturePath;
            if (!(file >> std::quoted(texturePath))) return false;
            if (!texturePath.empty()) {
                object->SetTexturePath(texturePath, baseDir);
            }
        }

        loaded.push_back(std::move(object));
    }

    if (version >= 7) {
        size_t modelCount = 0;
        if (!(file >> modelCount)) return false;
        models.reserve(modelCount);
        for (size_t i = 0; i < modelCount; ++i) {
            std::string modelName;
            size_t memberCount = 0;
            if (!(file >> std::quoted(modelName) >> memberCount)) return false;
            auto model = std::make_unique<ModelGroup>();
            model->name = modelName;
            model->members.reserve(memberCount);
            for (size_t m = 0; m < memberCount; ++m) {
                size_t index = 0;
                if (!(file >> index)) return false;
                if (index < loaded.size()) {
                    ScatteredObject* member = loaded[index].get();
                    member->parentModel = model.get();
                    model->members.push_back(member);
                }
            }
            models.push_back(std::move(model));
        }
    }

    for (auto& object : loaded) {
        objects.push_back(object.get());
        engine.AddEntity(std::move(object));
    }
    return true;
}