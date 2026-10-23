#include "../../../include/Engine/Backend/VisualScript.hpp"
// ScriptRuntime.cpp - world context + lifecycle backing the native script host.

#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../../../include/Engine/Scripts/FlyScriptApi.hpp"
#include "../../../include/Engine.hpp"
#include "../../../include/Graphics/Graphics.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/PhysicsCollision.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"

#include "../../../include/Engine/Backend/ScenePersistence.hpp"
#include "../../../include/Graphics/Vfs.hpp"
#include "../../../include/Engine/GameUI.hpp"
#include "../../../include/Engine/Navigation.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/CityGen/City.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "../../../include/Engine/Backend/ObjectNode.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include <algorithm>
#include <filesystem>
#include <cstring>
#include <cmath>
#include <cctype>
#include <sstream>

namespace {
PlayStateHook g_playStateHook = nullptr;
PlayEdgeHook g_prePlayHook, g_postPlayHook;
ScriptRuntime* g_activeRuntime = nullptr;

bool ParseBool(const std::string& s, bool& out) {
    if (s == "true" || s == "1") { out = true; return true; }
    if (s == "false" || s == "0") { out = false; return true; }
    return false;
}

bool ParseVec3(const std::string& s, Vector3& out) {
    std::string t = s;
    t.erase(std::remove(t.begin(), t.end(), '('), t.end());
    t.erase(std::remove(t.begin(), t.end(), ')'), t.end());
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream iss(t);
    float v;
    std::vector<float> nums;
    while (iss >> v) nums.push_back(v);
    if (nums.size() != 3) return false;
    out = { nums[0], nums[1], nums[2] };
    return true;
}

bool ParseColor(const std::string& s, Color& out) {
    std::string t = s;
    t.erase(std::remove(t.begin(), t.end(), '('), t.end());
    t.erase(std::remove(t.begin(), t.end(), ')'), t.end());
    std::replace(t.begin(), t.end(), ',', ' ');
    std::istringstream iss(t);
    float v;
    std::vector<float> nums;
    while (iss >> v) nums.push_back(v);
    if (nums.size() != 3) return false;
    // Clamp first: casting an out-of-range float (300, -5) to unsigned char is undefined behaviour.
    auto byte = [](float f) { return static_cast<unsigned char>(std::clamp(f, 0.0f, 255.0f)); };
    out = { byte(nums[0]), byte(nums[1]), byte(nums[2]), 255 };
    return true;
}
} // namespace

void SetPlayStateHook(PlayStateHook hook) { g_playStateHook = hook; }
void SetPrePlayHook(PlayEdgeHook hook) { g_prePlayHook = std::move(hook); }
void SetPostPlayHook(PlayEdgeHook hook) { g_postPlayHook = std::move(hook); }
PlayStateHook GetPlayStateHook() { return g_playStateHook; }

void SetActiveRuntime(ScriptRuntime* rt) { g_activeRuntime = rt; }
ScriptRuntime* GetActiveRuntime() { return g_activeRuntime; }

ScriptRuntime::ScriptRuntime() {
    runtimeMagic = RUNTIME_MAGIC;
}
ScriptRuntime::~ScriptRuntime() {
    runtimeMagic = 0;
}

bool ScriptRuntime::IsValid() const {
    return runtimeMagic == RUNTIME_MAGIC;
}

void ScriptRuntime::Update(float dt)
{
    // The owning NativeScriptHost ticks the scripts and drives play-state via
    // SetSimPlaying(); nothing to step here.
    (void)dt;
}

void ScriptRuntime::BindWorld(std::vector<ScatteredObject*>& objects,
                              std::vector<std::unique_ptr<ModelGroup>>& models,
                              Camera3D& camera, phys::Simulation& sim, Engine& engine)
{
    objectsPtr = &objects;
    modelsPtr = &models;
    cameraPtr = &camera;
    this->sim = &sim;
    this->engine = &engine;
}

ScatteredObject* ScriptRuntime::FindByName(const std::string& name) const
{
    if (!objectsPtr) return nullptr;
    for (auto* obj : *objectsPtr)
        if (obj && obj->GetName() == name) return obj;
    return nullptr;
}

ScatteredObject* ScriptRuntime::FindByNameNth(const std::string& name, int occurrence) const
{
    if (!objectsPtr) return nullptr;
    for (auto* obj : *objectsPtr)
        if (obj && obj->GetName() == name && --occurrence <= 0) return obj;
    return nullptr;
}

ScatteredObject* ScriptRuntime::CreateObject(const std::string& shapeName, bool deferSpawn)
{
    if (!objectsPtr || !engine) return nullptr;

    ShapeType type = ShapeType::Cube;
    if (shapeName == "Sphere")        type = ShapeType::Sphere;
    else if (shapeName == "Cylinder") type = ShapeType::Cylinder;
    else if (shapeName == "Wedge")    type = ShapeType::Wedge;
    else if (shapeName != "Cube")     return nullptr;

    auto newObj = std::make_unique<ScatteredObject>(
        Vector3{0, 0, 0}, Vector3{1.5f, 1.5f, 1.5f}, SKYBLUE, type);
    newObj->SetName(shapeName);
    newObj->anchored = false;
    ScatteredObject* raw = newObj.get();
    objectsPtr->push_back(raw);
    engine->AddEntity(std::move(newObj));
    if (isPlaying) {
        playCreatedObjects.push_back(raw);
        if (sim && !deferSpawn) sim->SpawnBodyForObject(raw);
    }
    return raw;
}

