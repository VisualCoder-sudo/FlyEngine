/*
 * CPluginAPI.h - C ABI Plugin API for FlyEngine
 *
 * This is the stable C interface that all native plugins (nat/, lib/) must use.
 * Plugins include this header and implement the CP_PluginEntry structure.
 *
 * Version: 1.2.0 (1.1: ui_set_accent_color; 1.2: editor camera and viewport mouse hooks)
 */

#ifndef CPLUGIN_API_H
#define CPLUGIN_API_H

#ifdef __cplusplus
#include <cstdint>
#include <cstddef>
#else
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#endif

// ============================================================================
// VERSIONING
// ============================================================================

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t patch;
} CP_API_Version;

#define CP_API_CURRENT_VERSION {1, 2, 0}
#define CP_API_VERSION_STRING "1.2.0"

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
typedef uint64_t CP_GroupID;    // 1.2: an Explorer group (0 = none)
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

// An editor mode a plugin offers (1.2). Strings must outlive the plugin.
typedef struct {
    const char* id;               // unique, e.g. "planster"
    const char* label;            // shown in the top bar
    void (*on_enter)(void);       // optional
    void (*on_exit)(void);        // optional
} CP_EditorMode;

// A context-menu entry for an Explorer group kind (1.2). `label` must outlive the plugin.
typedef struct {
    const char* label;
    void (*on_click)(CP_GroupID group);
} CP_GroupMenuItem;

// Part shapes for entity_create_shaped (1.2). Every shape fits a 1x1x1 box
// scaled by its size.
typedef enum {
    CP_SHAPE_CUBE = 0,
    CP_SHAPE_SPHERE,
    CP_SHAPE_CYLINDER,
    CP_SHAPE_WEDGE,
} CP_Shape;

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
    // 1.2: the scene was replaced under the plugin (project/scene opened, or
    // undo/redo/stop-play restored a snapshot). Entity and group handles from
    // before the event are invalid; look them up again (group_find_by_tag).
    CP_EVENT_SCENE_LOADED,
    CP_EVENT_UNDO_REDO,
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

// Permissions (bitmask). Declarative only today: the editor loads plugins
// in-process with its own permissions and does not enforce these.
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

    // Added in 1.2 (the engine reads these only when api_version >= 1.2).
    // Editor viewport pass, inside the 3D camera and outside Play. Draw with
    // draw_line3d / draw_box_outline3d.
    void (*on_draw_overlay3d)(void);
};

// Macro to register plugin entry point. C linkage, so a C++ plugin exports the
// plain symbol the loader looks up instead of a mangled name.
#ifdef __cplusplus
#define CP_REGISTER_PLUGIN(entry_struct) \
    extern "C" CP_EXPORT CP_PluginEntry* CP_GetPluginEntry(void) { \
        return &entry_struct; \
    }
#else
#define CP_REGISTER_PLUGIN(entry_struct) \
    CP_EXPORT CP_PluginEntry* CP_GetPluginEntry(void) { \
        return &entry_struct; \
    }
