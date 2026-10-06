/*
 * CPluginAPI.h - C ABI Plugin API for FlyEngine
 *
 * This is the stable C interface that all native plugins (nat/, lib/) must use.
 * Plugins include this header and implement the CP_PluginEntry structure.
 *
 * Version: 1.0.0
 */

#ifndef CPLUGIN_API_H
#define CPLUGIN_API_H

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#else
#include <stdint.h>
#include <stddef.h>
#endif

// ============================================================================
// VERSIONING
// ============================================================================

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} CP_API_Version;

#define CP_API_CURRENT_VERSION {1, 0, 0}
#define CP_API_VERSION_STRING "1.0.0"

// Version comparison helpers
static inline int CP_API_VersionCompare(CP_API_Version a, CP_API_Version b) {
    if (a.major != b.major) return (a.major > b.major) ? 1 : -1;
    if (a.minor != b.minor) return (a.minor > b.minor) ? 1 : -1;
    if (a.patch != b.patch) return (a.patch > b.patch) ? 1 : -1;
    return 0;
}

static inline int CP_API_VersionAtLeast(CP_API_Version have, CP_API_Version need) {
    return CP_API_VersionCompare(have, need) >= 0;
}

// ============================================================================
// BASIC TYPES
// ============================================================================

// Opaque handles
typedef struct CP_EngineAPI CP_EngineAPI;
typedef struct CP_PluginEntry CP_PluginEntry;
typedef uint64_t CP_EntityID;
typedef uint64_t CP_AssetHandle;
typedef uint64_t CP_TextureHandle;
typedef uint64_t CP_MeshHandle;
typedef uint64_t CP_MaterialHandle;
typedef uint64_t CP_FileHandle;

// Vector/Math types
typedef struct { float x, y, z; } CP_Vec3;
typedef struct { float x, y; } CP_Vec2;
typedef struct { float x, y, z, w; } CP_Quat;
typedef struct { float r, g, b, a; } CP_Color;

// Transform
typedef struct {
    CP_Vec3 position;
    CP_Vec3 rotation;  // Euler degrees
    CP_Vec3 scale;
} CP_Transform;

// Log levels
typedef enum {
    CP_LOG_DEBUG = 0,
    CP_LOG_INFO  = 1,
    CP_LOG_WARN  = 2,
    CP_LOG_ERROR = 3,
} CP_LogLevel;

// File modes
typedef enum {
    CP_FILE_READ   = 1 << 0,
    CP_FILE_WRITE  = 1 << 1,
    CP_FILE_APPEND = 1 << 2,
    CP_FILE_BINARY = 1 << 3,
} CP_FileMode;

// Key codes (match raylib)
typedef enum {
    CP_KEY_SPACE = 32, CP_KEY_APOSTROPHE = 39, CP_KEY_COMMA = 44, CP_KEY_MINUS = 45,
    CP_KEY_PERIOD = 46, CP_KEY_SLASH = 47, CP_KEY_ZERO = 48, CP_KEY_ONE = 49,
    CP_KEY_TWO = 50, CP_KEY_THREE = 51, CP_KEY_FOUR = 52, CP_KEY_FIVE = 53,
    CP_KEY_SIX = 54, CP_KEY_SEVEN = 55, CP_KEY_EIGHT = 56, CP_KEY_NINE = 57,
    CP_KEY_SEMICOLON = 59, CP_KEY_EQUAL = 61, CP_KEY_A = 65, CP_KEY_B = 66,
    CP_KEY_C = 67, CP_KEY_D = 68, CP_KEY_E = 69, CP_KEY_F = 70, CP_KEY_G = 71,
    CP_KEY_H = 72, CP_KEY_I = 73, CP_KEY_J = 74, CP_KEY_K = 75, CP_KEY_L = 76,
    CP_KEY_M = 77, CP_KEY_N = 78, CP_KEY_O = 79, CP_KEY_P = 80, CP_KEY_Q = 81,
    CP_KEY_R = 82, CP_KEY_S = 83, CP_KEY_T = 84, CP_KEY_U = 85, CP_KEY_V = 86,
    CP_KEY_W = 87, CP_KEY_X = 88, CP_KEY_Y = 89, CP_KEY_Z = 90,
    CP_KEY_LEFT_BRACKET = 91, CP_KEY_BACKSLASH = 92, CP_KEY_RIGHT_BRACKET = 93,
    CP_KEY_GRAVE = 96, CP_KEY_ESCAPE = 256, CP_KEY_ENTER = 257, CP_KEY_TAB = 258,
    CP_KEY_BACKSPACE = 259, CP_KEY_INSERT = 260, CP_KEY_DELETE = 261, CP_KEY_RIGHT = 262,
    CP_KEY_LEFT = 263, CP_KEY_DOWN = 264, CP_KEY_UP = 265, CP_KEY_PAGE_UP = 266,
    CP_KEY_PAGE_DOWN = 267, CP_KEY_HOME = 268, CP_KEY_END = 269,
    CP_KEY_CAPS_LOCK = 280, CP_KEY_SCROLL_LOCK = 281, CP_KEY_NUM_LOCK = 282,
    CP_KEY_PRINT_SCREEN = 283, CP_KEY_PAUSE = 284, CP_KEY_F1 = 290, CP_KEY_F2 = 291,
    CP_KEY_F3 = 292, CP_KEY_F4 = 293, CP_KEY_F5 = 294, CP_KEY_F6 = 295,
    CP_KEY_F7 = 296, CP_KEY_F8 = 297, CP_KEY_F9 = 298, CP_KEY_F10 = 299,
    CP_KEY_F11 = 300, CP_KEY_F12 = 301,
    CP_KEY_LEFT_SHIFT = 340, CP_KEY_LEFT_CONTROL = 341, CP_KEY_LEFT_ALT = 342,
    CP_KEY_LEFT_SUPER = 343, CP_KEY_RIGHT_SHIFT = 344, CP_KEY_RIGHT_CONTROL = 345,
    CP_KEY_RIGHT_ALT = 346, CP_KEY_RIGHT_SUPER = 347, CP_KEY_KB_MENU = 348,
} CP_KeyCode;

