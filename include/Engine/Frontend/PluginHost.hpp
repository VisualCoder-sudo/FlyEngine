#pragma once
#include "../Backend/ScatteredObject.hpp"
#include "raylib.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// Editor-side services the plugin API (CPluginAPI.cpp) needs but cannot reach on its own: spawning and removing scene
// objects, building groups, the top-down camera, editor modes, and telling plugins that the scene was swapped under them.
// The editor installs them; the player never does, so the plugin scene hooks do nothing there.
namespace pluginhost {

struct SceneHost {
    std::function<ScatteredObject*(ShapeType, Vector3 pos, Vector3 size, Color, const std::string& name)> spawn;
    // Deletes an object from the scene (also from the selection).
    std::function<void(ScatteredObject*)> remove;
    std::vector<std::unique_ptr<ModelGroup>>* models = nullptr;

    // Top-down orthographic camera (CameraController). enterTopDownFromView picks a centre and zoom from where the
    // fly camera is looking, so entering a mode lands on what the user was looking at.
    std::function<void(Vector3 center, float viewHeight)> enterTopDown;
    std::function<void()> enterTopDownFromView;
    std::function<void(Vector3 center, float viewHeight)> setTopDown;
    std::function<void()> exitTopDown;
    std::function<bool()> isTopDown;
    std::function<bool(Vector3& center, float& viewHeight)> getTopDown;
};
void SetSceneHost(SceneHost* host);
SceneHost* GetSceneHost();
bool IsEditor();   // true when a SceneHost is installed

// Groups. All no-ops/null without a SceneHost.
ModelGroup* CreateGroup(const std::string& name, ModelGroup* parent);
bool IsLiveGroup(const ModelGroup* group);
void AddObjectToGroup(ModelGroup* group, ScatteredObject* object);
// Removes the group and everything nested in it; the objects are deleted from the scene when `deleteMembers`,
// otherwise they stay as loose objects.
void DeleteGroup(ModelGroup* group, bool deleteMembers);
std::vector<ModelGroup*> FindGroupsByTag(const std::string& tag);

// A plugin tool that owns the viewport. While claimed the interaction manager does no picking, selection, shortcuts or
// gizmo work, so clicks reach the plugin only. Modes (below) claim it for you.
void SetViewportClaimed(bool claimed);
bool IsViewportClaimed();

// Editor modes. A plugin registers a mode (e.g. "Planster"); the top bar lists it next to "Normal". Entering a mode
// switches to the top-down map camera and claims the viewport; leaving restores the fly camera. The plugin only
// supplies what happens inside (onEnter/onExit).
struct EditorMode {
    std::string id;
    std::string label;
    void (*onEnter)(void) = nullptr;
    void (*onExit)(void) = nullptr;
    const void* owner = nullptr;   // the plugin that registered it
};
void SetLoadingOwner(const void* owner);          // the plugin whose on_load is running: owner of new modes
void RegisterMode(const std::string& id, const std::string& label, void (*onEnter)(void), void (*onExit)(void));
void RemoveModesOf(const void* owner);            // plugin unloading: leaves its mode if active, drops its modes, group kinds and menu items
const std::vector<EditorMode>& Modes();
const std::string& ActiveModeId();                // "" = normal editing
void RequestMode(const std::string& id);          // "" = back to normal; applied by ProcessModeRequest
void ProcessModeRequest();                        // once per editor frame (ObjectInteractionManager)
void LeaveMode();                                 // immediately

// Explorer group kinds. A group whose pluginTag matches a registered kind shows the plugin's icon and the plugin's
// context-menu items (instead of Ungroup / New Group Inside / Lock, which would break what the plugin owns).
struct GroupKindItem {
    std::string label;
    std::function<void(ModelGroup*)> run;
};
struct GroupKind {
    std::string tag;
    std::string iconPath;   // absolute path to a PNG, drawn in its own colours
    std::vector<GroupKindItem> items;
    const void* owner = nullptr;
};
void RegisterGroupKind(const std::string& tag, const std::string& iconPath, std::vector<GroupKindItem> items);
const GroupKind* FindGroupKind(const std::string& tag);

// Top-bar menus. Each distinct `menu` name becomes a button (e.g. "Plugins") with its items listed in a popup.
struct TopMenuItem {
    std::string menu;
    std::string label;
    std::function<void()> run;
    const void* owner = nullptr;
};
void AddTopMenuItem(const std::string& menu, const std::string& label, std::function<void()> run);
const std::vector<TopMenuItem>& TopMenuItems();

// The scene was replaced under the plugins. Handles they hold are no longer valid.
enum class SceneEvent { Loaded, UndoRedo };
void SetEventSink(std::function<void(SceneEvent)> sink);
void Notify(SceneEvent event);

} // namespace pluginhost
