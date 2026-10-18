#include "../../../include/Engine/Frontend/PluginHost.hpp"
#include "ui_internal.hpp"
#include <algorithm>

namespace pluginhost {

namespace {
SceneHost* g_host = nullptr;
bool g_viewportClaimed = false;
std::function<void(SceneEvent)> g_sink;
}

void SetSceneHost(SceneHost* host) { g_host = host; }
SceneHost* GetSceneHost() { return g_host; }
bool IsEditor() { return g_host != nullptr; }

bool IsLiveGroup(const ModelGroup* group) {
    if (!g_host || !g_host->models || !group) return false;
    for (const auto& m : *g_host->models)
        if (m.get() == group) return true;
    return false;
}

ModelGroup* CreateGroup(const std::string& name, ModelGroup* parent) {
    if (!g_host) return nullptr;
    if (parent && !IsLiveGroup(parent)) return nullptr;
    ModelGroup* group = ui::CreateEmptyGroup(parent);
    if (group) group->name = name.empty() ? "Group" : name;
    return group;
}

void AddObjectToGroup(ModelGroup* group, ScatteredObject* object) {
    if (!g_host || !object || !IsLiveGroup(group)) return;
    ui::MoveObjectsToGroup({ object }, group, nullptr, false);
}

void DeleteGroup(ModelGroup* group, bool deleteMembers) {
    if (!g_host || !IsLiveGroup(group)) return;
    std::vector<ScatteredObject*> objects = ui::DissolveModelTree(group);
    if (!deleteMembers || !g_host->remove) return;
    for (auto* obj : objects) g_host->remove(obj);
}

std::vector<ModelGroup*> FindGroupsByTag(const std::string& tag) {
    std::vector<ModelGroup*> out;
    if (!g_host || !g_host->models || tag.empty()) return out;
    for (const auto& m : *g_host->models)
        if (m && m->pluginTag == tag) out.push_back(m.get());
    return out;
}

void SetViewportClaimed(bool claimed) { g_viewportClaimed = claimed; }
bool IsViewportClaimed() { return g_viewportClaimed; }

namespace {
std::vector<EditorMode> g_modes;
std::string g_activeMode;
std::string g_pendingMode;
bool g_modePending = false;
const void* g_loadingOwner = nullptr;
std::vector<GroupKind> g_groupKinds;
std::vector<TopMenuItem> g_topMenuItems;
}

void SetLoadingOwner(const void* owner) { g_loadingOwner = owner; }

void RegisterMode(const std::string& id, const std::string& label, void (*onEnter)(void), void (*onExit)(void)) {
    if (id.empty()) return;
    for (auto& m : g_modes) {
        if (m.id == id) {   // a hot-reloaded plugin registers again: replace
            m.label = label.empty() ? id : label;
            m.onEnter = onEnter; m.onExit = onExit; m.owner = g_loadingOwner;
            return;
        }
    }
    EditorMode m;
    m.id = id;
    m.label = label.empty() ? id : label;
    m.onEnter = onEnter;
    m.onExit = onExit;
    m.owner = g_loadingOwner;
    g_modes.push_back(std::move(m));
}

const std::vector<EditorMode>& Modes() { return g_modes; }
const std::string& ActiveModeId() { return g_activeMode; }

void LeaveMode() {
    if (g_activeMode.empty()) return;
    const std::string id = g_activeMode;
    g_activeMode.clear();
    for (auto& m : g_modes)
        if (m.id == id && m.onExit) m.onExit();
    if (g_host && g_host->exitTopDown) g_host->exitTopDown();
    g_viewportClaimed = false;
}

void RegisterGroupKind(const std::string& tag, const std::string& iconPath, std::vector<GroupKindItem> items) {
    if (tag.empty()) return;
    for (auto& k : g_groupKinds) {
        if (k.tag == tag) {   // a hot-reloaded plugin registers again: replace
            k.iconPath = iconPath; k.items = std::move(items); k.owner = g_loadingOwner;
            return;
        }
    }
    g_groupKinds.push_back(GroupKind{ tag, iconPath, std::move(items), g_loadingOwner });
}

const GroupKind* FindGroupKind(const std::string& tag) {
    if (tag.empty()) return nullptr;
    for (const auto& k : g_groupKinds)
        if (k.tag == tag) return &k;
    return nullptr;
}

void AddTopMenuItem(const std::string& menu, const std::string& label, std::function<void()> run) {
    if (menu.empty() || label.empty() || !run) return;
    g_topMenuItems.push_back(TopMenuItem{ menu, label, std::move(run), g_loadingOwner });
}

const std::vector<TopMenuItem>& TopMenuItems() { return g_topMenuItems; }

void RemoveModesOf(const void* owner) {
    for (auto& m : g_modes) {
        if (m.owner == owner && m.id == g_activeMode) { LeaveMode(); break; }
    }
    g_groupKinds.erase(std::remove_if(g_groupKinds.begin(), g_groupKinds.end(), [&](const GroupKind& k) { return k.owner == owner; }),
                       g_groupKinds.end());
    g_topMenuItems.erase(std::remove_if(g_topMenuItems.begin(), g_topMenuItems.end(), [&](const TopMenuItem& t) { return t.owner == owner; }),
                         g_topMenuItems.end());
    g_modes.erase(std::remove_if(g_modes.begin(), g_modes.end(), [&](const EditorMode& m) { return m.owner == owner; }),
                  g_modes.end());
}

void RequestMode(const std::string& id) {
    g_pendingMode = id;
    g_modePending = true;
}

void ProcessModeRequest() {
    if (!g_modePending) return;
    g_modePending = false;
    const std::string id = g_pendingMode;
    if (id == g_activeMode) return;
    LeaveMode();
    if (id.empty() || !g_host) return;
    for (auto& m : g_modes) {
        if (m.id != id) continue;
        g_activeMode = id;
        g_viewportClaimed = true;
        if (g_host->enterTopDownFromView) g_host->enterTopDownFromView();
        if (m.onEnter) m.onEnter();
        return;
    }
}

void SetEventSink(std::function<void(SceneEvent)> sink) { g_sink = std::move(sink); }
void Notify(SceneEvent event) { if (g_sink) g_sink(event); }

} // namespace pluginhost
