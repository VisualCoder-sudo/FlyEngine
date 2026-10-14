#pragma once
#include <functional>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

// Node-graph scripting. The graph lives in the scene file and runs on play, in the editor and in exported players.
class ScriptRuntime;

namespace vscript {

enum class NodeType { Start = 0, Change = 1, End = 2 };

struct Node {
    int id = 0;
    NodeType type = NodeType::Start;
    float x = 0.0f, y = 0.0f;   // canvas position
    std::string target;         // Change: "Workspace/Group1/Cube" or a world group such as "Lighting"
    std::string prop;           // Change: property name
    std::string value;          // Change: console-syntax value, e.g. "(255, 0, 0)" or "true"
};

struct Link { int from = 0, to = 0; };   // exec wire: node `from` (output) -> node `to` (input)

struct Graph {
    std::string name = "Script 1";
    unsigned lastUsed = 0;      // not saved: orders the manager by recency
    std::vector<Node> nodes;
    std::vector<Link> links;
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
void Connect(int from, int to);   // replaces any wire already leaving `from` or entering `to`

void Save(std::ostream& out);     // writes a "VSCRIPT" block
void Load(std::istream& in);      // reads one, after the caller has seen the tag; restores position on failure

// Runs every Start chain: Change nodes apply their property, End stops the chain.
void Run(ScriptRuntime& rt);   // needs the world's script runtime to find objects

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
