// Scene grouping: building, dissolving and re-parenting ModelGroups (the explorer's Group/Ungroup
// commands and drag-and-drop call these).
#include "ui_internal.hpp"
#include <algorithm>
#include <memory>

namespace ui {


// Helpers that dissolve or build model containers. These live here (rather
// than in the interaction manager) because the explorer context menu triggers
// them, and the models vector is registered through SetSceneModels.
static void UnlinkObjectFromModel(ScatteredObject* obj) {
    if (!obj || !obj->parentModel) return;
    auto& members = obj->parentModel->members;
    members.erase(std::remove(members.begin(), members.end(), obj), members.end());
    obj->parentModel = nullptr;
}

// Places `g` under `newParent` (nullptr = root) right before/after `ref` (a sibling under that same parent).
// Root order is the order of g_sceneModels, nested order is ModelGroup::children.
static void PlaceGroup(ModelGroup* g, ModelGroup* newParent, ModelGroup* ref, bool after) {
    if (g->parent) g->parent->RemoveChild(g);
    g->parent = newParent;
    if (newParent) {
        auto& kids = newParent->children;
        auto it = ref ? std::find(kids.begin(), kids.end(), ref) : kids.end();
        if (it != kids.end() && after) ++it;
        kids.insert(it, g);
        return;
    }
    if (!g_sceneModels || !ref) return;
    // Root: reorder inside g_sceneModels.
    auto& v = *g_sceneModels;
    auto self = std::find_if(v.begin(), v.end(), [&](const std::unique_ptr<ModelGroup>& m) { return m.get() == g; });
    if (self == v.end()) return;
    std::unique_ptr<ModelGroup> owned = std::move(*self);
    v.erase(self);
    auto at = std::find_if(v.begin(), v.end(), [&](const std::unique_ptr<ModelGroup>& m) { return m.get() == ref; });
    if (at != v.end() && after) ++at;
    v.insert(at, std::move(owned));
}

// Helpers that dissolve or build model containers. These live here (rather
// than in the interaction manager) because the explorer context menu triggers
// them, and the models vector is registered through SetSceneModels.
ModelGroup* CreateEmptyGroup(ModelGroup* parent) {
    if (!g_sceneModels) return nullptr;
    auto model = std::make_unique<ModelGroup>();
    model->name = "Group";
    ModelGroup* raw = model.get();
    g_sceneModels->push_back(std::move(model));
    if (parent) parent->AddChild(raw);
    return raw;
}

void GroupSelectedObjects() {
    if (!g_sceneModels || !g_sceneObjects) return;
    if (g_selection.size() < 2) return;

    // The new group goes where the selection already lives when everything shares one parent.
    ModelGroup* common = g_selection.front() ? g_selection.front()->parentModel : nullptr;
    for (auto* obj : g_selection) {
        if (obj && obj->parentModel != common) { common = nullptr; break; }
    }

    ModelGroup* model = CreateEmptyGroup(common);
    if (!model) return;
    model->name = "Model";
    for (auto* obj : g_selection) {
        if (!obj) continue;
        // A selected object already inside a model: pull it out first so
        // members never end up owned by two containers.
        UnlinkObjectFromModel(obj);
        model->members.push_back(obj);
        obj->parentModel = model;
    }
}

// Dissolves `model`: its objects and sub-groups move up to its parent (or the root).
void UngroupModel(ModelGroup* model) {
    if (!g_sceneModels || !model) return;
    ModelGroup* up = model->parent;
    for (auto* m : model->members) {
        if (!m) continue;
        m->parentModel = up;
        if (up) up->members.push_back(m);
    }
    model->members.clear();
    std::vector<ModelGroup*> kids = model->children;
    for (auto* c : kids) {
        model->RemoveChild(c);
        if (up) up->AddChild(c);
    }
    if (up) up->RemoveChild(model);
    g_sceneModels->erase(std::remove_if(g_sceneModels->begin(), g_sceneModels->end(),
        [&](const std::unique_ptr<ModelGroup>& m) { return m.get() == model; }), g_sceneModels->end());
}

bool CanGroupSelection() { return g_sceneModels && g_selection.size() >= 2; }

bool UngroupSelection(bool dryRun) {
    if (!g_sceneModels) return false;
    auto fully = [](const ModelGroup* g) {
        std::vector<ScatteredObject*> all;
        g->CollectObjects(all);
        if (all.empty()) return false;
        for (auto* o : all) if (!o->isSelected) return false;
        return true;
    };
    // Outermost fully selected groups.
    std::vector<ModelGroup*> targets;
    for (auto* o : g_selection) {
        if (!o) continue;
        ModelGroup* best = nullptr;
        for (ModelGroup* g = o->parentModel; g; g = g->parent) if (fully(g)) best = g;
        if (best && std::find(targets.begin(), targets.end(), best) == targets.end()) targets.push_back(best);
    }
    if (targets.empty() && g_primarySelection && g_primarySelection->parentModel)
        targets.push_back(g_primarySelection->parentModel);
    if (targets.empty()) return false;
    if (dryRun) return true;
    for (auto* g : targets) UngroupModel(g);
    return true;
}

void UngroupModelContaining(ScatteredObject* member) {
    if (!member || !member->parentModel) return;
    UngroupModel(member->parentModel);
}

// Removes `model` and every group below it. Returns the objects they held so the caller can delete them.
std::vector<ScatteredObject*> DissolveModelTree(ModelGroup* model) {
    std::vector<ScatteredObject*> objs;
    if (!g_sceneModels || !model) return objs;
    model->CollectObjects(objs);
    std::vector<ModelGroup*> doomed;
    std::vector<ModelGroup*> stack{ model };
    while (!stack.empty()) {
        ModelGroup* g = stack.back();
        stack.pop_back();
        doomed.push_back(g);
        for (auto* c : g->children) stack.push_back(c);
    }
    for (auto* o : objs) o->parentModel = nullptr;
    if (model->parent) model->parent->RemoveChild(model);
    g_sceneModels->erase(std::remove_if(g_sceneModels->begin(), g_sceneModels->end(),
        [&](const std::unique_ptr<ModelGroup>& m) { return std::find(doomed.begin(), doomed.end(), m.get()) != doomed.end(); }),
        g_sceneModels->end());
    return objs;
}

// Moves explorer-dragged objects into `target` (nullptr = scene root), before/after `ref` when it is
// one of target's members.
void MoveObjectsToGroup(const std::vector<ScatteredObject*>& dragged, ModelGroup* target,
                        ScatteredObject* ref, bool after) {
    for (auto* obj : dragged) UnlinkObjectFromModel(obj);
    if (!target) return;
    auto& members = target->members;
    auto it = ref ? std::find(members.begin(), members.end(), ref) : members.end();
    if (it != members.end() && after) ++it;
    for (auto* obj : dragged) {
        if (!obj) continue;
        it = members.insert(it, obj);
        ++it;
        obj->parentModel = target;
    }
}

// Re-parents a group (and everything inside it). Returns false for a drop that would put a group inside itself.
bool MoveGroupTo(ModelGroup* g, ModelGroup* newParent, ModelGroup* ref, bool after) {
    if (!g) return false;
    if (newParent == g || (newParent && newParent->IsDescendantOf(g))) return false;
    PlaceGroup(g, newParent, ref, after);
    return true;
}

} // namespace ui
