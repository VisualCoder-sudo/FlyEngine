/*
 * TechnicalTools.cpp - Implementation of developer tools suite
 */

#include "../../../include/Engine/TechnicalTools.hpp"
#include "../../../include/Engine.hpp"
#include "../../../include/Engine/Backend/Entity.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Frontend/ui.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Graphics.hpp"

#include <raylib.h>

// Uniform type tags used by the shader editor UI. They keep OpenGL's numeric
// values because ShaderUniform::type (TechnicalTools.hpp) stores them.
namespace {
constexpr int GL_FLOAT = 0x1406;
constexpr int GL_FLOAT_VEC2 = 0x8B50;
constexpr int GL_FLOAT_VEC3 = 0x8B51;
constexpr int GL_FLOAT_VEC4 = 0x8B52;
constexpr int GL_INT = 0x1404;
constexpr int GL_INT_VEC2 = 0x8B53;
constexpr int GL_INT_VEC3 = 0x8B54;
constexpr int GL_INT_VEC4 = 0x8B55;
constexpr int GL_BOOL = 0x8B56;
constexpr int GL_FLOAT_MAT2 = 0x8B5A;
constexpr int GL_FLOAT_MAT3 = 0x8B5B;
constexpr int GL_FLOAT_MAT4 = 0x8B5C;
constexpr int GL_SAMPLER_1D = 0x8B5D;
constexpr int GL_SAMPLER_2D = 0x8B5E;
constexpr int GL_SAMPLER_3D = 0x8B5F;
constexpr int GL_SAMPLER_CUBE = 0x8B60;
} // namespace
#include <imgui.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <climits>
#include <cfloat>

#if defined(__linux__)
    #include <unistd.h> // sysconf(_SC_PAGESIZE) for the /proc/self/statm read
#endif

// Callstack capture for the memory tracker. glibc/macOS get real symbol names
// via backtrace() + dladdr(); Windows gets raw return addresses only, which
// is deliberate -- resolving them properly needs dbghelp, and that is a new
// link dependency the player build does not otherwise have.
#if defined(_WIN32)
    // windows.h arrives with raylib.h on Windows; CaptureStackBackTrace is
    // exported from kernel32, which every target here already links.
#else
    #if defined(__GLIBC__) || defined(__APPLE__)
        #include <execinfo.h>
        #include <dlfcn.h>
        #include <cxxabi.h>
        #define FLYENGINE_HAVE_CALLSTACK 1
    #endif
#endif

// raylib allocates the Shader::locs array with this many entries. rlgl.h
// defines it, but TechnicalTools only includes raylib.h, and the shader size
// calculation needs the number.
#ifndef RL_MAX_SHADER_LOCATIONS
    #define RL_MAX_SHADER_LOCATIONS 32
#endif

// CYAN color constant (raylib uses SKYBLUE for cyan)
#ifndef CYAN
#define CYAN SKYBLUE
#endif

namespace fs = std::filesystem;

namespace TechTools {

// ============================================================================
// SHARED HELPERS
// ============================================================================
//
// These sit at the top because the console commands below format byte counts
// for their output and the memory tracker needs the same formatting for its
// UI, and two copies of "bytes to a human string" would drift apart.

namespace {

// Resource handle -> the pointer the tracker keys it by. raylib's Load*/Unload*
// take resources by value, so the struct address is useless as a key; these are
// the stable identities instead.
inline void* TextureKey(const Texture& t)  { return (void*)(uintptr_t)t.id; }
inline void* ImageKey(const Image& i)      { return i.data; }
inline void* ShaderKey(const Shader& s)    { return (void*)(uintptr_t)s.id; }
inline void* RenderTexKey(const RenderTexture& rt) { return (void*)(uintptr_t)rt.id; }
inline void* WaveKey(const Wave& w)        { return w.data; }
// A Mesh and a Model are both identified by their heap array of sub-resources;
// the Model's key is the meshes array itself, so tracking a model and tracking
// each of its meshes are separate entries that do not collide.
inline void* MeshKey(const Mesh& m)        { return (void*)(uintptr_t)(m.vboId ? (uintptr_t)m.vboId : (uintptr_t)m.vertices); }
inline void* ModelKey(const Model& m)      { return (void*)(uintptr_t)m.meshes; }

// Human-readable byte counts.
std::string FormatBytes(size_t bytes) {
    char buf[64];
    if (bytes < 1024ull) { std::snprintf(buf, sizeof(buf), "%llu B", (unsigned long long)bytes); return buf; }
    double kb = (double)bytes / 1024.0;
    if (kb < 1024.0)  { std::snprintf(buf, sizeof(buf), "%.1f KB", kb); return buf; }
    double mb = kb / 1024.0;
    if (mb < 1024.0)   { std::snprintf(buf, sizeof(buf), "%.2f MB", mb); return buf; }
    double gb = mb / 1024.0;
    std::snprintf(buf, sizeof(buf), "%.2f GB", gb);
    return buf;
}

std::string FormatMB(size_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

std::string FormatPtr(const void* ptr) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)ptr);
    return buf;
}

std::string FormatSeconds(double seconds) {
    char buf[64];
    if (seconds < 0) seconds = 0;
    if (seconds < 60.0)   { std::snprintf(buf, sizeof(buf), "%.1fs", seconds); return buf; }
    int mins = (int)(seconds / 60.0);
    double secs = seconds - mins * 60.0;
    if (mins < 60)       { std::snprintf(buf, sizeof(buf), "%dm %.0fs", mins, secs); return buf; }
    int hrs = mins / 60;
    std::snprintf(buf, sizeof(buf), "%dh %dm", hrs, mins % 60);
    return buf;
}

std::string JsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// RFC 4180 field escaping. An embedded quote is doubled and the whole field is
// wrapped in quotes, so a tag containing a quote, comma or newline stays inside
// its own field instead of splitting the row. Tags come from asset paths and
// script names, so they routinely contain commas and quotes.
std::string CsvEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 2);
    out += '"';
    for (char c : in) {
        if (c == '"') out += '"';  // doubled
        out += c;
    }
    out += '"';
    return out;
}

// Ensure a path's parent directory exists so an export to "Captures/mem.csv"
// does not fail just because the directory was never made.
bool EnsureParentDir(const std::string& path) {
    try {
        fs::path parent = fs::path(path).parent_path();
        if (parent.empty()) return true;
        std::error_code ec;
        fs::create_directories(parent, ec);
        return true;
    } catch (...) {
        return false;
    }
}

} // anonymous namespace

// ============================================================================
// CVAR SYSTEM
// ============================================================================

CVarSystem& CVarSystem::Instance() {
    static CVarSystem instance;
    return instance;
}

CVar* CVarSystem::RegisterBool(const std::string& name, bool defaultVal, const std::string& desc) {
    std::lock_guard<std::mutex> lock(mutex);
    CVar& cvar = cvars[name];
    cvar.name = name;
    cvar.description = desc;
    cvar.type = CVarType::Bool;
    cvar.boolVal = defaultVal;
    return &cvar;
}

CVar* CVarSystem::RegisterInt(const std::string& name, int defaultVal, const std::string& desc, int min, int max) {
    std::lock_guard<std::mutex> lock(mutex);
    CVar& cvar = cvars[name];
    cvar.name = name;
    cvar.description = desc;
    cvar.type = CVarType::Int;
    cvar.intVal = defaultVal;
    cvar.hasMin = (min != INT_MIN);
    cvar.hasMax = (max != INT_MAX);
    cvar.minVal = (float)min;
    cvar.maxVal = (float)max;
    return &cvar;
}

CVar* CVarSystem::RegisterFloat(const std::string& name, float defaultVal, const std::string& desc, float min, float max) {
    std::lock_guard<std::mutex> lock(mutex);
    CVar& cvar = cvars[name];
    cvar.name = name;
    cvar.description = desc;
    cvar.type = CVarType::Float;
    cvar.floatVal = defaultVal;
    cvar.hasMin = (min != -FLT_MAX);
    cvar.hasMax = (max != FLT_MAX);
    cvar.minVal = min;
    cvar.maxVal = max;
    return &cvar;
}

CVar* CVarSystem::RegisterString(const std::string& name, const std::string& defaultVal, const std::string& desc) {
    std::lock_guard<std::mutex> lock(mutex);
    CVar& cvar = cvars[name];
    cvar.name = name;
    cvar.description = desc;
    cvar.type = CVarType::String;
    cvar.stringVal = defaultVal;
    return &cvar;
}

CVar* CVarSystem::RegisterColor(const std::string& name, Color defaultVal, const std::string& desc) {
    std::lock_guard<std::mutex> lock(mutex);
    CVar& cvar = cvars[name];
    cvar.name = name;
    cvar.description = desc;
    cvar.type = CVarType::Color;
    cvar.colorVal = defaultVal;
    return &cvar;
}

CVar* CVarSystem::Find(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    return (it != cvars.end()) ? &it->second : nullptr;
}

const CVar* CVarSystem::Find(const std::string& name) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    return (it != cvars.end()) ? &it->second : nullptr;
}

bool CVarSystem::SetBool(const std::string& name, bool val) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    if (it == cvars.end() || it->second.type != CVarType::Bool) return false;
    it->second.boolVal = val;
    if (it->second.onChanged) it->second.onChanged(it->second);
    return true;
}

bool CVarSystem::SetInt(const std::string& name, int val) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    if (it == cvars.end() || it->second.type != CVarType::Int) return false;
    if (it->second.hasMin && val < (int)it->second.minVal) val = (int)it->second.minVal;
    if (it->second.hasMax && val > (int)it->second.maxVal) val = (int)it->second.maxVal;
    it->second.intVal = val;
    if (it->second.onChanged) it->second.onChanged(it->second);
    return true;
}

bool CVarSystem::SetFloat(const std::string& name, float val) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    if (it == cvars.end() || it->second.type != CVarType::Float) return false;
    if (it->second.hasMin && val < it->second.minVal) val = it->second.minVal;
    if (it->second.hasMax && val > it->second.maxVal) val = it->second.maxVal;
    it->second.floatVal = val;
    if (it->second.onChanged) it->second.onChanged(it->second);
    return true;
}

bool CVarSystem::SetString(const std::string& name, const std::string& val) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    if (it == cvars.end() || it->second.type != CVarType::String) return false;
    it->second.stringVal = val;
    if (it->second.onChanged) it->second.onChanged(it->second);
    return true;
}

bool CVarSystem::SetColor(const std::string& name, Color val) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    if (it == cvars.end() || it->second.type != CVarType::Color) return false;
    it->second.colorVal = val;
    if (it->second.onChanged) it->second.onChanged(it->second);
    return true;
}

bool CVarSystem::GetBool(const std::string& name, bool fallback) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    return (it != cvars.end() && it->second.type == CVarType::Bool) ? it->second.boolVal : fallback;
}

int CVarSystem::GetInt(const std::string& name, int fallback) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    return (it != cvars.end() && it->second.type == CVarType::Int) ? it->second.intVal : fallback;
}

float CVarSystem::GetFloat(const std::string& name, float fallback) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    return (it != cvars.end() && it->second.type == CVarType::Float) ? it->second.floatVal : fallback;
}

const std::string& CVarSystem::GetString(const std::string& name, const std::string& fallback) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    static std::string empty;
    return (it != cvars.end() && it->second.type == CVarType::String) ? it->second.stringVal : fallback;
}

Color CVarSystem::GetColor(const std::string& name, Color fallback) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = cvars.find(name);
    return (it != cvars.end() && it->second.type == CVarType::Color) ? it->second.colorVal : fallback;
}

void CVarSystem::SaveToFile(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex);
    std::ofstream file(path);
    if (!file) return;
    
    file << "# FlyEngine CVar Config\n";
    file << "# Auto-generated\n\n";
    
    for (const auto& [name, cvar] : cvars) {
        file << name << " = ";
        switch (cvar.type) {
            case CVarType::Bool: file << (cvar.boolVal ? "true" : "false"); break;
            case CVarType::Int: file << cvar.intVal; break;
            case CVarType::Float: file << cvar.floatVal; break;
            case CVarType::String: file << "\"" << cvar.stringVal << "\""; break;
            case CVarType::Color: 
                file << cvar.colorVal.r << " " << cvar.colorVal.g << " " << cvar.colorVal.b << " " << cvar.colorVal.a; 
                break;
        }
        file << "  # " << cvar.description << "\n";
    }
}

void CVarSystem::LoadFromFile(const std::string& path) {
    std::ifstream file(path);
    if (!file) return;
    
    std::string line;
    while (std::getline(file, line)) {
        // Strip comments
        size_t commentPos = line.find('#');
        if (commentPos != std::string::npos) line = line.substr(0, commentPos);
        
        // Trim
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);
        if (line.empty()) continue;
        
        // Parse name = value
        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;
        
        std::string name = line.substr(0, eqPos);
        std::string value = line.substr(eqPos + 1);
        
        name.erase(0, name.find_first_not_of(" \t"));
        name.erase(name.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);
        
        auto it = cvars.find(name);
        if (it == cvars.end()) continue;
        
        CVar& cvar = it->second;
        try {
            switch (cvar.type) {
                case CVarType::Bool: 
                    cvar.boolVal = (value == "true" || value == "1" || value == "yes"); 
                    break;
                case CVarType::Int: 
                    cvar.intVal = std::stoi(value); 
                    break;
                case CVarType::Float: 
                    cvar.floatVal = std::stof(value); 
                    break;
                case CVarType::String: 
                    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                        cvar.stringVal = value.substr(1, value.size() - 2);
                    } else {
                        cvar.stringVal = value;
                    }
                    break;
                case CVarType::Color: {
                    std::istringstream iss(value);
                    int r, g, b, a;
                    if (iss >> r >> g >> b >> a) {
                        cvar.colorVal = {(unsigned char)r, (unsigned char)g, (unsigned char)b, (unsigned char)a};
                    }
                    break;
                }
            }
            if (cvar.onChanged) cvar.onChanged(cvar);
        } catch (...) {}
    }
}

// ============================================================================
// CONSOLE
// ============================================================================

Console& Console::Instance() {
    static Console instance;
    return instance;
}

void Console::Initialize(Engine* eng) {
    engine = eng;
    visible = false;
    inputBuffer.clear();
    history.clear();
    output.clear();
    historyIndex = -1;
    scrollOffset = 0;
    
    RegisterBuiltinCommands(*this, engine, CVarSystem::Instance());
    
    Log("Console initialized. Type 'help' for commands.");
}

void Console::Shutdown() {
    engine = nullptr;
}

void Console::Update(float dt) {
    (void)dt;
    ProcessInput();
}

void Console::ProcessInput() {
    if (!visible) return;
    
    // Handle text input
    int key = GetCharPressed();
    while (key > 0) {
        if (key >= 32 && key <= 125) {
            inputBuffer += (char)key;
        }
        key = GetCharPressed();
    }
    
    if (IsKeyPressed(KEY_BACKSPACE) && !inputBuffer.empty()) {
        inputBuffer.pop_back();
    }
    
    if (IsKeyPressed(KEY_ENTER)) {
        if (!inputBuffer.empty()) {
            ExecuteCommand(inputBuffer);
            history.push_front(inputBuffer);
            if (history.size() > 100) history.pop_back();
            historyIndex = -1;
            inputBuffer.clear();
        }
    }
    
    // History navigation
    if (IsKeyPressed(KEY_UP)) {
        if (historyIndex < (int)history.size() - 1) {
            historyIndex++;
            inputBuffer = history[historyIndex];
        }
    }
    if (IsKeyPressed(KEY_DOWN)) {
        if (historyIndex > 0) {
            historyIndex--;
            inputBuffer = history[historyIndex];
        } else if (historyIndex == 0) {
            historyIndex = -1;
            inputBuffer.clear();
        }
    }
    
    // Tab completion
    if (IsKeyPressed(KEY_TAB)) {
        RebuildCompletions();
        if (!completions.empty()) {
            inputBuffer = completions[completionIndex];
            completionIndex = (completionIndex + 1) % completions.size();
        }
    }
    
    // Scroll output
    if (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) {
        if (IsKeyPressed(KEY_UP)) scrollOffset = std::max(0, scrollOffset - 1);
        if (IsKeyPressed(KEY_DOWN)) scrollOffset = std::min((int)output.size() - 1, scrollOffset + 1);
    }
}

void Console::ExecuteCommand(const std::string& cmdLine) {
    auto tokens = Tokenize(cmdLine);
    if (tokens.empty()) return;
    
    std::string cmdName = tokens[0];
    std::vector<std::string> args(tokens.begin() + 1, tokens.end());
    
    auto it = commands.find(cmdName);
    if (it != commands.end()) {
        AddOutput("> " + cmdLine, YELLOW);
        bool success = it->second.handler(args);
        if (!success && !it->second.usage.empty()) {
            AddOutput("Usage: " + it->second.usage, RED);
        }
    } else {
        // Try as cvar
        std::string cvarName = tokens[0];
        if (tokens.size() == 1) {
            // Get cvar
            CVar* cvar = CVarSystem::Instance().Find(cvarName);
            if (cvar) {
                std::string valStr;
                switch (cvar->type) {
                    case CVarType::Bool: valStr = cvar->boolVal ? "true" : "false"; break;
                    case CVarType::Int: valStr = std::to_string(cvar->intVal); break;
                    case CVarType::Float: valStr = std::to_string(cvar->floatVal); break;
                    case CVarType::String: valStr = "\"" + cvar->stringVal + "\""; break;
                    case CVarType::Color: valStr = std::to_string(cvar->colorVal.r) + " " + std::to_string(cvar->colorVal.g) + " " + std::to_string(cvar->colorVal.b) + " " + std::to_string(cvar->colorVal.a); break;
                }
                AddOutput(cvarName + " = " + valStr, CYAN);
            } else {
                AddOutput("Unknown command: " + cmdName, RED);
            }
        } else if (tokens.size() >= 2) {
            // Set cvar
            CVar* cvar = CVarSystem::Instance().Find(cvarName);
            if (cvar) {
                std::string value = tokens[1];
                bool ok = false;
                switch (cvar->type) {
                    case CVarType::Bool: ok = CVarSystem::Instance().SetBool(cvarName, value == "true" || value == "1"); break;
                    case CVarType::Int: ok = CVarSystem::Instance().SetInt(cvarName, std::stoi(value)); break;
                    case CVarType::Float: ok = CVarSystem::Instance().SetFloat(cvarName, std::stof(value)); break;
                    case CVarType::String: ok = CVarSystem::Instance().SetString(cvarName, value); break;
                }
                if (ok) {
                    AddOutput(cvarName + " = " + value, GREEN);
                } else {
                    AddOutput("Failed to set " + cvarName, RED);
                }
            } else {
                AddOutput("Unknown cvar: " + cvarName, RED);
            }
        }
    }
}

