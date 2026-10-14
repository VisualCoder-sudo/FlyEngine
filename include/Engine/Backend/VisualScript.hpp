#pragma once
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

// Node-graph scripting. The graph lives in the scene file and runs on play, in the editor and in exported players.
class ScriptRuntime;

namespace vscript {

enum class NodeType {
    Start = 0, Change = 1, End = 2,
    OnUpdate = 3,       // event: fires every frame while playing
    OnStop = 4,         // event: fires when Play stops
    Branch = 5,         // out 0 = True, out 1 = False; target = variable name (truthy test)
    Sequence = 6,       // outs 0..N-1 run in order; value = N
    Delay = 7,          // continues after `value` seconds
    Repeat = 8,         // out 0 = loop body (value times), out 1 = Done
    DoOnce = 9,         // passes the first time only
    Print = 10,         // logs `value`
    SetVariable = 11,   // target = variable name, value = literal (data input 0 overrides it)
    // Value nodes: no exec pins, evaluated on demand when an input pin asks for them. Unwired input pins fall back
    // to a literal: pin 0 = target, pin 1 = value, pin 2 = prop (Compare keeps its operator in prop).
    GetVariable = 12,   // target = variable name
    Compare = 13,       // A, B; prop = == != < > <= >=
    And = 14, Or = 15, Not = 16,
    Add = 17, Sub = 18, Mul = 19, Div = 20,
    Random = 21,        // Min, Max
    Lerp = 22,          // A, B, T
    GetProperty = 23,   // target = object path, prop = property
    Count_
};

struct Node {
    int id = 0;
    NodeType type = NodeType::Start;
    float x = 0.0f, y = 0.0f;   // canvas position
    std::string target;         // Change: "Workspace/Group1/Cube" or a world group such as "Lighting"
    std::string prop;           // Change: property name
    std::string value;          // Change: console-syntax value, e.g. "(255, 0, 0)" or "true"
};

// exec wire: node `from` output pin `fromPin` -> node `to` (single exec input)
struct Link { int from = 0, to = 0, fromPin = 0; };

// value wire: value node `from` (its single output) -> data input pin `toPin` of node `to`
struct DataLink { int from = 0, to = 0, toPin = 0; };

struct Graph {
    std::string name = "Script 1";
    unsigned lastUsed = 0;      // not saved: orders the manager by recency
    std::vector<Node> nodes;
    std::vector<Link> links;
    std::vector<DataLink> dataLinks;
    int nextId = 1;
};

Graph& Active();                  // the script being edited (an empty scratch graph when there are none)
void Clear();                     // drops every script

// The scene's visual scripts. All of them run on Play.
int Count();
int CurrentIndex();
const std::string& NameAt(int i);
void Select(int i);
std::vector<int> RecentOrder();                // script indices, most recently used first
int NewScript(const std::string& name = "");   // returns its index, and selects it
int OpenByName(const std::string& name);       // selects the script with that name, creating it if missing
void RenameCurrent(const std::string& name);
void RemoveCurrent();
bool Has(const std::string& name);
void RemoveByName(const std::string& name);
void RenameKey(const std::string& from, const std::string& to);   // a script was renamed: its graph follows
Node& AddNode(NodeType type, float x, float y);
void RemoveNode(int id);
Node* Find(int id);
void Connect(int from, int to, int fromPin = 0);   // replaces any wire already leaving that output pin or entering `to`
void ConnectData(int from, int to, int toPin);   // replaces any wire already entering that input pin
bool IsPure(NodeType t);                         // value node (no exec pins)
int DataInputCount(NodeType t);                  // value input pins (Branch condition, Print text, Math A/B, ...)
const char* DataInputLabel(NodeType t, int pin);
int OutputCount(NodeType t, const Node* n = nullptr);   // exec output pins (0 for End and events' none)
const char* OutputLabel(const Node& n, int pin);        // "" for a plain single output

void Save(std::ostream& out);     // writes a "VSCRIPT" block
void Load(std::istream& in);      // reads one, after the caller has seen the tag; restores position on failure

// Runs every Start chain: Change nodes apply their property, End stops the chain.
void Run(ScriptRuntime& rt);   // needs the world's script runtime to find objects
void Tick(ScriptRuntime& rt, float dt);   // each frame while playing: OnUpdate chains, pending Delays
void Stop(ScriptRuntime& rt);             // Play stopped: OnStop chains, then pending state is dropped

const char* TypeName(NodeType t);
// Properties offered by the Change node for a given target ("" = object).
const std::vector<std::string>& ObjectProperties();
const std::vector<std::string>& WorldGroups();
std::vector<std::string> WorldProperties(const std::string& group);

} // namespace vscript

namespace vscript {

// What the Change node's target browser shows under Game > Workspace.
struct SceneListing {
    std::function<void(const std::string&)> deleteScript;   // deletes a script for real (graph, entry, file)
    std::vector<std::string> existingScripts;   // scripts already in the scene, offered by "Open existing"
    struct Group { std::string name; std::vector<std::string> members; };
    std::vector<Group> groups;
    std::vector<std::string> loose;   // objects outside any group
};

// Draws the node editor into the current ImGui window (the "Visual Script" tab).
void DrawEditor(const SceneListing& scene);

} // namespace vscript