// ---- water and terrain ----

namespace {
bool ParseNumber(const std::string& s, float& out) {
    char* end = nullptr;
    const float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    out = v;
    return true;
}

// "(a, b[, c[, d]])" -> up to four numbers.
bool ParseNumberList(const std::string& s, std::vector<float>& out) {
    std::string t = s;
    while (!t.empty() && (t.front() == ' ' || t.front() == '(')) t.erase(t.begin());
    while (!t.empty() && (t.back() == ' ' || t.back() == ')')) t.pop_back();
    out.clear();
    std::stringstream ss(t);
    std::string part;
    while (std::getline(ss, part, ',')) {
        float v;
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!ParseNumber(part, v)) return false;
        out.push_back(v);
    }
    return !out.empty() && out.size() <= 4;
}

std::string LowerKey(std::string k) {
    for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return k;
}
}  // namespace

bool ScriptRuntime::EnvironmentNameTaken(const std::string& name) const {
    for (WaterBody* w : WaterBody::GetInstances()) if (w && w->alive && w->GetName() == name) return true;
    return terrain::GetTerrainRegistry().NameExists(name) || city::GetCityRegistry().NameExists(name);
}

std::string ScriptRuntime::CreateWater(const std::string& name, const PropList& props) {
    if (isPlaying) return "Stop the game first: water, terrain and cities are made in edit mode";
    if (!engine) return "No scene is loaded";
    Vector3 pos = { 0, 0, 0 };
    Vector3 size = { 100.0f, 1.0f, 100.0f };
    float height = 0.0f, transparency = -1.0f;
    int playerCollision = -1;
    Color color = { 0, 100, 200, 180 };
    std::vector<float> n;
    for (const auto& [rawKey, value] : props) {
        const std::string key = LowerKey(rawKey);
        if (key == "position" || key == "size") {
            if (!ParseNumberList(value, n) || n.size() != 3) return rawKey + ": expected (x, y, z)";
            (key == "position" ? pos : size) = Vector3{ n[0], n[1], n[2] };
        } else if (key == "height") {
            if (!ParseNumber(value, height)) return "Height: expected a number";
        } else if (key == "transparency") {
            if (!ParseNumber(value, transparency) || transparency < 0.0f || transparency > 1.0f) return "Transparency: expected 0..1";
        } else if (key == "playercollision") {
            bool b;
            if (!ParseBool(value, b)) return "PlayerCollision: expected true or false";
            playerCollision = b ? 1 : 0;
        } else if (key == "color") {
            if (!ParseNumberList(value, n) || (n.size() != 3 && n.size() != 4)) return "Color: expected (r, g, b) or (r, g, b, a)";
            for (float c : n) if (c < 0.0f || c > 255.0f) return "Color: channels are 0..255";
            color = { (unsigned char)n[0], (unsigned char)n[1], (unsigned char)n[2], (unsigned char)(n.size() == 4 ? n[3] : 180.0f) };
        } else {
            return "Unknown water property '" + rawKey + "' (Position, Size, Height, Color, Transparency, PlayerCollision)";
        }
    }
    size.x = std::clamp(size.x, 1.0f, 20000.0f);
    size.z = std::clamp(size.z, 1.0f, 20000.0f);
    auto water = std::make_unique<WaterBody>(pos, size, height, color);
    water->SetName(name);
    if (transparency >= 0.0f) water->SetTransparency(transparency);
    if (playerCollision >= 0) water->SetPlayerCollision(playerCollision != 0);
    engine->AddEntity(std::move(water));
    return {};
}

