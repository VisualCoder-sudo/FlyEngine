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
    Raycast = 24,       // Origin, Direction, Distance -> name of the first object hit ("" = none)
    // Object actions (target = object path like Change). Vector actions take their "(x, y, z)" from value / data input 0.
    CreateObject = 25,  // prop = shape, value = optional name
    Destroy = 26,
    Clone = 27,         // value = optional name for the copy
    SetVelocity = 28,
    AddForce = 29,
    AddImpulse = 30,
    Teleport = 31,
    RunScript = 32,     // starts the script attached to the object
    OnKey = 33,         // event: prop = key name (see KeyNames)
    PlaySound = 34,     // target = file (relative to the project), value = volume 0..1
    SetVolume = 35,     // value = master volume 0..1
    ShowText = 36,      // value = text, prop = screen position (see HudPositions), target = seconds (0 = until replaced; empty text clears)
    SetWorld = 37,      // target = world group, prop = property, value = value (like Change, world groups only)
    CodeSnippet = 38,   // value = C++ (the body of a fly::Script::Run coroutine), started when the flow reaches it
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
void Connect(int from, int to, int fromPin = 0);   // replaces any wire already entering `to`, and leaving that pin unless AllowsFanOut
bool AllowsFanOut(NodeType t);                     // event nodes: one output pin may feed several nodes
bool AllowsFanIn(NodeType t);                      // End: its input may take several wires
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
const std::vector<std::string>& KeyNames();   // keys offered by On Key Pressed
const std::vector<std::string>& ShapeNames(); // shapes offered by Create Object
const std::vector<std::string>& HudPositions();   // screen spots offered by Show Text

// Code Snippet nodes. Their C++ is written to Scripts/.visualscript/snippets.cpp as one hidden fly::Script class per
// node, compiled with the project's other scripts, and started by the node. Errors point at "Code Snippet <Graph>/<n>"
// with line numbers counted from the snippet's own first line.
inline constexpr const char* kSnippetClassPrefix = "VSnip_";
std::string SnippetClassName(const std::string& graphName, int nodeId);
std::string GenerateSnippetSource();                          // "" when no graph has a snippet
// Writes (or deletes) the file; true if it changed on disk. With createFolders false it only touches a project that
// already has a Scripts folder (scene loading uses that, so an exported player never grows one).
bool WriteSnippetSources(const std::string& projectDir, bool createFolders = true);
size_t SnippetFingerprint();                                  // changes whenever any snippet's text or identity does

// The engine has no audio backend yet. Play Sound hands the resolved file path and a 0..1 volume to this hook;
// until one is installed the node logs that there is no audio and carries on.
using SoundPlayer = std::function<void(const std::string& path, float volume)>;
void SetSoundPlayer(SoundPlayer player);

// Draws the Show Text messages inside the given screen rectangle (the game view). No-op when none are showing.
void DrawHud(float x, float y, float w, float h);
// Properties offered by the Change node for a given target ("" = object).
const std::vector<std::string>& ObjectProperties();
const std::vector<std::string>& WorldGroups();
std::vector<std::string> WorldProperties(const std::string& group);

} // namespace vscript

namespace vscript {

// What the Change node's target browser shows under Game > Workspace.
struct SceneListing {
    std::string projectDir;   // the project folder: Play Sound's file browser starts here and stores paths relative to it
    std::function<void(const std::string&)> deleteScript;   // deletes a script for real (graph, entry, file)
    std::vector<std::string> existingScripts;   // scripts already in the scene, offered by "Open existing"
    struct Group { std::string name; std::vector<std::string> members; };
    std::vector<Group> groups;
    std::vector<std::string> loose;   // objects outside any group
};

// Draws the node editor into the current ImGui window (the "Visual Script" tab).
void DrawEditor(const SceneListing& scene);

} // namespace vscript