#endif

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

    // --------------------------------------------------------
    // Editor theme (added in 1.1; check engine_api->version before calling)
    // --------------------------------------------------------
    // Sets the editor's accent colour (r,g,b in 0..1; alpha ignored). Hover and
    // pressed shades are derived. Editor only: has no effect in the player.
    void (*ui_set_accent_color)(CP_Color color);

    // --------------------------------------------------------
    // Editor camera and viewport mouse (added in 1.2; check engine_api->version
    // before calling). Editor only: no-ops in the player.
    // --------------------------------------------------------
    // Top-down orthographic view looking straight down at `center`.
    // `view_height` is the world-space height visible on screen. While active
    // the fly controls are suspended; pan and zoom by calling set_topdown_view.
    void (*camera_enter_topdown)(CP_Vec3 center, float view_height);
    void (*camera_set_topdown_view)(CP_Vec3 center, float view_height);
    // Restores the camera that was active before camera_enter_topdown.
    void (*camera_exit_topdown)(void);
    bool (*camera_is_topdown)(void);

    // Intersects the mouse ray with the horizontal plane at `plane_y`.
    // Returns false when the ray misses the plane.
    bool (*mouse_ground_point)(float plane_y, CP_Vec3* out_point);
    // True when the mouse is over an editor panel or ImGui window.
    bool (*mouse_over_ui)(void);
    // While claimed, the editor does no picking, selection or gizmo work, so
    // viewport clicks belong to the plugin. Release it when the tool closes.
    void (*viewport_claim)(bool claimed);
    // World position to screen pixels (same space as input_get_mouse_pos).
    CP_Vec2 (*world_to_screen)(CP_Vec3 pos);

    // Overlay drawing: call only from on_draw_overlay3d.
    void (*draw_line3d)(CP_Vec3 a, CP_Vec3 b, CP_Color color);
    void (*draw_box_outline3d)(CP_Vec3 center, CP_Vec3 size, CP_Color color);
    // Text at a world position, drawn over the viewport. Call from on_update
    // (any time the ImGui frame is open).
    void (*ui_draw_text_at)(CP_Vec3 pos, const char* text, CP_Color color);

    // Scene objects. entity_create (above) makes an object the editor does not
    // save; these make real scene parts that save, undo and export.
    CP_EntityID (*entity_create_shaped)(CP_Shape shape, CP_Vec3 pos, CP_Vec3 size, CP_Color color, const char* name);
    // Deletes an object made by entity_create_shaped (or found in the scene).
    void (*entity_remove)(CP_EntityID id);

    // Groups. Handles die on CP_EVENT_SCENE_LOADED / CP_EVENT_UNDO_REDO.
    CP_GroupID (*group_create)(const char* name, CP_GroupID parent);
    void (*group_add_entity)(CP_GroupID group, CP_EntityID entity);
    // Removes the group with everything nested in it. With delete_members the
    // parts are deleted too, otherwise they stay as loose objects.
    void (*group_delete)(CP_GroupID group, bool delete_members);
    void (*group_set_name)(CP_GroupID group, const char* name);
    void (*group_set_locked)(CP_GroupID group, bool locked);
    // Marks the group as owned by a plugin and stores the plugin's editable
    // source data on it. Both are saved with the scene and with undo
    // snapshots. An empty tag makes it an ordinary group again.
    void (*group_set_plugin)(CP_GroupID group, const char* tag, const char* data);
    // Returned strings stay valid until the next group_get_* call.
    const char* (*group_get_plugin_tag)(CP_GroupID group);
    const char* (*group_get_plugin_data)(CP_GroupID group);
    // The index-th group (from 0) carrying `tag`, or 0.
    CP_GroupID (*group_find_by_tag)(const char* tag, int index);

    // Records an undo step of the scene as it is NOW. Call before changing it.
    void (*push_undo)(void);

    // Editor modes. Register one from on_load; the editor lists it in the top
    // bar mode switcher. Entering a mode switches to the top-down map camera
    // (WASD/arrows or right/middle-drag pan, wheel zoom) and gives the plugin
    // the viewport: no picking, shortcuts or gizmos. Leaving restores the fly
    // camera. Play, plugin unload and the top bar all leave the mode.
    // on_enter / on_exit run inside those transitions.
    void (*mode_register)(const CP_EditorMode* mode);
    void (*mode_enter)(const char* id);
    void (*mode_exit)(void);
    bool (*mode_is_active)(const char* id);

    // Map view state and screen, for hit radii and grid drawing. Returns false
    // when the top-down camera is not active.
    bool (*camera_get_topdown_view)(CP_Vec3* center, float* view_height);
    CP_Vec2 (*screen_size)(void);

    // True while a text or number field has keyboard focus: ignore shortcuts
    // such as Delete.
    bool (*ui_wants_keyboard)(void);
    // Number fields that report a finished edit, not every keystroke: they
    // return true when the user pressed Enter, left the field after typing, or
    // clicked the +/- buttons. *value changes only then.
    bool (*ui_input_float)(const char* label, float* value, float step);
    bool (*ui_input_int)(const char* label, int* value);
    bool (*ui_selectable)(const char* label, bool selected);
    // For the item just drawn (ui_input_text, ...): true on the frame the user
    // finished editing it.
    bool (*ui_item_deactivated_after_edit)(void);

    // Explorer group kind. Groups whose plugin tag equals `tag` show the PNG at
    // `icon_path` (absolute, own colours) and the given context-menu items;
    // Ungroup, New Group Inside and Lock are hidden for them. Call from on_load.
    // Menu callbacks should call push_undo() themselves if they change the scene.
    void (*explorer_register_group_kind)(const char* tag, const char* icon_path, const CP_GroupMenuItem* items, int count);
    // Adds an entry to a top-bar menu. `menu` ("Plugins") becomes a button next
    // to Mode, shared by every plugin that uses the same name. Call from on_load.
    void (*ui_add_menu_item)(const char* menu, const char* label, void (*on_click)(void));
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

// Called by the host when the scene is replaced (load, undo/redo): drops every
// entity and group handle handed out so far.
void CP_Internal_InvalidateHandles(void);

// Get the engine API vtable for plugins
const CP_EngineAPI* CP_GetEngineAPI(void);

#ifdef __cplusplus
}
#endif

#endif // CPLUGIN_API_H