void Console::Draw() {
    if (!visible) return;
    
    int screenW = GetScreenWidth();
    int screenH = GetScreenHeight();
    int consoleH = screenH / 2;
    
    // Background
    DrawRectangle(0, 0, screenW, consoleH, {0, 0, 0, 200});
    DrawRectangle(0, consoleH, screenW, 1, {100, 100, 100, 255});
    
    // Output log (scrollable)
    int lineH = 20;
    int maxLines = (consoleH - 40) / lineH;
    int startIdx = std::max(0, (int)output.size() - maxLines - scrollOffset);
    int endIdx = std::min((int)output.size(), startIdx + maxLines);
    
    for (int i = startIdx; i < endIdx; i++) {
        int y = 5 + (i - startIdx) * lineH;
        DrawText(output[i].c_str(), 10, y, 16, WHITE);
    }
    
    // Input line
    int inputY = consoleH - 25;
    DrawRectangle(5, inputY, screenW - 10, 22, {30, 30, 30, 255});
    DrawText("> ", 10, inputY + 2, 18, GREEN);
    DrawText(inputBuffer.c_str(), 35, inputY + 2, 18, WHITE);
    
    // Cursor
    if ((GetTime() * 2.0f - (int)(GetTime() * 2.0f)) > 0.5f) {
        int textWidth = MeasureText(inputBuffer.c_str(), 18);
        DrawText("_", 35 + textWidth, inputY + 2, 18, WHITE);
    }
    
    // Completions
    if (!completions.empty()) {
        int compY = inputY - completions.size() * 20 - 5;
        for (size_t i = 0; i < completions.size(); i++) {
            Color c = (i == completionIndex) ? YELLOW : WHITE;
            DrawText(completions[i].c_str(), 10, compY + i * 20, 16, c);
        }
    }
}

void Console::RegisterCommand(const ConsoleCommand& cmd) {
    commands[cmd.name] = cmd;
}

void Console::Log(const std::string& msg) {
    AddOutput(msg, WHITE);
}

void Console::LogError(const std::string& msg) {
    AddOutput("[ERROR] " + msg, RED);
}

void Console::LogWarn(const std::string& msg) {
    AddOutput("[WARN] " + msg, YELLOW);
}

void Console::AddOutput(const std::string& msg, Color color) {
    // Split multi-line
    std::istringstream iss(msg);
    std::string line;
    while (std::getline(iss, line)) {
        output.push_back(line);
        if (output.size() > 1000) output.pop_front();
    }
    scrollOffset = 0;
}

std::vector<std::string> Console::Tokenize(const std::string& str) {
    std::vector<std::string> tokens;
    std::string token;
    bool inQuotes = false;
    
    for (size_t i = 0; i < str.size(); i++) {
        char c = str[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == ' ' && !inQuotes) {
            if (!token.empty()) {
                tokens.push_back(token);
                token.clear();
            }
        } else {
            token += c;
        }
    }
    if (!token.empty()) tokens.push_back(token);
    return tokens;
}

void Console::RebuildCompletions() {
    completions.clear();
    completionIndex = 0;
    
    auto tokens = Tokenize(inputBuffer);
    if (tokens.empty()) return;
    
    std::string partial = tokens.back();
    
    // Complete commands
    for (const auto& [name, cmd] : commands) {
        if (name.rfind(partial, 0) == 0) {
            completions.push_back(name);
        }
    }
    // Complete cvars
    for (const auto& [name, cvar] : CVarSystem::Instance().GetAll()) {
        if (name.rfind(partial, 0) == 0) {
            completions.push_back(name);
        }
    }
    std::sort(completions.begin(), completions.end());
}

std::vector<std::string> Console::GetCompletions(const std::string& partial) const {
    std::vector<std::string> result;
    for (const auto& [name, cmd] : commands) {
        if (name.rfind(partial, 0) == 0) result.push_back(name);
    }
    for (const auto& [name, cvar] : CVarSystem::Instance().GetAll()) {
        if (name.rfind(partial, 0) == 0) result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

void RegisterBuiltinCommands(Console& console, Engine* engine, CVarSystem& cvars) {
    // Help
    console.Register(
        "help",
        "Show available commands",
        "help [command]",
        [&](const std::vector<std::string>& args) {
            if (args.empty()) {
                console.AddOutput("Available commands:", CYAN);
                for (const auto& [name, cmd] : console.commands) {
                    console.AddOutput("  " + name + " - " + cmd.description, WHITE);
                }
                console.AddOutput("\nCVars (get: name, set: name value):", CYAN);
                for (const auto& [name, cvar] : cvars.GetAll()) {
                    console.AddOutput("  " + name + " - " + cvar.description, WHITE);
                }
            } else {
                auto it = console.commands.find(args[0]);
                if (it != console.commands.end()) {
                    console.AddOutput(it->first + " - " + it->second.description, CYAN);
                    console.AddOutput("Usage: " + it->second.usage, WHITE);
                } else {
                    console.AddOutput("Unknown command: " + args[0], RED);
                }
            }
            return true;
        }
    );
    
    // Clear
    console.Register(
        "clear",
        "Clear console output",
        "clear",
        [&](const std::vector<std::string>&) {
            console.Clear();
            return true;
        }
    );
    
    // Echo
    console.Register(
        "echo",
        "Print message to console",
        "echo <message>",
        [&](const std::vector<std::string>& args) {
            std::string msg;
            for (const auto& arg : args) msg += arg + " ";
            console.AddOutput(msg, WHITE);
            return true;
        }
    );
    
    // Cvar list
    console.Register(
        "cvars",
        "List all console variables",
        "cvars [filter]",
        [&](const std::vector<std::string>& args) {
            std::string filter = args.empty() ? "" : args[0];
            for (const auto& [name, cvar] : cvars.GetAll()) {
                if (filter.empty() || name.find(filter) != std::string::npos) {
                    std::string valStr;
                    switch (cvar.type) {
                        case CVarType::Bool: valStr = cvar.boolVal ? "true" : "false"; break;
                        case CVarType::Int: valStr = std::to_string(cvar.intVal); break;
                        case CVarType::Float: valStr = std::to_string(cvar.floatVal); break;
                        case CVarType::String: valStr = "\"" + cvar.stringVal + "\""; break;
                        case CVarType::Color: valStr = std::to_string(cvar.colorVal.r) + " " + std::to_string(cvar.colorVal.g) + " " + std::to_string(cvar.colorVal.b) + " " + std::to_string(cvar.colorVal.a); break;
                    }
                    console.AddOutput(name + " = " + valStr + "  # " + cvar.description, CYAN);
                }
            }
            return true;
        }
    );
    
    // Save/load config
    console.Register(
        "config_save",
        "Save cvars to config file",
        "config_save [path]",
        [&](const std::vector<std::string>& args) {
            std::string path = args.empty() ? "config.cfg" : args[0];
            cvars.SaveToFile(path);
            console.AddOutput("Config saved to " + path, GREEN);
            return true;
        }
    );
    
    console.Register(
        "config_load",
        "Load cvars from config file",
        "config_load [path]",
        [&](const std::vector<std::string>& args) {
            std::string path = args.empty() ? "config.cfg" : args[0];
            cvars.LoadFromFile(path);
            console.AddOutput("Config loaded from " + path, GREEN);
            return true;
        }
    );
    
    // Quit
    console.Register(
        "quit",
        "Quit the application",
        "quit",
        [&](const std::vector<std::string>&) {
            if (engine) {
                // Signal engine to close
                CloseWindow();
            }
            return true;
        }
    );
    
    // Entity spawn
    console.Register(
        "spawn",
        "Spawn an entity",
        "spawn <cube|sphere|cylinder|plane> [size] [r g b a]",
        [&](const std::vector<std::string>& args) {
            if (args.empty()) return false;
            // Implementation would create entity via engine
            console.AddOutput("Spawn not fully implemented yet", YELLOW);
            return true;
        }
    );
    
    // Time scale. Note the "timescale" command above writes through
    // Engine::SetTimeScale directly and does not go via this cvar, so the two
    // are independent: this one only takes effect from a config file.
    cvars.RegisterFloat("timescale", 1.0f, "Global time scale (0.01-10)", 0.01f, 10.0f);
    // -9.81 is what the engine has always simulated (and what Box3DWrapper and
    // CharacterController still hardcode). This cvar advertised -19.62 while
    // nothing read it, so `config_save` would have written a default that
    // contradicted the running sim.
    cvars.RegisterFloat("physics_gravity", -9.81f, "Physics gravity (m/s^2, 0 = none)", -50.0f, 0.0f);
    
    // --- Physics debug ------------------------------------------------------
    //
    // The F2 panel is the primary way to pick layers, but the player build can
    // run without a panel, and a layer you can only reach through a mouse is a
    // layer you cannot use from a script or a headless capture. These also keep
    // the per-layer Toggle* methods reachable.

    // Pause, single-step and time scale. The keys (P and .) are the fast path,
    // but a key you have to hold the right modifier over is not reachable from a
    // script, a headless capture, or the console while the game has focus --
    // and pausing is exactly the thing you want reachable from a capture
    // harness, so a recording can hold on an interesting frame.

    console.Register(
        "pause",
        "Freeze entity updates (physics, controller, scripts); rendering continues",
        "pause [on|off|toggle]",
        [&](const std::vector<std::string>& args) -> bool {
            Engine& e = *engine;
            if (args.empty()) {
                e.TogglePause();
            } else if (args[0] == "on") {
                e.SetPaused(true);
            } else if (args[0] == "off") {
                e.SetPaused(false);
            } else if (args[0] != "toggle") {
                console.AddOutput("pause: expected 'on', 'off' or 'toggle'", RED);
                return false;
            }
            console.AddOutput(e.IsPaused() ? "Paused (use 'step' to advance one frame)"
                                           : "Resumed", YELLOW);
            return true;
        }
    );

    console.Register(
        "step",
        "Advance exactly one frame while paused (implies pause if running)",
        "step [frames]",
        [&](const std::vector<std::string>& args) -> bool {
            Engine& e = *engine;
            int n = 1;
            if (!args.empty()) {
                try {
                    n = std::stoi(args[0]);
                } catch (...) {
                    console.AddOutput("step: expected a frame count", RED);
                    return false;
                }
                if (n < 1 || n > 60) {
                    console.AddOutput("step: frame count must be 1..60", RED);
                    return false;
                }
            }
            // Stepping a running engine would be invisible -- the world is
            // already advancing -- so pause first. Engine::StepFrame caps the
            // backlog at the same 60.
            e.SetPaused(true);
            for (int i = 0; i < n; ++i) e.StepFrame();
            console.AddOutput("Stepping " + std::to_string(n) +
                              (n == 1 ? " frame" : " frames"), YELLOW);
            return true;
        }
    );

    console.Register(
        "timescale",
        "Global multiplier on the dt handed to entities (0 = frozen, 1 = normal)",
        "timescale [x]",
        [&](const std::vector<std::string>& args) -> bool {
            Engine& e = *engine;
            if (args.empty()) {
                console.AddOutput("timescale: " + std::to_string(e.GetTimeScale()), WHITE);
                return true;
            }
            float v;
            try {
                v = std::stof(args[0]);
            } catch (...) {
                console.AddOutput("timescale: expected a number, e.g. 0.5", RED);
                return false;
            }
            e.SetTimeScale(v);
            // Report what was actually stored, not what was asked for, so a
            // clamped value is visible rather than silently different.
            char buf[64];
            std::snprintf(buf, sizeof(buf), "timescale: %.2fx (clamped to 0..10)", e.GetTimeScale());
            console.AddOutput(buf, WHITE);
            return true;
        }
    );

    console.Register(
        "phys_debug",
        "Show/hide the physics debug overlay (F2)",
        "phys_debug [on|off]",
        [&](const std::vector<std::string>& args) -> bool {
            PhysicsDebugVisualizer& pd = PHYSICS_DEBUG;
            if (!args.empty()) {
                if (args[0] == "on") {
                    pd.SetVisible(true);
                } else if (args[0] == "off") {
                    pd.SetVisible(false);
                } else {
                    console.AddOutput("phys_debug: expected 'on' or 'off'", RED);
                    return false;
                }
            } else {
                pd.Toggle();
            }
            console.AddOutput(std::string("Physics debug overlay ") +
                              (pd.IsVisible() ? "shown" : "hidden"), WHITE);
            return true;
        }
    );

    // One command per layer, named after the layer, so `phys_contacts` reads as
    // what it does and tab-completion lists them together.
    struct LayerCommand {
        const char* name;
        const char* help;
        void (PhysicsDebugVisualizer::*toggle)();
        bool PhysicsDebugVisualizer::*flag;
    };
    const LayerCommand layerCommands[] = {
        { "phys_shapes",    "Toggle collision shape wireframes",   &PhysicsDebugVisualizer::ToggleShapes,    &PhysicsDebugVisualizer::showShapes },
        { "phys_velocities","Toggle velocity vectors",             &PhysicsDebugVisualizer::ToggleVelocities,&PhysicsDebugVisualizer::showVelocities },
        { "phys_contacts",  "Toggle contact points and normals",   &PhysicsDebugVisualizer::ToggleContacts,  &PhysicsDebugVisualizer::showContacts },
        { "phys_aabbs",     "Toggle object AABBs",                 &PhysicsDebugVisualizer::ToggleAABBs,     &PhysicsDebugVisualizer::showAABBs },
        { "phys_joints",    "Toggle joint segments",               &PhysicsDebugVisualizer::ToggleJoints,    &PhysicsDebugVisualizer::showJoints },
        { "phys_raycasts",  "Toggle debug raycasts",               &PhysicsDebugVisualizer::ToggleRaycasts,  &PhysicsDebugVisualizer::showRaycasts },
    };
    for (const LayerCommand& lc : layerCommands) {
        console.Register(
            lc.name,
            lc.help,
            "",
            [lc, &console](const std::vector<std::string>&) -> bool {
                PhysicsDebugVisualizer& pd = PHYSICS_DEBUG;
                // Turning on a layer from the console implies you want to see
                // it, so reveal the overlay rather than setting a flag on a
                // hidden tool and reporting nothing. SetVisible, not Toggle, so
                // asking for one layer does not also switch on the defaults.
                pd.SetVisible(true);
                (pd.*(lc.toggle))();
                console.AddOutput(std::string(lc.name) + ": " +
                                  (pd.*(lc.flag) ? "on" : "off"), WHITE);
                return true;
            }
        );
    }

    // --- Memory tracker -----------------------------------------------------
    
    // mem_stats
    console.Register(
        "mem_stats",
        "Print tracked memory totals, per-category usage and process RSS",
        "mem_stats",
        [&](const std::vector<std::string>&) {
            MemoryTracker& mem = MEMORY_TRACKER;
            console.AddOutput("--- Memory ---", CYAN);
            console.AddOutput("Tracked live:   " + FormatBytes(mem.GetCurrentAllocated()) +
                              "  (peak " + FormatBytes(mem.GetPeakAllocated()) + ")", WHITE);
            console.AddOutput("Cumulative:     " + FormatBytes(mem.GetTotalAllocated()) +
                              " allocated, " + FormatBytes(mem.GetTotalFreed()) + " freed", WHITE);
            console.AddOutput("Allocations:    " + std::to_string(mem.GetAllocationCount()) +
                              " live, " + std::to_string(mem.GetTotalAllocCount()) + " total (" +
                              std::to_string(mem.GetTotalFreeCount()) + " freed)", WHITE);
            size_t rss = MemoryTracker::GetProcessMemoryBytes();
            if (rss > 0) {
                std::string line = "Process RSS:    " + FormatBytes(rss);
                size_t peak = MemoryTracker::GetProcessPeakMemoryBytes();
                if (peak > 0) line += "  (peak " + FormatBytes(peak) + ")";
                if (rss > mem.GetCurrentAllocated()) {
                    line += "  | untracked: " + FormatBytes(rss - mem.GetCurrentAllocated());
                }
                console.AddOutput(line, WHITE);
            } else {
                console.AddOutput("Process RSS:    not available on this platform", DARKGRAY);
            }
            if (!mem.GetCategoryUsage().empty()) {
                console.AddOutput("By category:", CYAN);
                for (const auto& [category, bytes] : mem.GetCategoryUsage()) {
                    console.AddOutput("  " + category + ": " + FormatBytes(bytes), WHITE);
                }
            }
            for (const auto& over : mem.GetOverBudgetCategories()) {
                console.AddOutput("  OVER BUDGET: " + over, RED);
            }
            return true;
        }
    );
    
    // mem_leaks
    console.Register(
        "mem_leaks",
        "Report allocations still live, relative to the leak baseline",
        "mem_leaks [set_baseline|clear_baseline]",
        [&](const std::vector<std::string>& args) {
            MemoryTracker& mem = MEMORY_TRACKER;
            if (!args.empty() && args[0] == "set_baseline") {
                mem.MarkLeakBaseline();
                return true;
            }
            if (!args.empty() && args[0] == "clear_baseline") {
                mem.ClearLeakBaseline();
                console.AddOutput("Leak baseline cleared", GREEN);
                return true;
            }
            MemoryTracker::LeakReport report = mem.DetectLeaks();
            if (!mem.HasLeakBaseline()) {
                console.AddOutput("No baseline set -- listing all live allocations.", YELLOW);
            }
            if (report.leaks.empty()) {
                console.AddOutput("No leaks detected.", GREEN);
                return true;
            }
            console.AddOutput("Leaks: " + FormatBytes(report.totalLeakedBytes) + " in " +
                              std::to_string(report.leaks.size()) + " allocation(s)", RED);
            for (const auto& leak : report.leaks) {
                char ptrBuf[32];
                std::snprintf(ptrBuf, sizeof(ptrBuf), "0x%llx", (unsigned long long)(uintptr_t)leak.ptr);
                console.AddOutput("  " + FormatBytes(leak.size) + "  [" + leak.category + "]  " +
                                  (leak.tag.empty() ? "<untagged>" : leak.tag) + "  " + ptrBuf +
                                  "  frame " + std::to_string(leak.frameNumber), WHITE);
            }
            return true;
        }
    );
    
    // mem_budget
    console.Register(
        "mem_budget",
        "Show or set a per-category memory budget (0 to clear)",
        "mem_budget <category> [megabytes]",
        [&](const std::vector<std::string>& args) {
            if (args.empty()) {
                MemoryTracker& mem = MEMORY_TRACKER;
                if (mem.GetBudgets().empty()) {
                    console.AddOutput("No budgets set.", YELLOW);
                    return true;
                }
                for (const auto& [category, budget] : mem.GetBudgets()) {
                    console.AddOutput(category + ": " + FormatBytes(mem.GetCurrentUsage(category)) +
                                      " / " + FormatBytes(budget) + "  (" +
                                      std::to_string((int)mem.GetBudgetUsagePercent(category)) + "%)", WHITE);
                }
                return true;
            }
            if (args.size() < 2) return false;
            double mb = 0;
            try {
                mb = std::stod(args[1]);
            } catch (...) {
                console.AddOutput("Not a number: " + args[1], RED);
                return false;
            }
            if (mb < 0) {
                console.AddOutput("Budget must be >= 0", RED);
                return false;
            }
            MEMORY_TRACKER.SetBudget(args[0], (size_t)(mb * 1024.0 * 1024.0));
            if (mb == 0) {
                console.AddOutput("Budget cleared for " + args[0], GREEN);
            } else {
                console.AddOutput("Budget for " + args[0] + " set to " + FormatBytes((size_t)(mb * 1024.0 * 1024.0)), GREEN);
            }
            return true;
        }
    );
    
    // mem_snapshot
    console.Register(
        "mem_snapshot",
        "Take a memory snapshot, or diff against the stored baseline",
        "mem_snapshot [take|baseline|diff]",
        [&](const std::vector<std::string>& args) {
            MemoryTracker& mem = MEMORY_TRACKER;
            std::string action = args.empty() ? "take" : args[0];
            if (action == "take") {
                mem.SetSnapshot(mem.TakeSnapshot());
                console.AddOutput("Snapshot taken at frame " + std::to_string(mem.GetFrameNumber()) +
                                  " with " + std::to_string(mem.GetAllocationCount()) + " live allocations", GREEN);
                return true;
            }
            if (action == "baseline") {
                mem.SetSnapshot(mem.TakeSnapshot());
                console.AddOutput("Snapshot stored as baseline. Run 'mem_snapshot diff' later.", GREEN);
                return true;
            }
            if (action == "diff") {
                MemoryTracker::DiffResult diff = mem.DiffSnapshots(mem.GetSnapshot(), mem.TakeSnapshot());
                console.AddOutput("Since snapshot: net ", CYAN);
                if (diff.netBytes > 0) console.AddOutput(FormatBytes((size_t)diff.netBytes), YELLOW);
                else if (diff.netBytes < 0) console.AddOutput("-" + FormatBytes((size_t)(-diff.netBytes)), GREEN);
                else console.AddOutput("0 B", GREEN);
                console.AddOutput(std::to_string(diff.newAllocs.size()) + " new, " +
                                  std::to_string(diff.freedAllocs.size()) + " freed, " +
                                  std::to_string(diff.grownAllocs.size()) + " grown, " +
                                  std::to_string(diff.shrunkAllocs.size()) + " shrunk", WHITE);
                for (size_t i = 0; i < diff.newAllocs.size() && i < 10; ++i) {
                    const auto& alloc = diff.newAllocs[i];
                    console.AddOutput("  + " + FormatBytes(alloc.size) + " [" + alloc.category + "] " +
                                      (alloc.tag.empty() ? "<untagged>" : alloc.tag), WHITE);
                }
                for (size_t i = 0; i < diff.freedAllocs.size() && i < 10; ++i) {
                    const auto& alloc = diff.freedAllocs[i];
                    console.AddOutput("  - " + FormatBytes(alloc.size) + " [" + alloc.category + "] " +
                                      (alloc.tag.empty() ? "<untagged>" : alloc.tag), WHITE);
                }
                return true;
            }
            return false;
        }
    );
    
    // mem_export
    console.Register(
        "mem_export",
        "Export tracked allocations to a file (.csv or .json by extension)",
        "mem_export <path>",
        [&](const std::vector<std::string>& args) {
            if (args.empty()) return false;
            const std::string& path = args[0];
            std::string ext = fs::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return (char)std::tolower(c); });
            if (ext == ".json") {
                MEMORY_TRACKER.ExportJSON(path);
            } else if (ext == ".csv") {
                MEMORY_TRACKER.ExportCSV(path);
            } else {
                console.AddOutput("Unknown extension '" + ext + "' -- expected .csv or .json", RED);
                return false;
            }
            return true;
        }
    );
    
    // mem_clear / mem_reset
    console.Register(
        "mem_clear",
        "Forget all tracked allocations (frees nothing)",
        "mem_clear",
        [&](const std::vector<std::string>&) {
            MEMORY_TRACKER.ClearTracked();
            return true;
        }
    );
    
    console.Register(
        "mem_reset",
        "Zero the cumulative memory counters and peaks",
        "mem_reset",
        [&](const std::vector<std::string>&) {
            MEMORY_TRACKER.ResetCounters();
            return true;
        }
    );
    
    // mem_track
    console.Register(
        "mem_track",
        "Enable or disable allocation tracking",
        "mem_track [on|off]",
        [&](const std::vector<std::string>& args) {
            MemoryTracker& mem = MEMORY_TRACKER;
            if (args.empty()) {
                console.AddOutput(std::string("Tracking is ") +
                                  (mem.IsTrackingEnabled() ? "enabled" : "disabled"), CYAN);
                return true;
            }
            if (args[0] == "on")  mem.SetTrackingEnabled(true);
            else if (args[0] == "off") mem.SetTrackingEnabled(false);
            else return false;
            console.AddOutput("Memory tracking " + args[0], GREEN);
            return true;
        }
    );
}

