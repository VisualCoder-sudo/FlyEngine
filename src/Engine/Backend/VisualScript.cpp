#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/GameUI.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include <algorithm>
#include <iomanip>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <set>

namespace vscript {

bool IsUiTarget(const std::string& t) { return t.size() > 3 && t.compare(0, 3, "UI/") == 0; }
std::string UiTargetId(const std::string& t) { return IsUiTarget(t) ? t.substr(3) : std::string(); }

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
            if (g_current > i) --g_current;
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
    else if (type == NodeType::Raycast) { n.target = "(0, 5, 0)"; n.value = "(0, -1, 0)"; n.prop = "100"; }
    else if (type == NodeType::CreateObject) n.prop = "Cube";
    else if (type == NodeType::SetVelocity || type == NodeType::AddForce || type == NodeType::AddImpulse) n.value = "(0, 5, 0)";
    else if (type == NodeType::Teleport) n.value = "(0, 5, 0)";
    else if (type == NodeType::OnKey) n.prop = "Space";
    else if (type == NodeType::PlaySound) n.value = "1";   // the sound options default to off (see SoundOption)
    else if (type == NodeType::SetVolume) n.value = "1";
    else if (type == NodeType::ShowText) { n.value = "Hello"; n.prop = "Top Center"; n.target = "3"; }
    else if (type == NodeType::UIText) { n.target = "label"; n.value = "Hello"; n.prop = "Top Left"; SoundOptionRef(n, "offset", "(20, 20)"); SoundOptionRef(n, "size", "28"); SoundOptionRef(n, "color", "(255, 255, 255)"); SoundOptionRef(n, "font", "Sans"); }
    else if (type == NodeType::UIPanel) { n.target = "panel"; n.prop = "Top Left"; SoundOptionRef(n, "offset", "(20, 20)"); SoundOptionRef(n, "size", "(200, 60)"); SoundOptionRef(n, "color", "(0, 0, 0, 140)"); }
    else if (type == NodeType::UIBar) { n.target = "bar"; n.value = "1"; n.prop = "Bottom Left"; SoundOptionRef(n, "offset", "(20, -20)"); SoundOptionRef(n, "size", "(240, 22)"); SoundOptionRef(n, "color", "(80, 200, 90)"); SoundOptionRef(n, "background", "(0, 0, 0, 160)"); }
    else if (type == NodeType::UIImage) { n.target = "image"; n.prop = "Top Left"; SoundOptionRef(n, "offset", "(20, 20)"); SoundOptionRef(n, "size", "(0, 0)"); SoundOptionRef(n, "color", "(255, 255, 255)"); }
    else if (type == NodeType::UISet) { n.target = "label"; n.prop = "Text"; n.value = "Hello"; }
    else if (type == NodeType::UIRemove) n.target = "label";
    else if (type == NodeType::UIGet) { n.target = "label"; n.prop = "Text"; }
    else if (type == NodeType::KeyDown) n.prop = "Space";
    else if (type == NodeType::AddToVariable) { n.target = "flag"; n.value = "1"; }
    else if (type == NodeType::EveryN) n.value = "1";
    else if (type == NodeType::MakeVec3) { n.target = "0"; n.value = "0"; n.prop = "0"; }
    else if (type == NodeType::VecPart) { n.target = "(0, 0, 0)"; n.prop = "X"; }
    else if (type == NodeType::Distance) { n.target = "(0, 0, 0)"; n.value = "(0, 0, 0)"; }
    else if (type == NodeType::Normalize) n.target = "(0, 1, 0)";
    else if (type == NodeType::VecMath) { n.target = "(0, 0, 0)"; n.value = "(0, 0, 0)"; n.prop = "+"; }
    else if (type == NodeType::MathFunc) { n.target = "0"; n.value = "0"; n.prop = "Min"; }
    else if (type == NodeType::Clamp) { n.target = "0"; n.value = "0"; n.prop = "1"; }   // value, min, max
    else if (type == NodeType::Time) n.prop = "Delta Time";
    else if (type == NodeType::CodeSnippet) {
        n.value = "fly::Print(\"Hello from a snippet\");\n";
        n.target = "0"; n.prop = "0";   // value inputs / value outputs
    }
    else if (type == NodeType::SetWorld) { n.target = "Lighting"; n.prop = "GlobalShadows"; n.value = "true"; }
    else if (type == NodeType::GamepadButtonDown || type == NodeType::OnGamepadButton) n.prop = "A";
    else if (type == NodeType::GamepadAxis) n.prop = "Left X";
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
bool IsEvent(NodeType t) {
    return t == NodeType::Start || t == NodeType::OnUpdate || t == NodeType::OnStop || t == NodeType::OnKey ||
           t == NodeType::EveryN || t == NodeType::OnGamepadButton;
}
}

bool IsPure(NodeType t) {
    return t == NodeType::UIGet || (t >= NodeType::GetVariable && t <= NodeType::Raycast) ||
           (t >= NodeType::KeyDown && t <= NodeType::ObjectExists && t != NodeType::AddToVariable && t != NodeType::EveryN) ||
           t == NodeType::GamepadButtonDown || t == NodeType::GamepadAxis;
}

int DataOutputCount(const Node& n) {
    if (n.type == NodeType::CodeSnippet) return SnippetOutputs(n);
    if (n.type == NodeType::PlaySound) return 1;
    return IsPure(n.type) ? 1 : 0;
}

namespace {
// Which wires may be drawn: Play Sound's "Sound" output goes only to Stop Sound's "Sound" input, and that input takes nothing else.
bool SoundWireAllowed(const Node& from, const Node& to) {
    return (from.type == NodeType::PlaySound) == (to.type == NodeType::StopSound);
}
}

const Node* LinkedSoundSource(const Graph& g, int stopSoundNodeId) {
    for (const DataLink& d : g.dataLinks) {
        if (d.to != stopSoundNodeId || d.toPin != 0) continue;
        const Node* src = FindIn(g, d.from);
        if (src && src->type == NodeType::PlaySound) return src;
    }
    return nullptr;
}

int SnippetInputs(const Node& n) { return std::clamp(std::atoi(n.target.c_str()), 0, 6); }
int SnippetOutputs(const Node& n) { return std::clamp(std::atoi(n.prop.c_str()), 0, 6); }
int DataInputCount(const Node& n) {
    if (n.type == NodeType::CodeSnippet) return SnippetInputs(n);
    if (n.type == NodeType::Change) return 1 + static_cast<int>(n.extra.size());   // the value, then one per extra property
    return DataInputCount(n.type);
}

void RemoveExtraProperty(int nodeId, int index) {
    Graph& g = Active();
    Node* n = nullptr;
    for (Node& c : g.nodes) if (c.id == nodeId) n = &c;
    if (!n || index < 0 || index >= static_cast<int>(n->extra.size())) return;
    n->extra.erase(n->extra.begin() + index);
    const int pin = index + 1;                                   // its value pin goes, the ones above it move down
    g.dataLinks.erase(std::remove_if(g.dataLinks.begin(), g.dataLinks.end(),
        [&](const DataLink& l) { return l.to == nodeId && l.toPin == pin; }), g.dataLinks.end());
    for (DataLink& l : g.dataLinks) if (l.to == nodeId && l.toPin > pin) --l.toPin;
}

int DataInputCount(NodeType t) {
    switch (t) {
        case NodeType::Branch: case NodeType::SetVariable: case NodeType::Print: case NodeType::Delay:
        case NodeType::Change: case NodeType::Not: return 1;
        case NodeType::Compare: case NodeType::And: case NodeType::Or: case NodeType::Add: case NodeType::Sub:
        case NodeType::Mul: case NodeType::Div: case NodeType::Random: return 2;
        case NodeType::Lerp: case NodeType::Raycast: case NodeType::Concat: case NodeType::MakeVec3: case NodeType::Clamp: return 3;
        case NodeType::Distance: case NodeType::VecMath: case NodeType::MathFunc: return 2;
        case NodeType::VecPart: case NodeType::Normalize: case NodeType::AddToVariable: return 1;
        case NodeType::SetVelocity: case NodeType::AddForce: case NodeType::AddImpulse: case NodeType::Teleport:
        case NodeType::PlaySound: case NodeType::SetVolume: case NodeType::ShowText: case NodeType::SetWorld:
        case NodeType::StopSound: return 1;   // Stop Sound's one is its "Sound" input
        case NodeType::UIText: case NodeType::UIBar: return 7;     // one pin per row of the node, top to bottom
        case NodeType::UIImage: return 6;
        case NodeType::UIPanel: return 5;
        case NodeType::UISet: return 3;
        case NodeType::UIGet: return 2;
        case NodeType::UIRemove: return 1;
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
        case NodeType::Raycast: return pin == 0 ? "Origin" : pin == 1 ? "Direction" : "Distance";
        case NodeType::Concat: return pin == 0 ? "A" : pin == 1 ? "B" : "C";
        case NodeType::MakeVec3: return pin == 0 ? "X" : pin == 1 ? "Y" : "Z";
        case NodeType::Clamp: return pin == 0 ? "Value" : pin == 1 ? "Min" : "Max";
        case NodeType::VecPart: case NodeType::Normalize: return "Vec";
        case NodeType::AddToVariable: return "Amount";
        case NodeType::SetVelocity: case NodeType::AddForce: case NodeType::AddImpulse: case NodeType::Teleport: return "Vector";
        case NodeType::PlaySound: case NodeType::SetVolume: return pin == 0 ? "Volume" : "";
        case NodeType::StopSound: return "Sound";
        case NodeType::ShowText: return "Text";
        case NodeType::UIText: { static const char* const l[] = { "Id", "Text", "Anchor", "Offset", "Font Size", "Colour", "Font" }; return pin >= 0 && pin < 7 ? l[pin] : ""; }
        case NodeType::UIPanel: { static const char* const l[] = { "Id", "Anchor", "Offset", "Size", "Colour" }; return pin >= 0 && pin < 5 ? l[pin] : ""; }
        case NodeType::UIBar: { static const char* const l[] = { "Id", "Value", "Anchor", "Offset", "Size", "Colour", "Background" }; return pin >= 0 && pin < 7 ? l[pin] : ""; }
        case NodeType::UIImage: { static const char* const l[] = { "Id", "File", "Anchor", "Offset", "Size", "Tint" }; return pin >= 0 && pin < 6 ? l[pin] : ""; }
        case NodeType::UISet: { static const char* const l[] = { "Id", "Property", "Value" }; return pin >= 0 && pin < 3 ? l[pin] : ""; }
        case NodeType::UIGet: return pin == 0 ? "Id" : "Read";
        case NodeType::UIRemove: return "Id";
        default: return pin == 0 ? "A" : "B";
    }
}

