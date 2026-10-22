# CPluginAPI: native plugins

Native plugins are shared libraries (`.so` on Linux, `.dll` on Windows) that the editor loads into its own process.
They talk to the engine through one C header, [`include/Engine/Plugin/CPluginAPI.h`](../../include/Engine/Plugin/CPluginAPI.h)
(API version **1.2.0**). Because the interface is plain C, a plugin can be written in C or C++.

Use a plugin to extend the **editor**: add a window, a top-bar menu entry, an editor mode, or a tool that creates scene
objects. To write gameplay, use [C++ scripts](CppCoding.md) instead.

> This page was written from the source. It was not built or run. Items marked **Open question** could not be settled
> from the code and are collected at the end.

- [Quick start](#quick-start)
- [Folder layout and loading rules](#folder-layout-and-loading-rules)
- [The plugin entry](#the-plugin-entry)
- [The engine API table](#the-engine-api-table)
- [Versioning](#versioning)
- [Editor-only features (API 1.1 and 1.2)](#editor-only-features-api-11-and-12)
- [Plugins in exported games](#plugins-in-exported-games)
- [The `flyplugin` tool](#the-flyplugin-tool)
- [Internals](#internals)
- [Open questions](#open-questions)

## Quick start

The tutorial plugin lives in `assets/tutorial/HelloPlugin/`. It adds *Plugins > Hello Plugin* to the top bar, which
opens a small ImGui window.

```c
#include "CPluginAPI.h"

static const CP_EngineAPI* g_api;

static void OnMenuClick(void) { /* open your window */ }

static int OnLoad(const CP_EngineAPI* api) {
    g_api = api;
    api->ui_add_menu_item("Plugins", "Hello Plugin", OnMenuClick);
    return 0;                       /* 0 = success */
}
static void OnUnload(void) {}
static void OnUpdate(float dt) { (void)dt; /* draw UI here */ }

static CP_PluginEntry g_entry = {
    CP_API_CURRENT_VERSION,         /* api_version */
    "HelloPlugin",                  /* plugin_name */
    "1.0.0",                        /* plugin_version */
    OnLoad, OnUnload, OnUpdate,
    0,                              /* on_event */
    0,                              /* on_draw_overlay3d */
};
CP_REGISTER_PLUGIN(g_entry)
```

Build it yourself; the editor does not compile plugins:

```sh
# Linux
cc -shared -fPIC -Iinclude -o build/hello_plugin.so src/hello_plugin.c
# Windows (MinGW)
gcc -shared -Iinclude -o build/hello_plugin.dll src/hello_plugin.c
```

Copy the plugin folder into `<project>/plugins/nat/`. The editor loads it at startup and reloads it whenever the
library file changes, so you can rebuild without restarting.

## Folder layout and loading rules

```
<project>/plugins/nat/<name>/
    id.json          identity (used by the flyplugin tool)
    plugin.json      manifest (used by the flyplugin tool)
    icon.png         optional
    src/             your source (needed for export, see below)
    build/<any>.so   the built library the editor loads
```

What the editor actually does (`src/Engine/Scripts/NativeScriptHost.cpp`):

- It scans `<project>/plugins/nat/*/build/` and `<project>/plugins/lib/*/build/` and loads **every** file with the
  platform extension (`.so` or `.dll`). It does not read `id.json` or `plugin.json`.
- The library must export `CP_GetPluginEntry` (`CP_REGISTER_PLUGIN` does this) and it must not return `NULL`.
- The plugin is refused with *"Plugin API version newer than engine supports"* when its `api_version` is higher than
  the engine's.
- `on_load` must return `0`. Any other value fails the load.
- Hot reload compares file timestamps. A failed reload is not retried until the file changes again.
- `CP_PluginEntry::plugin_name` is documented as "must match id.json Name" and `plugin_version` as "must match
  id.json Version". The `flyplugin validate` tool checks identity against the manifest; the editor loader does not.

Plugins run **in the editor process with the editor's permissions and no sandbox**. Only install plugins you trust.
The `permissions` and `sandbox` fields in `plugin.json` are parsed but not enforced when the editor loads a plugin, and
`has_permission()` always returns `true` in the editor.

### `id.json`

Fields from `PluginIdentity` (`include/Engine/Plugin/PluginManifest.h`). Required: `Name`, `DateCreated`, `Publisher`,
`Version` (semver), `Type` (`nat`, `fpc` or `lib`), `CPluginAPIVersion`, `SupportedEngineVersions`. Optional:
`Description`, category, license, dependencies and cross-plugin call permissions. The HelloPlugin example:

```json
{
  "Name": "HelloPlugin",
  "DateCreated": "2026-10-19",
  "Publisher": "Flyengine",
  "Version": "1.0.0",
  "Type": "nat",
  "CPluginAPIVersion": "1.2.0",
  "SupportedEngineVersions": [">=0.0.0"],
  "Description": "The Flyengine tutorial's sample plugin."
}
```

### `plugin.json`

```json
{
  "entry_point": "hello_plugin",
  "exports": [],
  "permissions": [],
  "sandbox": { "isolate_process": false, "max_memory_mb": 256, "allow_threads": false }
}
```

`icon.png`, when present, must be a square PNG between 64x64 and 500x500 (`PluginIcon_Validate`).

## The plugin entry

`CP_PluginEntry` is what `CP_GetPluginEntry` returns.

| Field | Meaning |
| --- | --- |
| `api_version` | The API version you built against. Use `CP_API_CURRENT_VERSION`. |
| `plugin_name`, `plugin_version` | Identity strings. |
| `on_load(const CP_EngineAPI*)` | Runs once. Keep the pointer, register menus and modes here. Return `0`. |
| `on_unload()` | Shutdown, or just before a reload. |
| `on_update(float dt)` | Optional. Every frame; ImGui calls go here. |
| `on_event(const CP_Event*)` | Optional. Key, mouse, resize, entity and scene events. |
| `on_draw_overlay3d()` | Optional, added in 1.2. Editor viewport pass, outside Play. Draw with `draw_line3d` / `draw_box_outline3d`. The engine only reads this field when your `api_version` is 1.2 or newer. |

`CP_EventType` values: `KEY_DOWN`, `KEY_UP`, `MOUSE_MOVE`, `MOUSE_BUTTON`, `WINDOW_RESIZE`, `ENTITY_CREATED`,
`ENTITY_DESTROYED`, `ASSET_LOADED`, `SCENE_LOADED`, `UNDO_REDO`, and `CUSTOM` (1000 and up).

**Handles die when the scene changes.** On `CP_EVENT_SCENE_LOADED` (a project or scene was opened, or undo, redo or
stop-play restored a snapshot) and `CP_EVENT_UNDO_REDO`, every `CP_EntityID` and `CP_GroupID` you held is invalid.
Look them up again, for example with `group_find_by_tag`.

## The engine API table

`CP_EngineAPI` is a struct of function pointers passed to `on_load`. Not all of it does something yet. The table below
reflects `src/Engine/Scripts/CPluginAPI.cpp`.

| Area | Functions | Status |
| --- | --- | --- |
| Logging, memory | `log`, `alloc`, `free` | Working. `log` writes to the editor Output tab. |
| Entities | `entity_create`, `entity_destroy`, `entity_find_by_name`, `entity_find_by_tag`, `entity_get_count`, `entity_get_at` | Working. `entity_create` makes a cube the editor does **not** save. |
| Transform | `entity_get/set_position`, `_rotation` (Euler degrees), `_scale`, `_transform`, `entity_translate`, `entity_rotate` | Working. |
| Components | `entity_get_component` | Only the type `"Transform"` returns data. |
| | `entity_add_component`, `entity_remove_component` | No-ops that return `0`. |
| | `entity_has_component` | Always `false`. |
| Rendering | `mesh_*`, `material_*`, `texture_*`, `render_submit_mesh` | Not usable. `render_submit_mesh` draws nothing, and the create functions return a pointer to a local variable. Do not use them. |
| Input | `input_is_key_down/pressed/released`, `input_get_key_pressed`, `input_get_mouse_pos/delta`, `input_is_mouse_down/pressed`, `input_set_mouse_pos`, gamepad queries | Implemented. Key codes match raylib. |
| Physics | `physics_add_rigidbody`, `physics_add_force`, `physics_add_torque`, `physics_set/get_velocity`, `physics_set/get_angular_velocity`, `physics_set_box_collider`, `physics_set_sphere_collider`, `physics_set_friction`, `physics_set_restitution`, `physics_raycast` | Mostly implemented against the physics simulation, which only exists while the game is playing. `physics_add_torque`, `physics_set_box_collider` and `physics_set_sphere_collider` are no-ops. Friction, restitution and raycast were not read in detail. See [Physics](Physics.md). |
| Audio | `audio_play_sound`, `audio_play_sound_3d` | Stubs; return `0` and play nothing. |
| UI (ImGui) | `ui_begin_window`/`ui_end_window` (always pair them), `ui_button`, `ui_checkbox`, `ui_slider_float/int`, `ui_input_text`, `ui_combo`, `ui_text`, `ui_text_colored`, `ui_separator`, `ui_spacing`, `ui_same_line`, `ui_set_next_item_width`, `ui_push_id`/`ui_pop_id` | Working. |
| Assets | `asset_load`, `asset_unload`, `asset_is_loaded`, `asset_get_path` | Stubs. |
| Plugin to plugin | `plugin_get_api`, `plugin_call` | Stubs (`NULL` and `-1`). |
| Files | `fs_open`, `fs_read`, `fs_write`, `fs_close`, `fs_exists`, `fs_size` | Work on the real filesystem. The header says "sandboxed to plugin's directory", but the implementation passes your path straight to `fopen` and does no sandboxing. See [Open questions](#open-questions). |
| Time | `time_now`, `time_delta`, `time_fps` | Working. |
| Reflection | `type_find`, `type_create_instance` | Stubs; return `NULL`. |
| Permissions | `has_permission` | Always `true` in the editor. |
| Project | `get_project_path`, `get_project_name` | Working. |

Treat the stub rows as "reserved for the future". Check the status column before relying on an entry.

## Versioning

- `CP_API_VERSION_STRING` is `"1.2.0"`. `CP_API_VersionAtLeast(have, need)` compares two versions.
- Features added after 1.0 are marked in the header. **Before calling a newer function, check
  `engine_api->version`.** An older editor has a shorter table, and calling past its end is undefined behaviour.
- 1.1 added `ui_set_accent_color`.
- 1.2 added the editor camera and viewport hooks, overlay drawing, shaped entities, groups, undo, editor modes,
  Explorer group kinds, and top-bar menus (all listed below), plus `on_draw_overlay3d`.
- An engine runs a plugin built for an **equal or older** API. A plugin built for a newer API is refused.

## Editor-only features (API 1.1 and 1.2)

These do nothing in the player (`pluginhost::IsEditor()` is false there).

**Top-bar menu**: `ui_add_menu_item("Plugins", "My Tool", callback)`. The first argument names a button next to *Mode*.
All plugins that use the same name share it. Call from `on_load`.

**Theme**: `ui_set_accent_color(CP_Color)`. RGB in 0..1; hover and pressed shades are derived.

**Editor modes**: `mode_register(&CP_EditorMode{id, label, on_enter, on_exit})`. The mode appears in the top-bar mode
switcher next to *Normal*. Entering it switches to a top-down orthographic camera (WASD or arrows, or right/middle drag,
to pan; wheel to zoom) and gives the plugin the viewport: no picking, shortcuts or gizmos. Leaving restores the fly
camera. Play, plugin unload and the top bar all leave the mode. Also: `mode_enter`, `mode_exit`, `mode_is_active`.

**Camera**: `camera_enter_topdown(center, view_height)`, `camera_set_topdown_view`, `camera_exit_topdown`,
`camera_is_topdown`, `camera_get_topdown_view`, `screen_size`.

**Viewport mouse**: `mouse_ground_point(plane_y, &point)` intersects the mouse ray with a horizontal plane.
`mouse_over_ui()` is true over any editor panel. `viewport_claim(true)` stops picking, selection and gizmos so clicks
belong to the plugin; release it when your tool closes. `world_to_screen(pos)` converts to pixels.
`ui_wants_keyboard()` is true while a text or number field has focus; ignore shortcuts such as Delete then.

**Drawing**: `draw_line3d` and `draw_box_outline3d` only inside `on_draw_overlay3d`. `ui_draw_text_at(pos, text, color)`
from `on_update`.

**Scene objects that save**: `entity_create_shaped(shape, pos, size, color, name)` with `CP_SHAPE_CUBE`, `SPHERE`,
`CYLINDER` or `WEDGE`. These are real scene parts that save, undo and export, unlike `entity_create`. `entity_remove(id)`
deletes one.

**Groups**: `group_create(name, parent)`, `group_add_entity`, `group_delete(group, delete_members)`, `group_set_name`,
`group_set_locked`. A plugin can claim a group with `group_set_plugin(group, tag, data)`: both strings are saved with
the scene and with undo snapshots. Read them back with `group_get_plugin_tag` / `group_get_plugin_data` (the returned
string is valid until the next `group_get_*` call) and find groups with `group_find_by_tag(tag, index)`.

**Undo**: `push_undo()` records the scene as it is now. Call it **before** you change something.

**Explorer group kinds**: `explorer_register_group_kind(tag, icon_path, items, count)`. Groups whose plugin tag matches
show your PNG (absolute path, own colours) and your context-menu items. *Ungroup*, *New Group Inside* and *Lock* are
hidden for them, because they would break what the plugin owns. Call from `on_load`. Menu callbacks should call
`push_undo()` if they change the scene.

**Text fields**: `ui_input_float(label, &v, step)` and `ui_input_int` report a **finished** edit (Enter, leaving the
field after typing, or the +/- buttons), not each keystroke. `ui_selectable`, and `ui_item_deactivated_after_edit()` for
the last item drawn.

Modes, group kinds and menu items belong to the plugin that registered them and are removed when it unloads
(`pluginhost::RemoveModesOf`).

## Plugins in exported games

An exported game does not load `.so` files. Plugins with source are **compiled into the player**:

- Export looks for `plugins/<name>/src`, and also the older `plugins/nat/<name>` and `plugins/lib/<name>` layouts, and
  compiles all `src/` files into a relinked player. This needs the **runtime kit**; see [Distribution](Distribution.md).
- A folder that has a built library but no `src/` cannot be linked in. Export reports it as binary-only.
- Inside an exported player, `plugins/` is not copied into the data archive.
- The editor-only features above are no-ops in the player.

## The `flyplugin` tool

`flyplugin` (`src/Plugin/cli/FlyPluginCLI.c`) manages plugin folders:

| Command | Does |
| --- | --- |
| `flyplugin list [--type nat\|fpc\|lib]` | List plugins. |
| `flyplugin info <plugin-name>` | Show one plugin's identity and manifest. |
| `flyplugin rm -nat "<name>"` | Remove a plugin (`-nat`, `-fpc` or `-lib`). |
| `flyplugin validate <plugin-path>` | Check `id.json`, `plugin.json` and the icon against each other. |
| `flyplugin doctor` | Diagnose the plugin setup. |
| `flyplugin run --socket <path> --plugin <path>` | Run a plugin in a child process (`flyplugin-runner`). |

`doctor` and the exact output of the other commands were not read in detail.

## Internals

For people changing the plugin system. See also [Subsystems.md](../Subsystems.md#plugins).

| File | Role |
| --- | --- |
| `include/Engine/Plugin/CPluginAPI.h` | The ABI. Append new functions to the **end** of `CP_EngineAPI`; never reorder. |
| `src/Engine/Scripts/CPluginAPI.cpp` | Builds the `CP_EngineAPI` table (`api_*` functions). |
| `src/Engine/Scripts/NativeScriptHost.cpp` | Scans, loads, reloads and unloads plugin libraries; fires scene events. |
| `src/Engine/Frontend/PluginHost.cpp`, `include/Engine/Frontend/PluginHost.hpp` | Editor services: spawn/remove, groups, top-down camera, modes, group kinds, top menus. Installed by the editor only. |
| `src/Plugin/*.c` | Manifest parsing, loader, identity validation, IPC, version compatibility, Linux sandbox. Used by `flyplugin`. |
| `src/Engine/Export/ExportManager.cpp` | Finds plugin sources and generates the registration table for exported players. |

The sandbox (`PluginSandbox.c`) is not wired in for in-process plugins; see `BUGS.md` #11.

## Open questions

1. **`fs_*` sandbox.** The header says file access is sandboxed to the plugin's directory. The implementation is not.
   Should the doc keep describing it as unsandboxed, or is a sandbox planned?
2. **`fs_open` with `CP_FILE_BINARY`.** `api_fs_open` does `strcat` onto a string literal (`const_cast<char*>("rb")`),
   which is undefined behaviour. This looks like a bug rather than documented behaviour.
3. **Stub functions.** `audio_*` playback, `asset_*`, `plugin_call`, `type_*`, and the mesh/material/render functions
   do nothing useful. Should they be documented as "reserved", as done here, or removed from the header?
4. **`.fpc` plugins.** The manifest parser accepts `fpc` and the header mentions "FPC script plugin (editor only)", but
   the editor loader only scans `nat` and `lib`. Nothing in the code I read loads FPC plugins. Is this still planned?