// ============================================================================
// PROFILER
// ============================================================================

Profiler& Profiler::Instance() {
    static Profiler instance;
    return instance;
}

Profiler::Scope::Scope(const char* name) : name(name) {
    Profiler::Instance().BeginScope(name);
}

Profiler::Scope::~Scope() {
    Profiler::Instance().EndScope(name);
}

uint64_t Profiler::GetCycles() const {
    // Use high-resolution timer
    return (uint64_t)(GetTime() * 1e9);
}

double Profiler::CyclesToMs(uint64_t cycles) const {
    return (double)cycles / 1e6;
}

void Profiler::BeginFrame() {
    if (!enabled_) return;
    frameStartCycle = GetCycles();
    activeZones.clear();
    zoneData.clear();
}

void Profiler::EndFrame() {
    if (!enabled_) return;
    uint64_t endCycle = GetCycles();
    frameTimeMs = CyclesToMs(endCycle - frameStartCycle);
    
    // Update histories
    frameTimeHistory.push_back(frameTimeMs);
    if (frameTimeHistory.size() > MAX_HISTORY) frameTimeHistory.erase(frameTimeHistory.begin());
    
    for (auto& [name, data] : zoneData) {
        auto& hist = zoneHistories[name];
        hist.push_back(data.totalMs);
        if (hist.size() > MAX_HISTORY) hist.erase(hist.begin());
    }
    
    frameCount++;
}

void Profiler::BeginScope(const char* name) {
    if (!enabled_) return;
    
    ActiveZone zone;
    zone.name = name;
    zone.startCycle = GetCycles();
    zone.depth = (int)activeZones.size();
    zone.parentSelfMs = activeZones.empty() ? 0 : activeZones.back().parentSelfMs;
    activeZones.push_back(zone);
}

void Profiler::EndScope(const char* name) {
    if (!enabled_ || activeZones.empty()) return;
    
    uint64_t endCycle = GetCycles();
    ActiveZone zone = activeZones.back();
    activeZones.pop_back();
    
    double elapsedMs = CyclesToMs(endCycle - zone.startCycle);
    
    ZoneData& data = zoneData[zone.name];
    data.name = zone.name;
    data.totalMs += elapsedMs;
    data.minMs = std::min(data.minMs, elapsedMs);
    data.maxMs = std::max(data.maxMs, elapsedMs);
    data.count++;
    data.depth = zone.depth;
    data.selfMs += elapsedMs;
    
    // Subtract from parent's self time
    if (!activeZones.empty()) {
        activeZones.back().parentSelfMs += elapsedMs;
    }
}

const std::vector<double>& Profiler::GetZoneHistory(const std::string& name) const {
    static std::vector<double> empty;
    auto it = zoneHistories.find(name);
    return (it != zoneHistories.end()) ? it->second : empty;
}

// ============================================================================
// ENTITY INSPECTOR
// ============================================================================

EntityInspector& EntityInspector::Instance() {
    static EntityInspector instance;
    return instance;
}

void EntityInspector::Initialize(Engine* eng) {
    engine = eng;
    visible = false;
    selectedEntity = nullptr;
    hoveredEntity = nullptr;
}

void EntityInspector::Update(float dt) {
    (void)dt;
    if (!visible) return;
    
    // Right-click to pick entity
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        PickEntityUnderMouse();
    }
}

void EntityInspector::PickEntityUnderMouse() {
    if (!engine) return;
    
    // Get camera ray
    Ray ray = GetMouseRay(GetMousePosition(), engine->GetCamera());
    
    float closestDist = FLT_MAX;
    Entity* hitEntity = nullptr;
    
    for (auto& e : engine->GetEntities()) {
        if (auto* obj = dynamic_cast<ScatteredObject*>(e.get())) {
            BoundingBox box = obj->GetBoundingBox();
            RayCollision collision = GetRayCollisionBox(ray, box);
            if (collision.hit && collision.distance < closestDist) {
                closestDist = collision.distance;
                hitEntity = obj;
            }
        }
    }
    
    if (hitEntity) {
        SelectEntity(hitEntity);
    }
}

void EntityInspector::SelectEntity(Entity* entity) {
    selectedEntity = entity;
}

void EntityInspector::Draw() {
    if (!visible) return;
    DrawInspectorPanel();
}

void EntityInspector::DrawInspectorPanel() {
    if (!engine || !selectedEntity) return;
    
    ImGui::Begin("Entity Inspector", &visible, ImGuiWindowFlags_AlwaysAutoResize);
    
    ImGui::Text("Entity Inspector");
    ImGui::Separator();
    
    if (auto* obj = dynamic_cast<ScatteredObject*>(selectedEntity)) {
        // Name
        static char nameBuf[256];
        strncpy(nameBuf, obj->GetName().c_str(), sizeof(nameBuf) - 1);
        if (ImGui::InputText("Name", nameBuf, sizeof(nameBuf))) {
            obj->SetName(nameBuf);
        }
        
        // Script (used as tag)
        static char scriptBuf[256];
        strncpy(scriptBuf, obj->script.c_str(), sizeof(scriptBuf) - 1);
        if (ImGui::InputText("Script", scriptBuf, sizeof(scriptBuf))) {
            obj->script = scriptBuf;
        }
        
        ImGui::Separator();
        ImGui::Text("Transform");
        
        // Position
        Vector3* pos = obj->GetPosPtr();
        if (pos) {
            float posArr[3] = {pos->x, pos->y, pos->z};
            if (ImGui::DragFloat3("Position", posArr, 0.1f)) {
                *pos = {posArr[0], posArr[1], posArr[2]};
            }
        }
        
        // Rotation
        Vector3* rot = obj->GetRotationPtr();
        if (rot) {
            float rotArr[3] = {rot->x, rot->y, rot->z};
            if (ImGui::DragFloat3("Rotation", rotArr, 1.0f)) {
                *rot = {rotArr[0], rotArr[1], rotArr[2]};
            }
        }
        
        // Scale
        Vector3* scale = obj->GetSizePtr();
        if (scale) {
            float scaleArr[3] = {scale->x, scale->y, scale->z};
            if (ImGui::DragFloat3("Scale", scaleArr, 0.1f, 0.01f, 100.0f)) {
                *scale = {scaleArr[0], scaleArr[1], scaleArr[2]};
            }
        }
        
        // Color
        Color* col = obj->GetColorPtr();
        if (col) {
            float colArr[4] = {col->r / 255.0f, col->g / 255.0f, col->b / 255.0f, col->a / 255.0f};
            if (ImGui::ColorEdit4("Color", colArr)) {
                *col = {(unsigned char)(colArr[0] * 255), (unsigned char)(colArr[1] * 255), 
                        (unsigned char)(colArr[2] * 255), (unsigned char)(colArr[3] * 255)};
            }
        }
        
        // Physics
        ImGui::Separator();
        ImGui::Text("Physics");
        // Find physics simulation
        phys::Simulation* sim = nullptr;
        if (engine) {
            for (auto& e : engine->GetEntities()) {
                if (auto* s = dynamic_cast<phys::Simulation*>(e.get())) {
                    sim = s;
                    break;
                }
            }
        }
        bool hasBody = sim && sim->HasBody(obj);
        ImGui::Text("Has Body: %s", hasBody ? "Yes" : "No");
        if (hasBody) {
            Vector3 vel = obj->GetVelocity();
            ImGui::Text("Velocity: %.2f, %.2f, %.2f", vel.x, vel.y, vel.z);
        }
        
        // Model
        ImGui::Separator();
        ImGui::Text("Model");
        const Model& model = obj->GetModel();
        ImGui::Text("Meshes: %d", model.meshCount);
        ImGui::Text("Materials: %d", model.materialCount);
        ImGui::Text("Model Path: %s", obj->GetModelPath().c_str());
    }
    
    ImGui::End();
}

// ============================================================================
// PHYSICS DEBUG VISUALIZER
// ============================================================================

PhysicsDebugVisualizer& PhysicsDebugVisualizer::Instance() {
    static PhysicsDebugVisualizer instance;
    return instance;
}

void PhysicsDebugVisualizer::Initialize(Engine* eng) {
    engine = eng;
}

namespace {

// raylib's DrawCube/DrawSphere/DrawCylinder are lit solids, which is the wrong
// tool here: a solid hides the interior of the shape and the overlap with its
// neighbour, which is exactly what you are trying to see. So the overlay draws
// edges instead.

// The eight world-space corners of a box of the given half-extents, rotated
// and centred at `center`. Shared by the wireframe and the AABB pass so both
// describe the same solid.
void CornersOfBox(const Vector3& center, const Vector3& half, const Quaternion& rot, Vector3 out[8]) {
    for (int i = 0; i < 8; ++i) {
        const Vector3 corner = {
            (i & 1) ?  half.x : -half.x,
            (i & 2) ?  half.y : -half.y,
            (i & 4) ?  half.z : -half.z,
        };
        out[i] = Vector3Add(center, Vector3RotateByQuaternion(corner, rot));
    }
}

void DrawWireBox(const Vector3& center, const Vector3& half, const Quaternion& rot, Color color) {
    Vector3 world[8];
    CornersOfBox(center, half, rot, world);
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},   // -Z face
        {4,5},{5,6},{6,7},{7,4},   // +Z face
        {0,4},{1,5},{2,6},{3,7},   // verticals
    };
    for (const auto& e : edges) {
        DrawLine3D(world[e[0]], world[e[1]], color);
    }
}

// World-axis-aligned bounds of that same box. Computed here rather than via
// ScatteredObject::GetBoundingBox() because that reads the object's own euler
// rotation, which WriteBack() does not sync during play.
BoundingBox WorldAabb(const Vector3& center, const Vector3& half, const Quaternion& rot) {
    Vector3 world[8];
    CornersOfBox(center, half, rot, world);
    BoundingBox bb;
    bb.min = {  1e30f,  1e30f,  1e30f };
    bb.max = { -1e30f, -1e30f, -1e30f };
    for (int i = 0; i < 8; ++i) {
        bb.min = Vector3Min(bb.min, world[i]);
        bb.max = Vector3Max(bb.max, world[i]);
    }
    return bb;
}