int OutputCount(NodeType t, const Node* n) {
    if (IsPure(t)) return 0;
    switch (t) {
        case NodeType::End: return 0;
        case NodeType::Branch: case NodeType::Repeat: case NodeType::CreateObject: case NodeType::Clone: return 2;
        case NodeType::Sequence: return n ? std::clamp(std::atoi(n->value.c_str()), 1, 8) : 2;
        default: return 1;
    }
}

const char* OutputLabel(const Node& n, int pin) {
    static const char* const kSeq[] = { "Then 0", "Then 1", "Then 2", "Then 3", "Then 4", "Then 5", "Then 6", "Then 7" };
    switch (n.type) {
        case NodeType::Branch: return pin == 0 ? "True" : "False";
        case NodeType::Repeat: return pin == 0 ? "Loop" : "Done";
        case NodeType::CreateObject: case NodeType::Clone: return pin == 0 ? "Then" : "Setup";
        case NodeType::Sequence: return kSeq[std::clamp(pin, 0, 7)];
        default: return "";
    }
}

bool AllowsFanOut(NodeType t) { return IsEvent(t); }
bool AllowsFanIn(NodeType t) { return t == NodeType::End; }

namespace {
// The nearest Play Sound above `id` along the exec wires (fewest hops), or null. Event nodes are the top of a chain.
const Node* NearestPlaySoundAbove(const Graph& g, int id) {
    std::vector<int> queue = { id };
    std::set<int> seen = { id };
    for (size_t i = 0; i < queue.size() && queue.size() < 512; ++i) {
        for (const Link& l : g.links) {
            if (l.to != queue[i] || !seen.insert(l.from).second) continue;
            const Node* n = FindIn(g, l.from);
            if (n && n->type == NodeType::PlaySound) return n;
            queue.push_back(l.from);
        }
    }
    return nullptr;
}
}

// Stop Sound nodes at or below `startId` (down the exec wires) that no Play Sound is wired to are linked to the nearest Play Sound above
// them, and take its file and object: wiring Play Sound -> (Delay ->) Stop Sound is enough to say which sound to stop.
void AutoLinkSounds(int startId) {
    Graph& g = Active();
    std::vector<int> queue = { startId };
    std::set<int> seen = { startId };
    for (size_t i = 0; i < queue.size() && queue.size() < 512; ++i) {
        for (const Link& l : g.links) if (l.from == queue[i] && seen.insert(l.to).second) queue.push_back(l.to);
    }
    for (int id : queue) {
        Node* stop = Find(id);
        if (!stop || stop->type != NodeType::StopSound || LinkedSoundSource(g, id)) continue;
        const Node* play = NearestPlaySoundAbove(g, id);
        if (!play) continue;
        const int playId = play->id;
        const std::string file = play->target, object = SoundOption(*play, "object");
        ConnectData(playId, id, 0, 0);
        stop->target = file;                       // its own fields keep what it was linked to, should the wire be removed
        SoundOptionRef(*stop, "object") = object;
    }
}

void Connect(int from, int to, int fromPin) {
    if (from == to) return;
    Node* a = Find(from); Node* b = Find(to);
    if (!a || !b || IsPure(a->type) || IsPure(b->type) || IsEvent(b->type) || fromPin < 0 || fromPin >= OutputCount(a->type, a)) return;
    auto& L = Active().links;
    // Most nodes have one input, so a new wire into `to` replaces the old one; End accepts any number, so separate
    // chains can finish at one End. Most output pins take one wire as well (use Sequence to do several things);
    // event nodes may fan out, and their wires run in the order they were made.
    const bool fanOut = AllowsFanOut(a->type), fanIn = AllowsFanIn(b->type);
    L.erase(std::remove_if(L.begin(), L.end(), [&](const Link& l) {
        if (l.from == from && l.fromPin == fromPin && l.to == to) return true;   // the same wire again: replace, never duplicate
        return (!fanOut && l.from == from && l.fromPin == fromPin) || (!fanIn && l.to == to);
    }), L.end());
    L.push_back({ from, to, fromPin });
    AutoLinkSounds(to);
}

void ConnectData(int from, int to, int toPin, int fromPin) {
    if (from == to) return;
    Node* a = Find(from); Node* b = Find(to);
    if (!a || !b || toPin < 0 || toPin >= DataInputCount(*b)) return;
    if (fromPin < 0 || fromPin >= DataOutputCount(*a)) return;
    if (!SoundWireAllowed(*a, *b)) return;
    auto& L = Active().dataLinks;
    {   // refuse a wire that would close a value loop: `to` must not already feed `from`
        std::vector<int> todo{ from }; std::set<int> seen{ from };
        while (!todo.empty()) {
            const int id = todo.back(); todo.pop_back();
            for (const DataLink& l : L)
                if (l.to == id && seen.insert(l.from).second) {
                    if (l.from == to) return;
                    todo.push_back(l.from);
                }
        }
    }
    L.erase(std::remove_if(L.begin(), L.end(), [&](const DataLink& l) { return l.to == to && l.toPin == toPin; }), L.end());
    L.push_back({ from, to, toPin, fromPin });
}

void SanitizeDataLinks() {
    Graph& g = Active();
    g.dataLinks.erase(std::remove_if(g.dataLinks.begin(), g.dataLinks.end(), [&](const DataLink& l) {
        const Node* a = FindIn(g, l.from); const Node* b = FindIn(g, l.to);
        if (!a || !b) return true;
        if (l.toPin >= DataInputCount(*b)) return true;
        return l.fromPin >= DataOutputCount(*a) || !SoundWireAllowed(*a, *b);
    }), g.dataLinks.end());
}

const std::vector<std::string>& KeyNames() {
    static const std::vector<std::string> k = { "Space", "Enter", "W", "A", "S", "D", "Q", "E", "R", "F", "Up", "Down", "Left", "Right",
                                                "1", "2", "3", "4", "Shift", "Ctrl" };
    return k;
}

const std::vector<std::string>& GamepadButtonNames() {
    static const std::vector<std::string> b = { "A", "B", "X", "Y", "Left Bumper", "Right Bumper", "Left Trigger",
                                                 "Right Trigger", "Back", "Start", "Guide", "Left Stick", "Right Stick",
                                                 "Up", "Down", "Left", "Right" };
    return b;
}
const std::vector<std::string>& GamepadAxisNames() {
    static const std::vector<std::string> a = { "Left X", "Left Y", "Right X", "Right Y", "Left Trigger", "Right Trigger" };
    return a;
}

const std::vector<std::string>& MathFuncNames() {
    static const std::vector<std::string> f = { "Min", "Max", "Abs", "Round", "Floor", "Ceil", "Sqrt", "Pow", "Modulo", "Sin", "Cos" };
    return f;
}
const std::vector<std::string>& VecMathOps() {
    static const std::vector<std::string> o = { "+", "-", "scale" };
    return o;
}
const std::vector<std::string>& TimeModes() {
    static const std::vector<std::string> t = { "Delta Time", "Time Since Play" };
    return t;
}

const std::vector<std::string>& HudPositions() {
    static const std::vector<std::string> p = { "Top Left", "Top Center", "Top Right", "Center", "Bottom Left", "Bottom Center", "Bottom Right" };
    return p;
}

const std::vector<std::string>& UiAnchors() {
    static const std::vector<std::string> a = { "Top Left", "Top Center", "Top Right", "Left", "Center", "Right",
                                                "Bottom Left", "Bottom Center", "Bottom Right", "Fill" };
    return a;
}
const std::vector<std::string>& UiSetProperties() {
    static const std::vector<std::string> p = { "Text", "Value", "Visible", "Color", "Background", "Font", "Font Size", "Anchor",
                                                 "Offset", "Size", "Image" };
    return p;
}
std::vector<std::string> UiIds() {
    std::vector<std::string> ids;
    for (const Graph& g : g_scripts)
        for (const Node& n : g.nodes)
            if ((n.type == NodeType::UIText || n.type == NodeType::UIPanel || n.type == NodeType::UIBar || n.type == NodeType::UIImage) &&
                !n.target.empty() && std::find(ids.begin(), ids.end(), n.target) == ids.end())
                ids.push_back(n.target);
    std::sort(ids.begin(), ids.end());
    return ids;
}
const std::vector<std::string>& UiGetProperties() {
    static const std::vector<std::string> p = { "Exists", "Text", "Value", "Visible", "Color", "Background", "Font Size", "Anchor", "Offset", "Size" };
    return p;
}