// Event types
typedef enum {
    CP_EVENT_NONE = 0,
    CP_EVENT_KEY_DOWN,
    CP_EVENT_KEY_UP,
    CP_EVENT_MOUSE_MOVE,
    CP_EVENT_MOUSE_BUTTON,
    CP_EVENT_WINDOW_RESIZE,
    CP_EVENT_ENTITY_CREATED,
    CP_EVENT_ENTITY_DESTROYED,
    CP_EVENT_ASSET_LOADED,
    CP_EVENT_CUSTOM = 1000,
} CP_EventType;

typedef struct {
    CP_EventType type;
    uint64_t timestamp;
    union {
        struct { CP_KeyCode key; bool repeat; } key;
        struct { CP_Vec2 pos; CP_Vec2 delta; } mouse;
        struct { uint32_t width; uint32_t height; } window;
        struct { CP_EntityID entity; } entity;
        struct { const char* name; void* data; } custom;
    } data;
} CP_Event;

// Type info for reflection
typedef struct CP_TypeInfo CP_TypeInfo;
struct CP_TypeInfo {
    const char* name;
    size_t size;
    uint32_t alignment;
    const CP_TypeInfo* base_type;
    // Fields would be here in a full implementation
};

// Permissions (bitmask)
typedef enum {
    CP_PERM_NONE       = 0,
    CP_PERM_MEMORY     = 1 << 0,  // Custom allocators
    CP_PERM_GPU        = 1 << 1,  // Direct GPU commands
    CP_PERM_INPUT      = 1 << 2,  // Raw input access
    CP_PERM_FILESYSTEM = 1 << 3,  // Sandboxed FS access
    CP_PERM_NETWORK    = 1 << 4,  // Network (future)
    CP_PERM_THREADS    = 1 << 5,  // Thread creation
    CP_PERM_AUDIO      = 1 << 6,  // Audio engine access
    CP_PERM_PHYSICS    = 1 << 7,  // Physics engine access
    CP_PERM_EDITOR     = 1 << 8,  // Editor-only APIs
    CP_PERM_SCENE_GRAPH= 1 << 9,  // Full scene manipulation
} CP_Permission;

// Export macro for plugin entry point
#if defined(_WIN32)
    #define CP_EXPORT __declspec(dllexport)
#else
    #define CP_EXPORT __attribute__((visibility("default")))
#endif

// ============================================================================
// PLUGIN ENTRY POINT (implemented by plugin)
// ============================================================================

struct CP_PluginEntry {
    CP_API_Version api_version;      // Version of CPluginAPI this plugin was built against
    const char* plugin_name;         // Must match id.json Name
    const char* plugin_version;      // Must match id.json Version

    // Lifecycle callbacks
    int (*on_load)(const CP_EngineAPI* engine_api);   // Return 0 on success
    void (*on_unload)(void);

    // Optional per-frame update
    void (*on_update)(float delta_time);

    // Optional event handling
    void (*on_event)(const CP_Event* event);
};

// Macro to register plugin entry point
#define CP_REGISTER_PLUGIN(entry_struct) \
    CP_EXPORT CP_PluginEntry* CP_GetPluginEntry(void) { \
        return &entry_struct; \
    }