void DrawWireSphere(const Vector3& center, float radius, Color color) {
    // Three orthogonal great circles, which read as a sphere without needing a
    // tessellated surface.
    constexpr int kSegments = 24;
    const Vector3 axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    for (const Vector3& axis : axes) {
        // Any vector not parallel to the axis, to span the circle's plane.
        const Vector3 u = (axis.x != 0.0f) ? Vector3{0,1,0} : Vector3{1,0,0};
        const Vector3 e1 = Vector3Normalize(Vector3CrossProduct(axis, u));
        const Vector3 e2 = Vector3CrossProduct(axis, e1);
        Vector3 prev = Vector3Add(center, Vector3Scale(e1, radius));
        for (int i = 1; i <= kSegments; ++i) {
            const float t = (float)i / (float)kSegments * 2.0f * PI;
            const Vector3 offset = Vector3Add(Vector3Scale(e1, cosf(t) * radius),
                                              Vector3Scale(e2, sinf(t) * radius));
            const Vector3 cur = Vector3Add(center, offset);
            DrawLine3D(prev, cur, color);
            prev = cur;
        }
    }
}

void DrawWireCylinder(const Vector3& center, float radius, float height, const Quaternion& rot, Color color) {
    constexpr int kSegments = 16;
    const float halfHeight = height * 0.5f;
    Vector3 prevTop, prevBottom;
    for (int i = 0; i <= kSegments; ++i) {
        const float t = (float)i / (float)kSegments * 2.0f * PI;
        const Vector3 radial = Vector3RotateByQuaternion({ cosf(t) * radius, 0.0f, sinf(t) * radius }, rot);
        const Vector3 top = Vector3Add(center, Vector3Add(radial, { 0.0f, halfHeight, 0.0f }));
        const Vector3 bottom = Vector3Add(center, Vector3Add(radial, { 0.0f, -halfHeight, 0.0f }));
        if (i > 0) {
            DrawLine3D(prevTop, top, color);
            DrawLine3D(prevBottom, bottom, color);
            // Four uprights instead of one per segment, or the wireframe turns
            // into a solid tube at this radius.
            if (i % (kSegments / 4) == 0) {
                DrawLine3D(bottom, top, color);
            }
        }
        prevTop = top;
        prevBottom = bottom;
    }
}

// Draw the collider that physics actually uses for an object. The radii and
// extents below mirror Simulation::CreateShapeForObject, so what you see is the
// shape that is really being tested against, not a box that happens to contain
// it.
void DrawCollisionShape(ScatteredObject& obj, const Vector3& pos, const Quaternion& rot, Color color) {
    const Vector3 size = *obj.GetSizePtr();
    const Vector3 half = Vector3Scale(size, 0.5f);

    switch (obj.GetShapeType()) {
        case ShapeType::Sphere:
            DrawWireSphere(pos, (size.x + size.y + size.z) / 6.0f, color);
            break;
        case ShapeType::Cylinder:
            DrawWireCylinder(pos, (size.x + size.z) / 4.0f, size.y, rot, color);
            break;
        case ShapeType::Cube:
        case ShapeType::Wedge:
        default:
            // Wedge falls back to a box hull in the physics shape builder too,
            // so a box is the honest depiction of it.
            DrawWireBox(pos, half, rot, color);
            break;
    }
}

// Anchored bodies do not move, so drawing their velocity arrow and orientation
// axes is just noise. Can-collide=false bodies still move and still show up in
// contacts, so they are drawn like any other.
Color ShapeColorFor(ScatteredObject& obj, bool hasBody) {
    if (!hasBody || !obj.canCollide) return { 90, 90, 100, 150 };  // inert
    if (obj.anchored) return { 120, 200, 120, 200 };               // static
    return { 0, 200, 255, 220 };                                   // dynamic
}

} // namespace

void PhysicsDebugVisualizer::DrawPanel() {
    ImGui::SetNextWindowSize(ImVec2(300, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Physics Debug", &visible, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("All")) SetAllLayers(true);
    ImGui::SameLine();
    if (ImGui::Button("None")) SetAllLayers(false);
    ImGui::SameLine();
    if (ImGui::Button("Defaults")) {
        showShapes = true;
        showVelocities = true;
        showContacts = showRaycasts = showAABBs = showJoints = false;
    }

    ImGui::Separator();
    ImGui::Checkbox("Collision shapes", &showShapes);
    ImGui::Checkbox("Velocities", &showVelocities);
    ImGui::Checkbox("Contacts", &showContacts);
    ImGui::Checkbox("AABBs", &showAABBs);
    ImGui::Checkbox("Joints", &showJoints);
    ImGui::Checkbox("Raycasts", &showRaycasts);

    ImGui::Separator();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Velocity scale", &velocityScale, 0.01f, 1.0f, "%.2f");
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderFloat("Contact scale", &contactScale, 0.1f, 5.0f, "%.2f");

    ImGui::Separator();
    // Contacts only exist while the simulation is stepping, so saying so is
    // more useful than an empty checkbox that looks broken.
    ImGui::TextDisabled("Contacts are collected only while playing.");

    ImGui::End();
}

void PhysicsDebugVisualizer::Draw() {
    if (!engine) return;

    if (visible) DrawPanel();
    if (!visible) return;

    // Find physics simulation
    phys::Simulation* sim = nullptr;
    for (auto& e : engine->GetEntities()) {
        if (auto* s = dynamic_cast<phys::Simulation*>(e.get())) {
            sim = s;
            break;
        }
    }

    // Objects without a physics body still have a position, so the overlay
    // falls back to the object's own transform for them. For bodies it reads
    // the body directly: WriteBack() syncs position but sends rotation to the
    // render matrix, so the object's euler angles are stale during play.
    auto transformOf = [sim](ScatteredObject& obj, Vector3& pos, Quaternion& rot) {
        if (sim && sim->GetBodyTransform(&obj, pos, rot)) return;
        pos = *obj.GetPosPtr();
        const Vector3 e = *obj.GetRotationPtr();
        rot = QuaternionFromMatrix(MatrixRotateXYZ({ DEG2RAD * e.x, DEG2RAD * e.y, DEG2RAD * e.z }));
    };

    if (showShapes || showAABBs || showVelocities) {
        for (auto& e : engine->GetEntities()) {
            auto* obj = dynamic_cast<ScatteredObject*>(e.get());
            if (!obj) continue;

            const bool hasBody = sim && sim->HasBody(obj);
            Vector3 pos{};
            Quaternion rot{};
            transformOf(*obj, pos, rot);

            if (showShapes) {
                DrawCollisionShape(*obj, pos, rot, ShapeColorFor(*obj, hasBody));
                if (hasBody) {
                    // Orientation axes, so a body spinning about the wrong axis
                    // is visible at a glance.
                    const Vector3 fwd = Vector3RotateByQuaternion({ 0, 0, 1 }, rot);
                    const Vector3 up = Vector3RotateByQuaternion({ 0, 1, 0 }, rot);
                    const Vector3 half = Vector3Scale(*obj->GetSizePtr(), 0.5f);
                    DrawLine3D(pos, Vector3Add(pos, Vector3Scale(fwd, half.z * 1.5f)), BLUE);
                    DrawLine3D(pos, Vector3Add(pos, Vector3Scale(up, half.y * 1.5f)), GREEN);
                }
            }

            if (showVelocities && hasBody) {
                // WriteBack() copies the body's linear velocity onto the object
                // each step, so this is the body's velocity, just read one step's
                // bookkeeping later. Anchored bodies read as zero and are skipped
                // by the threshold below.
                const Vector3 vel = obj->GetVelocity();
                if (Vector3LengthSqr(vel) > 1e-8f) {
                    const Vector3 end = Vector3Add(pos, Vector3Scale(vel, velocityScale));
                    DrawLine3D(pos, end, BLUE);
                    DrawSphere(end, 0.05f, BLUE);
                }
            }

            if (showAABBs) {
                DrawBoundingBox(WorldAabb(pos, Vector3Scale(*obj->GetSizePtr(), 0.5f), rot),
                                { 255, 200, 0, 120 });
            }
        }
    }

    // Contacts are only produced while the simulation is stepping, so this
    // section is empty outside play. That is inherent: there are no contacts
    // outside play, and drawing a stale list would be a lie.
    if (showContacts && sim) {
        const float normalLen = 0.1f * contactScale;
        const float pointRadius = 0.02f * contactScale;
        auto drawContact = [&](const phys::ContactEvent& ev, Color color) {
            DrawSphere(ev.point, pointRadius, color);
            DrawLine3D(ev.point, Vector3Add(ev.point, Vector3Scale(ev.normal, normalLen)), color);
        };
        // "Began this step" is the interesting set -- it is what tells you two
        // things started touching. Persistent hits are drawn dimmer.
        for (const auto& ev : sim->GetContactBeginEvents()) drawContact(ev, RED);
        for (const auto& ev : sim->GetContactHitEvents()) drawContact(ev, { 200, 120, 0, 160 });
    }

    if (showJoints && sim) {
        std::vector<std::pair<Vector3, Vector3>> segments;
        sim->GetJointSegments(segments);
        for (const auto& seg : segments) {
            DrawLine3D(seg.first, seg.second, YELLOW);
        }
    }

    if (showRaycasts) {
        for (const auto& rc : debugRaycasts) {
            const Vector3 end = {
                rc.origin.x + rc.direction.x * rc.maxDist,
                rc.origin.y + rc.direction.y * rc.maxDist,
                rc.origin.z + rc.direction.z * rc.maxDist
            };
            DrawLine3D(rc.origin, end, rc.color);
            if (rc.hit) {
                DrawSphere(rc.hitPoint, 0.1f, RED);
                DrawLine3D(rc.hitPoint, {
                    rc.hitPoint.x + rc.hitNormal.x * 0.5f,
                    rc.hitPoint.y + rc.hitNormal.y * 0.5f,
                    rc.hitPoint.z + rc.hitNormal.z * 0.5f
                }, YELLOW);
            }
        }
    }
    // Drained every frame regardless of showRaycasts, so a raycast reported
    // while the overlay was hidden is not drawn as a stale line the moment it
    // is toggled back on. Raycasts are per-frame data, not a persistent set.
    debugRaycasts.clear();
}

// ============================================================================
// INPUT RECORDER
// ============================================================================

InputRecorder& InputRecorder::Instance() {
    static InputRecorder instance;
    return instance;
}

void InputRecorder::Initialize() {
    // Create recordings directory
    fs::create_directories(recordingsPath);
}

void InputRecorder::Shutdown() {
    if (recording) StopRecording();
    if (playing) StopPlayback();
}

void InputRecorder::StartRecording(const std::string& name) {
    if (recording) StopRecording();
    recording = true;
    currentRecordingName = name;
    frames.clear();
}

void InputRecorder::StopRecording() {
    if (recording) {
        recording = false;
        if (!frames.empty()) {
            SaveToFile(recordingsPath + currentRecordingName + ".rec");
        }
    }
}

void InputRecorder::RecordFrame(const InputFrame& frame) {
    if (recording) frames.push_back(frame);
}

void InputRecorder::StartPlayback(const std::string& name) {
    if (playing) StopPlayback();
    if (LoadFromFile(recordingsPath + name + ".rec")) {
        playing = true;
        currentRecordingName = name;
        playbackIndex = 0;
    }
}

void InputRecorder::StopPlayback() {
    playing = false;
    playbackIndex = 0;
}

const InputFrame* InputRecorder::GetNextFrame() {
    if (!playing || playbackIndex >= frames.size()) return nullptr;
    return &frames[playbackIndex++];
}

bool InputRecorder::SaveToFile(const std::string& path) {
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    
    uint32_t frameCount = (uint32_t)frames.size();
    file.write(reinterpret_cast<char*>(&frameCount), sizeof(frameCount));
    for (const auto& frame : frames) {
        file.write(reinterpret_cast<const char*>(&frame), sizeof(frame));
    }
    return true;
}

bool InputRecorder::LoadFromFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    
    uint32_t frameCount;
    file.read(reinterpret_cast<char*>(&frameCount), sizeof(frameCount));
    frames.resize(frameCount);
    for (auto& frame : frames) {
        file.read(reinterpret_cast<char*>(&frame), sizeof(frame));
    }
    return true;
}

std::vector<std::string> InputRecorder::ListRecordings() const {
    std::vector<std::string> result;
    for (const auto& entry : fs::directory_iterator(recordingsPath)) {
        if (entry.path().extension() == ".rec") {
            result.push_back(entry.path().stem().string());
        }
    }
    return result;
}

void InputRecorder::Update(float dt) {
    (void)dt;
    // Update playback position if playing
    if (playing && playbackIndex >= frames.size()) {
        StopPlayback();
    }
}

// ============================================================================
// CAPTURE SYSTEM
// ============================================================================

CaptureSystem& CaptureSystem::Instance() {
    static CaptureSystem instance;
    return instance;
}

void CaptureSystem::Initialize(const std::string& outputDir) {
    this->outputDir = outputDir;
    fs::create_directories(outputDir);
}

void CaptureSystem::CaptureScreenshot(bool includeUI, int superSample) {
    // Simple screenshot using raylib
    std::string filename = outputDir + "screenshot_" + std::to_string((int)GetTime()) + "." + format;
    TakeScreenshot(filename.c_str());
}

void CaptureSystem::CaptureScreenshotAsync(bool includeUI, int superSample) {
    // Run in background thread
    std::thread([=]() {
        CaptureScreenshot(includeUI, superSample);
    }).detach();
}

void CaptureSystem::StartVideoCapture(int fps, int superSample) {
    if (capturingVideo) return;
    capturingVideo = true;
    videoFps = fps;
    videoFrameCount = 0;
    videoAccumulator = 0;
    this->superSample = superSample;
    fs::create_directories(outputDir + "video/");
}

void CaptureSystem::StopVideoCapture() {
    capturingVideo = false;
}

// ============================================================================
// TECHNICAL TOOLS MANAGER
// ============================================================================

TechnicalToolsManager& TechnicalToolsManager::Instance() {
    static TechnicalToolsManager instance;
    return instance;
}

void TechnicalToolsManager::Initialize(Engine* eng) {
    if (initialized) return;
    engine = eng;
    
    console.Initialize(engine);
    inspector.Initialize(engine);
    physicsDebug.Initialize(engine);
    recorder.Initialize();
    capture.Initialize();
    shaderReloader.Initialize(engine);
    // After the console, so Initialize() can log its ready message to it.
    MemoryTracker::Instance().Initialize();
    
    // Register default cvars.
    //
    // Every cvar here is read by a real consumer. Several that used to be
    // declared here were read by nothing at all: r.gamma (gamma is a hardcoded
    // 1.0/2.2 in terrain.frag), debug.show_navmesh (no navmesh exists), and
    // show_fps / show_debug / player_speed / player_jump. A cvar that no code
    // reads still shows up in `cvars`, gets written to config.cfg, and reads
    // back as if it were working -- so the dead ones are gone rather than left
    // to imply a feature that isn't there.
    cvars.RegisterBool("r.show_wireframe", false, "Show wireframe (consumed by gfx::SetWireframe)");
    cvars.RegisterBool("r.show_bounds", false, "Show object AABBs (consumed by the physics debug overlay)");
    cvars.RegisterFloat("physics.fixed_dt", 1.0f/120.0f, "Physics fixed timestep", 0.001f, 0.1f);
    cvars.RegisterInt("physics.max_substeps", 8, "Max physics sub-steps", 1, 16);

    // Apply the two render cvars to the state they describe. Both flags are
    // owned by a long-lived subsystem that outlives any config reload, so the
    // cvar has to be pushed into it once here; after this the console command
    // and the ImGui checkbox are the live controls, and the cvar records the
    // starting state (which is what config.cfg round-trips).
    gfx::SetWireframe(cvars.GetBool("r.show_wireframe", false));
    PhysicsDebugVisualizer::Instance().showAABBs = cvars.GetBool("r.show_bounds", false);

    initialized = true;
    console.Log("Technical Tools initialized. Press F12 for console, F7 for Shader Reloader, "
                "F9 for Memory Tracker.");
}

void TechnicalToolsManager::Shutdown() {
    // Tracker first: its shutdown reports what is still live, which is
    // meaningful only while the rest of the engine is still standing.
    MemoryTracker::Instance().Shutdown();
    console.Shutdown();
    recorder.Shutdown();
    shaderReloader.Shutdown();
    initialized = false;
}

void TechnicalToolsManager::Update(float dt) {
    if (!initialized) return;
    
    profiler.BeginFrame();
    
    console.Update(dt);
    inspector.Update(dt);
    recorder.Update(dt);
    shaderReloader.Update(dt);
    MemoryTracker::Instance().Update(dt);
    
    profiler.EndFrame();
}

void TechnicalToolsManager::Draw() {
    Draw3D();
    DrawUI();
}

void TechnicalToolsManager::Draw3D() {
    if (!initialized) return;
    physicsDebug.Draw();
}

void TechnicalToolsManager::DrawUI() {
    if (!initialized) return;
    inspector.Draw();
    console.Draw();
    shaderReloader.Draw();
    MemoryTracker::Instance().Draw();
    // The small "Profiler" frame-time window was removed from here; the player's
    // Ctrl+F5 debug stats overlay already shows frame time.
}

void TechnicalToolsManager::HandleInput() {
    if (!initialized) return;
    
    // F12 - Console
    if (IsKeyPressed(KEY_F12)) {
        console.Toggle();
    }
    
    // F1 - Inspector
    if (IsKeyPressed(KEY_F1)) {
        inspector.Toggle();
    }
    
    // F2 - Physics Debug. Shows the layer panel; the layers themselves are
    // chosen there, because turning on all six at once is rarely what you want.
    if (IsKeyPressed(KEY_F2)) {
        physicsDebug.Toggle();
    }
    
    // F3 - Start/Stop Recording
    if (IsKeyPressed(KEY_F3)) {
        if (recorder.IsRecording()) {
            recorder.StopRecording();
            console.Log("Recording stopped");
        } else {
            recorder.StartRecording("session_" + std::to_string((int)GetTime()));
            console.Log("Recording started");
        }
    }
    
    // F4 - Start/Stop Playback
    if (IsKeyPressed(KEY_F4)) {
        if (recorder.IsPlaying()) {
            recorder.StopPlayback();
            console.Log("Playback stopped");
        } else {
            auto recordings = recorder.ListRecordings();
            if (!recordings.empty()) {
                recorder.StartPlayback(recordings.back());
                console.Log("Playing back: " + recordings.back());
            }
        }
    }
    
    // F5 - Screenshot
    if (IsKeyPressed(KEY_F5)) {
        capture.CaptureScreenshot();
        console.Log("Screenshot saved");
    }
    
    // F6 - Video Capture
    if (IsKeyPressed(KEY_F6)) {
        if (capture.IsCapturingVideo()) {
            capture.StopVideoCapture();
            console.Log("Video capture stopped");
        } else {
            capture.StartVideoCapture();
            console.Log("Video capture started");
        }
    }
    
    // F7 - Shader Reloader
    if (IsKeyPressed(KEY_F7)) {
        shaderReloader.Toggle();
    }
    
    // F9 - Memory Tracker. F8 is left free: the player's own debug overlay
    // binds it to reset min/max frame times.
    if (IsKeyPressed(KEY_F9)) {
        MemoryTracker::Instance().Toggle();
    }
}

void TechnicalToolsManager::DrawProfilerOverlay() {
    // Simple ImGui profiler window
    ImGui::Begin("Profiler", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("Frame Time: %.2f ms (%.1f FPS)", profiler.GetFrameTimeMs(), 1000.0 / profiler.GetFrameTimeMs());
    ImGui::Separator();
    
    for (const auto& [name, data] : profiler.GetZoneData()) {
        ImGui::Text("%s: %.2f ms (min %.2f, max %.2f, %d calls)", 
                    name.c_str(), data.totalMs, data.minMs, data.maxMs, data.count);
    }
    ImGui::End();
}

// ============================================================================
// SHADER RELOAD / EDITOR IMPLEMENTATION
// ============================================================================

ShaderReloader& ShaderReloader::Instance() {
    static ShaderReloader instance;
    return instance;
}

void ShaderReloader::Initialize(Engine* eng) {
    engine = eng;
    initialized = true;
    visible = false;
    
    // Create shader directory
    std::error_code ec;
    fs::create_directories(shaderDir, ec);
    
    CONSOLE.Log("Shader Reloader initialized. Press F7 for editor.");
}

void ShaderReloader::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [name, tracked] : shaders) {
        if (tracked.shader.id != 0) {
            UnloadShader(tracked.shader);
        }
    }
    shaders.clear();
}