const std::vector<std::string>& ShapeNames() {
    static const std::vector<std::string> s = { "Cube", "Sphere", "Cylinder", "Wedge" };
    return s;
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
        case NodeType::Raycast: return "Raycast";
        case NodeType::CreateObject: return "Create Object";
        case NodeType::Destroy: return "Destroy";
        case NodeType::Clone: return "Clone";
        case NodeType::SetVelocity: return "Set Velocity";
        case NodeType::AddForce: return "Add Force";
        case NodeType::AddImpulse: return "Add Impulse";
        case NodeType::Teleport: return "Teleport";
        case NodeType::RunScript: return "Run Script";
        case NodeType::OnKey: return "On Key Pressed";
        case NodeType::PlaySound: return "Play Sound";
        case NodeType::SetVolume: return "Set Volume";
        case NodeType::ShowText: return "Show Text";
        case NodeType::UIText: return "UI Text";
        case NodeType::UIPanel: return "UI Panel";
        case NodeType::UIBar: return "UI Bar";
        case NodeType::UIImage: return "UI Image";
        case NodeType::UISet: return "Set UI";
        case NodeType::UIRemove: return "Remove UI";
        case NodeType::UIGet: return "Get UI";
        case NodeType::SetWorld: return "Set World Property";
        case NodeType::CodeSnippet: return "Code Snippet";
        case NodeType::KeyDown: return "Is Key Down";
        case NodeType::AddToVariable: return "Add To Variable";
        case NodeType::EveryN: return "Every N Seconds";
        case NodeType::Concat: return "Join Text";
        case NodeType::MakeVec3: return "Make Vec3";
        case NodeType::VecPart: return "Vec3 Part";
        case NodeType::Distance: return "Distance";
        case NodeType::Normalize: return "Normalize";
        case NodeType::VecMath: return "Vec3 Math";
        case NodeType::MathFunc: return "Math Function";
        case NodeType::Clamp: return "Clamp";
        case NodeType::Time: return "Time";
        case NodeType::ObjectExists: return "Object Exists";
        case NodeType::StopSound: return "Stop Sound";
        case NodeType::GamepadButtonDown: return "Is Gamepad Button Down";
        case NodeType::GamepadAxis: return "Gamepad Axis";
        case NodeType::OnGamepadButton: return "On Gamepad Button Pressed";
        default: break;
    }
    return "?";
}

