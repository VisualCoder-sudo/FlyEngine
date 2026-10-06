/*
 * CPluginAPI.cpp - Implementation of the vtable-based CPluginAPI for FlyEngine
 * 
 * This provides the CP_EngineAPI vtable that plugins use to interact with the engine.
 * The engine initializes this by calling CP_Internal_SetEngine() and CP_Internal_SetScriptRuntime().
 */

#include "../../../include/CPluginAPI.h"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Backend/Entity.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"

#include <raylib.h>
#include <raymath.h>
#include <imgui.h>
#include <string>
#include <vector>
#include <map>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>

namespace fs = std::filesystem;

namespace {

// Global engine pointer
Engine* g_engine = nullptr;
ScriptRuntime* g_scriptRuntime = nullptr;

// Entity handle mapping
using FlyEntity = uint64_t;
static std::map<FlyEntity, ScatteredObject*> g_entityMap;
static FlyEntity g_nextEntityHandle = 1;

inline FlyEntity MakeHandle(ScatteredObject* obj) {
    FlyEntity h = g_nextEntityHandle++;
    g_entityMap[h] = obj;
    return h;
}

inline ScatteredObject* GetObject(FlyEntity handle) {
    auto it = g_entityMap.find(handle);
    return (it != g_entityMap.end()) ? it->second : nullptr;
}

inline void RemoveHandle(FlyEntity handle) {
    g_entityMap.erase(handle);
}

// Helpers
inline Color ToColor(CP_Color c) {
    return Color{(unsigned char)(c.r * 255), (unsigned char)(c.g * 255), 
                 (unsigned char)(c.b * 255), (unsigned char)(c.a * 255)};
}
inline CP_Color FromColor(Color c) {
    return CP_Color{c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f};
}
inline Vector3 ToVec3(CP_Vec3 v) { return Vector3{v.x, v.y, v.z}; }
inline CP_Vec3 FromVec3(Vector3 v) { return CP_Vec3{v.x, v.y, v.z}; }
inline Vector2 ToVec2(CP_Vec2 v) { return Vector2{v.x, v.y}; }
inline CP_Vec2 FromVec2(Vector2 v) { return CP_Vec2{v.x, v.y}; }
inline Quaternion ToQuat(CP_Quat q) { return Quaternion{q.x, q.y, q.z, q.w}; }
inline CP_Quat FromQuat(Quaternion q) { return CP_Quat{q.x, q.y, q.z, q.w}; }

std::string GetProjectRoot() {
    if (g_scriptRuntime) return g_scriptRuntime->GetProjectPath();
    return "";
}

phys::Simulation* GetSimulation(Engine* eng) {
    if (!eng) return nullptr;
    for (auto& e : eng->GetEntities()) {
        if (auto* s = dynamic_cast<phys::Simulation*>(e.get())) return s;
    }
    return nullptr;
}

// ============================================================================
// CP_ENGINEAPI VTABLE IMPLEMENTATION
// ============================================================================

// Memory
static void* api_alloc(size_t size, const char* tag) {
    (void)tag;
    return malloc(size);
}
static void api_free(void* ptr) {
    free(ptr);
}

// Logging
static void api_log(CP_LogLevel level, const char* tag, const char* fmt, ...) {
    if (!g_engine) return;
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    
    const char* prefix = "";
    switch (level) {
        case CP_LOG_DEBUG: prefix = "[DEBUG] "; break;
        case CP_LOG_INFO:  prefix = "[INFO] "; break;
        case CP_LOG_WARN:  prefix = "[WARN] "; break;
        case CP_LOG_ERROR: prefix = "[ERROR] "; break;
    }
    ui::LogAlways("%s[%s] %s", prefix, tag ? tag : "plugin", buf);
}

// Entity/Component
static CP_EntityID api_entity_create(const char* name) {
    Engine* eng = g_engine;
    if (!eng) return 0;
    
    auto obj = std::make_unique<ScatteredObject>(Vector3{0,0,0}, Vector3{1,1,1}, WHITE, ShapeType::Cube);
    obj->SetName(name ? name : "Entity");
    obj->anchored = false;
    ScatteredObject* raw = obj.get();
    
    FlyEntity handle = MakeHandle(raw);
    eng->AddEntity(std::move(obj));
    return handle;
}

static void api_entity_destroy(CP_EntityID id) {
    if (!id) return;
    ScatteredObject* obj = GetObject(id);
    if (obj) obj->alive = false;
    RemoveHandle(id);
}

static CP_EntityID api_entity_find_by_name(const char* name) {
    Engine* eng = g_engine;
    if (!eng || !name) return 0;
    for (auto& e : eng->GetEntities()) {
        if (auto* obj = dynamic_cast<ScatteredObject*>(e.get())) {
            if (obj->GetName() == name) return MakeHandle(obj);
        }
    }
    return 0;
}

static CP_EntityID api_entity_find_by_tag(const char* tag) {
    Engine* eng = g_engine;
    if (!eng || !tag) return 0;
    for (auto& e : eng->GetEntities()) {
        if (auto* obj = dynamic_cast<ScatteredObject*>(e.get())) {
            if (obj->script == tag) return MakeHandle(obj);
        }
    }
    return 0;
}

static int api_entity_get_count(void) {
    Engine* eng = g_engine;
    if (!eng) return 0;
    int count = 0;
    for (auto& e : eng->GetEntities())
        if (dynamic_cast<ScatteredObject*>(e.get())) count++;
    return count;
}

static CP_EntityID api_entity_get_at(int index) {
    Engine* eng = g_engine;
    if (!eng) return 0;
    int i = 0;
    for (auto& e : eng->GetEntities()) {
        if (auto* obj = dynamic_cast<ScatteredObject*>(e.get())) {
            if (i == index) return MakeHandle(obj);
            i++;
        }
    }
    return 0;
}

// Transform
static CP_Vec3 api_entity_get_position(CP_EntityID id) {
    ScatteredObject* obj = GetObject(id);
    if (!obj) return CP_Vec3{0,0,0};
    Vector3* pos = obj->GetPosPtr();
    return pos ? FromVec3(*pos) : CP_Vec3{0,0,0};
}

static void api_entity_set_position(CP_EntityID id, CP_Vec3 pos) {
    ScatteredObject* obj = GetObject(id);
    if (obj) {
        Vector3* p = obj->GetPosPtr();
        if (p) *p = ToVec3(pos);
    }
}

static CP_Vec3 api_entity_get_rotation(CP_EntityID id) {
    ScatteredObject* obj = GetObject(id);
    if (!obj) return CP_Vec3{0,0,0};
    Vector3* rot = obj->GetRotationPtr();
    return rot ? FromVec3(*rot) : CP_Vec3{0,0,0};
}

static void api_entity_set_rotation(CP_EntityID id, CP_Vec3 euler) {
    ScatteredObject* obj = GetObject(id);
    if (obj) {
        Vector3* rot = obj->GetRotationPtr();
        if (rot) *rot = ToVec3(euler);
    }
}

static CP_Vec3 api_entity_get_scale(CP_EntityID id) {
    ScatteredObject* obj = GetObject(id);
    if (!obj) return CP_Vec3{1,1,1};
    Vector3* size = obj->GetSizePtr();
    return size ? FromVec3(*size) : CP_Vec3{1,1,1};
}

static void api_entity_set_scale(CP_EntityID id, CP_Vec3 scale) {
    ScatteredObject* obj = GetObject(id);
    if (obj) {
        Vector3* size = obj->GetSizePtr();
        if (size) *size = ToVec3(scale);
    }
}

static CP_Transform api_entity_get_transform(CP_EntityID id) {
    CP_Transform t = {0};
    ScatteredObject* obj = GetObject(id);
    if (!obj) return t;
    Vector3* pos = obj->GetPosPtr();
    Vector3* rot = obj->GetRotationPtr();
    Vector3* size = obj->GetSizePtr();
    if (pos) t.position = FromVec3(*pos);
    if (rot) t.rotation = FromVec3(*rot);
    if (size) t.scale = FromVec3(*size);
    return t;
}

static void api_entity_set_transform(CP_EntityID id, const CP_Transform* transform) {
    if (!transform) return;
    ScatteredObject* obj = GetObject(id);
    if (obj) {
        Vector3* pos = obj->GetPosPtr();
        Vector3* rot = obj->GetRotationPtr();
        Vector3* size = obj->GetSizePtr();
        if (pos) *pos = ToVec3(transform->position);
        if (rot) *rot = ToVec3(transform->rotation);
        if (size) *size = ToVec3(transform->scale);
    }
}

static void api_entity_translate(CP_EntityID id, CP_Vec3 offset) {
    ScatteredObject* obj = GetObject(id);
    if (!obj) return;
    Vector3* pos = obj->GetPosPtr();
    if (pos) *pos = Vector3Add(*pos, ToVec3(offset));
}

static void api_entity_rotate(CP_EntityID id, CP_Vec3 euler_delta) {
    ScatteredObject* obj = GetObject(id);
    if (!obj) return;
    Vector3* rot = obj->GetRotationPtr();
    if (rot) {
        rot->x += euler_delta.x;
        rot->y += euler_delta.y;
        rot->z += euler_delta.z;
    }
}

// Components (generic)
static void* api_entity_get_component(CP_EntityID id, const char* component_type) {
    ScatteredObject* obj = GetObject(id);
    if (!obj || !component_type) return nullptr;
    
    if (strcmp(component_type, "Transform") == 0) {
        // Return transform data (simplified)
        static CP_Transform transform;
        transform = api_entity_get_transform(id);
        return &transform;
    }
    return nullptr;
}

static int api_entity_add_component(CP_EntityID id, const char* type, void* data) {
    (void)id; (void)type; (void)data;
    // Would need more infrastructure for generic components
    return 0;
}

static int api_entity_remove_component(CP_EntityID id, const char* type) {
    (void)id; (void)type;
    return 0;
}

static bool api_entity_has_component(CP_EntityID id, const char* type) {
    (void)id; (void)type;
    return false;
}

// Rendering
static CP_MeshHandle api_mesh_create_cube(float size) {
    Model m = LoadModelFromMesh(GenMeshCube(size, size, size));
    return (CP_MeshHandle)(uintptr_t)&m;  // Simplified - would need proper handle management
}
static CP_MeshHandle api_mesh_create_sphere(float radius) {
    Model m = LoadModelFromMesh(GenMeshSphere(radius, 16, 16));
    return (CP_MeshHandle)(uintptr_t)&m;
}
static CP_MeshHandle api_mesh_create_cylinder(float radius, float height) {
    Model m = LoadModelFromMesh(GenMeshCylinder(radius, height, 16));
    return (CP_MeshHandle)(uintptr_t)&m;
}
static CP_MeshHandle api_mesh_create_plane(float width, float length) {
    Model m = LoadModelFromMesh(GenMeshPlane(width, length, 1, 1));
    return (CP_MeshHandle)(uintptr_t)&m;
}
static CP_MeshHandle api_mesh_load(const char* path) {
    if (!path) return 0;
    Model m = LoadModel(path);
    return (CP_MeshHandle)(uintptr_t)&m;
}
static void api_mesh_destroy(CP_MeshHandle handle) {
    if (handle) {
        Model* m = (Model*)(uintptr_t)handle;
        UnloadModel(*m);
    }
}

static CP_MaterialHandle api_material_create(const char* name) {
    (void)name;
    Material m = LoadMaterialDefault();
    return (CP_MaterialHandle)(uintptr_t)&m;
}
static void api_material_destroy(CP_MaterialHandle handle) {
    (void)handle;
}
static void api_material_set_color(CP_MaterialHandle handle, CP_Color color) {
    if (!handle) return;
    Material* m = (Material*)(uintptr_t)handle;
    m->maps[MATERIAL_MAP_ALBEDO].color = ToColor(color);
}
static void api_material_set_texture(CP_MaterialHandle handle, CP_TextureHandle texture) {
    (void)handle; (void)texture;
}

static CP_TextureHandle api_texture_load(const char* path) {
    if (!path) return 0;
    Texture2D tex = LoadTexture(path);
    return (CP_TextureHandle)(uintptr_t)&tex;
}
static void api_texture_destroy(CP_TextureHandle handle) {
    if (handle) {
        Texture2D* tex = (Texture2D*)(uintptr_t)handle;
        UnloadTexture(*tex);
    }
}

static void api_render_submit_mesh(CP_MeshHandle mesh, CP_MaterialHandle material, const CP_Transform* transform) {
    (void)mesh; (void)material; (void)transform;
    // Would draw the mesh with the material at the transform
}

// Input
static bool api_input_is_key_down(CP_KeyCode key) {
    return IsKeyDown((int)key);
}
static bool api_input_is_key_pressed(CP_KeyCode key) {
    return IsKeyPressed((int)key);
}
static bool api_input_is_key_released(CP_KeyCode key) {
    return IsKeyReleased((int)key);
}
static int api_input_get_key_pressed(void) {
    return GetKeyPressed();
}
static CP_Vec2 api_input_get_mouse_pos(void) {
    Vector2 p = GetMousePosition();
    return FromVec2(p);
}
static CP_Vec2 api_input_get_mouse_delta(void) {
    Vector2 d = GetMouseDelta();
    return FromVec2(d);
}
static bool api_input_is_mouse_down(int button) {
    return IsMouseButtonDown(button);
}
static bool api_input_is_mouse_pressed(int button) {
    return IsMouseButtonPressed(button);
}
static void api_input_set_mouse_pos(int x, int y) {
    SetMousePosition(x, y);
}
static bool api_input_is_gamepad_available(int gamepad) {
    return IsGamepadAvailable(gamepad);
}
static bool api_input_is_gamepad_button_down(int gamepad, int button) {
    return IsGamepadButtonDown(gamepad, button);
}
static float api_input_get_gamepad_axis(int gamepad, int axis) {
    return GetGamepadAxisMovement(gamepad, axis);
}

// Physics
static CP_EntityID api_physics_add_rigidbody(CP_EntityID entity, float mass) {
    Engine* eng = g_engine;
    if (!eng) return 0;
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return 0;
    
    phys::Simulation* sim = GetSimulation(eng);
    if (sim) {
        sim->SpawnBodyForObject(obj, mass > 0 ? b3_dynamicBody : b3_staticBody);
        obj->anchored = false;
    }
    return entity;
}

static void api_physics_add_force(CP_EntityID entity, CP_Vec3 force) {
    Engine* eng = g_engine;
    if (!eng) return;
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return;
    phys::Simulation* sim = GetSimulation(eng);
    if (sim) sim->ApplyForceToBody(obj, ToVec3(force));
}

static void api_physics_add_torque(CP_EntityID entity, CP_Vec3 torque) {
    (void)entity; (void)torque;
}

static void api_physics_set_velocity(CP_EntityID entity, CP_Vec3 vel) {
    Engine* eng = g_engine;
    if (!eng) return;
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return;
    phys::Simulation* sim = GetSimulation(eng);
    if (sim) sim->SetBodyVelocity(obj, ToVec3(vel));
}

static void api_physics_set_angular_velocity(CP_EntityID entity, CP_Vec3 vel) {
    Engine* eng = g_engine;
    if (!eng) return;
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return;
    phys::Simulation* sim = GetSimulation(eng);
    if (sim) sim->SetBodyAngularVelocity(obj, ToVec3(vel));
}

static CP_Vec3 api_physics_get_velocity(CP_EntityID entity) {
    Engine* eng = g_engine;
    if (!eng) return CP_Vec3{0,0,0};
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return CP_Vec3{0,0,0};
    phys::Simulation* sim = GetSimulation(eng);
    return sim ? FromVec3(sim->GetBodyVelocity(obj)) : CP_Vec3{0,0,0};
}

static CP_Vec3 api_physics_get_angular_velocity(CP_EntityID entity) {
    Engine* eng = g_engine;
    if (!eng) return CP_Vec3{0,0,0};
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return CP_Vec3{0,0,0};
    phys::Simulation* sim = GetSimulation(eng);
    return sim ? FromVec3(sim->GetBodyAngularVelocity(obj)) : CP_Vec3{0,0,0};
}

static void api_physics_set_box_collider(CP_EntityID entity, CP_Vec3 half_extents) {
    (void)entity; (void)half_extents;
}
static void api_physics_set_sphere_collider(CP_EntityID entity, float radius) {
    (void)entity; (void)radius;
}
static void api_physics_set_friction(CP_EntityID entity, float friction) {
    Engine* eng = g_engine;
    if (!eng) return;
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return;
    phys::Simulation* sim = GetSimulation(eng);
    if (sim) sim->SetFriction(friction);
}
static void api_physics_set_restitution(CP_EntityID entity, float restitution) {
    Engine* eng = g_engine;
    if (!eng) return;
    ScatteredObject* obj = GetObject(entity);
    if (!obj) return;
    phys::Simulation* sim = GetSimulation(eng);
    if (sim) sim->SetRestitution(restitution);
}

static CP_RaycastHit api_physics_raycast(CP_Vec3 origin, CP_Vec3 direction, float max_distance) {
    CP_RaycastHit hit = {false, {0,0,0}, {0,0,0}, 0, max_distance};
    Engine* eng = g_engine;
    if (!eng) return hit;
    phys::Simulation* sim = GetSimulation(eng);
    if (!sim) return hit;
    
    Vector3 end = Vector3Add(ToVec3(origin), Vector3Scale(Vector3Normalize(ToVec3(direction)), max_distance));
    phys::RaycastHit result = sim->RayCast(ToVec3(origin), end);
    
    hit.hit = result.hit;
    hit.point = FromVec3(result.point);
    hit.normal = FromVec3(result.normal);
    hit.distance = Vector3Distance(ToVec3(origin), result.point);
    
    if (result.object) {
        for (auto& [h, obj] : g_entityMap) {
            if (obj == result.object) {
                hit.entity = h;
                break;
            }
        }
    }
    return hit;
}

// Audio (stubs)
static CP_EntityID api_audio_play_sound(const char* path, float volume, float pitch, bool loop) {
    (void)path; (void)volume; (void)pitch; (void)loop;
    return 0;
}
static CP_EntityID api_audio_play_sound_3d(const char* path, CP_Vec3 pos, float vol, float pitch, bool loop, float min_dist, float max_dist) {
    (void)path; (void)pos; (void)vol; (void)pitch; (void)loop; (void)min_dist; (void)max_dist;
    return 0;
}
static void api_audio_set_volume(CP_EntityID sound, float volume) { (void)sound; (void)volume; }
static void api_audio_set_pitch(CP_EntityID sound, float pitch) { (void)sound; (void)pitch; }
static void api_audio_set_position(CP_EntityID sound, CP_Vec3 pos) { (void)sound; (void)pos; }
static void api_audio_stop(CP_EntityID sound) { (void)sound; }
static bool api_audio_is_playing(CP_EntityID sound) { (void)sound; return false; }
static void api_audio_set_master_volume(float volume) { SetMasterVolume(volume); }
static float api_audio_get_master_volume(void) { return GetMasterVolume(); }

// UI
static bool api_ui_begin_window(const char* name, CP_Vec2 pos, CP_Vec2 size, bool* open) {
    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(size.x, size.y), ImGuiCond_FirstUseEver);
    return ImGui::Begin(name, open);
}
static void api_ui_end_window(void) { ImGui::End(); }
static bool api_ui_button(const char* label, CP_Vec2 size) {
    return (size.x > 0 || size.y > 0) ? ImGui::Button(label, ImVec2(size.x, size.y)) : ImGui::Button(label);
}
static bool api_ui_checkbox(const char* label, bool* value) { return ImGui::Checkbox(label, value); }
static bool api_ui_slider_float(const char* label, float* value, float min, float max) { return ImGui::SliderFloat(label, value, min, max); }
static bool api_ui_slider_int(const char* label, int* value, int min, int max) { return ImGui::SliderInt(label, value, min, max); }
static bool api_ui_input_text(const char* label, char* buffer, int buffer_size) { return ImGui::InputText(label, buffer, buffer_size); }
static bool api_ui_combo(const char* label, int* current, const char* const* items, int count) { return ImGui::Combo(label, current, items, count); }
static void api_ui_text(const char* text) { ImGui::Text("%s", text); }
static void api_ui_text_colored(const char* text, CP_Color color) { ImGui::TextColored(ImVec4(color.r, color.g, color.b, color.a), "%s", text); }
static void api_ui_separator(void) { ImGui::Separator(); }
static void api_ui_spacing(void) { ImGui::Spacing(); }
static void api_ui_same_line(void) { ImGui::SameLine(); }
static void api_ui_set_next_item_width(float width) { ImGui::SetNextItemWidth(width); }
static void api_ui_push_id(const char* id) { ImGui::PushID(id); }
static void api_ui_pop_id(void) { ImGui::PopID(); }