void ShaderReloader::Update(float dt) {
    if (!initialized) return;
    
    if (autoReload) {
        watchTimer += dt;
        if (watchTimer >= watchInterval) {
            watchTimer = 0.0f;
            CheckForChanges();
        }
    }
}

void ShaderReloader::Draw() {
    if (!visible) return;
    
    ImGui::Begin("Shader Reloader", &visible, ImGuiWindowFlags_MenuBar);
    ImGui::TextWrapped("Shaders are compiled at build time (shaders/*.glsl via sokol-shdc); "
                       "runtime recompilation is not available on this backend.");
    
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Reload All")) ReloadAll();
            if (ImGui::MenuItem("Save Sources to Disk")) {
                // Save all shader sources to files
                for (auto& [name, tracked] : shaders) {
                    std::ofstream vert(shaderDir + name + ".vert");
                    vert << tracked.source.vertSource;
                    std::ofstream frag(shaderDir + name + ".frag");
                    frag << tracked.source.fragSource;
                }
                CONSOLE.Log("Shader sources saved to " + shaderDir);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            ImGui::MenuItem("Auto Reload", nullptr, &autoReload);
            ImGui::DragFloat("Watch Interval (s)", &watchInterval, 0.1f, 0.1f, 10.0f);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
    
    // Shader list
    if (ImGui::BeginChild("ShaderList", ImVec2(250, 0), true)) {
        for (auto& [name, tracked] : shaders) {
            bool selected = false; // Could track selection
            ImGui::Selectable((name + (tracked.source.hasError ? " [ERROR]" : "")).c_str(), &selected);
            if (ImGui::IsItemClicked()) {
                // Select this shader for editing
            }
        }
        ImGui::EndChild();
    }
    ImGui::SameLine();
    
    // Shader editor (show first shader for now, or selected)
    for (auto& [name, tracked] : shaders) {
        if (ImGui::BeginChild(("Editor_" + name).c_str(), ImVec2(0, 0), true)) {
            DrawShaderEditor(tracked);
            ImGui::EndChild();
        }
        break; // Show first shader only for now
    }
    
    ImGui::End();
}

bool ShaderReloader::RegisterShader(const std::string& name, const std::string& vertSource, const std::string& fragSource) {
    std::lock_guard<std::mutex> lock(mutex);
    if (shaders.find(name) != shaders.end()) return false;
    
    TrackedShader tracked;
    tracked.name = name;
    tracked.source.name = name;
    tracked.source.vertSource = vertSource;
    tracked.source.fragSource = fragSource;
    tracked.needsReload = true;
    
    shaders[name] = std::move(tracked);
    return true;
}

Shader* ShaderReloader::GetShader(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex);
    auto it = shaders.find(name);
    if (it != shaders.end() && it->second.shader.id != 0) {
        return &it->second.shader;
    }
    return nullptr;
}

void ShaderReloader::ReloadAll() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [name, tracked] : shaders) {
        tracked.needsReload = true;
    }
}

void ShaderReloader::CheckForChanges() {
    std::lock_guard<std::mutex> lock(mutex);
    for (auto& [name, tracked] : shaders) {
        if (!fs::exists(shaderDir + name + ".vert") || !fs::exists(shaderDir + name + ".frag")) continue;
        
        auto vertTime = fs::last_write_time(shaderDir + name + ".vert");
        auto fragTime = fs::last_write_time(shaderDir + name + ".frag");
        
        if (vertTime != tracked.source.vertModTime || fragTime != tracked.source.fragModTime) {
            tracked.needsReload = true;
        }
    }
    
    // Reload any that need it
    for (auto& [name, tracked] : shaders) {
        if (tracked.needsReload) {
            CompileShader(tracked);
            tracked.needsReload = false;
        }
    }
}

bool ShaderReloader::LoadShaderSource(const std::string& path, std::string& outSource, std::filesystem::file_time_type& outModTime) {
    std::ifstream file(path);
    if (!file) return false;
    std::stringstream buffer;
    buffer << file.rdbuf();
    outSource = buffer.str();
    outModTime = fs::last_write_time(path);
    return true;
}

bool ShaderReloader::CompileShader(TrackedShader& tracked) {
    // Shaders are compiled offline by sokol-shdc (shaders/*.glsl) and baked
    // into the binary, so there is nothing to recompile at runtime: edit the
    // .glsl file and rebuild instead.
    tracked.source.hasError = true;
    tracked.lastError = "Runtime shader compilation is not available: edit shaders/*.glsl and rebuild";
    if (errorCallback) errorCallback(tracked.name, tracked.lastError);
    CONSOLE.LogError("Shader Reloader: " + tracked.name + " - " + tracked.lastError);
    return false;
}

void ShaderReloader::ExtractUniforms(TrackedShader& tracked) {
    tracked.uniforms.clear();
}

void ShaderReloader::DrawShaderEditor(TrackedShader& tracked) {
    ImGui::Text("Shader: %s", tracked.name.c_str());
    ImGui::SameLine();
    if (tracked.source.hasError) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "[ERROR]");
        if (ImGui::BeginItemTooltip()) {
            ImGui::TextUnformatted(tracked.lastError.c_str());
            ImGui::EndTooltip();
        }
    } else {
        ImGui::TextColored(ImVec4(0, 1, 0, 1), "[OK]");
    }
    
    ImGui::Separator();
    
    // Source code editors
    if (ImGui::CollapsingHeader("Vertex Shader Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        static char vertBuf[65536];
        strncpy(vertBuf, tracked.source.vertSource.c_str(), sizeof(vertBuf) - 1);
        vertBuf[sizeof(vertBuf) - 1] = '\0';
        
        if (ImGui::InputTextMultiline("##vert", vertBuf, sizeof(vertBuf), 
            ImVec2(-FLT_MIN, 300), ImGuiInputTextFlags_AllowTabInput)) {
            tracked.source.vertSource = vertBuf;
            tracked.needsReload = true;
        }
    }
    
    if (ImGui::CollapsingHeader("Fragment Shader Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        static char fragBuf[65536];
        strncpy(fragBuf, tracked.source.fragSource.c_str(), sizeof(fragBuf) - 1);
        fragBuf[sizeof(fragBuf) - 1] = '\0';
        
        if (ImGui::InputTextMultiline("##frag", fragBuf, sizeof(fragBuf), 
            ImVec2(-FLT_MIN, 300), ImGuiInputTextFlags_AllowTabInput)) {
            tracked.source.fragSource = fragBuf;
            tracked.needsReload = true;
        }
    }
    
    if (ImGui::Button("Recompile")) {
        tracked.needsReload = true;
    }
    
    ImGui::Separator();
    
    // Uniform editor
    if (ImGui::CollapsingHeader("Uniforms", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (auto& uniform : tracked.uniforms) {
            DrawUniformEditor(uniform);
        }
    }
    
    // Apply dirty uniforms
    for (auto& uniform : tracked.uniforms) {
        if (uniform.dirty) {
            switch (uniform.type) {
                case GL_FLOAT:
                    SetShaderValue(tracked.shader, uniform.location, uniform.floatVal, SHADER_UNIFORM_FLOAT);
                    break;
                case GL_FLOAT_VEC2:
                    SetShaderValue(tracked.shader, uniform.location, uniform.floatVal, SHADER_UNIFORM_VEC2);
                    break;
                case GL_FLOAT_VEC3:
                    SetShaderValue(tracked.shader, uniform.location, uniform.floatVal, SHADER_UNIFORM_VEC3);
                    break;
                case GL_FLOAT_VEC4:
                    SetShaderValue(tracked.shader, uniform.location, uniform.floatVal, SHADER_UNIFORM_VEC4);
                    break;
                case GL_FLOAT_MAT4:
                    {
                        Matrix mat = { uniform.floatVal[0], uniform.floatVal[1], uniform.floatVal[2], uniform.floatVal[3],
                                       uniform.floatVal[4], uniform.floatVal[5], uniform.floatVal[6], uniform.floatVal[7],
                                       uniform.floatVal[8], uniform.floatVal[9], uniform.floatVal[10], uniform.floatVal[11],
                                       uniform.floatVal[12], uniform.floatVal[13], uniform.floatVal[14], uniform.floatVal[15] };
                        SetShaderValueMatrix(tracked.shader, uniform.location, mat);
                    }
                    break;
                case GL_INT:
                case GL_BOOL:
                case GL_SAMPLER_1D:
                case GL_SAMPLER_2D:
                case GL_SAMPLER_3D:
                case GL_SAMPLER_CUBE:
                    SetShaderValue(tracked.shader, uniform.location, uniform.intVal, SHADER_UNIFORM_INT);
                    break;
                case GL_INT_VEC2:
                    SetShaderValue(tracked.shader, uniform.location, uniform.intVal, SHADER_UNIFORM_IVEC2);
                    break;
                case GL_INT_VEC3:
                    SetShaderValue(tracked.shader, uniform.location, uniform.intVal, SHADER_UNIFORM_IVEC3);
                    break;
                case GL_INT_VEC4:
                    SetShaderValue(tracked.shader, uniform.location, uniform.intVal, SHADER_UNIFORM_IVEC4);
                    break;
            }
            uniform.dirty = false;
        }
    }
}

void ShaderReloader::DrawUniformEditor(ShaderUniform& uniform) {
    ImGui::PushID(uniform.location);
    ImGui::Text("%s (%s)", uniform.name.c_str(), GetTypeName(uniform.type).c_str());
    ImGui::SameLine();
    
    bool changed = false;
    switch (uniform.type) {
        case GL_FLOAT:
            changed = ImGui::DragFloat("##val", &uniform.floatVal[0], 0.01f);
            break;
        case GL_FLOAT_VEC2:
            changed = ImGui::DragFloat2("##val", uniform.floatVal, 0.01f);
            break;
        case GL_FLOAT_VEC3:
            changed = ImGui::DragFloat3("##val", uniform.floatVal, 0.01f);
            break;
        case GL_FLOAT_VEC4:
            changed = ImGui::DragFloat4("##val", uniform.floatVal, 0.01f);
            break;
        case GL_FLOAT_MAT4:
            changed = ImGui::DragFloat4("##val0", uniform.floatVal, 0.01f);
            ImGui::SameLine(); changed |= ImGui::DragFloat4("##val1", uniform.floatVal+4, 0.01f);
            ImGui::SameLine(); changed |= ImGui::DragFloat4("##val2", uniform.floatVal+8, 0.01f);
            ImGui::SameLine(); changed |= ImGui::DragFloat4("##val3", uniform.floatVal+12, 0.01f);
            break;
        case GL_INT:
        case GL_BOOL:
        case GL_SAMPLER_1D:
        case GL_SAMPLER_2D:
        case GL_SAMPLER_3D:
        case GL_SAMPLER_CUBE:
            changed = ImGui::DragInt("##val", &uniform.intVal[0]);
            break;
        case GL_INT_VEC2:
            changed = ImGui::DragInt2("##val", uniform.intVal);
            break;
        case GL_INT_VEC3:
            changed = ImGui::DragInt3("##val", uniform.intVal);
            break;
        case GL_INT_VEC4:
            changed = ImGui::DragInt4("##val", uniform.intVal);
            break;
        default:
            ImGui::Text("Unsupported type");
    }
    
    if (changed) uniform.dirty = true;
    ImGui::PopID();
}

std::string ShaderReloader::GetTypeName(int glType) const {
    switch (glType) {
        case GL_FLOAT: return "float";
        case GL_FLOAT_VEC2: return "vec2";
        case GL_FLOAT_VEC3: return "vec3";
        case GL_FLOAT_VEC4: return "vec4";
        case GL_FLOAT_MAT2: return "mat2";
        case GL_FLOAT_MAT3: return "mat3";
        case GL_FLOAT_MAT4: return "mat4";
        case GL_INT: return "int";
        case GL_INT_VEC2: return "ivec2";
        case GL_INT_VEC3: return "ivec3";
        case GL_INT_VEC4: return "ivec4";
        case GL_BOOL: return "bool";
        case GL_SAMPLER_1D: return "sampler1D";
        case GL_SAMPLER_2D: return "sampler2D";
        case GL_SAMPLER_3D: return "sampler3D";
        case GL_SAMPLER_CUBE: return "samplerCube";
        default: return "unknown";
    }
}

// ============================================================================
// MEMORY TRACKER
// ============================================================================
//
// The tracker is opt-in bookkeeping over the engine's own resource handles: it
// does not hook the allocator, so it sees exactly the resources something
// explicitly tracked and nothing else. Process RSS is reported alongside it so
// a gap between "what I think I am holding" and "what the process actually
// holds" is visible rather than mysterious.

size_t TextureBytes(const Texture& tex) {
    if (tex.id == 0 || tex.width <= 0 || tex.height <= 0) return 0;
    size_t base = (size_t)GetPixelDataSize(tex.width, tex.height, tex.format);
    if (base == 0) return 0;
    // raylib's own UploadTexture allocates with the same 4x over-estimate then
    // shrinks it, but only the final 4x width*height*comp buffer is live, so
    // the mip chain is the only addition here.
    int mips = tex.mipmaps > 0 ? tex.mipmaps : 1;
    if (mips <= 1) return base;
    double total = 0.0;
    for (int i = 0; i < mips; ++i) {
        int w = tex.width >> i;
        int h = tex.height >> i;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        total += (double)GetPixelDataSize(w, h, tex.format);
    }
    return (size_t)total;
}

size_t ImageBytes(const Image& img) {
    if (img.data == nullptr || img.width <= 0 || img.height <= 0) return 0;
    int mips = img.mipmaps > 0 ? img.mipmaps : 1;
    size_t total = 0;
    for (int i = 0; i < mips; ++i) {
        int w = img.width >> i;
        int h = img.height >> i;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        size_t level = (size_t)GetPixelDataSize(w, h, img.format);
        if (level == 0) break;
        total += level;
    }
    return total;
}

size_t MeshBytes(const Mesh& mesh) {
    if (mesh.vertexCount <= 0 && mesh.vboId == nullptr) return 0;
    const size_t v = (size_t)std::max(mesh.vertexCount, 0);
    size_t bytes = 0;
    // Only the arrays actually present are counted: raylib leaves skinning and
    // anim pointers NULL for static meshes, and a NULL array is not a
    // vertexCount-sized allocation.
    if (mesh.vertices)     bytes += v * 3 * sizeof(float);
    if (mesh.texcoords)    bytes += v * 2 * sizeof(float);
    if (mesh.texcoords2)   bytes += v * 2 * sizeof(float);
    if (mesh.normals)      bytes += v * 3 * sizeof(float);
    if (mesh.tangents)     bytes += v * 4 * sizeof(float);
    if (mesh.colors)       bytes += v * 4 * sizeof(unsigned char);
    if (mesh.boneIndices)  bytes += v * 4 * sizeof(unsigned char);
    if (mesh.boneWeights)  bytes += v * 4 * sizeof(float);
    if (mesh.animVertices) bytes += v * 3 * sizeof(float);
    if (mesh.animNormals)  bytes += v * 3 * sizeof(float);
    if (mesh.indices) {
        size_t idxCount = (size_t)std::max(mesh.vertexCount, mesh.triangleCount * 3);
        bytes += idxCount * sizeof(unsigned short);
    }
    if (mesh.vboId) bytes += 7 * sizeof(unsigned int);
    return bytes;
}

size_t ShaderBytes(const Shader& shader) {
    if (shader.id == 0) return 0;
    return sizeof(Shader) + (size_t)RL_MAX_SHADER_LOCATIONS * sizeof(int);
}

size_t ModelBytes(const Model& model) {
    if (model.meshes == nullptr) return 0;
    size_t bytes = 0;
    for (int i = 0; i < model.meshCount; ++i) {
        bytes += MeshBytes(model.meshes[i]);
    }
    if (model.materials) {
        // Material structs only. The maps' textures are separate resources
        // with their own handles, so counting them here too would report the
        // same texture bytes once per material that references it.
        bytes += (size_t)model.materialCount * sizeof(Material);
    }
    if (model.meshMaterial) bytes += (size_t)model.meshCount * sizeof(int);
    if (model.skeleton.bones && model.skeleton.boneCount > 0) {
        bytes += (size_t)model.skeleton.boneCount * (sizeof(BoneInfo) + sizeof(Transform));
        if (model.skeleton.bindPose) {
            bytes += (size_t)model.skeleton.boneCount * sizeof(Transform);
        }
    }
    if (model.boneMatrices && model.skeleton.boneCount > 0) {
        bytes += (size_t)model.skeleton.boneCount * sizeof(Matrix);
    }
    return bytes;
}

size_t RenderTextureBytes(const RenderTexture& rt) {
    if (rt.id == 0) return 0;
    // This raylib's RenderTexture has no fboDepth flag, so the depth
    // attachment is counted from its own descriptor when it exists.
    size_t bytes = TextureBytes(rt.texture);
    bytes += TextureBytes(rt.depth);
    return bytes;
}

size_t WaveBytes(const Wave& wave) {
    if (wave.data == nullptr) return 0;
    return (size_t)wave.frameCount * (wave.sampleSize / 8) * wave.channels;
}

MemoryTracker& MemoryTracker::Instance() {
    static MemoryTracker instance;
    return instance;
}

double MemoryTracker::Now() const {
    // steady_clock, not raylib's GetTime(): GetTime() is tied to window
    // init, and allocations can be tracked before a window exists.
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

size_t MemoryTracker::GetProcessMemoryBytes() {
#if defined(__linux__)
    // /proc/self/statm reports sizes in pages: second field is resident set size.
    std::ifstream statm("/proc/self/statm");
    if (!statm) return 0;
    long long totalPages = 0, residentPages = 0;
    statm >> totalPages >> residentPages;
    if (residentPages <= 0) return 0;
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) pageSize = 4096;
    return (size_t)residentPages * (size_t)pageSize;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    ZeroMemory(&pmc, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (size_t)pmc.WorkingSetSize;
#elif defined(__APPLE__)
    // macOS has no /proc; this needs <mach/mach.h>, which is not pulled in
    // here, so report 0 rather than adding a platform header dependency.
    return 0;
#else
    return 0;
#endif
}

size_t MemoryTracker::GetProcessPeakMemoryBytes() {
#if defined(__linux__)
    // VmHWM is the kernel's high-water mark for the resident set, which is
    // exactly "peak RSS" -- and unlike statm it survives the peak passing.
    std::ifstream status("/proc/self/status");
    if (!status) return 0;
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            std::istringstream iss(line.substr(6));
            long long kb = 0;
            iss >> kb;
            if (kb <= 0) return 0;
            return (size_t)kb * 1024;
        }
    }
    return 0;
#elif defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    ZeroMemory(&pmc, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
    return (size_t)pmc.PeakWorkingSetSize;
#else
    return 0;
#endif
}

void MemoryTracker::Initialize() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (initialized) return;

    allocations.clear();
    allocList.clear();
    budgets.clear();
    peakUsage.clear();
    categoryUsage.clear();
    overBudgetWarned.clear();
    liveBytesMBHistory.clear();
    processMBHistory.clear();
    categoryMBHistory.clear();

    totalAllocated = 0;
    totalFreed = 0;
    currentAllocated = 0;
    peakAllocated = 0;
    allocCount = 0;
    freeCount = 0;

    leakBaselineSet = false;
    leakBaselineKeys.clear();
    leakBaselineFrame = 0;
    leakBaselineTime = 0;

    hasSnapshot = false;
    currentSnapshot = Snapshot();
    hasBaselineSnapshot = false;
    baselineSnapshot = Snapshot();
    hasBaselineDiff = false;
    baselineDiff = DiffResult();

    selectedAlloc = nullptr;
    startTime = Now();
    initialized = true;

    CONSOLE.Log("Memory Tracker initialized. Press F9 for the memory window, "
                "or 'mem_stats' in the console.");
}

