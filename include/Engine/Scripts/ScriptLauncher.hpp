#pragma once
#include <string>

class ScatteredObject;

// Replaces the in-engine Flyscript text editor. Instead of editing a script
// in an embedded overlay, C# scripts (.cs) are materialized to real files in
// the project's Scripts/ folder and opened in an external editor.
//
// Before the first open (per selection), a picker window asks the user which
// editor to use: Rider, VS Code, Visual Studio, Notepad, or auto-detect. That
// choice is remembered automatically and used for every subsequent open - no
// prompt - until it fails to launch (editor uninstalled/moved) or the user
// explicitly re-picks via a "Choose Editor..." action (see
// RequestChooseEditor / ChooseEditorForObjectScript / ChooseEditorForStandaloneScript).
namespace scriptLauncher {

// Builds a valid C# class identifier from an arbitrary object/script name:
// non-identifier characters become underscores and a leading digit is prefixed.
// Falls back to "Script". Used to wire object/standalone script type names.
std::string MakeClassName(const std::string& name);

// Ensures <project.path>/Scripts exists. Returns the folder, or empty on I/O
// failure.
std::string EnsureScriptsDir();

// Writes a C# IScript source stub for `className` to <Scripts>/<className>.cs
// if the file does not already exist (already-existing files are left touched
// so external edits are not overwritten). Returns the full file path, or empty
// on failure.
std::string MaterializeScript(const std::string& className);

// Queues `file` to open. If a remembered editor is active it is launched
// immediately; if that launch fails, the remembered choice is forgotten and
// the picker is shown instead. With no remembered editor, the picker is
// shown right away.
void RequestOpen(const std::string& file);

// Always shows the picker for `file`, even when an editor is already
// remembered - lets the user override or reset their default. A successful
// pick here becomes the new remembered default, same as RequestOpen.
void RequestChooseEditor(const std::string& file);

// Renders and handles the IDE picker modal. Call every frame from the UI pass
// (e.g. inside ui.cpp's ImGui frame). Returns true while the picker is open.
bool DrawImGuiModal();

// True if a file is queued waiting for the picker to resolve.
bool HasPending();

// Cancels the queued open (e.g. the user closes the dialog without picking).
void CancelPending();

// Convenience: materializes a scene object's C# script stub to
// <Scripts>/<typeName>.cs and queues it to open. `obj->script` holds the C#
// IScript type name; when unset it is auto-bound from the object name and the
// object is marked to run on Play. Returns the file path (which may be empty
// if materialization failed).
std::string EditObjectScript(ScatteredObject* obj);

// Convenience: materializes a standalone script's C# stub to
// <Scripts>/<typeName>.cs and queues it to open. `typeName` is the IScript
// class; when empty it is derived from `scriptName`. The resolved class name is
// written to `outClassName` (optional) so callers can persist it as the entry's
// typeName. Returns the file path, or empty on failure.
std::string EditStandaloneScript(const std::string& scriptName,
                                 const std::string& typeName,
                                 std::string* outClassName = nullptr);

// Right-click "Choose Editor..." for an object's script: materializes the
// file (if needed) and always shows the picker via RequestChooseEditor.
// Returns the file path, or empty if the object has no script set.
std::string ChooseEditorForObjectScript(ScatteredObject* obj);

// Same, for a standalone script.
std::string ChooseEditorForStandaloneScript(const std::string& scriptName,
                                            const std::string& typeName);

} // namespace scriptLauncher