std::string ScriptRuntime::CreateTerrain(const std::string& name, const PropList& props) {
    if (isPlaying) return "Stop the game first: water, terrain and cities are made in edit mode";
    if (!engine) return "No scene is loaded";
    Vector3 pos = { 0, 0, 0 };
    float width = 1000.0f, depth = 1000.0f, height = 0.0f, hills = 0.0f;
    int seed = 1337;
    std::vector<float> n;
    for (const auto& [rawKey, value] : props) {
        const std::string key = LowerKey(rawKey);
        if (key == "position") {
            if (!ParseNumberList(value, n) || n.size() != 3) return "Position: expected (x, y, z)";
            pos = { n[0], n[1], n[2] };
        } else if (key == "size") {
            if (!ParseNumberList(value, n) || n.size() != 2) return "Size: expected (width, depth)";
            width = n[0]; depth = n[1];
            if (width < 50.0f || depth < 50.0f || width > 4000.0f || depth > 4000.0f) return "Size: width and depth are 50..4000 metres";
        } else if (key == "height") {
            if (!ParseNumber(value, height)) return "Height: expected a number";
        } else if (key == "hills") {
            if (!ParseNumber(value, hills) || hills < 0.0f || hills > 500.0f) return "Hills: expected 0..500 (metres)";
        } else if (key == "seed") {
            float f;
            if (!ParseNumber(value, f) || std::fabs(f) > 2.0e9f) return "Seed: expected a whole number below 2 billion";
            seed = static_cast<int>(f);
        } else {
            return "Unknown terrain property '" + rawKey + "' (Position, Size, Height, Hills, Seed)";
        }
    }
    auto t = std::make_unique<terrain::Terrain>(pos, width, depth, 256, 65);
    t->CreateBlank(height);
    if (hills > 0.0f) {
        terrain::NoiseParams np;
        np.amplitude = hills;
        np.scale = 0.01f * 1000.0f / std::max(width, depth) * 2.0f;   // bigger terrain, longer hills
        np.seed = seed;
        t->GenerateFromNoise(np);
    }
    t->SetPhysicsMode(terrain::PhysicsMode::Heightfield);
    if (sim) t->SetPhysicsSimulation(sim);
    auto& registry = terrain::GetTerrainRegistry();
    t->SetName(name);
    registry.Register(t.get());
    engine->AddEntity(std::move(t));
    const project::Info& proj = project::GetCurrentProject();
    if (!proj.path.empty()) registry.WriteFile(proj.path + "/terrain.terrain");
    return {};
}

std::string ScriptRuntime::DeleteEnvironment(const std::string& name) {
    if (isPlaying) return {};
    for (WaterBody* w : WaterBody::GetInstances()) {
        if (w && w->alive && w->GetName() == name) {
            if (ui::GetSelectedWater() == w) ui::SetSelectedWater(nullptr);
            if (engine) engine->RemoveEntity(w);
            return "water";
        }
    }
    auto& registry = terrain::GetTerrainRegistry();
    if (terrain::Terrain* t = registry.GetByName(name)) {
        if (terrain::GetTerrainEditorState().selectedTerrainLegacy == t) terrain::HandleTerrainSelection(t, false);
        registry.Unregister(t);
        t->alive = false;
        const project::Info& proj = project::GetCurrentProject();
        if (!proj.path.empty()) registry.WriteFile(proj.path + "/terrain.terrain");
        return "terrain";
    }
    if (city::City* c = city::GetCityRegistry().GetByName(name)) {
        city::SetActiveCity(nullptr);
        city::GetCityRegistry().Unregister(c);
        c->alive = false;
        return "city";
    }
    return {};
}

bool ScriptRuntime::RenameEnvironment(const std::string& from, const std::string& to) {
    if (city::City* c = city::GetCityRegistry().GetByName(from)) { c->SetName(to); return true; }
    for (WaterBody* w : WaterBody::GetInstances())
        if (w && w->alive && w->GetName() == from) { w->SetName(to); return true; }
    if (terrain::Terrain* t = terrain::GetTerrainRegistry().GetByName(from)) {
        t->SetName(to);
        const project::Info& proj = project::GetCurrentProject();
        if (!proj.path.empty()) terrain::GetTerrainRegistry().WriteFile(proj.path + "/terrain.terrain");
        return true;
    }
    return false;
}

std::string ScriptRuntime::CreateCity(const std::string& name, const PropList& props) {
    if (isPlaying) return "Stop the game first: water, terrain and cities are made in edit mode";
    if (!engine) return "No scene is loaded";
    Vector2 origin = { 0, 0 };
    city::CityParams params;
    std::vector<float> n;
    for (const auto& [rawKey, value] : props) {
        const std::string key = LowerKey(rawKey);
        float f;
        if (key == "position") {
            if (!ParseNumberList(value, n) || n.size() != 2) return "Position: expected (x, z)";
            origin = { n[0], n[1] };
        } else if (key == "grid") {
            if (!ParseNumberList(value, n) || n.size() != 2 || n[0] < 2 || n[1] < 2 || n[0] > 64 || n[1] > 64) return "Grid: expected (x, z) blocks, 2..64";
            params.gridX = (int)n[0]; params.gridZ = (int)n[1];
        } else if (key == "cellsize") {
            if (!ParseNumber(value, f) || f < 15.0f || f > 200.0f) return "CellSize: expected 15..200 metres";
            params.cellSize = f;
        } else if (key == "organic") {
            bool b;
            if (!ParseBool(value, b)) return "Organic: expected true or false";
            params.organic = b;
        } else if (key == "seed") {
            if (!ParseNumber(value, f) || std::fabs(f) > 2.0e9f) return "Seed: expected a whole number below 2 billion";
            params.seed = (int)f;
        } else {
            return "Unknown city property '" + rawKey + "' (Position, Grid, CellSize, Organic, Seed)";
        }
    }
    auto c = std::make_unique<city::City>();
    c->SetName(name);
    c->GetParams() = params;
    c->GenerateGrid(origin);
    city::City* raw = c.get();
    city::GetCityRegistry().Register(raw);
    engine->AddEntity(std::move(c));
    return {};
}