void MemoryTracker::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialized) return;

    // Report what is still live at exit. By this point the engine is tearing
    // down, so whatever is left is either an intentional process-lifetime
    // resource or a genuine leak; printing it either way is the point of having
    // tracked it.
    if (visible || currentAllocated > 0) {
        LeakReport report = DetectLeaks();
        if (!report.leaks.empty()) {
            CONSOLE.LogWarn("[Memory] " + FormatBytes(report.totalLeakedBytes) + " in " +
                            std::to_string(report.leaks.size()) + " allocation(s) still live at shutdown:");
            for (const auto& leak : report.leaks) {
                if (report.leaks.size() > 16) break;
                CONSOLE.Log("  " + FormatBytes(leak.size) + "  [" + leak.category + "]  " +
                            (leak.tag.empty() ? "<untagged>" : leak.tag) +
                            "  @" + FormatPtr(leak.ptr));
            }
            if (report.leaks.size() > 16) {
                CONSOLE.Log("  ... and " + std::to_string(report.leaks.size() - 16) + " more");
            }
        }
    }

    allocations.clear();
    allocList.clear();
    categoryUsage.clear();
    peakUsage.clear();
    overBudgetWarned.clear();
    initialized = false;
}

void MemoryTracker::Update(float dt) {
    (void)dt;
    if (!initialized) return;

    std::lock_guard<std::recursive_mutex> lock(mutex);
    frameNumber++;
    RecordHistory();
    WarnOverBudgets();
}

void MemoryTracker::RecordHistory() {
    // One sample per frame, capped, so the graphs stay bounded no matter how
    // long the player runs.
    const double mb = (double)currentAllocated / (1024.0 * 1024.0);
    liveBytesMBHistory.push_back((float)mb);
    if (liveBytesMBHistory.size() > (size_t)HISTORY_SIZE) liveBytesMBHistory.erase(liveBytesMBHistory.begin());

    processMBHistory.push_back((float)((double)GetProcessMemoryBytes() / (1024.0 * 1024.0)));
    if (processMBHistory.size() > (size_t)HISTORY_SIZE) processMBHistory.erase(processMBHistory.begin());

    for (const auto& entry : categoryUsage) {
        auto& hist = categoryMBHistory[entry.first];
        hist.push_back((float)((double)entry.second / (1024.0 * 1024.0)));
        if (hist.size() > (size_t)HISTORY_SIZE) hist.erase(hist.begin());
    }
}

void MemoryTracker::WarnOverBudgets() {
    for (const auto& [category, budget] : budgets) {
        if (budget == 0) continue;
        auto usageIt = categoryUsage.find(category);
        size_t usage = (usageIt == categoryUsage.end()) ? 0 : usageIt->second;
        if (usage <= budget) {
            overBudgetWarned[category] = false;
            continue;
        }
        if (overBudgetWarned[category]) continue; // already reported
        overBudgetWarned[category] = true;
        CONSOLE.LogWarn("[Memory] Budget exceeded for '" + category + "': " +
                        FormatBytes(usage) + " of " + FormatBytes(budget) + " budget.");
    }
}

// allocList holds copies of the map entries, ordered newest-first so the UI can
// show recent allocations at the top. That means every mutation has to be
// applied twice: once to allocations (the authoritative keyed map) and once to
// the list copy. Forgetting the second half leaves the list reporting sizes and
// totals that disagree with every other accessor, which is exactly the kind of
// drift that makes a memory report untrustworthy.
MemAlloc* MemoryTracker::FindListEntry(void* ptr) {
    for (auto& entry : allocList) {
        if (entry.ptr == ptr) return &entry;
    }
    return nullptr;
}

// Apply a size change to an existing entry without touching the rest of the
// bookkeeping. Split into "grew" and "shrank" halves rather than just
// overwriting the size, because that is what keeps the cumulative invariant
// totalAllocated == totalFreed + currentAllocated true: growth is a fresh
// allocation, shrinkage is a partial free. Rewriting the size directly would
// make a cumulative counter go backwards.
void MemoryTracker::ResizeEntry(void* ptr, size_t newSize) {
    auto it = allocations.find(ptr);
    if (it == allocations.end()) return;
    size_t oldSize = it->second.size;
    const std::string& category = it->second.category;

    if (newSize > oldSize) {
        size_t delta = newSize - oldSize;
        currentAllocated += delta;
        totalAllocated += delta;
        categoryUsage[category] += delta;
    } else if (newSize < oldSize) {
        size_t delta = oldSize - newSize;
        currentAllocated -= std::min(delta, currentAllocated);
        totalFreed += delta;
        auto usageIt = categoryUsage.find(category);
        if (usageIt != categoryUsage.end()) {
            usageIt->second -= std::min(delta, usageIt->second);
            if (usageIt->second == 0) {
                categoryUsage.erase(usageIt);
                // Nothing left in this category, so stop plotting its history
                // rather than leaving a flat zero line on the graphs forever.
                categoryMBHistory.erase(category);
            }
        }
    }
    it->second.size = newSize;
    if (MemAlloc* listEntry = FindListEntry(ptr)) listEntry->size = newSize;
    UpdateCategoryPeak(category, categoryUsage[category]);
}

void MemoryTracker::TrackAlloc(void* ptr, size_t size, const std::string& category, const std::string& tag) {
    if (ptr == nullptr || size == 0) return;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialized || !trackingEnabled) return;

    auto it = allocations.find(ptr);
    if (it != allocations.end()) {
        // Same key tracked twice: resize in place rather than letting the
        // accounting drift into double-counting.
        if (it->second.category != category) {
            // Moving between categories has to move the bytes, not just relabel
            // the row, or the two category totals stop summing to the total.
            Rekey(ptr, size, category, tag);
            return;
        }
        ResizeEntry(ptr, size);
        if (!tag.empty()) it->second.tag = tag;
        return;
    }

    MemAlloc alloc;
    alloc.ptr = ptr;
    alloc.size = size;
    alloc.category = category;
    alloc.tag = tag;
    alloc.frameNumber = frameNumber;
    alloc.timestamp = Now() - startTime;
    alloc.isFreed = false;
    if (captureCallstacks) {
        alloc.callstack = CaptureCallstack(2);
    }

    allocations[ptr] = alloc;
    // Newest first: the interesting allocations are the recent ones, and the UI
    // defaults to showing them from the top.
    allocList.insert(allocList.begin(), alloc);

    totalAllocated += size;
    currentAllocated += size;
    allocCount++;
    if (currentAllocated > peakAllocated) peakAllocated = currentAllocated;

    size_t& usage = categoryUsage[category];
    usage += size;
    UpdateCategoryPeak(category, usage);
}

void MemoryTracker::UntrackKey(void* key) {
    if (key == nullptr) return;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialized) return;

    auto it = allocations.find(key);
    if (it == allocations.end()) return;

    const MemAlloc& alloc = it->second;
    currentAllocated -= std::min(alloc.size, currentAllocated);
    totalFreed += alloc.size;
    freeCount++;
    // peakAllocated is deliberately not touched: it is a high-water mark, so
    // releasing memory must not pull it down.

    auto usageIt = categoryUsage.find(alloc.category);
    if (usageIt != categoryUsage.end()) {
        usageIt->second -= std::min(alloc.size, usageIt->second);
        if (usageIt->second == 0) {
            categoryUsage.erase(usageIt);
            categoryMBHistory.erase(alloc.category);
        }
    }

    for (auto lit = allocList.begin(); lit != allocList.end(); ++lit) {
        if (lit->ptr == key) { allocList.erase(lit); break; }
    }
    allocations.erase(it);
}

void MemoryTracker::TrackFree(void* ptr) {
    if (ptr == nullptr) return;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialized) return;
    UntrackKey(ptr);
}

void MemoryTracker::TrackRealloc(void* oldPtr, void* newPtr, size_t newSize) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialized || !trackingEnabled) return;

    if (oldPtr == newPtr) {
        if (allocations.find(oldPtr) == allocations.end()) {
            TrackAlloc(newPtr, newSize, "general", "");
            return;
        }
        ResizeEntry(oldPtr, newSize);
        return;
    }

    if (newPtr == nullptr) {
        UntrackKey(oldPtr);
        return;
    }

    if (oldPtr != nullptr) {
        // Preserve the old entry's category/tag so a realloc is reported as a
        // move, not as a free plus an unrelated fresh "general" allocation.
        std::string category = "general";
        std::string tag;
        auto it = allocations.find(oldPtr);
        if (it != allocations.end()) {
            category = it->second.category;
            tag = it->second.tag;
        }
        UntrackKey(oldPtr);
        TrackAlloc(newPtr, newSize, category, tag);
        return;
    }

    TrackAlloc(newPtr, newSize, "general", "");
}

// Move an existing entry to a different category (and optionally size and tag).
// Returns false if there was nothing to re-key.
bool MemoryTracker::Rekey(void* ptr, size_t size, const std::string& category, const std::string& tag) {
    auto it = allocations.find(ptr);
    if (it == allocations.end()) return false;
    const std::string oldCategory = it->second.category;

    if (oldCategory == category) {
        ResizeEntry(ptr, size);
    } else {
        // Resize first while the entry still belongs to the old category, then
        // move it. Doing it in this order keeps every intermediate step's
        // category totals summing to the same live total.
        ResizeEntry(ptr, size);
        auto oldUsage = categoryUsage.find(oldCategory);
        if (oldUsage != categoryUsage.end()) {
            oldUsage->second -= std::min(size, oldUsage->second);
            if (oldUsage->second == 0) {
                categoryUsage.erase(oldUsage);
                categoryMBHistory.erase(oldCategory);
            }
        }
        categoryUsage[category] += size;
        it->second.category = category;
        if (MemAlloc* listEntry = FindListEntry(ptr)) listEntry->category = category;
        UpdateCategoryPeak(category, categoryUsage[category]);
    }

    if (!tag.empty()) {
        it->second.tag = tag;
        if (MemAlloc* listEntry = FindListEntry(ptr)) listEntry->tag = tag;
    }
    return true;
}

void MemoryTracker::TrackRaylibResource(void* key, size_t size, const std::string& category, const std::string& tag) {
    if (key == nullptr || size == 0) return;
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialized || !trackingEnabled) return;

    auto it = allocations.find(key);
    if (it != allocations.end()) {
        // Re-tracking an already-tracked resource: the size may have changed
        // (a texture reloaded at a new resolution) or the tag may have been
        // refined, so update in place instead of refusing.
        Rekey(key, size, category, tag);
        return;
    }
    TrackAlloc(key, size, category, tag);
}

void MemoryTracker::TrackTexture(Texture2D texture, const std::string& tag) {
    TrackRaylibResource(TextureKey(texture), TextureBytes(texture), "texture", tag);
}
void MemoryTracker::TrackMesh(Mesh mesh, const std::string& tag) {
    TrackRaylibResource(MeshKey(mesh), MeshBytes(mesh), "mesh", tag);
}
void MemoryTracker::TrackShader(Shader shader, const std::string& tag) {
    TrackRaylibResource(ShaderKey(shader), ShaderBytes(shader), "shader", tag);
}
void MemoryTracker::TrackModel(Model model, const std::string& tag) {
    TrackRaylibResource(ModelKey(model), ModelBytes(model), "model", tag);
}
void MemoryTracker::TrackImage(Image image, const std::string& tag) {
    TrackRaylibResource(ImageKey(image), ImageBytes(image), "image", tag);
}
void MemoryTracker::TrackRenderTexture(RenderTexture2D rt, const std::string& tag) {
    TrackRaylibResource(RenderTexKey(rt), RenderTextureBytes(rt), "rendertexture", tag);
}
void MemoryTracker::TrackWave(Wave wave, const std::string& tag) {
    TrackRaylibResource(WaveKey(wave), WaveBytes(wave), "audio", tag);
}

void MemoryTracker::UntrackTexture(Texture2D texture)   { UntrackKey(TextureKey(texture)); }
void MemoryTracker::UntrackMesh(Mesh mesh)               { UntrackKey(MeshKey(mesh)); }
void MemoryTracker::UntrackShader(Shader shader)         { UntrackKey(ShaderKey(shader)); }
void MemoryTracker::UntrackModel(Model model)            { UntrackKey(ModelKey(model)); }
void MemoryTracker::UntrackImage(Image image)            { UntrackKey(ImageKey(image)); }
void MemoryTracker::UntrackRenderTexture(RenderTexture2D rt) { UntrackKey(RenderTexKey(rt)); }
void MemoryTracker::UntrackWave(Wave wave)               { UntrackKey(WaveKey(wave)); }

void MemoryTracker::SetBudget(const std::string& category, size_t bytes) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    budgets[category] = bytes;
    // Re-arm the warning so a lowered budget reports immediately.
    overBudgetWarned[category] = false;
}

