#pragma once
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

// Node-graph scripting. The graph lives in the scene file and runs on play, in the editor and in exported players.
class ScriptRuntime;
class ScatteredObject;

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
    CreateObject = 25,  // prop = shape, value = optional name. Out 0 = Then, out 1 = Setup: a chain run on the new object
                        // before it gets its physics body, so it appears configured. Nodes in it that have no target
                        // apply to the new object automatically (a target that was picked still wins)
    Destroy = 26,
    Clone = 27,         // value = optional name for the copy; out 1 = Setup, like Create Object
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
    CodeSnippet = 38,   // value = C++ (the body of a fly::Script::Run coroutine), started when the flow reaches it;
                        // target = number of value inputs, prop = number of value outputs (0..6 each)
    // Added later. Value nodes (no exec pins) unless noted; literals of unwired pins as for the other value nodes.
    KeyDown = 39,       // prop = key (see KeyNames): true while it is held
    AddToVariable = 40, // exec: target = variable name, value / data input 0 = amount; the variable becomes variable + amount
    EveryN = 41,        // event: value = seconds between firings
    Concat = 42,        // A, B, C joined as text
    MakeVec3 = 43,      // X, Y, Z -> "(x, y, z)"
    VecPart = 44,       // Vec (pin 0), prop = X / Y / Z
    Distance = 45,      // A, B vectors
    Normalize = 46,     // Vec
    VecMath = 47,       // A, B; prop = "+", "-" or "scale" (B is then a number)
    MathFunc = 48,      // A, B; prop = a name from MathFuncNames()
    Clamp = 49,         // Value, Min, Max
    Time = 50,          // prop = "Delta Time" or "Time Since Play"
    ObjectExists = 51,  // target = object path: true if it is in the scene
    StopSound = 52,     // target = file; the "object" sound option limits it to that object's sound (see SoundOption)
    GamepadButtonDown = 53,   // prop = button (see GamepadButtonNames); gamepad 0: true while it is held
    GamepadAxis = 54,         // prop = axis (see GamepadAxisNames); gamepad 0: -1..1, triggers rest at -1
    OnGamepadButton = 55,     // event: prop = button name (see GamepadButtonNames); gamepad 0
    // In-game UI (see GameUI.hpp). All exec nodes: target = widget id, prop = anchor (see UiAnchors), value / data input 0 =
    // the text, bar value (0..1) or image file. The rest lives in Node::extra, read with SoundOption: "offset" "(x, y)",
    // "size" "(w, h)" (text: the font size), "color" "(r, g, b)" or "(r, g, b, a)", bars also "background".
    UIText = 56, UIPanel = 57, UIBar = 58, UIImage = 59,
    UISet = 60,         // changes a widget that exists: prop = one of UiSetProperties(), value / data input 0 = the new one
    UIRemove = 61,      // target = widget id; empty removes every widget
    UIGet = 62,         // value node: target = widget id, prop = what to read (see UiGetProperties); "" if the widget is not there
    Count_
};

struct Node {
    int id = 0;
    NodeType type = NodeType::Start;
    float x = 0.0f, y = 0.0f;   // canvas position
    std::string target;         // Change: "Workspace/Group1/Cube" or a world group such as "Lighting"
    std::string prop;           // Change: property name
    std::string value;          // Change: console-syntax value, e.g. "(255, 0, 0)" or "true"
    // Change: more (property, value) pairs applied after the main one, so a single node can set several properties.
    // Each has its own value pin (data pin i + 1 is extra[i]).
    std::vector<std::pair<std::string, std::string>> extra;
};

// exec wire: node `from` output pin `fromPin` -> node `to` (single exec input)
struct Link { int from = 0, to = 0, fromPin = 0; };