// Asset System (stubs)
static CP_AssetHandle api_asset_load(const char* path, int type) { (void)path; (void)type; return 0; }
static void api_asset_unload(CP_AssetHandle handle) { (void)handle; }
static bool api_asset_is_loaded(CP_AssetHandle handle) { (void)handle; return false; }
static const char* api_asset_get_path(CP_AssetHandle handle) { (void)handle; return ""; }

// Plugin Communication (stubs)
static void* api_plugin_get_api(const char* plugin_name, const char* api_name) { (void)plugin_name; (void)api_name; return nullptr; }
static int api_plugin_call(const char* plugin_name, const char* func_name, void* args, void* result) { (void)plugin_name; (void)func_name; (void)args; (void)result; return -1; }

// File System
static CP_FileHandle api_fs_open(const char* path, CP_FileMode mode) {
    if (!path) return 0;
    const char* c_mode = "rb";
    if (mode & CP_FILE_WRITE) c_mode = "wb";
    else if (mode & CP_FILE_APPEND) c_mode = "ab";
    if (mode & CP_FILE_BINARY) strcat(const_cast<char*>(c_mode), "b");  // Simplified
    FILE* f = fopen(path, c_mode);
    return (CP_FileHandle)(uintptr_t)f;
}
static size_t api_fs_read(CP_FileHandle handle, void* buffer, size_t size) {
    if (!handle) return 0;
    return fread(buffer, 1, size, (FILE*)(uintptr_t)handle);
}
static size_t api_fs_write(CP_FileHandle handle, const void* buffer, size_t size) {
    if (!handle) return 0;
    return fwrite(buffer, 1, size, (FILE*)(uintptr_t)handle);
}
static void api_fs_close(CP_FileHandle handle) {
    if (handle) fclose((FILE*)(uintptr_t)handle);
}
static bool api_fs_exists(const char* path) {
    if (!path) return false;
    return fs::exists(path);
}
static int64_t api_fs_size(const char* path) {
    if (!path) return 0;
    std::error_code ec;
    return fs::file_size(path, ec) ? fs::file_size(path, ec) : 0;
}