size_t MemoryTracker::GetBudget(const std::string& category) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = budgets.find(category);
    return (it == budgets.end()) ? 0 : it->second;
}

size_t MemoryTracker::GetCurrentUsage(const std::string& category) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    auto it = categoryUsage.find(category);
    return (it == categoryUsage.end()) ? 0 : it->second;
}

float MemoryTracker::GetBudgetUsagePercent(const std::string& category) const {
    size_t budget = GetBudget(category);
    if (budget == 0) return 0.0f;
    return (float)((double)GetCurrentUsage(category) / (double)budget * 100.0);
}

std::vector<std::string> MemoryTracker::GetOverBudgetCategories() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    std::vector<std::string> result;
    for (const auto& [category, budget] : budgets) {
        if (budget == 0) continue;
        auto usageIt = categoryUsage.find(category);
        size_t usage = (usageIt == categoryUsage.end()) ? 0 : usageIt->second;
        if (usage > budget) result.push_back(category);
    }
    return result;
}

void MemoryTracker::MarkLeakBaseline() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    leakBaselineSet = true;
    leakBaselineKeys.clear();
    for (const auto& [ptr, alloc] : allocations) {
        (void)alloc;
        leakBaselineKeys.insert(ptr);
    }
    leakBaselineFrame = frameNumber;
    leakBaselineTime = Now() - startTime;
    CONSOLE.Log("[Memory] Leak baseline set at frame " + std::to_string(leakBaselineFrame) +
                " (" + std::to_string(leakBaselineKeys.size()) + " live allocations).");
}

void MemoryTracker::ClearLeakBaseline() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    leakBaselineSet = false;
    leakBaselineKeys.clear();
    leakBaselineFrame = 0;
    leakBaselineTime = 0;
}

MemoryTracker::LeakReport MemoryTracker::DetectLeaks() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    LeakReport report;
    for (const auto& alloc : allocList) {
        if (alloc.isFreed) continue;
        if (leakBaselineSet) {
            // Only what was already live when the baseline was taken. Anything
            // allocated since is expected to be alive right now, so it is not
            // evidence of anything.
            if (leakBaselineKeys.find(alloc.ptr) == leakBaselineKeys.end()) continue;
        }
        report.leaks.push_back(alloc);
        report.totalLeakedBytes += alloc.size;
        report.leaksByCategory[alloc.category] += alloc.size;
        std::string tagKey = alloc.tag.empty() ? "<untagged>" : alloc.tag;
        report.leaksByTag[tagKey] += alloc.size;
    }
    // Biggest first: if only a couple of allocations are worth chasing, they
    // are at the top.
    std::sort(report.leaks.begin(), report.leaks.end(),
              [](const MemAlloc& a, const MemAlloc& b) { return a.size > b.size; });
    return report;
}

MemoryTracker::Snapshot MemoryTracker::TakeSnapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    Snapshot snap;
    snap.allocations = allocations;
    snap.frameNumber = frameNumber;
    snap.timestamp = Now() - startTime;
    return snap;
}

MemoryTracker::DiffResult MemoryTracker::DiffSnapshots(const Snapshot& before, const Snapshot& after) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    DiffResult result;

    for (const auto& [ptr, alloc] : after.allocations) {
        auto oldIt = before.allocations.find(ptr);
        if (oldIt == before.allocations.end()) {
            result.newAllocs.push_back(alloc);
            result.netBytes += (int64_t)alloc.size;
        } else if (oldIt->second.size != alloc.size) {
            MemAlloc changed = alloc;
            changed.tag = oldIt->second.tag; // keep the old tag for readability
            if (alloc.size > oldIt->second.size) {
                result.grownAllocs.push_back(changed);
                result.netBytes += (int64_t)(alloc.size - oldIt->second.size);
            } else {
                result.shrunkAllocs.push_back(changed);
                result.netBytes -= (int64_t)(oldIt->second.size - alloc.size);
            }
        }
    }

    for (const auto& [ptr, alloc] : before.allocations) {
        if (after.allocations.find(ptr) == after.allocations.end()) {
            result.freedAllocs.push_back(alloc);
            result.netBytes -= (int64_t)alloc.size;
        }
    }

    auto bySizeDesc = [](const MemAlloc& a, const MemAlloc& b) { return a.size > b.size; };
    std::sort(result.newAllocs.begin(), result.newAllocs.end(), bySizeDesc);
    std::sort(result.freedAllocs.begin(), result.freedAllocs.end(), bySizeDesc);
    std::sort(result.grownAllocs.begin(), result.grownAllocs.end(), bySizeDesc);
    std::sort(result.shrunkAllocs.begin(), result.shrunkAllocs.end(), bySizeDesc);
    return result;
}

void MemoryTracker::ClearTracked() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    allocations.clear();
    allocList.clear();
    categoryUsage.clear();
    currentAllocated = 0;
    CONSOLE.Log("[Memory] Tracking cleared. Cumulative counters kept (mem_reset to zero them).");
}

void MemoryTracker::ResetCounters() {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    totalAllocated = 0;
    totalFreed = 0;
    allocCount = 0;
    freeCount = 0;
    peakUsage.clear();
    liveBytesMBHistory.clear();
    processMBHistory.clear();
    categoryMBHistory.clear();
    // The peak is "high-water mark of live bytes since the reset", so it starts
    // from whatever is live right now rather than 0. Reporting a peak below the
    // current live total would be self-contradictory, and the only way back up
    // to the truth would be a new allocation.
    peakAllocated = currentAllocated;
    for (auto& [category, usage] : categoryUsage) peakUsage[category] = usage;
    CONSOLE.Log("[Memory] Counters reset. Live totals are untouched.");
}

size_t MemoryTracker::GetTotalAllocated() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return totalAllocated;
}
size_t MemoryTracker::GetTotalFreed() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return totalFreed;
}
size_t MemoryTracker::GetCurrentAllocated() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return currentAllocated;
}
size_t MemoryTracker::GetAllocationCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return allocList.size();
}
size_t MemoryTracker::GetPeakAllocated() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return peakAllocated;
}
size_t MemoryTracker::GetTotalAllocCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return allocCount;
}
size_t MemoryTracker::GetTotalFreeCount() const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    return freeCount;
}

void MemoryTracker::UpdateCategoryPeak(const std::string& category, size_t current) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    size_t& peak = peakUsage[category];
    if (current > peak) peak = current;
}

std::string MemoryTracker::CaptureCallstack(int skipFrames) {
    if (!captureCallstacks) return "";
    if (maxCallstackFrames <= 0) maxCallstackFrames = 1;
    if (maxCallstackFrames > 64) maxCallstackFrames = 64;

    std::string out;
    char buf[256];

#if defined(_WIN32)
    void* frames[64];
    USHORT n = CaptureStackBackTrace((DWORD)skipFrames, 64, frames, nullptr);
    for (USHORT i = 0; i < n && (int)i < maxCallstackFrames; ++i) {
        std::snprintf(buf, sizeof(buf), "  #%u  0x%p\n", i, frames[i]);
        out += buf;
    }
#elif defined(FLYENGINE_HAVE_CALLSTACK)
    void* frames[64];
    int n = backtrace(frames, 64);
    for (int i = skipFrames; i < n && (i - skipFrames) < maxCallstackFrames; ++i) {
        Dl_info info;
        std::memset(&info, 0, sizeof(info));
        // dladdr only sees dynamic symbols, so this resolves names for
        // anything exported. Both executables link with ENABLE_EXPORTS (which
        // adds -rdynamic on ELF), so ordinary functions are covered; the
        // fallback below is for static helpers and stripped code.
        if (dladdr(frames[i], &info) != 0 && info.dli_sname != nullptr) {
            int status = 0;
            char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
            const char* name = (status == 0 && demangled != nullptr) ? demangled : info.dli_sname;
            std::snprintf(buf, sizeof(buf), "  #%d  %s +0x%zx\n", i - skipFrames, name,
                          (size_t)((char*)frames[i] - (char*)info.dli_saddr));
            out += buf;
            if (demangled != nullptr) std::free(demangled);
        } else {
            // %p already prints the 0x prefix.
            std::snprintf(buf, sizeof(buf), "  #%d  %p  [no symbol]\n", i - skipFrames, frames[i]);
            out += buf;
        }
    }
#endif
    // Frames are emitted one per line, so the accumulator ends with a newline.
    // Drop it: "lines joined by \n" is a contract callers can rely on when they
    // print the block or concatenate two stacks, and it keeps the exported
    // callstack free of a stray blank final line.
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

void MemoryTracker::ExportCSV(const std::string& path) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!EnsureParentDir(path)) {
        lastExportMessage = "Failed to create directory for " + path;
        CONSOLE.LogError("[Memory] " + lastExportMessage);
        return;
    }
    std::ofstream file(path);
    if (!file) {
        lastExportMessage = "Could not open " + path + " for writing";
        CONSOLE.LogError("[Memory] " + lastExportMessage);
        return;
    }

    file << "pointer,size_bytes,category,tag,frame,timestamp_s,live\n";
    for (const auto& alloc : allocList) {
        char ptrBuf[32];
        std::snprintf(ptrBuf, sizeof(ptrBuf), "0x%llx", (unsigned long long)(uintptr_t)alloc.ptr);
        file << ptrBuf << ',' << alloc.size << ',' << CsvEscape(alloc.category) << ','
             << CsvEscape(alloc.tag) << ',' << alloc.frameNumber << ','
             << std::fixed << std::setprecision(3) << alloc.timestamp << ',' << (alloc.isFreed ? 0 : 1) << '\n';
    }

    file << "\n# summary\n";
    file << "total_allocated," << totalAllocated << "\n";
    file << "total_freed," << totalFreed << "\n";
    file << "current_allocated," << currentAllocated << "\n";
    file << "peak_allocated," << peakAllocated << "\n";
    file << "allocation_count," << allocCount << "\n";
    file << "free_count," << freeCount << "\n";
    file << "live_count," << allocList.size() << "\n";
    file << "frame," << frameNumber << "\n";

    lastExportMessage = "Exported " + std::to_string(allocList.size()) + " live allocations to " + path;
    CONSOLE.Log("[Memory] " + lastExportMessage);
}

void MemoryTracker::ExportJSON(const std::string& path) const {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!EnsureParentDir(path)) {
        lastExportMessage = "Failed to create directory for " + path;
        CONSOLE.LogError("[Memory] " + lastExportMessage);
        return;
    }
    std::ofstream file(path);
    if (!file) {
        lastExportMessage = "Could not open " + path + " for writing";
        CONSOLE.LogError("[Memory] " + lastExportMessage);
        return;
    }

    char buf[256];
    file << "{\n";
    file << "  \"frame\": " << frameNumber << ",\n";
    file << "  \"uptimeSeconds\": " << std::fixed << std::setprecision(3) << (Now() - startTime) << ",\n";
    file << "  \"trackingEnabled\": " << (trackingEnabled ? "true" : "false") << ",\n";
    file << "  \"totals\": {\n";
    file << "    \"allocated\": " << totalAllocated << ",\n";
    file << "    \"freed\": " << totalFreed << ",\n";
    file << "    \"current\": " << currentAllocated << ",\n";
    file << "    \"peak\": " << peakAllocated << ",\n";
    file << "    \"allocCount\": " << allocCount << ",\n";
    file << "    \"freeCount\": " << freeCount << ",\n";
    file << "    \"liveCount\": " << allocList.size() << "\n";
    file << "  },\n";
    file << "  \"process\": {\n";
    file << "    \"rssBytes\": " << GetProcessMemoryBytes() << ",\n";
    file << "    \"peakRssBytes\": " << GetProcessPeakMemoryBytes() << "\n";
    file << "  },\n";

    file << "  \"budgets\": {";
    bool first = true;
    for (const auto& [category, budget] : budgets) {
        file << (first ? "\n" : ",\n") << "    \"" << JsonEscape(category) << "\": " << budget;
        first = false;
    }
    if (!first) file << "\n  ";
    file << "},\n";

    file << "  \"categories\": {";
    first = true;
    for (const auto& [category, usage] : categoryUsage) {
        file << (first ? "\n" : ",\n") << "    \"" << JsonEscape(category) << "\": " << usage;
        first = false;
    }
    if (!first) file << "\n  ";
    file << "},\n";

    file << "  \"allocations\": [";
    first = true;
    for (const auto& alloc : allocList) {
        std::snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)(uintptr_t)alloc.ptr);
        file << (first ? "\n" : ",\n");
        file << "    {\"ptr\": \"" << buf << "\", \"size\": " << alloc.size
             << ", \"category\": \"" << JsonEscape(alloc.category)
             << "\", \"tag\": \"" << JsonEscape(alloc.tag)
             << "\", \"frame\": " << alloc.frameNumber
             << ", \"t\": " << std::fixed << std::setprecision(3) << alloc.timestamp
             << ", \"callstack\": \"" << JsonEscape(alloc.callstack) << "\"}";
        first = false;
    }
    if (!first) file << "\n  ";
    file << "]\n";
    file << "}\n";

    lastExportMessage = "Exported " + std::to_string(allocList.size()) + " live allocations to " + path;
    CONSOLE.Log("[Memory] " + lastExportMessage);
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