std::string ScriptRuntime::AddNode(ScatteredObject* obj, const std::string& typeName, const PropList& props) {
    if (!obj) return "No such object";
    const std::string t = LowerKey(typeName);
    ObjectNodeType type;
    if (t == "pointlight" || t == "point") type = ObjectNodeType::PointLight;
    else if (t == "spotlight" || t == "spot") type = ObjectNodeType::SpotLight;
    else if (t == "surfacelight" || t == "surface") type = ObjectNodeType::SurfaceLight;
    else if (t == "decal") type = ObjectNodeType::Decal;
    else if (t == "texture") type = ObjectNodeType::Texture;
    else return "Unknown node type '" + typeName + "' (PointLight, SpotLight, SurfaceLight, Decal or Texture)";
    ObjectNode node = MakeNode(type);
    std::vector<float> v;
    for (const auto& [rawKey, value] : props) {
        const std::string key = LowerKey(rawKey);
        float f;
        bool b;
        if (key == "name") {
            if (value.empty() || value.size() > 64) return "Name: 1..64 characters";
            node.name = value;
        } else if (key == "enabled" || key == "shadows") {
            if (!ParseBool(value, b)) return rawKey + ": expected true or false";
            (key == "enabled" ? node.enabled : node.castShadows) = b;
        } else if (key == "face") {
            const std::string fv = LowerKey(value);
            static const char* faces[] = { "front", "back", "left", "right", "top", "bottom" };
            int idx = -1;
            for (int i = 0; i < 6; ++i) if (fv == faces[i]) idx = i;
            if (idx < 0) return "Face: Front, Back, Left, Right, Top or Bottom";
            node.face = static_cast<NodeFace>(idx);
        } else if (key == "falloff") {
            const std::string fv = LowerKey(value);
            if (fv == "linear") node.falloff = LightFalloff::Linear;
            else if (fv == "inversesquare") node.falloff = LightFalloff::InverseSquare;
            else return "Falloff: Linear or InverseSquare";
        } else if (key == "color" || key == "tint") {
            if (!ParseNumberList(value, v) || v.size() != 3) return rawKey + ": expected (r, g, b) with channels 0..255";
            for (float c : v) if (c < 0.0f || c > 255.0f) return rawKey + ": channels are 0..255";
            float* dst = key == "color" ? node.color : node.tint;
            for (int i = 0; i < 3; ++i) dst[i] = v[i] / 255.0f;
        } else if (key == "brightness") {
            if (!ParseNumber(value, f) || f < 0.0f || f > 100.0f) return "Brightness: expected 0..100";
            node.brightness = f;
        } else if (key == "range") {
            if (!ParseNumber(value, f) || f <= 0.0f || f > 1000.0f) return "Range: expected 0..1000 metres";
            node.range = f;
        } else if (key == "angle") {
            if (!ParseNumber(value, f) || f <= 0.0f || f > 89.0f) return "Angle: expected 1..89 degrees";
            node.angle = f;
        } else if (key == "transparency") {
            if (!ParseNumber(value, f) || f < 0.0f || f > 1.0f) return "Transparency: expected 0..1";
            node.transparency = f;
        } else if (key == "texture") {
            if (value.find("..") != std::string::npos || (!value.empty() && (value[0] == '/' || value[0] == '\\' || value.find(':') != std::string::npos)))
                return "Texture: use a path inside the project (for example Textures/brick.png)";
            node.texturePath = value;
        } else if (key == "tile" || key == "offset") {
            if (!ParseNumberList(value, v) || v.size() != 2) return rawKey + ": expected (x, y)";
            if (key == "tile" && (v[0] <= 0.0f || v[1] <= 0.0f)) return "Tile: sizes must be above 0";
            float* dst = key == "tile" ? node.studsPerTile : node.offset;
            dst[0] = v[0]; dst[1] = v[1];
        } else {
            return "Unknown node property '" + rawKey + "' (Name, Enabled, Face, Color, Brightness, Range, Angle, Falloff, Shadows, Texture, Tint, Transparency, Tile, Offset)";
        }
    }
    obj->nodes.push_back(node);
    return {};
}

std::string ScriptRuntime::RemoveNode(ScatteredObject* obj, const std::string& nodeName) {
    if (!obj) return "No such object";
    for (size_t i = 0; i < obj->nodes.size(); ++i) {
        if (obj->nodes[i].name == nodeName) { obj->nodes.erase(obj->nodes.begin() + (long)i); return {}; }
    }
    return "'" + obj->GetName() + "' has no node named '" + nodeName + "'";
}

void ScriptRuntime::FinishCreate(ScatteredObject* obj)
{
    if (!obj || !isPlaying || !sim) return;
    sim->SpawnBodyForObject(obj);
    // Anything the Setup chain set before the body existed (velocity) is handed to it now.
    sim->SetBodyVelocity(obj, obj->GetVelocity());
}

