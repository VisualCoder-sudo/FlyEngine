#include "../../../include/Engine/Backend/ScenePersistence.hpp"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Backend/ModelImport.hpp"
#include "../../../include/Engine/PhysicsCollision.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../../../include/Terrain/Terrain.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/CityGen/City.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Graphics.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>

namespace {

const std::vector<platform::FileFilter>& SceneFilters() {
    static const std::vector<platform::FileFilter> filters = {
        {"Simple Engine Builds", "*.simplebuild"},
        {"All Files", "*"},
    };
    return filters;
}

} // namespace

void BeginChooseSceneSavePath(const std::string& startDir, bool keepTerrain) {
    // The purpose tag is what tells the result handler whether the terrain
    // entity should ride along: Ctrl+S / menu "Save" keep it, "Save As" has
    // always dropped it.
    const platform::DialogPurpose purpose = keepTerrain ? platform::DialogPurpose::SaveScene
                                                        : platform::DialogPurpose::SaveSceneAs;
    platform::BeginSaveFileDialog(purpose, "Save Scene", startDir, SceneFilters(),
                                  "Untitled.simplebuild");
}

void BeginChooseSceneOpenPath(const std::string& startDir) {
    platform::BeginOpenFileDialog(platform::DialogPurpose::OpenScene, "Open Scene",
                                  startDir, SceneFilters());
}