// value wire: output pin `fromPin` of node `from` (a value node has one; a Code Snippet has several) -> data input pin
// `toPin` of node `to`
struct DataLink { int from = 0, to = 0, toPin = 0, fromPin = 0; };

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
void ConnectData(int from, int to, int toPin, int fromPin = 0);   // replaces any wire already entering that input pin
void RemoveExtraProperty(int nodeId, int index);  // drops a Change node's extra property and re-numbers its value wires
void SanitizeDataLinks();                        // drops value wires and exec wires whose pins no longer exist (a snippet or Sequence lost a pin)
int SnippetInputs(const Node& n);                // Code Snippet pin counts, clamped to 0..6
int SnippetOutputs(const Node& n);
int DataInputCount(const Node& n);               // the node's own count: DataInputCount(type), or the snippet's pins
bool IsPure(NodeType t);                         // value node (no exec pins)
int DataInputCount(NodeType t);                  // value input pins (Branch condition, Print text, Math A/B, ...)
int DataOutputCount(const Node& n);              // value output pins: 1 for a value node, the snippet's own, 1 for Play Sound (its "Sound")
// Stop Sound's "Sound" input takes a wire from a Play Sound node's "Sound" output: it then stops exactly that sound (same file, same
// object) and shows those values instead of its own. Null when nothing is wired to it.
const Node* LinkedSoundSource(const Graph& g, int stopSoundNodeId);
// Called after an exec wire is made: Stop Sound nodes below it that have no Play Sound wired in are linked to the nearest Play Sound
// above them (and take its file and object), so Play Sound -> Delay -> Stop Sound stops that sound without any other wiring.
void AutoLinkSounds(int startNodeId);
const char* DataInputLabel(NodeType t, int pin);
int OutputCount(NodeType t, const Node* n = nullptr);   // exec output pins (0 for End and events' none)
const char* OutputLabel(const Node& n, int pin);        // "" for a plain single output

void Save(std::ostream& out);     // writes a "VSCRIPT" block
void Load(std::istream& in);      // reads one, after the caller has seen the tag; restores position on failure

// Runs every Start chain: Change nodes apply their property, End stops the chain.
void Run(ScriptRuntime& rt);   // needs the world's script runtime to find objects
void Tick(ScriptRuntime& rt, float dt);   // each frame while playing: OnUpdate chains, pending Delays
void Stop(ScriptRuntime& rt);             // Play stopped: OnStop chains, then pending state is dropped

// What older scenes stored as the target for "the object being made". Still understood; the editor no longer offers it,
// since an empty target does the same inside a Setup chain.
inline constexpr const char* kNewObjectTarget = "@new";
// Nodes reachable from a Create Object / Clone's Setup output (the ones whose empty target means the new object).
std::vector<int> SetupChainNodes();
const char* TypeName(NodeType t);
const char* NodeHelp(NodeType t);             // one or two sentences on what the node does (the Insert Node tooltip); "" if unknown
const std::vector<std::string>& KeyNames();   // keys offered by On Key Pressed
const std::vector<std::string>& GamepadButtonNames();   // buttons offered by the gamepad nodes (gamepad 0)
const std::vector<std::string>& GamepadAxisNames();      // axes offered by the Gamepad Axis node (gamepad 0)
const std::vector<std::string>& ShapeNames(); // shapes offered by Create Object
const std::vector<std::string>& MathFuncNames();   // functions offered by the Math Function node (Sin/Cos take degrees)
const std::vector<std::string>& VecMathOps();
const std::vector<std::string>& TimeModes();
const std::vector<std::string>& HudPositions();
const std::vector<std::string>& UiAnchors();      // the anchors of the UI nodes, in gameui::Anchor order
const std::vector<std::string>& UiSetProperties(); // what a Set UI node changes
const std::vector<std::string>& UiGetProperties(); // what a Get UI node reads
// Change and Get Property nodes aim at a UI item with the target "UI/<id>"; the properties are UiSetProperties() / UiGetProperties().
bool IsUiTarget(const std::string& target);
std::string UiTargetId(const std::string& target);   // "UI/hp" -> "hp"
std::vector<std::string> UiIds();                   // the ids the scripts' UI nodes make, sorted: what the target picker offers   // screen spots offered by Show Text