void ScriptRuntime::RebuildBody(ScatteredObject* obj)
{
    if (!obj || !isPlaying || !sim) return;
    sim->RebuildBodyForObject(obj);
}

void ScriptRuntime::DestroyObject(ScatteredObject* obj)
{
    if (!obj || !objectsPtr) return;
    auto fit = std::find(objectsPtr->begin(), objectsPtr->end(), obj);
    if (fit == objectsPtr->end()) return;   // already gone
    objectsPtr->erase(fit);
    playCreatedObjects.erase(std::remove(playCreatedObjects.begin(), playCreatedObjects.end(), obj), playCreatedObjects.end());
    if (ModelGroup* g = obj->parentModel) {
        g->members.erase(std::remove(g->members.begin(), g->members.end(), obj), g->members.end());
        obj->parentModel = nullptr;
    }
    if (sim) sim->RemoveObject(obj);
    if (engine) engine->RemoveEntity(obj);
}

ScatteredObject* ScriptRuntime::SpawnPrefab(const std::string& file, Vector3 at, std::vector<ScatteredObject*>* outObjects)
{
    if (!objectsPtr || !modelsPtr || !engine || file.empty()) return nullptr;
    std::string path = file;
    if (!std::filesystem::path(path).is_absolute() && !vfs::Handles(path) && !m_projectPath.empty())
        path = m_projectPath + "/" + path;

    std::vector<ScatteredObject*> made;
    ModelGroup* root = nullptr;
    const size_t groupsBefore = modelsPtr->size();
    if (!SpawnPrefabFromFile(*engine, *objectsPtr, *modelsPtr, path, m_projectPath, at, &made, &root)) return nullptr;
    if (made.empty()) return nullptr;

    if (isPlaying) {
        for (size_t i = groupsBefore; i < modelsPtr->size(); ++i) playCreatedGroups.push_back((*modelsPtr)[i].get());
        for (ScatteredObject* o : made) {
            playCreatedObjects.push_back(o);
            if (sim) {
                sim->SpawnBodyForObject(o);
                sim->SetBodyVelocity(o, o->GetVelocity());
            }
            if (hostPtr && o->runOnPlay && !o->script.empty()) hostPtr->StartObjectScriptOn(o);
        }
    }
    if (outObjects) *outObjects = made;
    return made.front();
}

ScatteredObject* ScriptRuntime::CloneObject(ScatteredObject* source, bool deferSpawn)
{
    if (!source || !objectsPtr || !engine) return nullptr;
    std::unique_ptr<ScatteredObject> copy = source->Clone();
    if (!copy) return nullptr;
    ScatteredObject* raw = copy.get();
    objectsPtr->push_back(raw);
    engine->AddEntity(std::move(copy));
    if (isPlaying) {
        playCreatedObjects.push_back(raw);
        if (sim && !deferSpawn) sim->SpawnBodyForObject(raw);
    }
    return raw;
}

ScatteredObject* ScriptRuntime::UnionObjects(const std::vector<ScatteredObject*>& sources, bool intersect)
{
    if (!objectsPtr || !engine || sources.size() < 2) return nullptr;
    for (size_t i = 0; i < sources.size(); ++i) {
        const ScatteredObject* s = sources[i];
        if (!s || s->IsUnion() || s->HasModel()) return nullptr;
        if (std::find(sources.begin(), sources.begin() + (long)i, s) != sources.begin() + (long)i) return nullptr;   // the same part twice
    }
    // The union is built from copies, so a result that comes out empty leaves the scene as it was.
    std::vector<std::unique_ptr<ScatteredObject>> parts;
    std::vector<UnionMode> modes;
    for (ScatteredObject* s : sources) {
        parts.push_back(s->Clone());
        modes.push_back(intersect ? UnionMode::Intersect : (s->negate ? UnionMode::Negate : UnionMode::Add));
    }
    std::unique_ptr<ScatteredObject> made = ScatteredObject::MakeUnion(std::move(parts), modes);
    if (!made) return nullptr;
    ScatteredObject* raw = made.get();
    objectsPtr->push_back(raw);
    engine->AddEntity(std::move(made));
    if (isPlaying) {
        playCreatedObjects.push_back(raw);
        if (sim) sim->SpawnBodyForObject(raw);
    }
    for (ScatteredObject* s : sources) DestroyObject(s);
    return raw;
}

bool ScriptRuntime::SeparateUnion(ScatteredObject* unionObject, std::vector<ScatteredObject*>& out)
{
    if (!unionObject || !unionObject->IsUnion() || !objectsPtr || !engine) return false;
    if (sim) sim->RemoveObject(unionObject);   // its body goes before its mesh does
    for (std::unique_ptr<ScatteredObject>& part : unionObject->TakeUnionParts()) {
        ScatteredObject* raw = part.get();
        objectsPtr->push_back(raw);
        engine->AddEntity(std::move(part));
        if (isPlaying) {
            playCreatedObjects.push_back(raw);
            if (sim) sim->SpawnBodyForObject(raw);
        }
        out.push_back(raw);
    }
    DestroyObject(unionObject);
    return true;
}

