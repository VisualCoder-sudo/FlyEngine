#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include <algorithm>
#include <iomanip>
#include <set>

namespace vscript {

namespace {
std::vector<Graph> g_scripts;
int g_current = -1;
Graph g_scratch;   // what Active() hands out while there are no scripts

bool IsWorldGroup(const std::string& t) {
    const auto& g = WorldGroups();
    return std::find(g.begin(), g.end(), t) != g.end();
}

// "Workspace/Group1/Cube" -> the object's name (the console finds objects by name).
const Node* FindIn(const Graph& g, int id) {
    for (const auto& n : g.nodes) if (n.id == id) return &n;
    return nullptr;
}

std::string LastSegment(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}
}

Graph& Active() {
    if (g_current < 0 || g_current >= static_cast<int>(g_scripts.size())) return g_scratch;
    return g_scripts[g_current];
}

void Clear() { g_scripts.clear(); g_current = -1; g_scratch = Graph{}; }

int Count() { return static_cast<int>(g_scripts.size()); }
int CurrentIndex() { return g_current; }
const std::string& NameAt(int i) { return g_scripts[i].name; }
namespace { unsigned g_useCounter = 0; }

void Select(int i) {
    if (i < 0 || i >= Count()) return;
    g_current = i;
    g_scripts[i].lastUsed = ++g_useCounter;
}

std::vector<int> RecentOrder() {
    std::vector<int> order(Count());
    for (int i = 0; i < Count(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [](int a, int b) { return g_scripts[a].lastUsed > g_scripts[b].lastUsed; });
    return order;
}

namespace {
std::string UniqueName(std::string base) {
    base = scriptLauncher::MakeClassName(base.empty() ? "Script" : base);   // graph names are class names, like the scripts they belong to
    auto taken = [&](const std::string& n) {
        for (const auto& g : g_scripts) if (g.name == n) return true;
        return false;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        std::string n = base + std::to_string(i);
        if (!taken(n)) return n;
    }
}
}

int NewScript(const std::string& name) {
    Graph g;
    g.name = UniqueName(name.empty() ? "Script" : name);
    g_scripts.push_back(std::move(g));
    Select(Count() - 1);
    return g_current;
}

int OpenByName(const std::string& name) {
    for (int i = 0; i < Count(); ++i)
        if (g_scripts[i].name == name) { Select(i); return i; }
    return NewScript(name);
}

void RenameCurrent(const std::string& name) {
    if (g_current < 0 || name.empty()) return;
    g_scripts[g_current].name = "";
    g_scripts[g_current].name = UniqueName(name);
}

bool Has(const std::string& name) {
    for (const auto& g : g_scripts) if (g.name == name) return true;
    return false;
}

void RemoveByName(const std::string& name) {
    for (int i = 0; i < Count(); ++i)
        if (g_scripts[i].name == name) {
            g_scripts.erase(g_scripts.begin() + i);
            if (g_current >= Count()) g_current = Count() - 1;
            return;
        }
}

void RenameKey(const std::string& from, const std::string& to) {
    if (from == to || Has(to)) return;
    for (auto& g : g_scripts) if (g.name == from) { g.name = to; return; }
}

void RemoveCurrent() {
    if (g_current < 0) return;
    g_scripts.erase(g_scripts.begin() + g_current);
    g_current = g_scripts.empty() ? -1 : std::min(g_current, Count() - 1);
}

Node& AddNode(NodeType type, float x, float y) {
    Node n;
    if (g_scripts.empty()) NewScript();
    Graph& g2 = Active();
    n.id = g2.nextId++;
    n.type = type;
    n.x = x; n.y = y;
    if (type == NodeType::Change) { n.prop = "Color"; n.value = "(255, 0, 0)"; }
    g2.nodes.push_back(n);
    return g2.nodes.back();
}

Node* Find(int id) {
    for (auto& n : Active().nodes) if (n.id == id) return &n;
    return nullptr;
}

void RemoveNode(int id) {
    auto& g = Active();
    g.nodes.erase(std::remove_if(g.nodes.begin(), g.nodes.end(), [&](const Node& n) { return n.id == id; }), g.nodes.end());
    g.links.erase(std::remove_if(g.links.begin(), g.links.end(), [&](const Link& l) { return l.from == id || l.to == id; }), g.links.end());
}

void Connect(int from, int to) {
    if (from == to) return;
    Node* a = Find(from); Node* b = Find(to);
    if (!a || !b || a->type == NodeType::End || b->type == NodeType::Start) return;
    auto& L = Active().links;
    L.erase(std::remove_if(L.begin(), L.end(), [&](const Link& l) { return l.from == from || l.to == to; }), L.end());
    L.push_back({ from, to });
}

const char* TypeName(NodeType t) {
    switch (t) {
        case NodeType::Start: return "Start";
        case NodeType::Change: return "Change";
        case NodeType::End: return "End";
    }
    return "?";
}

const std::vector<std::string>& ObjectProperties() {
    static const std::vector<std::string> p = { "Color", "Position", "Size", "Rotation", "Transparency", "Anchored", "CanCollide", "Mass" };
    return p;
}
const std::vector<std::string>& WorldGroups() {
    static const std::vector<std::string> g = { "Lighting", "Environment", "Rendering", "Camera", "Physics" };
    return g;
}
std::vector<std::string> WorldProperties(const std::string& group) {
    if (group == "Lighting") return { "GlobalShadows", "ShadowQuality", "Ambient" };
    if (group == "Environment") return { "TimeOfDay", "DayLength", "Overcast", "Rain", "WetGround", "Fog", "CloudCover",
                                         "WindSpeed", "WindDirection", "Exposure", "SunIntensity", "Bloom" };
    if (group == "Rendering") return { "Grid", "Wireframe" };
    if (group == "Camera") return { "FOV" };
    if (group == "Physics") return { "Gravity", "Friction", "Restitution" };
    return {};
}

namespace {
void SaveGraph(std::ostream& out, const Graph& g) {
    out << "VSCRIPT " << g.nextId << ' ' << g.nodes.size() << ' ' << g.links.size() << "\n";
    for (const Node& n : g.nodes)
        out << "VNODE " << n.id << ' ' << static_cast<int>(n.type) << ' ' << n.x << ' ' << n.y << ' '
            << std::quoted(n.target) << ' ' << std::quoted(n.prop) << ' ' << std::quoted(n.value) << "\n";
    for (const Link& l : g.links) out << "VLINK " << l.from << ' ' << l.to << "\n";
}

bool LoadGraph(std::istream& in, Graph& g) {
    std::string tag;
    int next = 1; size_t nn = 0, nl = 0;
    if (!(in >> tag >> next >> nn >> nl) || tag != "VSCRIPT" || nn > 100000 || nl > 100000) return false;
    g.nextId = std::max(next, 1);
    for (size_t i = 0; i < nn; ++i) {
        Node n; int t = 0;
        if (!(in >> tag >> n.id >> t >> n.x >> n.y >> std::quoted(n.target) >> std::quoted(n.prop) >> std::quoted(n.value)) || tag != "VNODE") return false;
        n.type = static_cast<NodeType>(std::clamp(t, 0, 2));
        g.nextId = std::max(g.nextId, n.id + 1);
        g.nodes.push_back(n);
    }
    for (size_t i = 0; i < nl; ++i) {
        Link l;
        if (!(in >> tag >> l.from >> l.to) || tag != "VLINK") return false;
        g.links.push_back(l);
    }
    return true;
}
}

void Save(std::ostream& out) {
    out << "VSCRIPTS " << g_scripts.size() << "\n";
    for (const Graph& g : g_scripts) {
        out << "VGRAPH " << std::quoted(g.name) << "\n";
        SaveGraph(out, g);
    }
}

void Load(std::istream& in) {
    const auto mark = in.tellg();
    std::string tag;
    if (!(in >> tag)) { in.clear(); in.seekg(mark); return; }
    if (tag == "VSCRIPT") {          // scenes saved with the single-graph format
        in.clear(); in.seekg(mark);
        Graph g;
        if (LoadGraph(in, g)) { g.name = scriptLauncher::MakeClassName(g.name); g_scripts.push_back(std::move(g)); g_current = 0; }
        else { in.clear(); in.seekg(mark); }
        return;
    }
    size_t count = 0;
    if (tag != "VSCRIPTS" || !(in >> count) || count > 10000) { in.clear(); in.seekg(mark); return; }
    std::vector<Graph> loaded;
    for (size_t i = 0; i < count; ++i) {
        Graph g;
        if (!(in >> tag >> std::quoted(g.name)) || tag != "VGRAPH" || !LoadGraph(in, g)) return;
        g.name = scriptLauncher::MakeClassName(g.name);
        loaded.push_back(std::move(g));
    }
    g_scripts = std::move(loaded);
    g_current = g_scripts.empty() ? -1 : 0;
}

void Run(ScriptRuntime& rt) {
  for (Graph& g : g_scripts) {
    for (const Node& start : g.nodes) {
        if (start.type != NodeType::Start) continue;
        std::set<int> seen;
        int cur = start.id;
        while (seen.insert(cur).second) {
            const Link* next = nullptr;
            for (const Link& l : g.links) if (l.from == cur) { next = &l; break; }
            if (!next) break;
            const Node* n = FindIn(g, next->to);
            if (!n || n->type == NodeType::End) break;
            if (n->type == NodeType::Change && !n->target.empty() && !n->prop.empty()) {
                std::string result;
                if (IsWorldGroup(n->target)) {
                    if (!console::RunLine("game." + n->target + "." + n->prop + " = " + n->value, &result))
                        ui::Log("[visual script] %s", result.c_str());
                } else {
                    // "Workspace/Group/Cube#2": the second object named Cube. Applied straight to that object.
                    std::string name = LastSegment(n->target);
                    int occurrence = 1;
                    const size_t hash = name.find_last_of('#');
                    if (hash != std::string::npos && hash + 1 < name.size() &&
                        name.find_first_not_of("0123456789", hash + 1) == std::string::npos) {
                        occurrence = std::max(1, std::atoi(name.c_str() + hash + 1));
                        name.erase(hash);
                    }
                    ScatteredObject* obj = rt.FindByNameNth(name, occurrence);
                    std::string error;
                    if (!obj) ui::Log("[visual script] No object '%s'", n->target.c_str());
                    else if (!rt.SetObjectProperty(obj, n->prop, n->value, error)) ui::Log("[visual script] %s", error.c_str());
                }
            }
            cur = n->id;
        }
    }
  }
}

} // namespace vscript