// Code Snippet nodes. Inside the snippet: In(i) / InNum(i) / InBool(i) / InVec3(i) read what was wired into input pin i
// when the flow reached the node, and Out(i, value) sets output pin i for the nodes wired after it. Their C++ is written to Scripts/.visualscript/snippets.cpp as one hidden fly::Script class per
// node, compiled with the project's other scripts, and started by the node. Errors point at "Code Snippet <Graph>/<n>"
// with line numbers counted from the snippet's own first line. Pin values are kept per node (by class name), not per
// running instance: if the same node is reached again while an earlier run is still going, or the graph runs on several
// objects, the runs share (and overwrite) the same In/Out values.
inline constexpr const char* kSnippetClassPrefix = "VSnip_";
std::string SnippetClassName(const std::string& graphName, int nodeId);
std::string GenerateSnippetSource();                          // "" when no graph has a snippet
// Writes (or deletes) the file; true if it changed on disk. With createFolders false it only touches a project that
// already has a Scripts folder (scene loading uses that, so an exported player never grows one).
bool WriteSnippetSources(const std::string& projectDir, bool createFolders = true);
size_t SnippetFingerprint();                                  // changes whenever any snippet's text or identity does

// Play Sound hands what to play to this hook (the engine installs one that mixes it, see Audio.hpp); until one is
// installed the node logs that there is no audio and carries on.
struct SoundRequest {
    std::string path;           // resolved against the project
    float volume = 1.0f;        // 0..1
    std::string objectPath;     // "" = a flat sound; else the sound comes from that object ("Workspace/Fan#1") and follows it
    bool loop = false;
    float range = 30.0f;        // metres: full volume close up, silent at this distance (objects only)
    bool muffle = false;        // sound rays: walls between the object and the listener muffle it
    bool echo = false;          // sound rays: the room around the object adds an echo
};
using SoundPlayer = std::function<void(const SoundRequest&)>;
void SetSoundPlayer(SoundPlayer player);
// Stop Sound: stops sounds of `path` (resolved like Play Sound), only those from `objectPath` when that is not empty.
using SoundStopper = std::function<void(const std::string& path, const std::string& objectPath)>;
void SetSoundStopper(SoundStopper stopper);

// A Play Sound / Stop Sound node's settings live in Node::extra (so scenes saved before they existed still load, with the
// defaults): "object", "loop", "range", "muffle", "echo".
std::string SoundOption(const Node& n, const std::string& key, const std::string& fallback = "");
std::string& SoundOptionRef(Node& n, const std::string& key, const std::string& fallback = "");   // creates it when absent
bool SoundFlag(const Node& n, const std::string& key);                                          // "true"
// The object a "Workspace/Group/Cube#2" path names, or null.
ScatteredObject* ResolveObject(ScriptRuntime& rt, const std::string& path);

// What Input / Audio / Show Text / Time nodes do, as plain calls: native scripts (fly.hpp) use the same ones, so a node and
// its C++ equivalent behave identically.
bool IsKeyHeld(const std::string& keyName);       // a name from KeyNames()
bool WasKeyPressed(const std::string& keyName);   // went down this frame
bool IsGamepadAvailable(int gamepad);
bool IsGamepadButtonHeld(int gamepad, const std::string& buttonName);       // a name from GamepadButtonNames()
bool WasGamepadButtonPressed(int gamepad, const std::string& buttonName);   // went down this frame
float GamepadAxisValue(int gamepad, const std::string& axisName);          // a name from GamepadAxisNames(); -1..1
void ShowText(const std::string& text, const std::string& position, float seconds);   // position from HudPositions(); seconds 0 = until replaced; "" clears
void PlaySoundFile(ScriptRuntime& rt, const std::string& file, float volume);        // file relative to the project (or absolute)
void PlaySoundNode(ScriptRuntime& rt, const Node& n, float volume);                  // Play Sound with all its options
void StopSoundNode(ScriptRuntime& rt, const Node& n);
void SetMasterVolumeLevel(float volume);                                              // 0..1
float TimeSincePlay();

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
    std::function<void(const std::string&, const std::string&)> renameScript;   // the current graph was renamed (old, new): its Explorer entry follows
    std::vector<std::string> existingScripts;   // scripts already in the scene, offered by "Open existing"
    struct Group { std::string name; std::vector<std::string> members; };
    std::vector<Group> groups;
    std::vector<std::string> loose;   // objects outside any group
};

// Draws the node editor into the current ImGui window (the "Visual Script" tab).
void DrawEditor(const SceneListing& scene);

} // namespace vscript