bool ScriptRuntime::AddForce(ScatteredObject* obj, const std::string& vec, bool impulse, std::string& outError)
{
    Vector3 v;
    if (!obj) { outError = "No target object"; return false; }
    if (!ParseVec3(vec, v)) { outError = "'" + vec + "' is not a valid (x, y, z) tuple"; return false; }
    if (!sim || !isPlaying) { outError = "Forces only work while playing"; return false; }
    if (impulse) sim->ApplyImpulseToBody(obj, v);
    else sim->ApplyForceToBody(obj, v);
    return true;
}

std::string ScriptRuntime::RayCastName(const std::string& origin, const std::string& dir, float maxDistance, std::string& outError)
{
    Vector3 o, d;
    if (!ParseVec3(origin, o)) { outError = "'" + origin + "' is not a valid (x, y, z) tuple"; return {}; }
    if (!ParseVec3(dir, d)) { outError = "'" + dir + "' is not a valid (x, y, z) tuple"; return {}; }
    if (!sim) { outError = "No physics simulation"; return {}; }
    const phys::RaycastHit hit = sim->RayCast(o, d, maxDistance > 0.0f ? maxDistance : 100.0f);
    return hit.hit && hit.object ? hit.object->GetName() : std::string();
}

void ScriptRuntime::SetSimPlaying(bool playing)
{
    if (playing == isPlaying) return;
    isPlaying = playing;
    if (playing) OnPlayStarted();
    else OnPlayStopped();
}

void ScriptRuntime::SetBodyVelocity(ScatteredObject* object, Vector3 velocity)
{ if (sim) sim->SetBodyVelocity(object, velocity); }
void ScriptRuntime::SetBodyAngularVelocity(ScatteredObject* object, Vector3 velocity)
{ if (sim) sim->SetBodyAngularVelocity(object, velocity); }
void ScriptRuntime::SetBodyPosition(ScatteredObject* object, Vector3 position)
{ if (sim) sim->SetBodyPosition(object, position); }
void ScriptRuntime::SetBodyOrientation(ScatteredObject* object, Vector3 eulerDeg)
{ if (sim) sim->SetBodyOrientation(object, eulerDeg); }

bool ScriptRuntime::SetObjectProperty(ScatteredObject* target, const std::string& prop,
                                      const std::string& rhs, std::string& outError)
{
    if (!target) { outError = "No target object"; return false; }

    if (prop == "Position") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyPosition(target, v);
        *target->GetPosPtr() = v;
        return true;
    }
    if (prop == "Size") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        *target->GetSizePtr() = v;
        target->ResetMassAuto();
        return true;
    }
    if (prop == "Mass") {
        float m = static_cast<float>(std::atof(rhs.c_str()));
        if (!(m > 0.0f)) { outError = "'" + rhs + "' is not a valid positive mass"; return false; }
        target->SetMass(m);
        return true;
    }
    if (prop == "Rotation") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyOrientation(target, v);
        *target->GetRotationPtr() = v;
        return true;
    }
    if (prop == "Origin") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        *target->GetOriginPtr() = v;
        return true;
    }
    if (prop == "Color") {
        Color c;
        if (!ParseColor(rhs, c)) { outError = "'" + rhs + "' is not a valid color (r, g, b)"; return false; }
        *target->GetColorPtr() = c;
        return true;
    }
    if (prop == "Velocity") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyVelocity(target, v);
        target->SetVelocity(v);
        return true;
    }
    if (prop == "AngularVelocity") {
        Vector3 v;
        if (!ParseVec3(rhs, v)) { outError = "'" + rhs + "' is not a valid (x, y, z) tuple"; return false; }
        if (isPlaying) SetBodyAngularVelocity(target, v);
        target->SetAngularVelocity(v);
        return true;
    }
    if (prop == "Anchored") {
        bool value;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        target->anchored = value;
        return true;
    }
    if (prop == "CanCollide") {
        bool value;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        target->canCollide = value;
        return true;
    }
    if (prop == "Negate") {   // a cutter: carved out of a union, not drawn in play, never collides
        bool value;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        if (target->HasModel() || target->IsUnion()) { outError = "Only a primitive can be a cutter (Negate)"; return false; }
        target->negate = value;
        target->canCollide = !value;
        if (isPlaying) RebuildBody(target);
        return true;
    }
    if (prop == "UseGlobalColor") {   // a union painted in one colour (its Color) instead of each part's own
        bool value;
        if (!ParseBool(rhs, value)) { outError = "'" + rhs + "' is not a valid boolean (true/false)"; return false; }
        if (!target->IsUnion()) { outError = "UseGlobalColor only applies to a union"; return false; }
        target->SetUseGlobalColor(value);
        return true;
    }
    if (prop == "CollisionAccuracy") {
        pcoll::CollisionAccuracy acc;
        if (!pcoll::ParseCollisionAccuracy(rhs, acc)) {
            outError = "'" + rhs + "' is not a valid collision accuracy (Box, Hull, Default, Precise)";
            return false;
        }
        target->SetCollisionAccuracy(acc);
        return true;
    }
    if (prop == "Transparency") {
        float t = static_cast<float>(std::atof(rhs.c_str()));
        if (!(t >= 0.0f && t <= 1.0f)) { outError = "'" + rhs + "' is out of range (0 to 1)"; return false; }
        target->SetTransparency(t);
        return true;
    }

    outError = "Unknown property '" + prop + "'";
    return false;
}