bool SaveSceneToStream(std::ostream& file, const std::vector<ScatteredObject*>& objects,
                       const std::vector<std::unique_ptr<ModelGroup>>& models,
                       const std::string& baseDir,
                       terrain::Terrain* terrain) {
    file << "SIMPLE_ENGINE_BUILD 19\n" << objects.size() << "\n" << std::setprecision(9);
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

    // Standalone scripts (the explorer "Scripts" group). Each entry carries a
    // name and the C# IScript type name compiled into FlyScript.dll.
    if (ScriptRuntime* rt = GetActiveRuntime()) {
        const auto& scripts = rt->StandaloneScripts();
        file << scripts.size() << "\n";
        for (const auto& s : scripts) {
            file << (int)s.runOnPlay << ' ' << s.name.size() << '\n';
            if (!s.name.empty()) file.write(s.name.data(), (std::streamsize)s.name.size());
            file << '\n' << s.typeName.size() << '\n';
            if (!s.typeName.empty()) file.write(s.typeName.data(), (std::streamsize)s.typeName.size());
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

    // v17: cities (all cities stored in one city.city sidecar next to the scene
    // or project; the scene records the count so the loader restores them from
    // that file).
    {
        auto& reg = city::GetCityRegistry();
        file << reg.Count() << "\n";
        if (!baseDir.empty()) {
            std::string cityFile = baseDir + "/city.city";
            if (reg.Count() > 0) {
                if (reg.WriteFile(cityFile)) {
                    ui::LogAlways("[city] saved %d city(ies) to %s", (int)reg.Count(), cityFile.c_str());
                } else {
                    ui::LogAlways("[city] failed to write %s", cityFile.c_str());
                }
            } else {
                // No cities left: drop a stale sidecar so deleted cities can't linger.
                std::error_code ec;
                std::filesystem::remove(cityFile, ec);
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

    // v18: BasicTerrains (all of them stored in one basicterrain.bt sidecar
    // next to the scene/project; the .flyproj records the count so the loader
    // restores them from that file). BasicTerrain is an Entity, not a
    // ScatteredObject, and it never registers in terrain::GetTerrainRegistry(),
    // so without this section it appeared in none of the counts above and a
    // scene made of one vanished on save-close-reopen.
    {
        const auto& bts = BasicTerrain::GetInstances();
        file << bts.size() << "\n";
        if (!bts.empty() && !baseDir.empty()) {
            std::string btFile = baseDir + "/basicterrain.bt";
            if (BasicTerrain::WriteCollection(btFile)) {
                ui::LogAlways("[terrain] saved %zu BasicTerrain(s) to %s", bts.size(), btFile.c_str());
            } else {
                ui::LogAlways("[terrain] failed to write %s", btFile.c_str());
            }
        }
    }

    // v19: scene-wide lighting (the Lighting section of the Explorer).
    file << "LIGHTING " << gfx::Lighting().timeOfDay << ' ' << gfx::Lighting().dayLengthMinutes << "\n";
    {   // The rest of the lighting items. A separate line, so a reader that only knows the line above ignores it.
        const gfx::LightingSettings& L = gfx::Lighting();
        file << "LIGHTING2 " << (L.hasSun ? 1 : 0) << ' ' << (L.hasAmbient ? 1 : 0) << ' ' << (L.hasSky ? 1 : 0) << ' ' << (L.hasFog ? 1 : 0) << ' '
             << L.sunAzimuth << ' ' << L.sunElevation << ' ' << L.sunIntensity << ' ' << L.sunColor[0] << ' ' << L.sunColor[1] << ' ' << L.sunColor[2] << ' '
             << L.ambient << ' ' << L.skyColor[0] << ' ' << L.skyColor[1] << ' ' << L.skyColor[2] << ' ' << L.fogDensity << "\n";
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
    if (!(file >> signature >> version >> count) || signature != "SIMPLE_ENGINE_BUILD" || version < 1 || version > 19) return false;
    gfx::ResetLighting();   // a scene that does not store lighting starts at noon

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
    // Only load scripts if there's a valid, active ScriptRuntime with correct magic
    ScriptRuntime* runtime = GetActiveRuntime();
    printf("[ScenePersistence] GetActiveRuntime() = %p, version = %d\n", (void*)runtime, version);
    if (runtime) {
        printf("[ScenePersistence] runtime->IsValid() = %d\n", runtime->IsValid());
    }
    // The section is always consumed, even with no usable runtime (e.g. the player's
    // --noscripts): skipping it left the script count in the stream and shifted every
    // later section, so the whole scene (terrain, city, ...) failed to load.
    if (version >= 2) {
        const bool keepScripts = runtime && runtime->IsValid();
        size_t scriptCount = 0;
        if (file >> scriptCount) {
            std::vector<ScriptRuntime::StandaloneScript> discarded;
            auto& scripts = keepScripts ? runtime->StandaloneScripts() : discarded;
            scripts.clear();
            scripts.reserve(scriptCount);
            for (size_t i = 0; i < scriptCount; ++i) {
                int runOnPlay = 1;
                size_t nameLen = 0, typeLen = 0;
                if (!(file >> runOnPlay >> nameLen)) return false;
                file.ignore(); // newline after the length
                std::string name;
                if (nameLen > 0) {
                    name.resize(nameLen);
                    file.read(&name[0], (std::streamsize)nameLen);
                    if (!file) return false;
                }
                if (!(file >> typeLen)) return false;
                file.ignore(); // newline after the length
                std::string typeName;
                if (typeLen > 0) {
                    typeName.resize(typeLen);
                    file.read(&typeName[0], (std::streamsize)typeLen);
                    if (!file) return false;
                }
                ScriptRuntime::StandaloneScript s;
                s.name = std::move(name);
                s.typeName = std::move(typeName);
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

    // v17: cities (one city.city sidecar next to the scene/project).
    if (version >= 17) {
        int cityCount = 0;
        if (!(file >> cityCount)) return false;
        if (cityCount > 0 && !baseDir.empty()) {
            std::string cityFile = baseDir + "/city.city";
            int loaded = city::GetCityRegistry().ReadFile(cityFile, [&]() -> city::City* {
                auto c = std::make_unique<city::City>();
                city::City* raw = c.get();
                engine.AddEntity(std::move(c));
                return raw;
            });
            if (loaded == 0) {
                ui::LogAlways("Failed to load city data from: %s", cityFile.c_str());
            } else {
                ui::LogAlways("[city] loaded %d city(ies) from %s", loaded, cityFile.c_str());
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

    // v18: BasicTerrains stored in one basicterrain.bt sidecar. The count lets
    // the loader ignore the section entirely when the file predates v18.
    if (version >= 18) {
        int basicTerrainCount = 0;
        if (!(file >> basicTerrainCount)) return false;
        if (basicTerrainCount > 0 && !baseDir.empty()) {
            std::string btFile = baseDir + "/basicterrain.bt";
            int loaded = BasicTerrain::ReadCollection(btFile, [&]() -> BasicTerrain* {
                auto bt = std::make_unique<BasicTerrain>();
                BasicTerrain* raw = bt.get();
                engine.AddEntity(std::move(bt));
                return raw;
            });
            if (loaded == 0) {
                ui::LogAlways("Failed to load BasicTerrain data from: %s", btFile.c_str());
            } else {
                ui::LogAlways("[terrain] loaded %d BasicTerrain(s) from %s", loaded, btFile.c_str());
            }
        }
    }

    // v19: scene-wide lighting.
    if (version >= 19) {
        std::string tag;
        float tod = 12.0f, len = 0.0f;
        if (file >> tag >> tod >> len && tag == "LIGHTING") {
            gfx::Lighting().timeOfDay = std::clamp(tod, 0.0f, 24.0f);
            gfx::Lighting().dayLengthMinutes = std::max(len, 0.0f);
            const auto here = file.tellg();
            std::string tag2; int hs = 0, ha = 0, hk = 0, hf = 0;
            gfx::LightingSettings L2 = gfx::Lighting();
            if (file >> tag2 && tag2 == "LIGHTING2" &&
                file >> hs >> ha >> hk >> hf >> L2.sunAzimuth >> L2.sunElevation >> L2.sunIntensity >> L2.sunColor[0] >> L2.sunColor[1] >> L2.sunColor[2] >>
                       L2.ambient >> L2.skyColor[0] >> L2.skyColor[1] >> L2.skyColor[2] >> L2.fogDensity) {
                L2.hasSun = hs != 0; L2.hasAmbient = ha != 0; L2.hasSky = hk != 0; L2.hasFog = hf != 0;
                L2.sunIntensity = std::clamp(L2.sunIntensity, 0.0f, 3.0f);
                L2.ambient = std::clamp(L2.ambient, 0.0f, 2.0f);
                L2.fogDensity = std::clamp(L2.fogDensity, 0.0f, 0.2f);
                gfx::Lighting() = L2;
            } else {
                file.clear();
                file.seekg(here);
            }
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