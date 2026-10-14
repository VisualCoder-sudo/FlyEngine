#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
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
    else if (type == NodeType::Sequence) n.value = "2";
    else if (type == NodeType::Delay) n.value = "1";
    else if (type == NodeType::Repeat) n.value = "3";
    else if (type == NodeType::Print) n.value = "Hello";
    else if (type == NodeType::SetVariable) { n.target = "flag"; n.value = "true"; }
    else if (type == NodeType::Branch) n.target = "flag";
    else if (type == NodeType::GetVariable) n.target = "flag";
    else if (type == NodeType::Compare) { n.target = "0"; n.value = "0"; n.prop = "=="; }
    else if (type == NodeType::And || type == NodeType::Or || type == NodeType::Not) { n.target = "false"; n.value = "false"; }
    else if (type >= NodeType::Add && type <= NodeType::Div) { n.target = "0"; n.value = "0"; }
    else if (type == NodeType::Random) { n.target = "0"; n.value = "1"; }
    else if (type == NodeType::Lerp) { n.target = "0"; n.value = "1"; n.prop = "0.5"; }
    else if (type == NodeType::GetProperty) n.prop = "Position";
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
    g.dataLinks.erase(std::remove_if(g.dataLinks.begin(), g.dataLinks.end(), [&](const DataLink& l) { return l.from == id || l.to == id; }), g.dataLinks.end());
}

namespace {
bool IsEvent(NodeType t) { return t == NodeType::Start || t == NodeType::OnUpdate || t == NodeType::OnStop; }
}

bool IsPure(NodeType t) { return t >= NodeType::GetVariable && t < NodeType::Count_; }

int DataInputCount(NodeType t) {
    switch (t) {
        case NodeType::Branch: case NodeType::SetVariable: case NodeType::Print: case NodeType::Delay:
        case NodeType::Change: case NodeType::Not: return 1;
        case NodeType::Compare: case NodeType::And: case NodeType::Or: case NodeType::Add: case NodeType::Sub:
        case NodeType::Mul: case NodeType::Div: case NodeType::Random: return 2;
        case NodeType::Lerp: return 3;
        default: return 0;
    }
}

const char* DataInputLabel(NodeType t, int pin) {
    switch (t) {
        case NodeType::Branch: return "Condition";
        case NodeType::SetVariable: case NodeType::Change: return "Value";
        case NodeType::Print: return "Text";
        case NodeType::Delay: return "Seconds";
        case NodeType::Random: return pin == 0 ? "Min" : "Max";
        case NodeType::Lerp: return pin == 0 ? "A" : pin == 1 ? "B" : "T";
        case NodeType::Not: return "In";
        default: return pin == 0 ? "A" : "B";
    }
}

int OutputCount(NodeType t, const Node* n) {
    if (IsPure(t)) return 0;
    switch (t) {
        case NodeType::End: return 0;
        case NodeType::Branch: case NodeType::Repeat: return 2;
        case NodeType::Sequence: return n ? std::clamp(std::atoi(n->value.c_str()), 1, 8) : 2;
        default: return 1;
    }
}

const char* OutputLabel(const Node& n, int pin) {
    static const char* const kSeq[] = { "Then 0", "Then 1", "Then 2", "Then 3", "Then 4", "Then 5", "Then 6", "Then 7" };
    switch (n.type) {
        case NodeType::Branch: return pin == 0 ? "True" : "False";
        case NodeType::Repeat: return pin == 0 ? "Loop" : "Done";
        case NodeType::Sequence: return kSeq[std::clamp(pin, 0, 7)];
        default: return "";
    }
}

void Connect(int from, int to, int fromPin) {
    if (from == to) return;
    Node* a = Find(from); Node* b = Find(to);
    if (!a || !b || IsPure(a->type) || IsPure(b->type) || IsEvent(b->type) || fromPin < 0 || fromPin >= OutputCount(a->type, a)) return;
    auto& L = Active().links;
    L.erase(std::remove_if(L.begin(), L.end(), [&](const Link& l) { return (l.from == from && l.fromPin == fromPin) || l.to == to; }), L.end());
    L.push_back({ from, to, fromPin });
}