bool ScriptRuntime::GetObjectProperty(ScatteredObject* target, const std::string& prop, std::string& out)
{
    if (!target) return false;
    char b[96];
    auto vec = [&](Vector3 v) { std::snprintf(b, sizeof(b), "(%g, %g, %g)", v.x, v.y, v.z); out = b; return true; };
    if (prop == "Position") return vec(*target->GetPosPtr());
    if (prop == "Size") return vec(*target->GetSizePtr());
    if (prop == "Rotation") return vec(*target->GetRotationPtr());
    if (prop == "Origin") return vec(*target->GetOriginPtr());
    if (prop == "Velocity") return vec(target->GetVelocity());
    if (prop == "Color") {
        const Color c = *target->GetColorPtr();
        std::snprintf(b, sizeof(b), "(%d, %d, %d)", c.r, c.g, c.b);
        out = b;
        return true;
    }
    if (prop == "Mass") { std::snprintf(b, sizeof(b), "%g", target->GetMass()); out = b; return true; }
    if (prop == "Transparency") { std::snprintf(b, sizeof(b), "%g", target->GetTransparency()); out = b; return true; }
    if (prop == "Anchored") { out = target->anchored ? "true" : "false"; return true; }
    if (prop == "CanCollide") { out = target->canCollide ? "true" : "false"; return true; }
    if (prop == "Negate") { out = target->negate ? "true" : "false"; return true; }
    if (prop == "UseGlobalColor") { out = target->IsUnion() && target->useGlobalColor ? "true" : "false"; return true; }
    return false;
}

void ScriptRuntime::SetPhysicsGravity(float g) { if (sim) sim->SetGravity(g); }
float ScriptRuntime::GetPhysicsGravity() const { return sim ? sim->GetGravity() : -19.62f; }
void ScriptRuntime::SetPhysicsFriction(float f) { if (sim) sim->SetFriction(f); }
float ScriptRuntime::GetPhysicsFriction() const { return sim ? sim->GetFriction() : 0.4f; }
void ScriptRuntime::SetPhysicsRestitution(float r) { if (sim) sim->SetRestitution(r); }
float ScriptRuntime::GetPhysicsRestitution() const { return sim ? sim->GetRestitution() : 0.7f; }

void ScriptRuntime::RemoveStandalone(int index)
{
    if (index < 0 || index >= static_cast<int>(standaloneScripts.size())) return;
    if (hostPtr) hostPtr->OnStandaloneRemoved(index);
    standaloneScripts.erase(standaloneScripts.begin() + index);
}

void ScriptRuntime::StartObjectScript(ScatteredObject* obj)
{
    if (hostPtr && obj && !obj->script.empty()) hostPtr->StartObjectScriptOn(obj);
}

bool ScriptRuntime::StartSnippet(const std::string& className)
{
    return hostPtr && hostPtr->StartSnippetScript(className);
}

void ScriptRuntime::SetSnippetInputs(const std::string& className, std::vector<std::string> values)
{
    snippetValues[className].in = std::move(values);
}

std::string ScriptRuntime::SnippetInput(const std::string& className, int pin) const
{
    const auto it = snippetValues.find(className);
    if (it == snippetValues.end() || pin < 0 || pin >= static_cast<int>(it->second.in.size())) return {};
    return it->second.in[pin];
}

void ScriptRuntime::SetSnippetOutput(const std::string& className, int pin, const std::string& value)
{
    if (pin < 0 || pin > 63) return;
    auto& out = snippetValues[className].out;
    if (static_cast<int>(out.size()) <= pin) out.resize(pin + 1);
    out[pin] = value;
}

std::string ScriptRuntime::SnippetOutput(const std::string& className, int pin) const
{
    const auto it = snippetValues.find(className);
    if (it == snippetValues.end() || pin < 0 || pin >= static_cast<int>(it->second.out.size())) return {};
    return it->second.out[pin];
}

void ScriptRuntime::ClearSnippetValues() { snippetValues.clear(); }

void ScriptRuntime::StopObjectScript(ScatteredObject* obj)
{
    if (hostPtr && obj) hostPtr->StopObjectScriptOn(obj);
}

void ScriptRuntime::StartStandaloneScript(int index)
{
    if (hostPtr && index >= 0 && index < static_cast<int>(standaloneScripts.size()))
        hostPtr->StartStandaloneScriptAt(index);
}

void ScriptRuntime::StopStandaloneScript(int index)
{
    if (hostPtr) hostPtr->StopStandaloneScriptAt(index);
}