const char* NodeHelp(NodeType t) {
    switch (t) {
        case NodeType::Start: return "Runs once when Play starts. Connect the first node of the script here.";
        case NodeType::OnUpdate: return "Runs every frame while playing. Use it for things that must keep happening, like movement.";
        case NodeType::OnStop: return "Runs once when Play stops, just before everything is put back.";
        case NodeType::OnKey: return "Runs once each time the chosen key is pressed (not while it is held).";
        case NodeType::EveryN: return "Runs again and again, every N seconds while playing. Set the seconds on the node.";
        case NodeType::KeyDown: return "True while the chosen key is held down, false otherwise. Wire it into Branch or any true/false input.";
        case NodeType::Branch: return "Picks a path: True runs when the condition is true, False when it is not. The condition can be wired or a variable name.";
        case NodeType::Sequence: return "Runs its outputs one after another, top to bottom. Use it to do several things from one point.";
        case NodeType::Delay: return "Waits the given number of seconds, then continues.";
        case NodeType::Repeat: return "Runs the loop output the given number of times, then continues from Done.";
        case NodeType::DoOnce: return "Lets the flow through the first time only. Later arrivals are ignored.";
        case NodeType::End: return "Ends this chain. Several chains can end here.";
        case NodeType::Change: return "Sets properties of an object (colour, position, size and more). Use + New Property to set several in one node.";
        case NodeType::SetWorld: return "Sets a world setting: lighting, environment, rendering, camera or physics. Put back when Play stops.";
        case NodeType::Print: return "Writes a message to the console log, tagged [script]. Good for checking what your script does.";
        case NodeType::ShowText: return "Shows text on the game screen at a chosen spot, for a number of seconds (0 = until replaced). Empty text clears it.";
        case NodeType::PlaySound: return "Plays a sound file from your project at the chosen volume (0 to 1). Pick an object to make the sound come from it, fading out over its Range. Muffle makes walls between it and you muffle it; Echo adds the room's echo (these need an object).";
        case NodeType::StopSound: return "Stops a sound that is playing. Wire a Play Sound's Sound output into its Sound input to stop exactly that sound (same file, same object), or pick a file and an object yourself.";
        case NodeType::SetVolume: return "Sets the master volume, 0 to 1.";
        case NodeType::UIText: return "Shows text on the game screen. Give it an id; running it again with the same id changes that text. Pick a Font from the list: Sans, Serif, Mono, Pixel (the blocky one), or any .ttf / .otf you put in your project's assets/Fonts folder. Offset moves it right and down from its anchor, in pixels of a 1280 x 720 screen.";
        case NodeType::UIPanel: return "Shows a filled rectangle on the game screen, for a backdrop. Colour is (r, g, b) or (r, g, b, a).";
        case NodeType::UIBar: return "Shows a bar filled to a value from 0 to 1 (health, progress). Colour is the fill, Background what shows behind it.";
        case NodeType::UIImage: return "Shows a picture from your project (like assets/ui/heart.png). Size (0, 0) keeps the picture's own size.";
        case NodeType::UISet: return "Changes something on a UI item that is already on screen: its Text, Bar value, Visible (true/false), Colour, Background, Font, Font Size, Anchor, Offset, Size or Image. Wire a value in to update it every frame.";
        case NodeType::UIGet: return "Reads something from a UI item: whether it Exists, its Text, bar Value, Visible, Colour, Background, Font Size, Anchor, Offset or Size. Empty if there is no such item.";
        case NodeType::UIRemove: return "Takes a UI item off the screen. An empty id removes all of them.";
        case NodeType::CreateObject: return "Makes a new object of a chosen shape. The Setup output configures it before it appears.";
        case NodeType::Destroy: return "Removes an object from the scene.";
        case NodeType::Clone: return "Makes a copy of an object. The Setup output configures the copy before it appears.";
        case NodeType::Teleport: return "Moves an object straight to a position, as (x, y, z).";
        case NodeType::RunScript: return "Starts the script that is attached to an object.";
        case NodeType::ObjectExists: return "True if an object with that name is in the scene. Use it before acting on an object that may be gone.";
        case NodeType::SetVelocity: return "Sets how fast and in which direction an object is moving, as (x, y, z).";
        case NodeType::AddForce: return "Pushes an object gradually. Use it every frame for a steady push, like an engine.";
        case NodeType::AddImpulse: return "Gives an object a sudden kick, once. Good for jumps and explosions.";
        case NodeType::Raycast: return "Shoots an invisible line and gives the name of the first object it hits (empty if none).";
        case NodeType::SetVariable: return "Stores a value under a name so other nodes can read it later.";
        case NodeType::AddToVariable: return "Adds an amount to a variable (variable = variable + amount). A variable never set counts as 0.";
        case NodeType::GetVariable: return "Gives the value stored under a variable name.";
        case NodeType::GetProperty: return "Reads a property of an object, such as its position or colour.";
        case NodeType::Add: return "A + B. Works on numbers.";
        case NodeType::Sub: return "A - B.";
        case NodeType::Mul: return "A x B.";
        case NodeType::Div: return "A / B. Dividing by zero gives 0.";
        case NodeType::MathFunc: return "A maths function: Min, Max, Abs, Round, Floor, Ceil, Sqrt, Pow, Modulo, Sin or Cos (degrees).";
        case NodeType::Clamp: return "Keeps a value between a minimum and a maximum.";
        case NodeType::Random: return "A random number between Min and Max, picked each time it is read.";
        case NodeType::Lerp: return "Blends from A to B. T = 0 gives A, T = 1 gives B, 0.5 is halfway.";
        case NodeType::Time: return "Delta Time (seconds since the last frame) or Time Since Play (seconds since Play started).";
        case NodeType::MakeVec3: return "Builds a position/direction (x, y, z) from three numbers.";
        case NodeType::VecPart: return "Takes the X, Y or Z number out of a (x, y, z) value.";
        case NodeType::VecMath: return "Adds or subtracts two (x, y, z) values, or scales one by a number.";
        case NodeType::Distance: return "How far apart two (x, y, z) positions are.";
        case NodeType::Normalize: return "Turns a direction into one of length 1. A zero direction stays zero.";
        case NodeType::Concat: return "Joins A, B and C into one piece of text, e.g. \"Score: \" + a number.";
        case NodeType::Compare: return "Compares A and B (equal, not equal, less, greater...). Gives true or false.";
        case NodeType::And: return "True only when both inputs are true.";
        case NodeType::Or: return "True when at least one input is true.";
        case NodeType::Not: return "Flips true to false and false to true.";
        case NodeType::CodeSnippet: return "Your own C++ code, run when the flow reaches it. Add value inputs and outputs to pass data in and out.";
        case NodeType::GamepadButtonDown: return "True while the chosen gamepad button is held down (gamepad 1), false otherwise.";
        case NodeType::GamepadAxis: return "The chosen gamepad stick/trigger's position, -1 to 1 (gamepad 1). Triggers rest at -1.";
        case NodeType::OnGamepadButton: return "Runs once each time the chosen gamepad button is pressed (gamepad 1, not while it is held).";
        default: break;
    }
    return "";
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
    size_t extras = 0;
    for (const Node& n : g.nodes) extras += n.extra.size();
    if (extras) {   // optional block: extra Change properties
        out << "VPROPS " << extras << "\n";
        for (const Node& n : g.nodes)
            for (const auto& [prop, value] : n.extra) out << "VPROP " << n.id << ' ' << std::quoted(prop) << ' ' << std::quoted(value) << "\n";
    }
    if (!g.dataLinks.empty()) {
        out << "VDLINKS " << g.dataLinks.size() << "\n";
        for (const DataLink& l : g.dataLinks) {   // pin-0 sources keep the original line
            if (l.fromPin == 0) out << "VDLINK " << l.from << ' ' << l.to << ' ' << l.toPin << "\n";
            else out << "VDLINKP " << l.from << ' ' << l.to << ' ' << l.toPin << ' ' << l.fromPin << "\n";
        }
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
    for (;;) {   // optional blocks, written only when the graph has any: extra Change properties, value wires
        const auto mark = in.tellg();
        size_t count = 0;
        if (!(in >> tag)) { in.clear(); in.seekg(mark); break; }
        if (tag == "VPROPS" && in >> count && count <= 100000) {
            for (size_t i = 0; i < count; ++i) {
                int id = 0; std::string prop, value;
                if (!(in >> tag >> id >> std::quoted(prop) >> std::quoted(value)) || tag != "VPROP") return false;
                for (Node& n : g.nodes) if (n.id == id) n.extra.emplace_back(prop, value);
            }
        } else if (tag == "VDLINKS" && in >> count && count <= 100000) {
            for (size_t i = 0; i < count; ++i) {
                DataLink d;
                if (!(in >> tag >> d.from >> d.to >> d.toPin) || (tag != "VDLINK" && tag != "VDLINKP")) return false;
                if (tag == "VDLINKP" && !(in >> d.fromPin)) return false;
                g.dataLinks.push_back(d);
            }
        } else { in.clear(); in.seekg(mark); break; }
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
        if (LoadGraph(in, g)) { g.name = UniqueName(g.name); g_scripts.push_back(std::move(g)); g_current = 0; }
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
        // MakeClassName can fold two saved names into one (a-b, a_b): keep each graph's class name distinct
        const std::string base = g.name;
        for (int n = 2; std::any_of(loaded.begin(), loaded.end(), [&](const Graph& o) { return o.name == g.name; }); ++n)
            g.name = base + std::to_string(n);
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
std::map<std::pair<int, int>, float> g_every;    // (graph, node) Every N Seconds: time since it last fired
float g_dt = 0.0f, g_time = 0.0f;                // the last frame's length, and seconds since Play began
std::vector<ScatteredObject*> g_setupObjects;     // every object whose Setup chain is running (nested Create/Clone), outermost first
std::vector<ScatteredObject*> g_deferredDestroy;  // Destroy nodes that hit one of them, carried out when its chain ends
ScatteredObject* g_newObject = nullptr;          // the object a Create Object / Clone is setting up while its Setup chain runs

// How an error names a target.
std::string TargetName(const std::string& path) {
    return path == kNewObjectTarget ? std::string("New Object (only inside a Create Object / Clone Setup)") : path;
}

struct HudLine { std::string text; float left = -1.0f; };   // left < 0: stays until replaced
std::map<std::string, HudLine> g_hud;                     // keyed by screen position
// What Change / Set UI nodes asked of each UI item, in order: kept until Play ends, and put on top of the item every time a
// UI node makes or refreshes it, so a Change that runs first (or a UI node that runs again) cannot undo it.
std::map<std::string, std::vector<std::pair<std::string, std::string>>> g_uiOverrides;
SoundPlayer g_soundPlayer;
SoundStopper g_soundStopper;

bool Truthy(const std::string& v) { return !v.empty() && v != "0" && v != "false"; }

std::string Input(ScriptRuntime& rt, int gi, const Node& n, int pin, int depth);

bool IsUiNode(NodeType t) { return t >= NodeType::UIText && t <= NodeType::UIGet; }

// What an unwired pin of a UI node holds: the field its row edits. Pin numbers are the rows, top to bottom.
std::string UiLiteral(const Node& n, int pin) {
    auto opt = [&](const char* key, const char* fallback) { return SoundOption(n, key, fallback); };
    switch (n.type) {
        case NodeType::UIText:
            return pin == 0 ? n.target : pin == 1 ? n.value : pin == 2 ? n.prop : pin == 3 ? opt("offset", "(20, 20)")
                 : pin == 4 ? opt("size", "28") : pin == 5 ? opt("color", "(255, 255, 255)") : opt("font", "Sans");
        case NodeType::UIPanel:
            return pin == 0 ? n.target : pin == 1 ? n.prop : pin == 2 ? opt("offset", "(20, 20)") : pin == 3 ? opt("size", "(200, 60)") : opt("color", "(0, 0, 0, 140)");
        case NodeType::UIBar:
            return pin == 0 ? n.target : pin == 1 ? n.value : pin == 2 ? n.prop : pin == 3 ? opt("offset", "(20, -20)")
                 : pin == 4 ? opt("size", "(240, 22)") : pin == 5 ? opt("color", "(80, 200, 90)") : opt("background", "(0, 0, 0, 160)");
        case NodeType::UIImage:
            return pin == 0 ? n.target : pin == 1 ? n.value : pin == 2 ? n.prop : pin == 3 ? opt("offset", "(20, 20)")
                 : pin == 4 ? opt("size", "(0, 0)") : opt("color", "(255, 255, 255)");
        case NodeType::UISet: return pin == 0 ? n.target : pin == 1 ? n.prop : n.value;
        case NodeType::UIGet: return pin == 0 ? n.target : n.prop;
        default: return n.target;   // Remove UI
    }
}

// "(1, 2)" or "(r, g, b, a)": the numbers in a tuple; missing ones are 0.
std::vector<float> Numbers(const std::string& s) {
    std::vector<float> out;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) out.push_back(static_cast<float>(std::atof(cur.c_str()))); cur.clear(); };
    for (char c : s) {
        if (c == ',' || c == '(' || c == ')') flush();
        else if (!std::isspace(static_cast<unsigned char>(c))) cur += c;
    }
    flush();
    return out;
}
Color ParseUiColor(const std::string& s, Color fallback) {
    const std::vector<float> v = Numbers(s);
    if (v.size() < 3) return fallback;
    auto byte = [](float f) { return static_cast<unsigned char>(std::clamp(f, 0.0f, 255.0f)); };
    return { byte(v[0]), byte(v[1]), byte(v[2]), v.size() > 3 ? byte(v[3]) : static_cast<unsigned char>(255) };
}

// What a Get UI node / a Get Property node on "UI/<id>" reads; "" if the widget is not there ("Exists" says so).
std::string ReadUiProperty(const std::string& id, const std::string& prop) {
    gameui::Info i;
    const bool there = gameui::Get(id, i);
    if (prop == "Exists") return there ? "true" : "false";
    if (!there) return {};
    auto num = [](float f) { char b[32]; std::snprintf(b, sizeof(b), "%g", f); return std::string(b); };
    auto col = [](Color c) { return "(" + std::to_string(c.r) + ", " + std::to_string(c.g) + ", " + std::to_string(c.b) + (c.a == 255 ? std::string() : ", " + std::to_string(c.a)) + ")"; };
    if (prop == "Text") return i.text;
    if (prop == "Value") return num(i.value);
    if (prop == "Visible") return i.visible ? "true" : "false";
    if (prop == "Color") return col(i.color);
    if (prop == "Background") return col(i.color2);
    if (prop == "Font") return i.font;
    if (prop == "Font Size") return num(i.fontSize);
    if (prop == "Anchor") { const auto& a = UiAnchors(); const size_t k = static_cast<size_t>(i.anchor); return k < a.size() ? a[k] : std::string(); }
    if (prop == "Offset") return "(" + num(i.x) + ", " + num(i.y) + ")";
    if (prop == "Size") return "(" + num(i.w) + ", " + num(i.h) + ")";
    if (prop == "Image") return i.image;
    return {};
}

// Changes one part of a widget; used by Set UI and by Change nodes aimed at "UI/<id>".
void ApplyUiPropertyNow(const std::string& id, const std::string& prop, const std::string& text) {
    if (prop == "Text") gameui::SetText(id, text, 0.0f);
    else if (prop == "Value") gameui::SetValue(id, static_cast<float>(std::atof(text.c_str())));
    else if (prop == "Visible") gameui::SetVisible(id, Truthy(text));
    else if (prop == "Color") gameui::SetPrimaryColor(id, ParseUiColor(text, WHITE));
    else if (prop == "Font") gameui::SetFont(id, text);
    else if (prop == "Image") gameui::SetImage(id, text);
    else if (prop == "Background" || prop == "Font Size" || prop == "Anchor" || prop == "Offset" || prop == "Size") {
        gameui::Info i;
        if (!gameui::Get(id, i)) return;
        if (prop == "Background") gameui::SetColor(id, i.color, ParseUiColor(text, i.color2));
        else if (prop == "Font Size") gameui::SetText(id, i.text, static_cast<float>(std::atof(text.c_str())));
        else {
            const std::vector<float> v = Numbers(text);
            if (prop == "Anchor") {
                const auto& anchors = UiAnchors();
                const auto at = std::find(anchors.begin(), anchors.end(), text);
                if (at != anchors.end()) gameui::SetLayout(id, static_cast<gameui::Anchor>(at - anchors.begin()), i.x, i.y, i.w, i.h);
            } else if (prop == "Offset" && v.size() >= 2) gameui::SetLayout(id, i.anchor, v[0], v[1], i.w, i.h);
            else if (prop == "Size" && v.size() >= 2) gameui::SetLayout(id, i.anchor, i.x, i.y, v[0], v[1]);
        }
    }
}

// Change / Set UI: the item gets the value now (if a UI node has made it) and keeps it from then on, whenever a UI node makes
// or refreshes the item, and even if the item does not exist yet.
void ApplyUiProperty(const std::string& id, const std::string& prop, const std::string& text) {
    if (id.empty() || prop.empty()) return;
    auto& list = g_uiOverrides[id];
    const auto it = std::find_if(list.begin(), list.end(), [&](const auto& kv) { return kv.first == prop; });
    if (it != list.end()) it->second = text; else list.emplace_back(prop, text);
    gameui::Info there;
    if (gameui::Get(id, there)) ApplyUiPropertyNow(id, prop, text);
}

// Puts the recorded overrides on a freshly made or refreshed item.
void ApplyUiOverrides(const std::string& id) {
    const auto it = g_uiOverrides.find(id);
    if (it == g_uiOverrides.end()) return;
    for (const auto& [prop, text] : it->second) ApplyUiPropertyNow(id, prop, text);
}

// The UI nodes: they make or change a gameui widget named by the node's id. Every field can be wired (see UiLiteral).
void RunUiNode(ScriptRuntime& rt, int gi, const Node& n) {
    auto in = [&](int pin) { return Input(rt, gi, n, pin, 0); };
    const std::string id = in(0);
    if (n.type == NodeType::UIRemove) {
        if (id.empty()) { gameui::Clear(); g_uiOverrides.clear(); }
        else { gameui::Remove(id); g_uiOverrides.erase(id); }
        return;
    }
    if (id.empty()) { ui::Log("[script] UI node #%d has no id", n.id); return; }
    if (n.type == NodeType::UISet) {
        ApplyUiProperty(id, in(1), in(2));
        return;
    }

    // Pins by node: Text  Id Text Anchor Offset FontSize Colour Font     Panel  Id Anchor Offset Size Colour
    //               Bar   Id Value Anchor Offset Size Colour Background   Image  Id File Anchor Offset Size Tint
    int pAnchor = 2, pOffset = 3, pSize = 4, pColor = 5, pBack = -1, pMain = 1;   // pMain: the text, bar value or image file
    gameui::Kind kind = gameui::Kind::Text;
    if (n.type == NodeType::UIPanel) { kind = gameui::Kind::Panel; pAnchor = 1; pOffset = 2; pSize = 3; pColor = 4; pMain = -1; }
    else if (n.type == NodeType::UIBar) { kind = gameui::Kind::Bar; pBack = 6; }
    else if (n.type == NodeType::UIImage) { kind = gameui::Kind::Image; }
    else { pSize = -1; }   // Text: pin 4 is the font size, there is no size
    gameui::Create(id, kind);

    const auto& anchors = UiAnchors();
    const std::string anchorName = in(pAnchor);
    const auto at = std::find(anchors.begin(), anchors.end(), anchorName);
    const gameui::Anchor anchor = at == anchors.end() ? gameui::Anchor::TopLeft : static_cast<gameui::Anchor>(at - anchors.begin());
    const std::vector<float> offset = Numbers(in(pOffset));
    const float ox = offset.size() > 0 ? offset[0] : 0.0f, oy = offset.size() > 1 ? offset[1] : 0.0f;
    float w = 0.0f, h = 0.0f;
    if (pSize >= 0) {
        const std::vector<float> size = Numbers(in(pSize));
        w = size.size() > 0 ? size[0] : 0.0f;
        h = size.size() > 1 ? size[1] : 0.0f;
    }
    gameui::SetLayout(id, anchor, ox, oy, w, h);

    const Color fallback = kind == gameui::Kind::Panel ? Color{ 0, 0, 0, 140 } : kind == gameui::Kind::Bar ? Color{ 80, 200, 90, 255 } : WHITE;
    gameui::SetColor(id, ParseUiColor(in(pColor), fallback), pBack >= 0 ? ParseUiColor(in(pBack), Color{ 0, 0, 0, 160 }) : Color{ 0, 0, 0, 160 });
    const std::string main = pMain >= 0 ? in(pMain) : std::string();
    if (kind == gameui::Kind::Text) {
        gameui::SetText(id, main, static_cast<float>(std::atof(in(4).c_str())));
        gameui::SetFont(id, in(6));
    }
    else if (kind == gameui::Kind::Bar) gameui::SetValue(id, static_cast<float>(std::atof(main.c_str())));
    else if (kind == gameui::Kind::Image) gameui::SetImage(id, main);
    ApplyUiOverrides(id);   // what Change / Set UI asked for wins over this node's own fields
}

// "Workspace/Group/Cube#2": the second object named Cube.
ScatteredObject* FindTarget(ScriptRuntime& rt, const std::string& path) {
    // Inside a Setup chain a node with no target works on the object being made; a target that was picked still wins.
    if (path == kNewObjectTarget || (path.empty() && g_newObject)) return g_newObject;
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

int KeyCodeFor(const std::string& name) {
    static const std::map<std::string, int> codes = {
        { "Space", KEY_SPACE }, { "Enter", KEY_ENTER }, { "W", KEY_W }, { "A", KEY_A }, { "S", KEY_S }, { "D", KEY_D },
        { "Q", KEY_Q }, { "E", KEY_E }, { "R", KEY_R }, { "F", KEY_F }, { "Up", KEY_UP }, { "Down", KEY_DOWN },
        { "Left", KEY_LEFT }, { "Right", KEY_RIGHT }, { "1", KEY_ONE }, { "2", KEY_TWO }, { "3", KEY_THREE }, { "4", KEY_FOUR },
        { "Shift", KEY_LEFT_SHIFT }, { "Ctrl", KEY_LEFT_CONTROL } };
    const auto it = codes.find(name);
    return it == codes.end() ? -1 : it->second;
}

int GamepadButtonCodeFor(const std::string& name) {
    static const std::map<std::string, int> codes = {
        { "A", GAMEPAD_BUTTON_RIGHT_FACE_DOWN }, { "B", GAMEPAD_BUTTON_RIGHT_FACE_RIGHT },
        { "X", GAMEPAD_BUTTON_RIGHT_FACE_LEFT }, { "Y", GAMEPAD_BUTTON_RIGHT_FACE_UP },
        { "Left Bumper", GAMEPAD_BUTTON_LEFT_TRIGGER_1 }, { "Right Bumper", GAMEPAD_BUTTON_RIGHT_TRIGGER_1 },
        { "Left Trigger", GAMEPAD_BUTTON_LEFT_TRIGGER_2 }, { "Right Trigger", GAMEPAD_BUTTON_RIGHT_TRIGGER_2 },
        { "Back", GAMEPAD_BUTTON_MIDDLE_LEFT }, { "Start", GAMEPAD_BUTTON_MIDDLE_RIGHT }, { "Guide", GAMEPAD_BUTTON_MIDDLE },
        { "Left Stick", GAMEPAD_BUTTON_LEFT_THUMB }, { "Right Stick", GAMEPAD_BUTTON_RIGHT_THUMB },
        { "Up", GAMEPAD_BUTTON_LEFT_FACE_UP }, { "Down", GAMEPAD_BUTTON_LEFT_FACE_DOWN },
        { "Left", GAMEPAD_BUTTON_LEFT_FACE_LEFT }, { "Right", GAMEPAD_BUTTON_LEFT_FACE_RIGHT } };
    const auto it = codes.find(name);
    return it == codes.end() ? -1 : it->second;
}

int GamepadAxisCodeFor(const std::string& name) {
    static const std::map<std::string, int> codes = {
        { "Left X", GAMEPAD_AXIS_LEFT_X }, { "Left Y", GAMEPAD_AXIS_LEFT_Y },
        { "Right X", GAMEPAD_AXIS_RIGHT_X }, { "Right Y", GAMEPAD_AXIS_RIGHT_Y },
        { "Left Trigger", GAMEPAD_AXIS_LEFT_TRIGGER }, { "Right Trigger", GAMEPAD_AXIS_RIGHT_TRIGGER } };
    const auto it = codes.find(name);
    return it == codes.end() ? -1 : it->second;
}

bool ParseVec(const std::string& s, double v[3]) {
    float x = 0, y = 0, z = 0;
    if (std::sscanf(s.c_str(), " ( %f , %f , %f", &x, &y, &z) != 3) { v[0] = v[1] = v[2] = 0.0; return false; }
    v[0] = x; v[1] = y; v[2] = z;
    return true;
}

std::string VecStr(double x, double y, double z) {
    char b[96];
    std::snprintf(b, sizeof(b), "(%.7g, %.7g, %.7g)", x, y, z);
    return b;
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

// Value nodes reached by the current top-level Input call: each is evaluated once (shared inputs would otherwise be
// re-evaluated per path, exponential in diamonds), and one still being evaluated means the wires loop.
struct EvalMemo {
    std::map<std::pair<int, int>, std::string> done;
    std::set<std::pair<int, int>> active;
};
EvalMemo g_evalMemo;

// Value on input pin `pin` of `n`: the wired value node's result, else the pin's literal. A wire that yields nothing
// (e.g. a Get Variable that was never set) also falls back to the literal, so the field acts as a default.
std::string Input(ScriptRuntime& rt, int gi, const Node& n, int pin, int depth) {
    const Graph& g = g_scripts[gi];
    if (depth == 0) { g_evalMemo.done.clear(); g_evalMemo.active.clear(); }   // a new read: variables may have changed since
    for (const DataLink& d : g.dataLinks)
        if (d.to == n.id && d.toPin == pin) {
            const Node* src = FindIn(g, d.from);
            std::string v;
            if (src && src->type == NodeType::CodeSnippet) v = rt.SnippetOutput(SnippetClassName(g.name, src->id), d.fromPin);
            else if (src) {
                const std::pair<int, int> key{ gi, src->id };
                const auto hit = g_evalMemo.done.find(key);
                if (hit != g_evalMemo.done.end()) v = hit->second;
                else if (!g_evalMemo.active.insert(key).second) ui::Log("[script] Value wires loop back on themselves");
                else {
                    v = Eval(rt, gi, *src, depth + 1);
                    g_evalMemo.active.erase(key);
                    g_evalMemo.done[key] = v;
                }
            }
            if (!v.empty()) return v;
            break;
        }
    if (n.type == NodeType::CodeSnippet) return {};   // an unwired snippet input is empty (its `value` is the code)
    if (IsUiNode(n.type)) return UiLiteral(n, pin);
    if (n.type == NodeType::Change && pin >= 1 && pin <= static_cast<int>(n.extra.size())) return n.extra[pin - 1].second;
    if (IsPure(n.type)) return pin == 0 ? n.target : pin == 1 ? n.value : n.prop;
    if (n.type == NodeType::Branch) {   // unwired Branch tests the variable named in its field
        const auto it = g_vars.find(n.target);
        return it != g_vars.end() ? it->second : std::string();
    }
    return n.value;
}

std::string Eval(ScriptRuntime& rt, int gi, const Node& n, int depth) {
    if (depth > 64) { ui::Log("[script] Value wires loop back on themselves"); return {}; }
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
        case NodeType::KeyDown: { const int k = KeyCodeFor(n.prop); return k >= 0 && IsKeyDown(k) ? "true" : "false"; }
        case NodeType::GamepadButtonDown: {
            const int b = GamepadButtonCodeFor(n.prop);
            return b >= 0 && ::IsGamepadButtonDown(0, b) ? "true" : "false";
        }
        case NodeType::GamepadAxis: {
            const int a = GamepadAxisCodeFor(n.prop);
            return NumStr(a >= 0 ? ::GetGamepadAxisMovement(0, a) : 0.0);
        }
        case NodeType::Concat: return in(0) + in(1) + in(2);
        case NodeType::MakeVec3: return VecStr(num(0), num(1), num(2));
        case NodeType::VecPart: {
            double v[3]; ParseVec(in(0), v);
            return NumStr(n.prop == "Y" ? v[1] : n.prop == "Z" ? v[2] : v[0]);
        }
        case NodeType::Distance: {
            double a[3], b[3]; ParseVec(in(0), a); ParseVec(in(1), b);
            return NumStr(std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])));
        }
        case NodeType::Normalize: {
            double v[3]; ParseVec(in(0), v);
            const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            return len > 0.0 ? VecStr(v[0] / len, v[1] / len, v[2] / len) : VecStr(0, 0, 0);
        }
        case NodeType::VecMath: {
            double a[3], b[3]; ParseVec(in(0), a);
            if (n.prop == "scale") { const double k = num(1); return VecStr(a[0] * k, a[1] * k, a[2] * k); }
            ParseVec(in(1), b);
            const double sign = n.prop == "-" ? -1.0 : 1.0;
            return VecStr(a[0] + sign * b[0], a[1] + sign * b[1], a[2] + sign * b[2]);
        }
        case NodeType::MathFunc: {
            const double a = num(0), b = num(1), rad = 3.14159265358979323846 / 180.0;
            const std::string& f = n.prop;
            if (f == "Min") return NumStr(std::min(a, b));
            if (f == "Max") return NumStr(std::max(a, b));
            if (f == "Abs") return NumStr(std::fabs(a));
            if (f == "Round") return NumStr(std::round(a));
            if (f == "Floor") return NumStr(std::floor(a));
            if (f == "Ceil") return NumStr(std::ceil(a));
            if (f == "Sqrt") return NumStr(a < 0.0 ? 0.0 : std::sqrt(a));
            if (f == "Pow") { const double r = std::pow(a, b); return NumStr(std::isfinite(r) ? r : 0.0); }
            if (f == "Modulo") return NumStr(b == 0.0 ? 0.0 : std::fmod(a, b));
            if (f == "Sin") return NumStr(std::sin(a * rad));
            if (f == "Cos") return NumStr(std::cos(a * rad));
            return NumStr(a);
        }
        case NodeType::Clamp: {
            double lo = num(1), hi = num(2);
            if (lo > hi) std::swap(lo, hi);
            return NumStr(std::min(std::max(num(0), lo), hi));
        }
        case NodeType::Time: return NumStr(n.prop == "Time Since Play" ? g_time : g_dt);
        case NodeType::ObjectExists: return FindTarget(rt, n.target) ? "true" : "false";
        case NodeType::UIGet: return ReadUiProperty(in(0), in(1));
        case NodeType::Raycast: {
            std::string error;
            std::string hit = rt.RayCastName(in(0), in(1), static_cast<float>(num(2)), error);
            if (!error.empty()) ui::Log("[script] %s", error.c_str());
            return hit;
        }
        case NodeType::GetProperty: {
            if (IsUiTarget(n.target)) return ReadUiProperty(UiTargetId(n.target), n.prop);
            ScatteredObject* obj = FindTarget(rt, n.target);
            std::string out;
            if (!obj) ui::Log("[script] No object '%s'", TargetName(n.target).c_str());
            else if (!rt.GetObjectProperty(obj, n.prop, out)) ui::Log("[script] Can't read '%s'", n.prop.c_str());
            return out;
        }
        default: return {};
    }
}

