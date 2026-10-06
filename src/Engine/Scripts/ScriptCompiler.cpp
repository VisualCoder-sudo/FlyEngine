// ScriptCompiler.cpp - builds the project's C# scripts into Scripts/FlyScript.dll
// via the dotnet CLI. Stages the SDK wrapper sources next to the user's *.cs
// files and compiles them all into one assembly CoreCLRHost then loads.

#include "../../../include/Engine/Scripts/ScriptCompiler.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace fs = std::filesystem;

namespace {

// A solid (unquoted) command argument. Paths are quoted by the caller.
std::string PlainArg(const fs::path& p) {
    return p.string();
}

// Find the engine's SDK source directory. The SDK .cs files live in
// ScriptingSDK/FlyScript relative to the source tree. At runtime we resolve
// relative to the executable's directory (Flyengine.exe sits in the build dir,
// which is a sibling of ScriptingSDK).
fs::path FindSdkDir() {
    // GetModuleFileNameW on Windows, readlink("/proc/self/exe") on Linux.
    const std::string exe = platform::ExecutablePath();
    if (exe.empty()) return {};
    fs::path exePath = fs::u8path(exe);
    fs::path candidate = exePath.parent_path().parent_path() / "ScriptingSDK" / "FlyScript";
    if (fs::exists(candidate / "FlyScript.cs")) return candidate;

    // Fallback: assume the working directory is the repo root.
    if (fs::exists(fs::path("ScriptingSDK") / "FlyScript" / "FlyScript.cs"))
        return fs::path("ScriptingSDK") / "FlyScript";
    return {};
}

// Copy a text file if the destination is missing or older than the source.
bool StageFile(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (fs::exists(src, ec) && fs::is_regular_file(src, ec)) {
        if (!fs::exists(dst, ec) || fs::last_write_time(src, ec) > fs::last_write_time(dst, ec)) {
            std::ifstream in(src, std::ios::binary);
            std::ofstream out(dst, std::ios::binary);
            if (!in || !out) return false;
            out << in.rdbuf();
            out.close();
            in.close();
        }
        return true;
    }
    return false;
}

const char* kCsprojContent =
    "<Project Sdk=\"Microsoft.NET.Sdk\">\n"
    "  <PropertyGroup>\n"
    "    <TargetFramework>net8.0</TargetFramework>\n"
    "    <AllowUnsafeBlocks>true</AllowUnsafeBlocks>\n"
    "    <ImplicitUsings>enable</ImplicitUsings>\n"
    "    <Nullable>enable</Nullable>\n"
    "    <PlatformTarget>x64</PlatformTarget>\n"
    "    <OutputType>Library</OutputType>\n"
    "    <GenerateAssemblyInfo>false</GenerateAssemblyInfo>\n"
    "    <UseLibraryImport>true</UseLibraryImport>\n"
    "    <EnableDefaultItems>false</EnableDefaultItems>\n"
    "    <CopyLocalLockFileAssemblies>true</CopyLocalLockFileAssemblies>\n"
    "  </PropertyGroup>\n"
    "  <ItemGroup>\n"
    "    <Compile Include=\"**/*.cs\" Exclude=\"obj/**;bin/**\" />\n"
    "  </ItemGroup>\n"
    "  <ItemGroup>\n"
    "    <PackageReference Include=\"Microsoft.CodeAnalysis.CSharp.Scripting\" Version=\"4.9.2\" />\n"
    "  </ItemGroup>\n"
    "</Project>\n";

// Run `dotnet` with arguments and capture the exit code.
bool RunDotnet(const std::string& dotnetExe, const std::vector<std::string>& args,
               std::string& outLog) {
    // posix_spawn on Linux, CreateProcess on Windows, both with a pipe for the
    // child's stdout+stderr. 10-minute cap, matching the old
    // WaitForSingleObject(pi.hProcess, 600000).
    platform::ProcessResult r = platform::RunProcessCapture(dotnetExe, args, 600000);
    outLog = r.output;
    if (!r.launched) return false;
    return r.exitCode == 0;
}

} // namespace

namespace scriptCompiler {

bool IsPresent(const std::string& projectFolder)
{
    return fs::exists(fs::path(projectFolder) / "Scripts" / "FlyScript.dll");
}

bool EnsureBuilt(const std::string& projectFolder)
{
    fs::path project(projectFolder);
    fs::path scriptsDir = project / "Scripts";
    std::error_code ec;

    if (!fs::exists(scriptsDir, ec)) fs::create_directories(scriptsDir, ec);
    if (!fs::exists(scriptsDir, ec)) {
        ui::LogAlways("[ScriptCompiler] Could not create Scripts dir: %s", scriptsDir.string().c_str());
        return false;
    }

    // 1. Stage the SDK wrapper sources next to the user scripts.
    fs::path sdkDir = FindSdkDir();
    if (sdkDir.empty()) {
        ui::LogAlways("[ScriptCompiler] SDK sources not found (ScriptingSDK/FlyScript).");
        return false;
    }
    StageFile(sdkDir / "FlyScript.cs",   scriptsDir / "_FlyScriptSDK.cs");
    StageFile(sdkDir / "ScriptHost.cs",  scriptsDir / "_FlyScriptSDK_Host.cs");

    // 2. Write the project file (always resync so user .cs files are globbed).
    {
        std::ofstream csproj(scriptsDir / "FlyScript.csproj", std::ios::binary);
        if (!csproj) {
            ui::LogAlways("[ScriptCompiler] Could not write FlyScript.csproj.");
            return false;
        }
        csproj.write(kCsprojContent, static_cast<std::streamsize>(std::strlen(kCsprojContent)));
        csproj.close();
    }

    // 3. Invoke the dotnet CLI. Prefer the per-user .dotnet SDK, then PATH.
    //    The SDK install script drops `dotnet` (no extension) in ~/.dotnet on
    //    Unix and `dotnet.exe` on Windows.
    std::string dotnet = {};
    const std::string home = platform::UserHomeDir();
    if (!home.empty()) {
#if defined(_WIN32)
        const char* exeName = "dotnet.exe";
#else
        const char* exeName = "dotnet";
#endif
        fs::path candidate = fs::u8path(home) / ".dotnet" / exeName;
        if (fs::exists(candidate)) dotnet = candidate.string();
    }
    if (dotnet.empty()) dotnet = "dotnet"; // fall back to PATH

    std::vector<std::string> args;
    args.push_back("build");
    args.push_back(PlainArg(scriptsDir / "FlyScript.csproj"));
    args.push_back("-c");
    args.push_back("Release");
    args.push_back("-o");
    args.push_back(PlainArg(scriptsDir));
    args.push_back("-v");
    args.push_back("m");
    args.push_back("--nologo");

    ui::LogAlways("[ScriptCompiler] Building scripts with dotnet CLI...");
    std::string log;
    bool ok = RunDotnet(dotnet, args, log);
    if (!log.empty()) {
        for (std::string::size_type s = 0, i; s < log.size(); s = i + 1) {
            i = log.find('\n', s);
            if (i == std::string::npos) i = log.size();
            std::string line = log.substr(s, i - s);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) ui::LogAlways("[dotnet] %s", line.c_str());
        }
    }

    if (!ok) {
        ui::LogAlways("[ScriptCompiler] Build FAILED (exit code != 0).");
        return false;
    }

    ui::LogAlways("[ScriptCompiler] FlyScript.dll built: %s",
            (scriptsDir / "FlyScript.dll").string().c_str());
    return true;
}

} // namespace scriptCompiler