void ScriptRuntime::StopAllScripts()
{
    if (hostPtr) hostPtr->ClearAllScripts();
}

void ScriptRuntime::OnPlayStarted()
{
    gameui::Clear();
    nav::ResetWorld();
    gameui::SetProjectPath(m_projectPath);
    ClearSnippetValues();
    // Everything that will be put back when play stops is recorded BEFORE any script runs. The visual script's Start
    // chain runs below, and what it changes (or creates) must count as a change, not as how the scene started.
    if (hostPtr) {
        playCreatedObjects.clear();
        CaptureWorldSettings();
    }
    if (g_prePlayHook) g_prePlayHook();
    vscript::Run(*this);
    if (!hostPtr) return;

    // Start every runOnPlay object script and standalone script.
    if (objectsPtr) {
        for (auto* obj : *objectsPtr)
            if (obj && obj->runOnPlay && !obj->script.empty())
                hostPtr->StartObjectScriptOn(obj);
    }
    for (int i = 0; i < static_cast<int>(standaloneScripts.size()); ++i)
        if (standaloneScripts[i].runOnPlay)
            hostPtr->StartStandaloneScriptAt(i);
}

void ScriptRuntime::RollbackCreatedObjects()
{
    // Take the objects scripts created out of the scene for real: the scene
    // list, any physics body, and the engine entity that owns (and draws) them.
    for (auto it = playCreatedObjects.rbegin(); it != playCreatedObjects.rend(); ++it) {
        ScatteredObject* obj = *it;
        if (!obj) continue;
        if (objectsPtr) {
            auto fit = std::find(objectsPtr->begin(), objectsPtr->end(), obj);
            if (fit == objectsPtr->end()) continue; // already gone
            objectsPtr->erase(fit);
        }
        if (sim) sim->RemoveObject(obj);
        if (engine) engine->RemoveEntity(obj);
    }
    playCreatedObjects.clear();
    // The groups prefabs made. Their objects are gone by now; a group left inside another one is cut loose first.
    for (ModelGroup* g : playCreatedGroups)
        if (g->parent && std::find(playCreatedGroups.begin(), playCreatedGroups.end(), g->parent) == playCreatedGroups.end())
            g->parent->RemoveChild(g);
    if (modelsPtr)
        modelsPtr->erase(std::remove_if(modelsPtr->begin(), modelsPtr->end(), [&](const std::unique_ptr<ModelGroup>& m) {
            return std::find(playCreatedGroups.begin(), playCreatedGroups.end(), m.get()) != playCreatedGroups.end();
        }), modelsPtr->end());
    playCreatedGroups.clear();
}

// The scene's Lighting section as it was when play started: a script may change the time, the
// weather, the sky (Game::Environment), and stopping puts the scene back as it was authored.
static gfx::LightingSettings g_savedLighting;

void ScriptRuntime::CaptureWorldSettings()
{
    g_savedLighting = gfx::Lighting();
    savedWorld.valid = true;
    savedWorld.shadows = FlyNative_GetShadowsEnabled();
    savedWorld.shadowQuality = FlyNative_GetShadowQuality();
    savedWorld.grid = FlyNative_GetGridVisible();
    savedWorld.wireframe = FlyNative_GetWireframe();
    float v = 0.0f;
    FlyNative_GetAmbient(&v);
    savedWorld.ambient = v;
    FlyNative_GetFov(&v);
    savedWorld.fov = v;
    savedWorld.gravity = FlyNative_GetGravity();
    savedWorld.friction = FlyNative_GetFriction();
    savedWorld.restitution = FlyNative_GetRestitution();
}

void ScriptRuntime::RestoreWorldSettings()
{
    if (!savedWorld.valid) return;
    savedWorld.valid = false;
    gfx::Lighting() = g_savedLighting;      // (before the ambient below, which is one of its fields)
    FlyNative_SetShadowsEnabled(savedWorld.shadows);
    FlyNative_SetShadowQuality(savedWorld.shadowQuality);
    FlyNative_SetGridVisible(savedWorld.grid);
    FlyNative_SetWireframe(savedWorld.wireframe);
    FlyNative_SetAmbient(savedWorld.ambient);
    FlyNative_SetFov(savedWorld.fov);
    FlyNative_SetGravity(savedWorld.gravity);
    FlyNative_SetFriction(savedWorld.friction);
    FlyNative_SetRestitution(savedWorld.restitution);
}

void ScriptRuntime::OnPlayStopped()
{
    vscript::Stop(*this);
    ClearSnippetValues();
    if (hostPtr) hostPtr->ClearAllScripts();
    RollbackCreatedObjects();
    RestoreWorldSettings();
    gameui::Clear();
    nav::ResetWorld();
    if (g_postPlayHook) g_postPlayHook();   // last: the editor puts the scene back once nothing can change it any more
}

void ScriptRuntime::LogCreatedObject(ScatteredObject* obj)
{
    if (isPlaying && obj) playCreatedObjects.push_back(obj);
}