void ApplyProperty(ScriptRuntime& rt, const std::string& target, const std::string& prop, const std::string& value) {
    if (prop.empty()) return;
    std::string result;
    if (IsUiTarget(target)) { ApplyUiProperty(UiTargetId(target), prop, value); return; }
    if (IsWorldGroup(target)) {
        if (!console::RunLine("game." + target + "." + prop + " = " + value, &result))
            ui::Log("[script] %s", result.c_str());
        return;
    }
    ScatteredObject* obj = FindTarget(rt, target);
    std::string error;
    if (!obj) ui::Log("[script] No object '%s'", TargetName(target).c_str());
    else if (!rt.SetObjectProperty(obj, prop, value, error)) ui::Log("[script] %s", error.c_str());
}

// A Change node sets its main property, then each extra one, in order.
void ApplyChange(ScriptRuntime& rt, const Node& n) {
    if (n.target.empty() && !g_newObject) return;
    ApplyProperty(rt, n.target, n.prop, n.value);
    for (const auto& [prop, value] : n.extra) ApplyProperty(rt, n.target, prop, value);
}

void Emit(ScriptRuntime& rt, int gi, int fromId, int pin);

// Create Object / Clone: runs the chain on out 1 with target-less nodes working on `obj`, then gives it its physics body.
void RunSetup(ScriptRuntime& rt, int gi, int nodeId, ScatteredObject* obj) {
    ScatteredObject* outer = g_newObject;
    g_newObject = obj;
    g_setupObjects.push_back(obj);
    Emit(rt, gi, nodeId, 1);
    g_setupObjects.pop_back();
    g_newObject = outer;
    // A Destroy aimed at an object still being set up waits until its chain has finished, so nothing uses it freed.
    const auto doomed = std::find(g_deferredDestroy.begin(), g_deferredDestroy.end(), obj);
    if (doomed != g_deferredDestroy.end()) { g_deferredDestroy.erase(doomed); rt.DestroyObject(obj); return; }
    rt.FinishCreate(obj);
}

