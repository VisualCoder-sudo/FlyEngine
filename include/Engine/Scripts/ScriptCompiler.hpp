#pragma once
#include <string>

// ScriptCompiler - builds the project's C# scripts into Scripts/FlyScript.dll
// using the dotnet CLI. The SDK wrapper sources (FlyScript.cs, ScriptHost.cs)
// are staged into the project alongside the user's *.cs files, then compiled
// together into a single assembly that CoreCLR loads. This replaces the old
// pre-shipped FlyScript.dll.
namespace scriptCompiler {

// Builds (or rebuilds) the scripts for `projectFolder`. Returns true on
// success. Logs errors through ui::Log.
bool EnsureBuilt(const std::string& projectFolder);

// True if the current project's Scripts/FlyScript.dll exists on disk.
bool IsPresent(const std::string& projectFolder);

} // namespace scriptCompiler