// Raycast hit structure
typedef struct {
    bool hit;
    CP_Vec3 point;
    CP_Vec3 normal;
    CP_EntityID entity;
    float distance;
} CP_RaycastHit;

// ============================================================================
// ENGINE API (provided to plugins via vtable)
// ============================================================================

struct CP_EngineAPI {
    CP_API_Version version;          // Engine's CPluginAPI version

    // --------------------------------------------------------
    // Memory Management
    // --------------------------------------------------------
    void* (*alloc)(size_t size, const char* tag);
    void (*free)(void* ptr);

    // --------------------------------------------------------
    // Logging
    // --------------------------------------------------------
    void (*log)(CP_LogLevel level, const char* tag, const char* fmt, ...);

    // --------------------------------------------------------
    // Entity/Component System
    // --------------------------------------------------------
    CP_EntityID (*entity_create)(const char* name);
    void (*entity_destroy)(CP_EntityID id);
    CP_EntityID (*entity_find_by_name)(const char* name);
    CP_EntityID (*entity_find_by_tag)(const char* tag);
    int (*entity_get_count)(void);
    CP_EntityID (*entity_get_at)(int index);

    // Transform
    CP_Vec3 (*entity_get_position)(CP_EntityID id);
    void (*entity_set_position)(CP_EntityID id, CP_Vec3 pos);
    CP_Vec3 (*entity_get_rotation)(CP_EntityID id);
    void (*entity_set_rotation)(CP_EntityID id, CP_Vec3 euler);
    CP_Vec3 (*entity_get_scale)(CP_EntityID id);
    void (*entity_set_scale)(CP_EntityID id, CP_Vec3 scale);
    CP_Transform (*entity_get_transform)(CP_EntityID id);
    void (*entity_set_transform)(CP_EntityID id, const CP_Transform* transform);
    void (*entity_translate)(CP_EntityID id, CP_Vec3 offset);
    void (*entity_rotate)(CP_EntityID id, CP_Vec3 euler_delta);

    // Components (generic by type name string)
    void* (*entity_get_component)(CP_EntityID id, const char* component_type);
    int (*entity_add_component)(CP_EntityID id, const char* type, void* data);
    int (*entity_remove_component)(CP_EntityID id, const char* type);
    bool (*entity_has_component)(CP_EntityID id, const char* type);

    // --------------------------------------------------------
    // Rendering
    // --------------------------------------------------------
    CP_MeshHandle (*mesh_create_cube)(float size);
    CP_MeshHandle (*mesh_create_sphere)(float radius);
    CP_MeshHandle (*mesh_create_cylinder)(float radius, float height);
    CP_MeshHandle (*mesh_create_plane)(float width, float length);
    CP_MeshHandle (*mesh_load)(const char* path);
    void (*mesh_destroy)(CP_MeshHandle handle);

    CP_MaterialHandle (*material_create)(const char* name);
    void (*material_destroy)(CP_MaterialHandle handle);
    void (*material_set_color)(CP_MaterialHandle handle, CP_Color color);
    void (*material_set_texture)(CP_MaterialHandle handle, CP_TextureHandle texture);

    CP_TextureHandle (*texture_load)(const char* path);
    void (*texture_destroy)(CP_TextureHandle handle);

    void (*render_submit_mesh)(CP_MeshHandle mesh, CP_MaterialHandle material, const CP_Transform* transform);

    // --------------------------------------------------------
    // Input
    // --------------------------------------------------------
    bool (*input_is_key_down)(CP_KeyCode key);
    bool (*input_is_key_pressed)(CP_KeyCode key);
    bool (*input_is_key_released)(CP_KeyCode key);
    int (*input_get_key_pressed)(void);
    CP_Vec2 (*input_get_mouse_pos)(void);
    CP_Vec2 (*input_get_mouse_delta)(void);
    bool (*input_is_mouse_down)(int button);
    bool (*input_is_mouse_pressed)(int button);
    void (*input_set_mouse_pos)(int x, int y);
    bool (*input_is_gamepad_available)(int gamepad);
    bool (*input_is_gamepad_button_down)(int gamepad, int button);
    float (*input_get_gamepad_axis)(int gamepad, int axis);

    // --------------------------------------------------------
    // Physics
    // --------------------------------------------------------
    CP_EntityID (*physics_add_rigidbody)(CP_EntityID entity, float mass);
    void (*physics_add_force)(CP_EntityID entity, CP_Vec3 force);
    void (*physics_add_torque)(CP_EntityID entity, CP_Vec3 torque);
    void (*physics_set_velocity)(CP_EntityID entity, CP_Vec3 vel);
    void (*physics_set_angular_velocity)(CP_EntityID entity, CP_Vec3 vel);
    CP_Vec3 (*physics_get_velocity)(CP_EntityID entity);
    CP_Vec3 (*physics_get_angular_velocity)(CP_EntityID entity);
    void (*physics_set_box_collider)(CP_EntityID entity, CP_Vec3 half_extents);
    void (*physics_set_sphere_collider)(CP_EntityID entity, float radius);
    void (*physics_set_friction)(CP_EntityID entity, float friction);
    void (*physics_set_restitution)(CP_EntityID entity, float restitution);
    CP_RaycastHit (*physics_raycast)(CP_Vec3 origin, CP_Vec3 direction, float max_distance);

