#pragma once
#include <set>
#include <string>
#include <vector>

// Where plugins live on disk, and the operations on those folders that need no running editor. Two roots hold the same
// layout (<root>/{nat,lib}/<name>/{id.json,build/<name>.so}):
//   - local:  <project>/plugins           only that project loads them
//   - global: <config dir>/plugins        every project loads them
// The editor's NativeScriptHost loads both (a local plugin shadows a global one of the same name); the project manager's
// File > Manage Plugins window works on the folders directly, since no project (and no host) is open there.
namespace pluginstore {

struct Entry {
    std::string name;          // library stem: the identity the host and the disabled lists use
    std::string displayName, version, publisher, description, category;
    std::string type;          // "nat" or "lib"
    std::string dir;           // <root>/<type>/<folder>
    std::string libPath;       // build/<name>.so|.dll
    bool enabled = true;
    bool hasIdentity = false;
    bool global = false;
};

struct Result { bool ok = false; std::string message; };

// <config dir>/plugins. FLYENGINE_GLOBAL_PLUGINS (or SetGlobalRootOverride, for tests) replaces it.
std::string GlobalRoot();
void SetGlobalRootOverride(const std::string& root);
std::string LocalRoot(const std::string& projectPath);   // "" for an empty project path

bool IsLibrary(const std::string& file);                 // .so / .dll for this platform

// Every plugin below `root`, with the disabled list applied. Sorted by name.
std::vector<Entry> List(const std::string& root, bool global);

// <root>/disabled.txt: one library name per line.
std::set<std::string> LoadDisabled(const std::string& root);
void SaveDisabled(const std::string& root, const std::set<std::string>& names);
bool SetEnabled(const std::string& root, const std::string& name, bool enabled);

// Copies a plugin folder (the one with id.json and build/, or its build/) or a bare library into `root`. Never overwrites.
Result Import(const std::string& root, const std::string& source);
// Deletes a plugin's folder. Refuses anything that is not <root>/<type>/<folder>.
Result Remove(const std::string& root, const std::string& name);
// Moves a plugin folder from one root to the other, keeping its enabled state. Never overwrites.
Result Move(const std::string& fromRoot, const std::string& toRoot, const std::string& name);

} // namespace pluginstore