// Follows the wire leaving (`fromId`, `pin`) and everything after it. Branching nodes push several continuations;
// the step cap stops cyclic graphs.
int g_emitDepth = 0;   // Emit calls nested through Create Object / Clone Setup chains
int g_emitSteps = 0;   // steps taken since the outermost Emit began: the cap holds across nested chains

void Emit(ScriptRuntime& rt, int gi, int fromId, int pin) {
    constexpr int kMaxSteps = 100000, kMaxDepth = 32;
    if (g_emitDepth >= kMaxDepth) { ui::Log("[script] Stopped: Setup chains nest over %d deep (loop?)", kMaxDepth); return; }
    struct DepthGuard {
        DepthGuard() { if (g_emitDepth++ == 0) g_emitSteps = 0; }
        ~DepthGuard() { --g_emitDepth; }
    } depthGuard;
    // Entries are "continue from (node, output pin)". A negative id means "run node -id now": that is how an output
    // pin with several wires hands each target its turn.
    std::vector<std::pair<int, int>> stack{ { fromId, pin } };
    while (!stack.empty()) {
        if (++g_emitSteps > kMaxSteps) { ui::Log("[script] Stopped: graph ran over %d steps (loop?)", kMaxSteps); return; }
        if (gi < 0 || gi >= static_cast<int>(g_scripts.size())) return;
        const Graph& g = g_scripts[gi];
        const auto [id, p] = stack.back();
        stack.pop_back();
        int target = -id;
        if (id >= 0) {
            std::vector<int> targets;
            for (const Link& l : g.links) if (l.from == id && l.fromPin == p) targets.push_back(l.to);
            if (targets.empty()) continue;
            if (targets.size() > 1) {   // first wire on top of the stack, so it runs first
                for (auto it = targets.rbegin(); it != targets.rend(); ++it) stack.push_back({ -*it, 0 });
                continue;
            }
            target = targets[0];
        }
        const Node* n = FindIn(g, target);
        if (!n) continue;
        switch (n->type) {
            case NodeType::Change: {
                Node applied = *n;
                applied.value = Input(rt, gi, *n, 0, 0);
                for (size_t i = 0; i < applied.extra.size(); ++i) applied.extra[i].second = Input(rt, gi, *n, static_cast<int>(i) + 1, 0);
                ApplyChange(rt, applied);
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::Print: ui::Log("[script] %s", Input(rt, gi, *n, 0, 0).c_str()); stack.push_back({ n->id, 0 }); break;
            case NodeType::AddToVariable:
                if (!n->target.empty()) {
                    double current = 0.0, amount = 0.0;
                    ToNumber(g_vars[n->target], current);
                    ToNumber(Input(rt, gi, *n, 0, 0), amount);
                    g_vars[n->target] = NumStr(current + amount);
                }
                stack.push_back({ n->id, 0 });
                break;
            case NodeType::SetVariable:
                if (!n->target.empty()) g_vars[n->target] = Input(rt, gi, *n, 0, 0);
                stack.push_back({ n->id, 0 });
                break;
            case NodeType::Branch:
                stack.push_back({ n->id, Truthy(Input(rt, gi, *n, 0, 0)) ? 0 : 1 });
                break;
            case NodeType::CodeSnippet: {
                const std::string cls = SnippetClassName(g.name, n->id);
                std::vector<std::string> inputs;
                for (int i = 0; i < SnippetInputs(*n); ++i) inputs.push_back(Input(rt, gi, *n, i, 0));
                rt.SetSnippetInputs(cls, std::move(inputs));
                if (!rt.StartSnippet(cls))
                    ui::Log("[script] Code Snippet #%d is not compiled yet - check the compiler output, then Play again", n->id);
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::SetWorld: {
                Node applied = *n;
                applied.value = Input(rt, gi, *n, 0, 0);
                if (IsWorldGroup(n->target)) ApplyChange(rt, applied);
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::ShowText: {
                ShowText(Input(rt, gi, *n, 0, 0), n->prop, static_cast<float>(std::atof(n->target.c_str())));
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::UIText: case NodeType::UIPanel: case NodeType::UIBar: case NodeType::UIImage:
            case NodeType::UISet: case NodeType::UIRemove:
                RunUiNode(rt, gi, *n);
                stack.push_back({ n->id, 0 });
                break;
            case NodeType::SetVolume:
                SetMasterVolumeLevel(static_cast<float>(std::atof(Input(rt, gi, *n, 0, 0).c_str())));
                stack.push_back({ n->id, 0 });
                break;
            case NodeType::PlaySound: {
                PlaySoundNode(rt, *n, static_cast<float>(std::atof(Input(rt, gi, *n, 0, 0).c_str())));
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::StopSound: {
                // Wired to a Play Sound's "Sound" output: stop that sound. Otherwise the node's own file and object.
                Node eff = *n;
                if (const Node* src = LinkedSoundSource(g_scripts[gi], n->id)) {
                    eff.target = src->target;
                    SoundOptionRef(eff, "object") = SoundOption(*src, "object");
                }
                StopSoundNode(rt, eff);
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::CreateObject: {
                // Born without a physics body: the Setup chain (out 1) changes it first, then it is spawned as configured.
                ScatteredObject* obj = rt.CreateObject(n->prop, true);
                if (!obj) ui::Log("[script] Can't create '%s'", n->prop.c_str());
                else {
                    if (!n->value.empty()) obj->SetName(n->value);
                    RunSetup(rt, gi, n->id, obj);
                }
                stack.push_back({ n->id, 0 });
                break;
            }
            case NodeType::Destroy: case NodeType::Clone: case NodeType::RunScript:
            case NodeType::SetVelocity: case NodeType::AddForce: case NodeType::AddImpulse: case NodeType::Teleport: {
                ScatteredObject* obj = FindTarget(rt, n->target);
                std::string error;
                if (!obj) { ui::Log("[script] No object '%s'", TargetName(n->target).c_str()); stack.push_back({ n->id, 0 }); break; }
                if (n->type == NodeType::Destroy) {
                    if (std::find(g_setupObjects.begin(), g_setupObjects.end(), obj) != g_setupObjects.end()) g_deferredDestroy.push_back(obj);
                    else rt.DestroyObject(obj);
                }
                else if (n->type == NodeType::RunScript) rt.StartObjectScript(obj);
                else if (n->type == NodeType::Clone) {
                    ScatteredObject* copy = rt.CloneObject(obj, true);
                    if (!copy) ui::Log("[script] Can't clone '%s'", n->target.c_str());
                    else {
                        if (!n->value.empty()) copy->SetName(n->value);
                        RunSetup(rt, gi, n->id, copy);
                    }
                } else {
                    const std::string vec = Input(rt, gi, *n, 0, 0);
                    if (n->type == NodeType::AddForce || n->type == NodeType::AddImpulse) rt.AddForce(obj, vec, n->type == NodeType::AddImpulse, error);
                    else rt.SetObjectProperty(obj, n->type == NodeType::SetVelocity ? "Velocity" : "Position", vec, error);
                    if (!error.empty()) ui::Log("[script] %s", error.c_str());
                }
                stack.push_back({ n->id, 0 });
                break;
            }
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

bool KeyPressed(const std::string& name) {
    const int k = KeyCodeFor(name);
    return k >= 0 && IsKeyPressed(k);
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
    g_delays.clear(); g_vars.clear(); g_once.clear(); g_hud.clear(); g_uiOverrides.clear(); g_every.clear();
    g_dt = 0.0f; g_time = 0.0f;
    FireEvents(rt, NodeType::Start);
}

void Tick(ScriptRuntime& rt, float dt) {
    g_dt = dt;
    g_time += dt;
    std::vector<Pending> due;
    for (auto it = g_delays.begin(); it != g_delays.end();) {
        it->left -= dt;
        if (it->left <= 0.0f) { due.push_back(*it); it = g_delays.erase(it); }
        else ++it;
    }
    for (auto it = g_hud.begin(); it != g_hud.end();) {
        if (it->second.left >= 0.0f && (it->second.left -= dt) <= 0.0f) it = g_hud.erase(it);
        else ++it;
    }
    for (const Pending& d : due) Emit(rt, d.graph, d.node, 0);
    FireEvents(rt, NodeType::OnUpdate);
    for (int gi = 0; gi < static_cast<int>(g_scripts.size()); ++gi) {   // Every N Seconds
        std::vector<std::pair<int, float>> timers;
        for (const Node& n : g_scripts[gi].nodes)
            if (n.type == NodeType::EveryN) timers.push_back({ n.id, std::max(0.01f, static_cast<float>(std::atof(n.value.c_str()))) });
        for (const auto& [id, interval] : timers) {
            float& acc = g_every[{ gi, id }];
            acc += dt;
            for (int fired = 0; acc >= interval && fired < 4; ++fired) { acc -= interval; Emit(rt, gi, id, 0); }
            if (acc >= interval) acc = 0.0f;   // far behind (a long hitch): drop the backlog rather than spiral
        }
    }
    for (int gi = 0; gi < static_cast<int>(g_scripts.size()); ++gi) {
        std::vector<std::pair<int, std::string>> keys;
        for (const Node& n : g_scripts[gi].nodes) if (n.type == NodeType::OnKey) keys.push_back({ n.id, n.prop });
        for (const auto& [id, key] : keys) if (KeyPressed(key)) Emit(rt, gi, id, 0);
    }
    for (int gi = 0; gi < static_cast<int>(g_scripts.size()); ++gi) {
        std::vector<std::pair<int, std::string>> buttons;
        for (const Node& n : g_scripts[gi].nodes) if (n.type == NodeType::OnGamepadButton) buttons.push_back({ n.id, n.prop });
        for (const auto& [id, button] : buttons) if (WasGamepadButtonPressed(0, button)) Emit(rt, gi, id, 0);
    }
}

void Stop(ScriptRuntime& rt) {
    FireEvents(rt, NodeType::OnStop);
    g_delays.clear(); g_vars.clear(); g_once.clear(); g_hud.clear(); g_uiOverrides.clear(); g_every.clear();
}

std::vector<int> SetupChainNodes() {
    const Graph& g = Active();
    std::vector<int> found, todo;
    for (const Link& l : g.links) {
        const Node* a = FindIn(g, l.from);
        if (a && (a->type == NodeType::CreateObject || a->type == NodeType::Clone) && l.fromPin == 1) todo.push_back(l.to);
    }
    while (!todo.empty()) {
        const int id = todo.back();
        todo.pop_back();
        if (std::find(found.begin(), found.end(), id) != found.end()) continue;
        found.push_back(id);
        for (const Link& l : g.links) if (l.from == id) todo.push_back(l.to);
    }
    return found;
}

std::string SnippetClassName(const std::string& graphName, int nodeId) {
    std::string name = kSnippetClassPrefix;
    for (char c : graphName) name += (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ? c : '_';
    return name + "_" + std::to_string(nodeId);
}

namespace {
// Snippet text for the generated file: `#include` lines are hoisted to the top (a blank line keeps the numbering), and a
// closing co_return makes the body a coroutine even when it never co_awaits.
void EmitSnippet(std::string& includes, std::string& classes, const std::string& graphName, const Node& n) {
    std::string body, line;
    std::istringstream in(n.value);
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos && line.compare(first, 8, "#include") == 0) { includes += line.substr(first) + "\n"; line.clear(); }
        body += line + "\n";
    }
    const std::string cls = SnippetClassName(graphName, n.id);
    classes += "struct " + cls + " : vsnip::Base {\n    const char* Key() const override { return \"" + cls + "\"; }\n";
    classes += "    fly::Task Run() override {\n";
    classes += "#line 1 \"Code Snippet " + graphName + "/" + std::to_string(n.id) + "\"\n";
    classes += body;
    classes += "\n        co_return;\n    }\n};\nFLY_SCRIPT(" + cls + ")\n\n";
}

std::string SnippetsFilePath(const std::string& projectDir) {
    return (std::filesystem::path(projectDir) / "Scripts" / ".visualscript" / "snippets.cpp").string();
}
}

std::string GenerateSnippetSource() {
    std::string includes, classes;
    for (const Graph& g : g_scripts)
        for (const Node& n : g.nodes)
            if (n.type == NodeType::CodeSnippet) EmitSnippet(includes, classes, g.name, n);
    if (classes.empty()) return {};
    static const char* const kBase = R"CPP(
// What a snippet can use besides the whole of fly.hpp: the values wired into and out of its node.
//   In(i) / InNum(i) / InBool(i) / InVec3(i)   read input pin i (as it was when the flow reached the node)
//   Out(i, value)                              set output pin i (text, number, bool or fly::Vec3)
namespace vsnip {
struct Base : fly::Script {
    virtual const char* Key() const = 0;
    std::string In(int pin) const { return fly::detail::SnippetInput(Key(), pin); }
    double InNum(int pin, double fallback = 0.0) const {
        const std::string s = In(pin);
        char* end = nullptr;
        const double v = std::strtod(s.c_str(), &end);
        return (end && end != s.c_str()) ? v : fallback;
    }
    bool InBool(int pin) const { const std::string s = In(pin); return !s.empty() && s != "0" && s != "false"; }
    fly::Vec3 InVec3(int pin) const {
        float x = 0, y = 0, z = 0;
        std::sscanf(In(pin).c_str(), " ( %f , %f , %f", &x, &y, &z);
        return { x, y, z };
    }
    template <class T> void Out(int pin, const T& v) const {
        std::string s;
        if constexpr (std::is_same_v<T, bool>) s = v ? "true" : "false";
        else if constexpr (std::is_same_v<T, fly::Vec3>) { char b[96]; std::snprintf(b, sizeof(b), "(%g, %g, %g)", v.x, v.y, v.z); s = b; }
        else if constexpr (std::is_arithmetic_v<T>) { char b[48]; std::snprintf(b, sizeof(b), "%.10g", static_cast<double>(v)); s = b; }
        else s = std::string(v);
        fly::detail::SnippetOutput(Key(), pin, s);
    }
};
}

)CPP";
    return "// Generated by the Visual Script editor from Code Snippet nodes. Edit the nodes, not this file.\n"
           "#include \"fly.hpp\"\n#include <cmath>\n#include <cstdio>\n#include <cstdlib>\n#include <string>\n#include <type_traits>\n#include <vector>\n"
           + includes + kBase + classes;
}

bool WriteSnippetSources(const std::string& projectDir, bool createFolders) {
    if (projectDir.empty()) return false;
    namespace fs = std::filesystem;
    const std::string path = SnippetsFilePath(projectDir);
    const std::string want = GenerateSnippetSource();
    std::error_code ec;
    if (!createFolders && !fs::is_directory(fs::path(projectDir) / "Scripts", ec)) return false;
    if (want.empty()) return fs::remove(path, ec);          // the last snippet is gone: drop its classes
    {
        std::ifstream in(path, std::ios::binary);
        const std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (in && have == want) return false;
    }
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << want;
    return static_cast<bool>(out);
}

size_t SnippetFingerprint() {
    size_t h = 1469598103934665603ull;
    auto mix = [&h](const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } h ^= 0xff; h *= 1099511628211ull; };
    for (const Graph& g : g_scripts)
        for (const Node& n : g.nodes)
            if (n.type == NodeType::CodeSnippet) { mix(g.name); mix(std::to_string(n.id)); mix(n.value); }
    return h;
}

void SetSoundPlayer(SoundPlayer player) { g_soundPlayer = std::move(player); }
void SetSoundStopper(SoundStopper stopper) { g_soundStopper = std::move(stopper); }

std::string SoundOption(const Node& n, const std::string& key, const std::string& fallback) {
    for (const auto& [k, v] : n.extra) if (k == key) return v;
    return fallback;
}
std::string& SoundOptionRef(Node& n, const std::string& key, const std::string& fallback) {
    for (auto& kv : n.extra) if (kv.first == key) return kv.second;
    n.extra.emplace_back(key, fallback);
    return n.extra.back().second;
}
bool SoundFlag(const Node& n, const std::string& key) { return SoundOption(n, key) == "true"; }
ScatteredObject* ResolveObject(ScriptRuntime& rt, const std::string& path) { return FindTarget(rt, path); }

bool IsKeyHeld(const std::string& keyName) {
    const int k = KeyCodeFor(keyName);
    return k >= 0 && IsKeyDown(k);
}
bool WasKeyPressed(const std::string& keyName) { return KeyPressed(keyName); }
bool IsGamepadAvailable(int gamepad) { return ::IsGamepadAvailable(gamepad); }
bool IsGamepadButtonHeld(int gamepad, const std::string& buttonName) {
    const int b = GamepadButtonCodeFor(buttonName);
    return b >= 0 && ::IsGamepadButtonDown(gamepad, b);
}
bool WasGamepadButtonPressed(int gamepad, const std::string& buttonName) {
    const int b = GamepadButtonCodeFor(buttonName);
    return b >= 0 && ::IsGamepadButtonPressed(gamepad, b);
}
float GamepadAxisValue(int gamepad, const std::string& axisName) {
    const int a = GamepadAxisCodeFor(axisName);
    return a >= 0 ? ::GetGamepadAxisMovement(gamepad, a) : 0.0f;
}
void ShowText(const std::string& text, const std::string& position, float seconds) {
    if (text.empty()) g_hud.erase(position);
    else g_hud[position] = { text, seconds > 0.0f ? seconds : -1.0f };
}
namespace {
std::string SoundPath(ScriptRuntime& rt, const std::string& file) {
    std::string path = file;
    if (!std::filesystem::path(path).is_absolute() && !rt.GetProjectPath().empty())
        path = (std::filesystem::path(rt.GetProjectPath()) / path).string();
    return path;
}
}
void PlaySoundFile(ScriptRuntime& rt, const std::string& file, float volume) {
    if (!g_soundPlayer) { ui::Log("[script] Play Sound: this build has no audio backend"); return; }
    SoundRequest req;
    req.path = SoundPath(rt, file);
    req.volume = std::clamp(volume, 0.0f, 1.0f);
    g_soundPlayer(req);
}
void PlaySoundNode(ScriptRuntime& rt, const Node& n, float volume) {
    if (!g_soundPlayer) { ui::Log("[script] Play Sound: this build has no audio backend"); return; }
    SoundRequest req;
    req.path = SoundPath(rt, n.target);
    req.volume = std::clamp(volume, 0.0f, 1.0f);
    req.objectPath = SoundOption(n, "object");
    if (!req.objectPath.empty() && !FindTarget(rt, req.objectPath)) {
        ui::Log("[script] Play Sound: no object '%s'", TargetName(req.objectPath).c_str());
        return;
    }
    req.loop = SoundFlag(n, "loop");
    req.range = std::clamp(static_cast<float>(std::atof(SoundOption(n, "range", "30").c_str())), 1.0f, 1000.0f);
    req.muffle = !req.objectPath.empty() && SoundFlag(n, "muffle");   // rays need somewhere to start from
    req.echo = !req.objectPath.empty() && SoundFlag(n, "echo");
    g_soundPlayer(req);
}
void StopSoundNode(ScriptRuntime& rt, const Node& n) {
    if (g_soundStopper) g_soundStopper(SoundPath(rt, n.target), SoundOption(n, "object"));
}
void SetMasterVolumeLevel(float volume) { SetMasterVolume(std::clamp(volume, 0.0f, 1.0f)); }
float TimeSincePlay() { return g_time; }

void DrawHud(float x, float y, float w, float h) {
    if (g_hud.empty()) return;
    constexpr int kSize = 28, kPad = 16;
    for (const auto& [pos, line] : g_hud) {
        const int tw = MeasureText(line.text.c_str(), kSize);
        float px = x + kPad, py = y + kPad;
        if (pos.find("Center") != std::string::npos) px = x + (w - tw) * 0.5f;
        else if (pos.find("Right") != std::string::npos) px = x + w - tw - kPad;
        if (pos == "Center") py = y + (h - kSize) * 0.5f;
        else if (pos.find("Bottom") == 0) py = y + h - kSize - kPad;
        DrawText(line.text.c_str(), static_cast<int>(px) + 2, static_cast<int>(py) + 2, kSize, Color{ 0, 0, 0, 160 });
        DrawText(line.text.c_str(), static_cast<int>(px), static_cast<int>(py), kSize, RAYWHITE);
    }
}

} // namespace vscript