    // --------------------------------------------------------
    // Audio
    // --------------------------------------------------------
    CP_EntityID (*audio_play_sound)(const char* path, float volume, float pitch, bool loop);
    CP_EntityID (*audio_play_sound_3d)(const char* path, CP_Vec3 pos, float vol, float pitch, bool loop, float min_dist, float max_dist);
    void (*audio_set_volume)(CP_EntityID sound, float volume);
    void (*audio_set_pitch)(CP_EntityID sound, float pitch);
    void (*audio_set_position)(CP_EntityID sound, CP_Vec3 pos);
    void (*audio_stop)(CP_EntityID sound);
    bool (*audio_is_playing)(CP_EntityID sound);
    void (*audio_set_master_volume)(float volume);
    float (*audio_get_master_volume)(void);

    // --------------------------------------------------------
    // UI (ImGui wrapper)
    // --------------------------------------------------------
    bool (*ui_begin_window)(const char* name, CP_Vec2 pos, CP_Vec2 size, bool* open);
    void (*ui_end_window)(void);
    bool (*ui_button)(const char* label, CP_Vec2 size);
    bool (*ui_checkbox)(const char* label, bool* value);
    bool (*ui_slider_float)(const char* label, float* value, float min, float max);
    bool (*ui_slider_int)(const char* label, int* value, int min, int max);
    bool (*ui_input_text)(const char* label, char* buffer, int buffer_size);
    bool (*ui_combo)(const char* label, int* current, const char* const* items, int count);
    void (*ui_text)(const char* text);
    void (*ui_text_colored)(const char* text, CP_Color color);
    void (*ui_separator)(void);
    void (*ui_spacing)(void);
    void (*ui_same_line)(void);
    void (*ui_set_next_item_width)(float width);
    void (*ui_push_id)(const char* id);
    void (*ui_pop_id)(void);

    // --------------------------------------------------------
    // Asset System
    // --------------------------------------------------------
    CP_AssetHandle (*asset_load)(const char* path, int type);
    void (*asset_unload)(CP_AssetHandle handle);
    bool (*asset_is_loaded)(CP_AssetHandle handle);
    const char* (*asset_get_path)(CP_AssetHandle handle);

    // --------------------------------------------------------
    // Plugin Communication
    // --------------------------------------------------------
    void* (*plugin_get_api)(const char* plugin_name, const char* api_name);
    int (*plugin_call)(const char* plugin_name, const char* func_name, void* args, void* result);

    // --------------------------------------------------------
    // File System (Sandboxed to plugin's directory)
    // --------------------------------------------------------
    CP_FileHandle (*fs_open)(const char* path, CP_FileMode mode);
    size_t (*fs_read)(CP_FileHandle handle, void* buffer, size_t size);
    size_t (*fs_write)(CP_FileHandle handle, const void* buffer, size_t size);
    void (*fs_close)(CP_FileHandle handle);
    bool (*fs_exists)(const char* path);
    int64_t (*fs_size)(const char* path);

    // --------------------------------------------------------
    // Time
    // --------------------------------------------------------
    double (*time_now)(void);
    float (*time_delta)(void);
    float (*time_fps)(void);

    // --------------------------------------------------------
    // Reflection / Type System
    // --------------------------------------------------------
    const CP_TypeInfo* (*type_find)(const char* name);
    void* (*type_create_instance)(const CP_TypeInfo* type);

    // --------------------------------------------------------
    // Permission Check
    // --------------------------------------------------------
    bool (*has_permission)(CP_Permission perm);

    // --------------------------------------------------------
    // Project Info
    // --------------------------------------------------------
    const char* (*get_project_path)(void);
    const char* (*get_project_name)(void);
};

// ============================================================================
// ENGINE-TO-PLUGIN HELPERS (internal, not part of C ABI)
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Called by engine to set the global engine pointer for the C API implementation
void CP_Internal_SetEngine(void* engine);

// Called by engine to set script runtime pointer
void CP_Internal_SetScriptRuntime(void* runtime);

// Get the engine API vtable for plugins
const CP_EngineAPI* CP_GetEngineAPI(void);

#ifdef __cplusplus
}
#endif

#endif // CPLUGIN_API_H