void MemoryTracker::Draw() {
    if (!visible) return;

    std::lock_guard<std::recursive_mutex> lock(mutex);
    if (!initialized) return;

    ImGui::SetNextWindowSize(ImVec2(920, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory Tracker", &visible, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (ImGui::Button("Snapshot")) {
        currentSnapshot = TakeSnapshot();
        hasSnapshot = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Freeze the current live set so it can be diffed against a later one.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Diff vs Baseline")) {
        if (hasBaselineSnapshot) {
            // Compare now against the stored baseline snapshot, and remember the
            // result so it stays on screen while the window is open.
            baselineDiff = DiffSnapshots(baselineSnapshot, TakeSnapshot());
            hasBaselineDiff = true;
            uiTab = 4;
        } else {
            CONSOLE.LogWarn("[Memory] Set a baseline snapshot first.");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Set Baseline")) {
        baselineSnapshot = TakeSnapshot();
        hasBaselineSnapshot = true;
        hasBaselineDiff = false;
        CONSOLE.Log("[Memory] Snapshot baseline stored at frame " +
                    std::to_string(baselineSnapshot.frameNumber) + ".");
    }
    ImGui::SameLine();
    if (ImGui::Button("Mark Leak Baseline")) {
        MarkLeakBaseline();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (ImGui::Button("Clear Tracked")) {
        ClearTracked();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("frame %llu", (unsigned long long)frameNumber);

    ImGui::Separator();

    // uiTab is remembered so closing and reopening the window (F9 twice) lands
    // back on the tab you were reading, rather than resetting to Overview.
    static const char* tabs[] = {"Overview", "Allocations", "Leaks", "Budgets", "Snapshot Diff", "Settings"};
    ImGui::SetNextItemWidth(520.0f);
    if (ImGui::BeginTabBar("##memtabs", ImGuiTabBarFlags_Reorderable)) {
        for (int i = 0; i < 6; ++i) {
            ImGuiTabItemFlags flags = (i == uiTab) ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
            if (!ImGui::BeginTabItem(tabs[i], nullptr, flags)) continue;
            uiTab = i;
            switch (i) {
                case 0: DrawOverview(); break;
                case 1: DrawAllocationList(); break;
                case 2: DrawLeakReport(); break;
                case 3: DrawBudgetBars(); break;
                case 4: DrawSnapshotDiff(); break;
                case 5: DrawSettings(); break;
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

void MemoryTracker::DrawOverview() {
    const size_t procBytes = GetProcessMemoryBytes();
    const size_t procPeak = GetProcessPeakMemoryBytes();

    ImGui::Text("Frame %llu    Uptime %s", (unsigned long long)frameNumber, FormatSeconds(Now() - startTime).c_str());
    ImGui::Separator();

    if (ImGui::BeginTable("##memtotals", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Metric");
        ImGui::TableSetupColumn("Value");
        ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();

        auto totalRow = [](const char* label, const std::string& value, const char* extra) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(label);
            ImGui::TableNextColumn(); ImGui::TextUnformatted(value.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", extra);
        };

        totalRow("Tracked live", FormatBytes(currentAllocated),
                 (peakAllocated > 0 ? "peak " + FormatBytes(peakAllocated) : "peak n/a").c_str());
        totalRow("Live allocations", std::to_string(allocList.size()),
                 (std::to_string(allocCount) + " alloc / " + std::to_string(freeCount) + " free").c_str());
        totalRow("Cumulative allocated", FormatBytes(totalAllocated), "lifetime");
        totalRow("Cumulative freed", FormatBytes(totalFreed), "lifetime");

        if (procBytes > 0) {
            totalRow("Process RSS", FormatBytes(procBytes),
                     (procPeak > 0 ? "peak " + FormatBytes(procPeak) : "").c_str());
            // The untracked remainder is the interesting number: it is the heap,
            // the GL driver, ImGui's own font atlas, and anything nobody called
            // TrackAlloc on. If this dwarfs the tracked total, the leak is
            // somewhere the tracker was never told about.
            if (procBytes > currentAllocated) {
                totalRow("Untracked remainder", FormatBytes(procBytes - currentAllocated), "of RSS");
            }
        } else {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted("Process RSS");
            ImGui::TableNextColumn(); ImGui::TextDisabled("not available on this platform");
            ImGui::TableNextColumn();
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Graphs (last 120 frames)");

    if (!liveBytesMBHistory.empty()) {
        ImGui::PlotLines("Tracked live (MB)", liveBytesMBHistory.data(),
                         (int)liveBytesMBHistory.size(), 0, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 80));
    }
    if (!processMBHistory.empty()) {
        ImGui::PlotLines("Process RSS (MB)", processMBHistory.data(),
                         (int)processMBHistory.size(), 0, nullptr, 0.0f, FLT_MAX, ImVec2(-1, 80));
    }

    ImGui::Spacing();
    DrawCategoryBreakdown();
}

void MemoryTracker::DrawCategoryBreakdown() {
    ImGui::SeparatorText("By category");

    if (categoryUsage.empty()) {
        ImGui::TextDisabled("Nothing tracked yet. Call MEMORY_TRACKER.TrackTexture() and friends "
                            "where resources are created.");
        return;
    }

    // Sort by usage so the table order does not jump around every frame.
    std::vector<std::pair<std::string, size_t>> rows(categoryUsage.begin(), categoryUsage.end());
    std::sort(rows.begin(), rows.end(),
              [](const std::pair<std::string, size_t>& a, const std::pair<std::string, size_t>& b) {
                  return a.second > b.second;
              });

    const size_t maxUsage = rows.front().second;
    if (ImGui::BeginTable("##memcat", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Category");
        ImGui::TableSetupColumn("Live", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Peak", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (const auto& [category, usage] : rows) {
            auto peakIt = peakUsage.find(category);
            size_t peak = (peakIt == peakUsage.end()) ? usage : peakIt->second;

            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(category.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(usage).c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", FormatBytes(peak).c_str());

            ImGui::TableNextColumn();
            // Proportional bar, coloured by how much of the tracked total this
            // one category represents.
            const float frac = maxUsage > 0 ? (float)usage / (float)maxUsage : 0.0f;
            ImVec2 avail = ImGui::GetContentRegionAvail();
            ImGui::ProgressBar(frac, ImVec2(avail.x, 14),
                               (FormatBytes(usage) + "  (" + FormatMB(usage) + " MB)").c_str());
        }
        ImGui::EndTable();
    }

    // Per-category history, one line each, for the categories that actually
    // have samples.
    for (const auto& [category, hist] : categoryMBHistory) {
        if (hist.size() < 2) continue;
        ImGui::PlotLines(("[" + category + "] MB").c_str(), hist.data(), (int)hist.size(), 0,
                         nullptr, 0.0f, FLT_MAX, ImVec2(-1, 40));
    }
}

void MemoryTracker::DrawAllocationList() {
    // Filters
    static char categoryBuf[64] = "";
    static char tagBuf[128] = "";
    if (ImGui::InputText("Category contains", categoryBuf, sizeof(categoryBuf))) {
        categoryFilter = categoryBuf;
    }
    ImGui::SameLine();
    if (ImGui::InputText("Tag contains", tagBuf, sizeof(tagBuf))) {
        tagFilter = tagBuf;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        categoryBuf[0] = '\0';
        tagBuf[0] = '\0';
        categoryFilter.clear();
        tagFilter.clear();
    }

    ImGui::Checkbox("Hide allocations under", &hideSmallAllocs);
    if (hideSmallAllocs) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("KB", &minSizeKB, 0.0f, 1024.0f, "%.0f KB");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Sort by size", &sortBySize);

    const size_t minBytes = hideSmallAllocs ? (size_t)(minSizeKB * 1024.0f) : 0;

    std::vector<const MemAlloc*> filtered;
    filtered.reserve(allocList.size());
    for (const auto& alloc : allocList) {
        if (alloc.size < minBytes) continue;
        if (!categoryFilter.empty() && alloc.category.find(categoryFilter) == std::string::npos) continue;
        if (!tagFilter.empty() && alloc.tag.find(tagFilter) == std::string::npos) continue;
        filtered.push_back(&alloc);
    }
    if (sortBySize) {
        std::stable_sort(filtered.begin(), filtered.end(),
                         [](const MemAlloc* a, const MemAlloc* b) { return a->size > b->size; });
    }

    ImGui::Separator();
    ImGui::Text("%llu of %llu live allocations, %s total",
                (unsigned long long)filtered.size(), (unsigned long long)allocList.size(),
                FormatBytes(currentAllocated).c_str());

    // Cap the rows drawn. A long session can hold tens of thousands of live
    // allocations and an unbounded ImGui table would stall the frame every
    // time it is opened; the filters above are the way to reach the rest.
    static constexpr size_t MAX_ROWS = 500;

    ImGui::BeginChild("##alloclist", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##allocs", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit, ImVec2(0, ImGui::GetContentRegionAvail().y))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ptr", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableHeadersRow();

        size_t drawn = 0;
        for (const MemAlloc* alloc : filtered) {
            if (drawn++ >= MAX_ROWS) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("... %llu more (narrow the filter to see them)",
                                    (unsigned long long)(filtered.size() - MAX_ROWS));
                break;
            }

            ImGui::PushID((int)(uintptr_t)alloc->ptr);
            ImGui::TableNextRow();

            char ptrBuf[32];
            std::snprintf(ptrBuf, sizeof(ptrBuf), "0x%llx", (unsigned long long)(uintptr_t)alloc->ptr);
            ImGui::TableNextColumn();
            bool selected = (selectedAlloc == alloc->ptr);
            if (ImGui::Selectable(ptrBuf, selected)) {
                selectedAlloc = alloc->ptr;
            }

            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(alloc->size).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(alloc->category.c_str());
            ImGui::TableNextColumn();
            if (alloc->tag.empty()) ImGui::TextDisabled("<untagged>");
            else ImGui::TextUnformatted(alloc->tag.c_str());
            ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)alloc->frameNumber);
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", FormatSeconds(Now() - startTime - alloc->timestamp).c_str());

            if (selected) {
                ImGui::PopID();
                DrawCallstackViewer(*alloc);
                ImGui::PushID((int)(uintptr_t)alloc->ptr);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void MemoryTracker::DrawCallstackViewer(const MemAlloc& alloc) {
    if (alloc.callstack.empty()) {
        ImGui::TextDisabled("No callstack captured "
                            "(enable it in Settings, or the platform has no support).");
        return;
    }

    char header[256];
    std::snprintf(header, sizeof(header),
                  "Allocation 0x%llx  %s  [%s]  %s  frame %llu  age %s",
                  (unsigned long long)(uintptr_t)alloc.ptr,
                  FormatBytes(alloc.size).c_str(), alloc.category.c_str(),
                  alloc.tag.empty() ? "<untagged>" : alloc.tag.c_str(),
                  (unsigned long long)alloc.frameNumber,
                  FormatSeconds(Now() - startTime - alloc.timestamp).c_str());
    ImGui::SeparatorText(header);

    // The stack was captured as a newline-joined list of "  #N name" lines.
    // Render it in a fixed monospace-ish block so indentation lines up.
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.85f, 0.75f, 1.0f));
    ImGui::TextUnformatted(alloc.callstack.c_str());
    ImGui::PopStyleColor();
}

void MemoryTracker::DrawLeakReport() {
    ImGui::TextWrapped("A leak here means an allocation that was live when the baseline was set and "
                       "is still live now -- it outlived whatever created it. With no baseline set, "
                       "this lists everything currently live, so it doubles as an inventory.");
    ImGui::Spacing();

    if (ImGui::Button("Mark Leak Baseline (now)")) MarkLeakBaseline();
    ImGui::SameLine();
    if (ImGui::Button("Clear Baseline")) ClearLeakBaseline();

    ImGui::Spacing();
    if (leakBaselineSet) {
        ImGui::Text("Baseline: frame %llu, %s ago",
                    (unsigned long long)leakBaselineFrame,
                    FormatSeconds(Now() - startTime - leakBaselineTime).c_str());
    } else {
        ImGui::TextDisabled("No baseline set -- reporting all live allocations.");
    }
    ImGui::Separator();

    LeakReport report = DetectLeaks();

    if (report.leaks.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "No leaks detected.");
        return;
    }

    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "%s in %llu allocation(s)",
                       FormatBytes(report.totalLeakedBytes).c_str(),
                       (unsigned long long)report.leaks.size());

    if (ImGui::BeginTable("##leakcat", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Group");
        ImGui::TableSetupColumn("Bytes");
        ImGui::TableHeadersRow();

        for (const auto& [name, bytes] : report.leaksByCategory) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("category: %s", name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatBytes(bytes).c_str());
        }
        for (const auto& [name, bytes] : report.leaksByTag) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("tag: %s", name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatBytes(bytes).c_str());
        }
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Allocations");
    static constexpr size_t MAX_LEAK_ROWS = 300;
    if (ImGui::BeginTable("##leaks", 5,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                          ImGuiTableFlags_SizingFixedFit, ImVec2(0, 260))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Ptr", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Age", ImGuiTableColumnFlags_WidthFixed, 80.0f);
        ImGui::TableHeadersRow();

        size_t drawn = 0;
        for (const auto& leak : report.leaks) {
            if (drawn++ >= MAX_LEAK_ROWS) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("... %llu more",
                                    (unsigned long long)(report.leaks.size() - MAX_LEAK_ROWS));
                break;
            }
            ImGui::PushID((int)(uintptr_t)leak.ptr);
            ImGui::TableNextRow();
            char ptrBuf[32];
            std::snprintf(ptrBuf, sizeof(ptrBuf), "0x%llx", (unsigned long long)(uintptr_t)leak.ptr);
            ImGui::TableNextColumn();
            bool sel = (selectedAlloc == leak.ptr);
            if (ImGui::Selectable(ptrBuf, sel)) selectedAlloc = leak.ptr;
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(leak.size).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(leak.category.c_str());
            ImGui::TableNextColumn();
            if (leak.tag.empty()) ImGui::TextDisabled("<untagged>");
            else ImGui::TextUnformatted(leak.tag.c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", FormatSeconds(Now() - startTime - leak.timestamp).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    // Callstack of whichever leak row is selected, resolved from the live list.
    if (selectedAlloc != nullptr) {
        for (const auto& alloc : allocList) {
            if (alloc.ptr == selectedAlloc) {
                DrawCallstackViewer(alloc);
                break;
            }
        }
    }
}

void MemoryTracker::DrawBudgetBars() {
    ImGui::TextWrapped("Budgets are advisory: they drive these bars and a one-shot console warning "
                       "when exceeded. Nothing is freed or refused -- that would be the resource "
                       "owner's call, not the tracker's.");
    ImGui::Spacing();

    static char newCategory[64] = "";
    static double newMegabytes = 512.0;

    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("New category", newCategory, sizeof(newCategory));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputDouble("MB", &newMegabytes, 1.0, 64.0, "%.0f");
    ImGui::SameLine();
    if (ImGui::Button("Set Budget") && !std::string(newCategory).empty()) {
        SetBudget(newCategory, (size_t)(newMegabytes * 1024.0 * 1024.0));
        std::snprintf(newCategory, sizeof(newCategory), "%s", "");
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear Budgets")) {
        budgets.clear();
        overBudgetWarned.clear();
    }

    ImGui::Separator();

    if (budgets.empty()) {
        ImGui::TextDisabled("No budgets set. The player's --maxmem4G flag sets one for you.");
        return;
    }

    std::vector<std::string> over = GetOverBudgetCategories();

    for (const auto& [category, budget] : budgets) {
        size_t usage = GetCurrentUsage(category);
        float pct = budget > 0 ? (float)usage / (float)budget * 100.0f : 0.0f;
        bool isOver = usage > budget;

        ImGui::PushID(category.c_str());
        ImGui::Text("%s", category.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s of %s", FormatBytes(usage).c_str(), FormatBytes(budget).c_str());
        if (isOver) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f), "OVER BUDGET");
        }
        ImGui::ProgressBar(pct / 100.0f, ImVec2(-1, 16),
                           (std::to_string((int)(pct * 10.0) / 10.0) + "%").c_str());
        ImGui::PopID();
    }

    if (!over.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.3f, 1.0f),
                           "%zu categor(y/ies) over budget", over.size());
    }
}

void MemoryTracker::DrawSnapshotDiff() {
    ImGui::TextWrapped("A snapshot is a frozen copy of the live set. Diffing two of them shows what "
                       "a span of gameplay allocated, freed, grew or shrank -- which is how you "
                       "tell a one-off load spike from a leak that grows every loop.");
    ImGui::Spacing();

    if (ImGui::Button("Take Snapshot Now")) {
        currentSnapshot = TakeSnapshot();
        hasSnapshot = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Use As Baseline")) {
        baselineSnapshot = currentSnapshot;
        hasBaselineSnapshot = hasSnapshot;
        hasBaselineDiff = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Diff Current vs Baseline")) {
        if (hasBaselineSnapshot) {
            baselineDiff = DiffSnapshots(baselineSnapshot, TakeSnapshot());
            hasBaselineDiff = true;
        } else {
            CONSOLE.LogWarn("[Memory] Set a baseline snapshot first (Set Baseline above).");
        }
    }

    ImGui::Spacing();
    if (hasSnapshot) {
        ImGui::Text("Snapshot: frame %llu, %s in %llu allocations",
                    (unsigned long long)currentSnapshot.frameNumber,
                    FormatBytes([&]{
                        size_t total = 0;
                        for (const auto& [p, a] : currentSnapshot.allocations) { (void)p; total += a.size; }
                        return total;
                    }()).c_str(),
                    (unsigned long long)currentSnapshot.allocations.size());
    } else {
        ImGui::TextDisabled("No snapshot taken yet.");
    }

    if (!hasBaselineDiff) return;

    const DiffResult& diff = baselineDiff;
    ImGui::Separator();
    ImGui::Text("Diff since baseline: net ");
    ImGui::SameLine();
    if (diff.netBytes > 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.3f, 1.0f), "+%s", FormatBytes((size_t)diff.netBytes).c_str());
    } else if (diff.netBytes < 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "-%s", FormatBytes((size_t)(-diff.netBytes)).c_str());
    } else {
        ImGui::TextUnformatted("0 B");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%zu new, %zu freed, %zu grown, %zu shrunk)",
                        diff.newAllocs.size(), diff.freedAllocs.size(),
                        diff.grownAllocs.size(), diff.shrunkAllocs.size());

    struct DiffSection {
        const char* label;
        const std::vector<MemAlloc>* allocs;
        ImVec4 color;
    };
    const DiffSection sections[] = {
        {"New allocations", &diff.newAllocs,   ImVec4(0.95f, 0.75f, 0.3f, 1.0f)},
        {"Freed",           &diff.freedAllocs, ImVec4(0.4f, 0.9f, 0.4f, 1.0f)},
        {"Grown",           &diff.grownAllocs, ImVec4(0.6f, 0.7f, 0.95f, 1.0f)},
        {"Shrunk",          &diff.shrunkAllocs, ImVec4(0.7f, 0.7f, 0.7f, 1.0f)},
    };

    for (const auto& section : sections) {
        if (section.allocs->empty()) continue;
        char header[128];
        std::snprintf(header, sizeof(header), "%s (%zu)", section.label, section.allocs->size());
        ImGui::SeparatorText(header);
        if (ImGui::BeginTable("##diff", 4,
                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_SizingFixedFit, ImVec2(0, 160))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Ptr", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 90.0f);
            ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            size_t drawn = 0;
            for (const auto& alloc : *section.allocs) {
                if (drawn++ >= 100) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextDisabled("... %zu more", section.allocs->size() - 100);
                    break;
                }
                ImGui::TableNextRow();
                char ptrBuf[32];
                std::snprintf(ptrBuf, sizeof(ptrBuf), "0x%llx", (unsigned long long)(uintptr_t)alloc.ptr);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(ptrBuf);
                ImGui::TableNextColumn(); ImGui::TextUnformatted(FormatBytes(alloc.size).c_str());
                ImGui::TableNextColumn(); ImGui::TextUnformatted(alloc.category.c_str());
                ImGui::TableNextColumn();
                if (alloc.tag.empty()) ImGui::TextDisabled("<untagged>");
                else ImGui::TextUnformatted(alloc.tag.c_str());
            }
            ImGui::EndTable();
        }
    }
}

void MemoryTracker::DrawSettings() {
    ImGui::TextWrapped("Tracking is opt-in: only resources somebody explicitly tracked appear "
                       "below. Turn it off for release play -- capturing a callstack per "
                       "allocation is not free.");
    ImGui::Spacing();

    ImGui::Checkbox("Tracking enabled", &trackingEnabled);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("While off, TrackAlloc/Track* calls are dropped. Already-tracked "
                          "allocations stay listed until they are freed or cleared.");
    }

    bool capture = captureCallstacks;
    if (ImGui::Checkbox("Capture callstacks", &capture)) SetCaptureCallstacks(capture);
    if (captureCallstacks) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160.0f);
        int frames = maxCallstackFrames;
        if (ImGui::SliderInt("##maxframes", &frames, 4, 64)) SetMaxCallstackFrames(frames);
        ImGui::SameLine();
        ImGui::TextDisabled("max frames: %d", maxCallstackFrames);
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Export");

    static char csvPath[256] = "Captures/memory.csv";
    static char jsonPath[256] = "Captures/memory.json";
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputText("##csvpath", csvPath, sizeof(csvPath));
    ImGui::SameLine();
    if (ImGui::Button("Export CSV")) ExportCSV(csvPath);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(300.0f);
    ImGui::InputText("##jsonpath", jsonPath, sizeof(jsonPath));
    ImGui::SameLine();
    if (ImGui::Button("Export JSON")) ExportJSON(jsonPath);

    if (!lastExportMessage.empty()) {
        ImGui::TextDisabled("%s", lastExportMessage.c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Danger zone");
    if (ImGui::Button("Clear Tracked (forget, do not free)")) ClearTracked();
    ImGui::SameLine();
    if (ImGui::Button("Reset Counters")) ResetCounters();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Zeroes cumulative allocated/freed and the peaks, so the next "
                          "measurement starts from a clean baseline.");
    }
}

} // namespace TechTools