// Time
static double api_time_now(void) { return GetTime(); }
static float api_time_delta(void) { return GetFrameTime(); }
static float api_time_fps(void) { return GetFPS(); }

// Reflection (stubs)
static const CP_TypeInfo* api_type_find(const char* name) { (void)name; return nullptr; }
static void* api_type_create_instance(const CP_TypeInfo* type) { (void)type; return nullptr; }

// Permission check
static bool api_has_permission(CP_Permission perm) {
    // For now, all permissions granted in-editor
    return true;
}

// Project info
static const char* api_get_project_path(void) {
    static thread_local std::string cache;
    if (g_scriptRuntime) cache = g_scriptRuntime->GetProjectPath();
    return cache.c_str();
}
static const char* api_get_project_name(void) {
    static thread_local std::string cache;
    if (g_scriptRuntime) cache = g_scriptRuntime->GetProjectName();
    return cache.c_str();
}

// ============================================================================
// VTABLE INSTANCE
// ============================================================================

static CP_EngineAPI g_engine_api = {
    .version = CP_API_CURRENT_VERSION,
    
    .alloc = api_alloc,
    .free = api_free,
    .log = api_log,
    
    .entity_create = api_entity_create,
    .entity_destroy = api_entity_destroy,
    .entity_find_by_name = api_entity_find_by_name,
    .entity_find_by_tag = api_entity_find_by_tag,
    .entity_get_count = api_entity_get_count,
    .entity_get_at = api_entity_get_at,
    .entity_get_position = api_entity_get_position,
    .entity_set_position = api_entity_set_position,
    .entity_get_rotation = api_entity_get_rotation,
    .entity_set_rotation = api_entity_set_rotation,
    .entity_get_scale = api_entity_get_scale,
    .entity_set_scale = api_entity_set_scale,
    .entity_get_transform = api_entity_get_transform,
    .entity_set_transform = api_entity_set_transform,
    .entity_translate = api_entity_translate,
    .entity_rotate = api_entity_rotate,
    .entity_get_component = api_entity_get_component,
    .entity_add_component = api_entity_add_component,
    .entity_remove_component = api_entity_remove_component,
    .entity_has_component = api_entity_has_component,
    
    .mesh_create_cube = api_mesh_create_cube,
    .mesh_create_sphere = api_mesh_create_sphere,
    .mesh_create_cylinder = api_mesh_create_cylinder,
    .mesh_create_plane = api_mesh_create_plane,
    .mesh_load = api_mesh_load,
    .mesh_destroy = api_mesh_destroy,
    .material_create = api_material_create,
    .material_destroy = api_material_destroy,
    .material_set_color = api_material_set_color,
    .material_set_texture = api_material_set_texture,
    .texture_load = api_texture_load,
    .texture_destroy = api_texture_destroy,
    .render_submit_mesh = api_render_submit_mesh,
    
    .input_is_key_down = api_input_is_key_down,
    .input_is_key_pressed = api_input_is_key_pressed,
    .input_is_key_released = api_input_is_key_released,
    .input_get_key_pressed = api_input_get_key_pressed,
    .input_get_mouse_pos = api_input_get_mouse_pos,
    .input_get_mouse_delta = api_input_get_mouse_delta,
    .input_is_mouse_down = api_input_is_mouse_down,
    .input_is_mouse_pressed = api_input_is_mouse_pressed,
    .input_set_mouse_pos = api_input_set_mouse_pos,
    .input_is_gamepad_available = api_input_is_gamepad_available,
    .input_is_gamepad_button_down = api_input_is_gamepad_button_down,
    .input_get_gamepad_axis = api_input_get_gamepad_axis,
    
    .physics_add_rigidbody = api_physics_add_rigidbody,
    .physics_add_force = api_physics_add_force,
    .physics_add_torque = api_physics_add_torque,
    .physics_set_velocity = api_physics_set_velocity,
    .physics_set_angular_velocity = api_physics_set_angular_velocity,
    .physics_get_velocity = api_physics_get_velocity,
    .physics_get_angular_velocity = api_physics_get_angular_velocity,
    .physics_set_box_collider = api_physics_set_box_collider,
    .physics_set_sphere_collider = api_physics_set_sphere_collider,
    .physics_set_friction = api_physics_set_friction,
    .physics_set_restitution = api_physics_set_restitution,
    .physics_raycast = api_physics_raycast,
    
    .audio_play_sound = api_audio_play_sound,
    .audio_play_sound_3d = api_audio_play_sound_3d,
    .audio_set_volume = api_audio_set_volume,
    .audio_set_pitch = api_audio_set_pitch,
    .audio_set_position = api_audio_set_position,
    .audio_stop = api_audio_stop,
    .audio_is_playing = api_audio_is_playing,
    .audio_set_master_volume = api_audio_set_master_volume,
    .audio_get_master_volume = api_audio_get_master_volume,
    
    .ui_begin_window = api_ui_begin_window,
    .ui_end_window = api_ui_end_window,
    .ui_button = api_ui_button,
    .ui_checkbox = api_ui_checkbox,
    .ui_slider_float = api_ui_slider_float,
    .ui_slider_int = api_ui_slider_int,
    .ui_input_text = api_ui_input_text,
    .ui_combo = api_ui_combo,
    .ui_text = api_ui_text,
    .ui_text_colored = api_ui_text_colored,
    .ui_separator = api_ui_separator,
    .ui_spacing = api_ui_spacing,
    .ui_same_line = api_ui_same_line,
    .ui_set_next_item_width = api_ui_set_next_item_width,
    .ui_push_id = api_ui_push_id,
    .ui_pop_id = api_ui_pop_id,
    
    .asset_load = api_asset_load,
    .asset_unload = api_asset_unload,
    .asset_is_loaded = api_asset_is_loaded,
    .asset_get_path = api_asset_get_path,
    
    .plugin_get_api = api_plugin_get_api,
    .plugin_call = api_plugin_call,
    
    .fs_open = api_fs_open,
    .fs_read = api_fs_read,
    .fs_write = api_fs_write,
    .fs_close = api_fs_close,
    .fs_exists = api_fs_exists,
    .fs_size = api_fs_size,
    
    .time_now = api_time_now,
    .time_delta = api_time_delta,
    .time_fps = api_time_fps,
    
    .type_find = api_type_find,
    .type_create_instance = api_type_create_instance,
    
    .has_permission = api_has_permission,
    
    .get_project_path = api_get_project_path,
    .get_project_name = api_get_project_name,
};

} // namespace

// ============================================================================
// ENGINE INITIALIZATION (C linkage - must be outside unnamed namespace)
// ============================================================================

extern "C" {

void CP_Internal_SetEngine(void* engine) {
    g_engine = static_cast<Engine*>(engine);
}

void CP_Internal_SetScriptRuntime(void* runtime) {
    g_scriptRuntime = static_cast<ScriptRuntime*>(runtime);
}

// Provide the vtable to plugins
const CP_EngineAPI* CP_GetEngineAPI(void) {
    return &g_engine_api;
}

} // extern "C"