void ConnectData(int from, int to, int toPin) {
    if (from == to) return;
    Node* a = Find(from); Node* b = Find(to);
    if (!a || !b || !IsPure(a->type) || toPin < 0 || toPin >= DataInputCount(b->type)) return;
    auto& L = Active().dataLinks;
    L.erase(std::remove_if(L.begin(), L.end(), [&](const DataLink& l) { return l.to == to && l.toPin == toPin; }), L.end());
    L.push_back({ from, to, toPin });
}

const char* TypeName(NodeType t) {
    switch (t) {
        case NodeType::Start: return "Start";
        case NodeType::Change: return "Change";
        case NodeType::End: return "End";
        case NodeType::OnUpdate: return "On Update";
        case NodeType::OnStop: return "On Stop";
        case NodeType::Branch: return "Branch";
        case NodeType::Sequence: return "Sequence";
        case NodeType::Delay: return "Delay";
        case NodeType::Repeat: return "Repeat";
        case NodeType::DoOnce: return "Do Once";
        case NodeType::Print: return "Print";
        case NodeType::SetVariable: return "Set Variable";
        case NodeType::GetVariable: return "Get Variable";
        case NodeType::Compare: return "Compare";
        case NodeType::And: return "And";
        case NodeType::Or: return "Or";
        case NodeType::Not: return "Not";
        case NodeType::Add: return "Add";
        case NodeType::Sub: return "Subtract";
        case NodeType::Mul: return "Multiply";
        case NodeType::Div: return "Divide";
        case NodeType::Random: return "Random Range";
        case NodeType::Lerp: return "Lerp";
        case NodeType::GetProperty: return "Get Property";
        default: break;
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
    if (!g.dataLinks.empty()) {
        out << "VDLINKS " << g.dataLinks.size() << "\n";
        for (const DataLink& l : g.dataLinks) out << "VDLINK " << l.from << ' ' << l.to << ' ' << l.toPin << "\n";
    }
    for (const Link& l : g.links) {   // pin-0 wires keep the original line, so old readers still parse them
        if (l.fromPin == 0) out << "VLINK " << l.from << ' ' << l.to << "\n";
        else out << "VLINKP " << l.from << ' ' << l.to << ' ' << l.fromPin << "\n";
    }
}

bool LoadGraph(std::istream& in, Graph& g) {
    std::string tag;
    int next = 1; size_t nn = 0, nl = 0;
    if (!(in >> tag >> next >> nn >> nl) || tag != "VSCRIPT" || nn > 100000 || nl > 100000) return false;
    g.nextId = std::max(next, 1);
    for (size_t i = 0; i < nn; ++i) {
        Node n; int t = 0;
        if (!(in >> tag >> n.id >> t >> n.x >> n.y >> std::quoted(n.target) >> std::quoted(n.prop) >> std::quoted(n.value)) || tag != "VNODE") return false;
        n.type = static_cast<NodeType>(std::clamp(t, 0, static_cast<int>(NodeType::Count_) - 1));
        g.nextId = std::max(g.nextId, n.id + 1);
        g.nodes.push_back(n);
    }
    {   // optional value wires, written only when the graph has any
        const auto mark = in.tellg();
        size_t nd = 0;
        if (in >> tag && tag == "VDLINKS" && in >> nd && nd <= 100000) {
            for (size_t i = 0; i < nd; ++i) {
                DataLink d;
                if (!(in >> tag >> d.from >> d.to >> d.toPin) || tag != "VDLINK") return false;
                g.dataLinks.push_back(d);
            }
        } else { in.clear(); in.seekg(mark); }
    }
    for (size_t i = 0; i < nl; ++i) {
        Link l;
        if (!(in >> tag >> l.from >> l.to) || (tag != "VLINK" && tag != "VLINKP")) return false;
        if (tag == "VLINKP" && !(in >> l.fromPin)) return false;
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

namespace {
// A Delay waiting to continue: graph index, the Delay node, seconds left.
struct Pending { int graph; int node; float left; };
std::vector<Pending> g_delays;
std::map<std::string, std::string> g_vars;       // Set Variable / Branch
std::set<std::pair<int, int>> g_once;            // (graph, node) Do Once nodes already passed

bool Truthy(const std::string& v) { return !v.empty() && v != "0" && v != "false"; }

// "Workspace/Group/Cube#2": the second object named Cube.
ScatteredObject* FindTarget(ScriptRuntime& rt, const std::string& path) {
    std::string name = LastSegment(path);
    int occurrence = 1;
    const size_t hash = name.find_last_of('#');
    if (hash != std::string::npos && hash + 1 < name.size() &&
        name.find_first_not_of("0123456789", hash + 1) == std::string::npos) {
        occurrence = std::max(1, std::atoi(name.c_str() + hash + 1));
        name.erase(hash);
    }
    return rt.FindByNameNth(name, occurrence);
}

bool ToNumber(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0';
}

std::string NumStr(double v) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}

std::string Eval(ScriptRuntime& rt, int gi, const Node& n, int depth);

// Value on input pin `pin` of `n`: the wired value node's result, else the pin's literal.
std::string Input(ScriptRuntime& rt, int gi, const Node& n, int pin, int depth) {
    const Graph& g = g_scripts[gi];
    for (const DataLink& d : g.dataLinks)
        if (d.to == n.id && d.toPin == pin) {
            const Node* src = FindIn(g, d.from);
            return src ? Eval(rt, gi, *src, depth + 1) : std::string();
        }
    if (IsPure(n.type)) return pin == 0 ? n.target : pin == 1 ? n.value : n.prop;
    if (n.type == NodeType::Branch) {   // unwired Branch tests the variable named in its field
        const auto it = g_vars.find(n.target);
        return it != g_vars.end() ? it->second : std::string();
    }
    return n.value;
}

std::string Eval(ScriptRuntime& rt, int gi, const Node& n, int depth) {
    if (depth > 64) { ui::Log("[visual script] Value wires loop back on themselves"); return {}; }
    auto in = [&](int pin) { return Input(rt, gi, n, pin, depth); };
    auto num = [&](int pin) { double v = 0.0; ToNumber(in(pin), v); return v; };
    switch (n.type) {
        case NodeType::GetVariable: {
            const auto it = g_vars.find(n.target);
            return it != g_vars.end() ? it->second : std::string();
        }
        case NodeType::Compare: {
            const std::string a = in(0), b = in(1);
            double x = 0.0, y = 0.0;
            if (ToNumber(a, x) && ToNumber(b, y)) {
                const std::string& op = n.prop;
                const bool r = op == "==" ? x == y : op == "!=" ? x != y : op == "<" ? x < y :
                               op == ">" ? x > y : op == "<=" ? x <= y : x >= y;
                return r ? "true" : "false";
            }
            if (n.prop == "!=") return a != b ? "true" : "false";
            return n.prop == "==" && a == b ? "true" : "false";   // non-numbers only support equality
        }
        case NodeType::And: return Truthy(in(0)) && Truthy(in(1)) ? "true" : "false";
        case NodeType::Or: return Truthy(in(0)) || Truthy(in(1)) ? "true" : "false";
        case NodeType::Not: return Truthy(in(0)) ? "false" : "true";
        case NodeType::Add: return NumStr(num(0) + num(1));
        case NodeType::Sub: return NumStr(num(0) - num(1));
        case NodeType::Mul: return NumStr(num(0) * num(1));
        case NodeType::Div: { const double d = num(1); return NumStr(d == 0.0 ? 0.0 : num(0) / d); }
        case NodeType::Random: {
            static std::mt19937 rng{ std::random_device{}() };
            double lo = num(0), hi = num(1);
            if (lo > hi) std::swap(lo, hi);
            return NumStr(lo == hi ? lo : std::uniform_real_distribution<double>(lo, hi)(rng));
        }
        case NodeType::Lerp: { const double a = num(0); return NumStr(a + (num(1) - a) * num(2)); }
        case NodeType::GetProperty: {
            ScatteredObject* obj = FindTarget(rt, n.target);
            std::string out;
            if (!obj) ui::Log("[visual script] No object '%s'", n.target.c_str());
            else if (!rt.GetObjectProperty(obj, n.prop, out)) ui::Log("[visual script] Can't read '%s'", n.prop.c_str());
            return out;
        }
        default: return {};
    }
}

void ApplyChange(ScriptRuntime& rt, const Node& n) {
    if (n.target.empty() || n.prop.empty()) return;
    std::string result;
    if (IsWorldGroup(n.target)) {
        if (!console::RunLine("game." + n.target + "." + n.prop + " = " + n.value, &result))
            ui::Log("[visual script] %s", result.c_str());
        return;
    }
    ScatteredObject* obj = FindTarget(rt, n.target);
    std::string error;
    if (!obj) ui::Log("[visual script] No object '%s'", n.target.c_str());
    else if (!rt.SetObjectProperty(obj, n.prop, n.value, error)) ui::Log("[visual script] %s", error.c_str());
}

// Follows the wire leaving (`fromId`, `pin`) and everything after it. Branching nodes push several continuations;
// the step cap stops cyclic graphs.
void Emit(ScriptRuntime& rt, int gi, int fromId, int pin) {
    constexpr int kMaxSteps = 100000;
    std::vector<std::pair<int, int>> stack{ { fromId, pin } };
    int steps = 0;
    while (!stack.empty()) {
        if (++steps > kMaxSteps) { ui::Log("[visual script] Stopped: graph ran over %d steps (loop?)", kMaxSteps); return; }
        if (gi < 0 || gi >= static_cast<int>(g_scripts.size())) return;
        const Graph& g = g_scripts[gi];
        const auto [id, p] = stack.back();
        stack.pop_back();
        const Link* link = nullptr;
        for (const Link& l : g.links) if (l.from == id && l.fromPin == p) { link = &l; break; }
        if (!link) continue;
        const Node* n = FindIn(g, link->to);
        if (!n) continue;
        switch (n->type) {
            case NodeType::Change: {
                Node applied = *n;
                applied.value = Input(rt, gi, *n, 0, 0);
                ApplyChange(rt, applied);
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::Print: ui::Log("[visual script] %s", Input(rt, gi, *n, 0, 0).c_str()); stack.push_back({ n->id, 0 }); break;
            case NodeType::SetVariable:
                if (!n->target.empty()) g_vars[n->target] = Input(rt, gi, *n, 0, 0);
                stack.push_back({ n->id, 0 });
                break;
            case NodeType::Branch:
                stack.push_back({ n->id, Truthy(Input(rt, gi, *n, 0, 0)) ? 0 : 1 });
                break;
            case NodeType::Sequence: {
                const int count = std::clamp(std::atoi(n->value.c_str()), 1, 8);
                for (int i = count - 1; i >= 0; --i) stack.push_back({ n->id, i });
                break;
            }
            case NodeType::Delay:
                g_delays.push_back({ gi, n->id, std::max(0.0f, static_cast<float>(std::atof(Input(rt, gi, *n, 0, 0).c_str()))) });
                break;
            case NodeType::Repeat: {
                const int count = std::clamp(std::atoi(n->value.c_str()), 0, 1000);
                stack.push_back({ n->id, 1 });   // Done runs after every body pass
                for (int i = 0; i < count; ++i) stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::DoOnce:
                if (g_once.insert({ gi, n->id }).second) stack.push_back({ n->id, 0 });
                break;
            default: break;   // End, and event nodes (they have no input)
        }
    }
}

void FireEvents(ScriptRuntime& rt, NodeType type) {
    for (int gi = 0; gi < static_cast<int>(g_scripts.size()); ++gi) {
        std::vector<int> ids;
        for (const Node& n : g_scripts[gi].nodes) if (n.type == type) ids.push_back(n.id);
        for (int id : ids) Emit(rt, gi, id, 0);
    }
}
}

void Run(ScriptRuntime& rt) {
    g_delays.clear(); g_vars.clear(); g_once.clear();
    FireEvents(rt, NodeType::Start);
}

void Tick(ScriptRuntime& rt, float dt) {
    std::vector<Pending> due;
    for (auto it = g_delays.begin(); it != g_delays.end();) {
        it->left -= dt;
        if (it->left <= 0.0f) { due.push_back(*it); it = g_delays.erase(it); }
        else ++it;
    }
    for (const Pending& d : due) Emit(rt, d.graph, d.node, 0);
    FireEvents(rt, NodeType::OnUpdate);
}

void Stop(ScriptRuntime& rt) {
    FireEvents(rt, NodeType::OnStop);
    g_delays.clear(); g_vars.clear(); g_once.clear();
}

} // namespace vscript
