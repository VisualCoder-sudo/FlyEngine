/*
 * TechnicalTools.hpp - Developer tools suite for Play mode
 * Console, profiler, inspector, physics debug, replay, capture
 */

#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <deque>
#include <memory>
#include <mutex>
#include <utility> // std::forward, for Console::Register
#include <set>
#include <cfloat>
#include <climits>
#include <filesystem>
#include "raylib.h"

class Engine;
class Entity;

namespace TechTools {

// ============================================================================
// CONSOLE VARIABLES (CVars)
// ============================================================================

enum class CVarType {
    Bool,
    Int,
    Float,
    String,
    Color
};

struct CVar {
    std::string name;
    std::string description;
    CVarType type;
    
    // Storage (union-like via type)
    bool boolVal = false;
    int intVal = 0;
    float floatVal = 0.0f;
    std::string stringVal;
    Color colorVal = WHITE;
    
    // Constraints
    bool hasMin = false, hasMax = false;
    float minVal = 0, maxVal = 0;
    
    // Callbacks
    std::function<void(const CVar&)> onChanged;
};

class CVarSystem {
public:
    static CVarSystem& Instance();
    
    // Register cvars
    CVar* RegisterBool(const std::string& name, bool defaultVal, const std::string& desc);
    CVar* RegisterInt(const std::string& name, int defaultVal, const std::string& desc, int min = INT_MIN, int max = INT_MAX);
    CVar* RegisterFloat(const std::string& name, float defaultVal, const std::string& desc, float min = -FLT_MAX, float max = FLT_MAX);
    CVar* RegisterString(const std::string& name, const std::string& defaultVal, const std::string& desc);
    CVar* RegisterColor(const std::string& name, Color defaultVal, const std::string& desc);
    
    // Find/Get
    CVar* Find(const std::string& name);
    const CVar* Find(const std::string& name) const;
    
    // Set with notification
    bool SetBool(const std::string& name, bool val);
    bool SetInt(const std::string& name, int val);
    bool SetFloat(const std::string& name, float val);
    bool SetString(const std::string& name, const std::string& val);
    bool SetColor(const std::string& name, Color val);
    
    // Get (convenience)
    bool GetBool(const std::string& name, bool fallback = false) const;
    int GetInt(const std::string& name, int fallback = 0) const;
    float GetFloat(const std::string& name, float fallback = 0.0f) const;
    const std::string& GetString(const std::string& name, const std::string& fallback = "") const;
    Color GetColor(const std::string& name, Color fallback = WHITE) const;
    
    // List all
    const std::map<std::string, CVar>& GetAll() const { return cvars; }
    
    // Save/load to config file
    void SaveToFile(const std::string& path);
    void LoadFromFile(const std::string& path);
    
public:
    CVarSystem() = default;
private:
    std::map<std::string, CVar> cvars;
    mutable std::mutex mutex;
};

// Helper macros for easy cvar access
#define CVAR_BOOL(name) TechTools::CVarSystem::Instance().GetBool(name)
#define CVAR_INT(name) TechTools::CVarSystem::Instance().GetInt(name)
#define CVAR_FLOAT(name) TechTools::CVarSystem::Instance().GetFloat(name)
#define CVAR_STRING(name) TechTools::CVarSystem::Instance().GetString(name)
#define CVAR_COLOR(name) TechTools::CVarSystem::Instance().GetColor(name)


// ============================================================================
// IN-GAME CONSOLE
// ============================================================================

struct ConsoleCommand {
    std::string name;
    std::string description;
    std::string usage;
    std::function<bool(const std::vector<std::string>& args)> handler;
};

class Console {
public:
    static Console& Instance();
    
    // Initialize (call once)
    void Initialize(Engine* engine);
    void Shutdown();
    
    // Main update/draw (call from Engine)
    void Update(float dt);
    void Draw();
    
    // Toggle visibility
    void Toggle() { visible = !visible; }
    bool IsVisible() const { return visible; }
    
    // Execute command string
    bool Execute(const std::string& commandLine);
    
    // Register commands
    void RegisterCommand(const ConsoleCommand& cmd);

    // Registers a command from its parts. RegisterBuiltinCommands() used to
    // spell out the same four-field aggregate 21 times:
    //
    //     console.RegisterCommand({ "name", "desc", "usage", handler });
    //
    // The bodies differ, so only the boilerplate was factored out -- this is
    // that boilerplate, with the handler deduced so each site passes a plain
    // lambda instead of a std::function.
    template <typename Fn>
    void Register(const std::string& name, const std::string& description,
                  const std::string& usage, Fn&& handler) {
        RegisterCommand(ConsoleCommand{ name, description, usage, std::forward<Fn>(handler) });
    }
    
    // Log output (from anywhere)
    void Log(const std::string& msg);
    void LogError(const std::string& msg);
    void LogWarn(const std::string& msg);
    
    // History
    const std::deque<std::string>& GetHistory() const { return history; }
    const std::deque<std::string>& GetOutput() const { return output; }
    
    // Clear output
    void Clear() { output.clear(); history.clear(); }
    
    // Auto-complete
    std::vector<std::string> GetCompletions(const std::string& partial) const;
    
public:
    Console() = default;
    void AddOutput(const std::string& msg, Color color);
    std::map<std::string, ConsoleCommand> commands;
private:
    Engine* engine = nullptr;
    bool visible = false;
    std::string inputBuffer;
    std::deque<std::string> history;
    std::deque<std::string> output;
    int historyIndex = -1;
    std::vector<std::string> completions;
    int completionIndex = 0;
    int scrollOffset = 0;
    
    void ProcessInput();
    void ExecuteCommand(const std::string& cmdLine);
    std::vector<std::string> Tokenize(const std::string& str);
    void RebuildCompletions();
};

// Built-in commands (registered automatically)
void RegisterBuiltinCommands(Console& console, Engine* engine, CVarSystem& cvars);


// ============================================================================
// PROFILER / PERFORMANCE MONITOR
// ============================================================================

struct ProfileScope {
    const char* name;
    uint64_t startCycle;
    int depth;
};

class Profiler {
public:
    static Profiler& Instance();
    
    void BeginFrame();
    void EndFrame();
    
    // Scoped profiling (RAII)
    class Scope {
    public:
        Scope(const char* name);
        ~Scope();
    private:
        const char* name;
    };
    
    // Manual markers
    void BeginScope(const char* name);
    void EndScope(const char* name);
    
    // Data access
    struct ZoneData {
        std::string name;
        double totalMs = 0;
        double minMs = FLT_MAX;
        double maxMs = 0;
        int count = 0;
        int depth = 0;
        double selfMs = 0;
    };
    
    const std::map<std::string, ZoneData>& GetZoneData() const { return zoneData; }
    double GetFrameTimeMs() const { return frameTimeMs; }
    int GetFrameCount() const { return frameCount; }
    
    // History for graphs (120 frames)
    const std::vector<double>& GetFrameTimeHistory() const { return frameTimeHistory; }
    const std::vector<double>& GetZoneHistory(const std::string& name) const;
    
    void SetEnabled(bool enabled) { enabled_ = enabled; }
    bool IsEnabled() const { return enabled_; }
    
public:
    Profiler() = default;
private:
    bool enabled_ = true;
    
    struct ActiveZone {
        const char* name;
        uint64_t startCycle;
        int depth;
        double parentSelfMs;
    };
    
    std::vector<ActiveZone> activeZones;
    std::map<std::string, ZoneData> zoneData;
    std::map<std::string, std::vector<double>> zoneHistories;
    std::vector<double> frameTimeHistory;
    double frameTimeMs = 0;
    int frameCount = 0;
    uint64_t frameStartCycle = 0;
    static constexpr int MAX_HISTORY = 120;
    
    uint64_t GetCycles() const;
    double CyclesToMs(uint64_t cycles) const;
};

// Convenience macro
#define PROFILE_SCOPE(name) TechTools::Profiler::Scope _prof_scope__(name)
#define PROFILE_FUNCTION() PROFILE_SCOPE(__FUNCTION__)


// ============================================================================
// ENTITY INSPECTOR
// ============================================================================

class EntityInspector {
public:
    static EntityInspector& Instance();
    
    void Initialize(Engine* engine);
    void Update(float dt);
    void Draw();
    
    // Pick entity under mouse
    void PickEntityUnderMouse();
    void SelectEntity(Entity* entity);
    Entity* GetSelectedEntity() const { return selectedEntity; }
    
    // Inspect selected entity
    void DrawInspectorPanel();
    
    // Toggle
    void Toggle() { visible = !visible; }
    bool IsVisible() const { return visible; }
    
public:
    EntityInspector() = default;
private:
    Engine* engine = nullptr;
    bool visible = false;
    Entity* selectedEntity = nullptr;
    Entity* hoveredEntity = nullptr;
    
    // Component editing
    std::string editBuffer;
    int editComponentIndex = -1;
};

// ============================================================================
// PHYSICS DEBUG VISUALIZER
// ============================================================================

class PhysicsDebugVisualizer {
public:
    static PhysicsDebugVisualizer& Instance();
    
    void Initialize(Engine* engine);
    // 3D overlay plus the ImGui layer panel. The panel is what makes the
    // per-layer flags reachable; before it existed, F2 could only flip every
    // layer at once, so the individual toggles had no way to be called.
    void Draw();

    // Show/hide, like every other tool in this suite. Revealing the overlay
    // with nothing selected enables a useful default rather than an empty
    // screen, but SetVisible lets a caller that is about to turn on a specific
    // layer avoid picking up the defaults as well.
    void SetVisible(bool on) {
        if (on && !visible && !AnyLayerEnabled()) {
            showShapes = true;
            showVelocities = true;
        }
        visible = on;
    }
    void Toggle() { SetVisible(!visible); }
    bool IsVisible() const { return visible; }
    bool AnyLayerEnabled() const {
        return showShapes || showVelocities || showContacts || showRaycasts ||
               showAABBs || showJoints;
    }
    // Per-layer setters. The F2 panel drives these through the flags directly;
    // these exist for the console commands and for script-side callers that
    // would rather not go through the panel.
    void ToggleShapes() { showShapes = !showShapes; }
    void ToggleVelocities() { showVelocities = !showVelocities; }
    void ToggleContacts() { showContacts = !showContacts; }
    void ToggleRaycasts() { showRaycasts = !showRaycasts; }
    void ToggleAABBs() { showAABBs = !showAABBs; }
    void ToggleJoints() { showJoints = !showJoints; }
    void SetAllLayers(bool on) {
        showShapes = showVelocities = showContacts = showRaycasts = showAABBs = showJoints = on;
    }
    
    // Settings
    bool visible = false;
    bool showShapes = false;
    bool showVelocities = false;
    bool showContacts = false;
    bool showRaycasts = false;
    bool showAABBs = false;
    bool showJoints = false;
    float velocityScale = 0.1f;
    float contactScale = 1.0f;
    
    // Raycast debug (set from CharacterController, etc.)
    struct DebugRaycast {
        Vector3 origin, direction;
        float maxDist;
        bool hit;
        Vector3 hitPoint, hitNormal;
        Color color = GREEN;
    };
    void AddDebugRaycast(const DebugRaycast& rc) { debugRaycasts.push_back(rc); }
    void ClearDebugRaycasts() { debugRaycasts.clear(); }
    
public:
    PhysicsDebugVisualizer() = default;
private:
    void DrawPanel();

    Engine* engine = nullptr;
    std::vector<DebugRaycast> debugRaycasts;
};


// ============================================================================
// INPUT RECORDER / REPLAY
// ============================================================================

struct InputFrame {
    uint32_t frameNumber;
    float dt;
    Vector2 moveInput;
    Vector2 lookInput;
    bool jump;
    bool sprint;
    bool crouch;
    // Keyboard state snapshot
    bool keys[512] = {false};
    // Mouse
    Vector2 mousePos;
    Vector2 mouseDelta;
    bool mouseButtons[8] = {false};
};

class InputRecorder {
public:
    static InputRecorder& Instance();
    
    void Initialize();
    void Shutdown();
    
    // Recording
    void StartRecording(const std::string& name);
    void StopRecording();
    void RecordFrame(const InputFrame& frame);
    bool IsRecording() const { return recording; }
    
    // Playback
    void StartPlayback(const std::string& name);
    void StopPlayback();
    bool IsPlaying() const { return playing; }
    const InputFrame* GetNextFrame();
    
    // File I/O
    bool SaveToFile(const std::string& path);
    bool LoadFromFile(const std::string& path);
    
    // List recordings
    std::vector<std::string> ListRecordings() const;
    
    // Stats
    size_t GetFrameCount() const { return frames.size(); }
    float GetDuration() const { return frames.empty() ? 0 : frames.back().frameNumber / 60.0f; }
    
    // Update (called each frame for playback)
    void Update(float dt);
    
public:
    InputRecorder() = default;
private:
    bool recording = false;
    bool playing = false;
    std::string currentRecordingName;
    std::vector<InputFrame> frames;
    size_t playbackIndex = 0;
    std::string recordingsPath = "Recordings/";
};


// ============================================================================
// SCREENSHOT / VIDEO CAPTURE
// ============================================================================

class CaptureSystem {
public:
    static CaptureSystem& Instance();
    
    void Initialize(const std::string& outputDir = "Captures/");
    
    // Screenshots
    void CaptureScreenshot(bool includeUI = true, int superSample = 1);
    void CaptureScreenshotAsync(bool includeUI = true, int superSample = 1);
    
    // Video (frame sequence)
    void StartVideoCapture(int fps = 60, int superSample = 1);
    void StopVideoCapture();
    bool IsCapturingVideo() const { return capturingVideo; }
    
    // Settings
    void SetOutputDir(const std::string& dir) { outputDir = dir; }
    void SetFormat(const std::string& fmt) { format = fmt; } // "png", "jpg", "hdr"
    
public:
    CaptureSystem() = default;
private:
    std::string outputDir = "Captures/";
    std::string format = "png";
    bool capturingVideo = false;
    int videoFps = 60;
    int videoFrameCount = 0;
    float videoAccumulator = 0;
    int superSample = 1;
};


// ============================================================================
// MEMORY TRACKER
// ============================================================================

struct MemAlloc {
    void* ptr = nullptr;
    size_t size = 0;
    std::string category;      // "texture", "mesh", "shader", "model", "audio", "script", "general"
    std::string tag;           // User-provided tag (e.g., "player_texture", "level_mesh")
    std::string callstack;     // Captured callstack
    uint64_t frameNumber = 0;  // Frame when allocated
    double timestamp = 0;      // Time when allocated (seconds since start)
    bool isFreed = false;
    double freedTimestamp = 0; // Time when freed
};

// Byte-size helpers for the raylib resource types, exposed so subsystems that
// unload their own resources can report the same numbers the tracker would.
size_t TextureBytes(const Texture& tex);
size_t ImageBytes(const Image& img);
size_t MeshBytes(const Mesh& mesh);
size_t ShaderBytes(const Shader& shader);
size_t ModelBytes(const Model& model);
size_t RenderTextureBytes(const RenderTexture& rt);
size_t WaveBytes(const Wave& wave);

class MemoryTracker {
public:
    static MemoryTracker& Instance();

    void Initialize();
    void Shutdown();
    void Update(float dt);
    void Draw(); // ImGui window

    // Allocation tracking (call from custom allocators or wrappers)
    void TrackAlloc(void* ptr, size_t size, const std::string& category, const std::string& tag = "");
    void TrackFree(void* ptr);
    void TrackRealloc(void* oldPtr, void* newPtr, size_t newSize);

    // Raylib resource tracking helpers.
    // These key off the resource's *identity* (GL id, data pointer) rather than
    // the address of the C struct, because the struct itself is usually copied
    // around by value, so tracking `&texture` would register a different key
    // every time the handle is passed to a function.
    void TrackTexture(Texture2D texture, const std::string& tag = "");
    void TrackMesh(Mesh mesh, const std::string& tag = "");
    void TrackShader(Shader shader, const std::string& tag = "");
    void TrackModel(Model model, const std::string& tag = "");
    void TrackImage(Image image, const std::string& tag = "");
    void TrackRenderTexture(RenderTexture2D rt, const std::string& tag = "");
    void TrackWave(Wave wave, const std::string& tag = "");

    // Corresponding untrack helpers. The Load*/Unload* pairs in raylib take the
    // resource by value, so callers hold the handle and can untrack by it.
    void UntrackTexture(Texture2D texture);
    void UntrackMesh(Mesh mesh);
    void UntrackShader(Shader shader);
    void UntrackModel(Model model);
    void UntrackImage(Image image);
    void UntrackRenderTexture(RenderTexture2D rt);
    void UntrackWave(Wave wave);

    // Budget system. A budget is advisory: it drives the UI bars and a one-shot
    // console warning when exceeded, it does not block the allocation.
    void SetBudget(const std::string& category, size_t bytes);
    size_t GetBudget(const std::string& category) const;
    size_t GetCurrentUsage(const std::string& category) const;
    float GetBudgetUsagePercent(const std::string& category) const;
    const std::map<std::string, size_t>& GetBudgets() const { return budgets; }
    std::vector<std::string> GetOverBudgetCategories() const;

    // Leak detection.
    //
    // In a running game an unfreed allocation is not necessarily a bug -- it
    // may just be a live resource. So a leak is defined relative to a baseline:
    // MarkLeakBaseline() records the live set at a moment you consider clean
    // (a level finished loading, a test case torn down), and DetectLeaks()
    // then reports everything from *that* set which is still live. Those are the
    // allocations that outlived the thing that made them.
    //
    // The baseline is stored as the set of live keys rather than as a frame
    // number, because "allocated before frame N" is not the same question as
    // "live when the baseline was taken": two allocations made in the same
    // frame straddle a frame-number baseline ambiguously, and frame 0 has no
    // meaningful ordering at all. Keying on the actual set answers the question
    // that was asked.
    //
    // With no baseline set, DetectLeaks() reports every live allocation, so the
    // report is never empty just because nobody remembered to set one.
    struct LeakReport {
        std::vector<MemAlloc> leaks;
        size_t totalLeakedBytes = 0;
        std::map<std::string, size_t> leaksByCategory;
        std::map<std::string, size_t> leaksByTag;
    };
    void MarkLeakBaseline();
    void ClearLeakBaseline();
    bool HasLeakBaseline() const { return leakBaselineSet; }
    LeakReport DetectLeaks() const;

    // Snapshot / diff
    struct Snapshot {
        std::map<void*, MemAlloc> allocations;
        uint64_t frameNumber = 0;
        double timestamp = 0;
    };
    Snapshot TakeSnapshot() const;
    struct DiffResult {
        std::vector<MemAlloc> newAllocs;
        std::vector<MemAlloc> freedAllocs;
        std::vector<MemAlloc> grownAllocs;
        std::vector<MemAlloc> shrunkAllocs;
        int64_t netBytes = 0;
    };
    DiffResult DiffSnapshots(const Snapshot& before, const Snapshot& after) const;

    // The snapshot shown in the diff tab. Set from the UI, or by the console.
    void SetSnapshot(const Snapshot& s) { currentSnapshot = s; }
    const Snapshot& GetSnapshot() const { return currentSnapshot; }

    // UI controls
    void Toggle() { visible = !visible; }
    bool IsVisible() const { return visible; }
    void SetCaptureCallstacks(bool enabled) { captureCallstacks = enabled; }
    bool GetCaptureCallstacks() const { return captureCallstacks; }
    void SetMaxCallstackFrames(int frames) { maxCallstackFrames = frames; }
    int GetMaxCallstackFrames() const { return maxCallstackFrames; }

    // Master switch. Tracking costs a callstack capture per allocation, so
    // this is how a release player turns the cost off without recompiling the
    // call sites. Calls made while disabled are dropped, not deferred.
    void SetTrackingEnabled(bool enabled) { trackingEnabled = enabled; }
    bool IsTrackingEnabled() const { return trackingEnabled; }

    // Drop every tracked allocation and zero the counters. Does not free
    // anything -- it only forgets, so a leak report taken afterwards is
    // measured from this point on.
    void ClearTracked();
    // Zero the cumulative totals, the per-category peaks and the graphs. Live
    // bytes are left alone: what is allocated now is still allocated now. The
    // overall peak restarts from the current live total so it never reports
    // less than is actually held.
    void ResetCounters();

    // Process memory, straight from the OS (0 when the platform does not
    // report it). Independent of what is tracked: this is the whole process,
    // so it also covers the heap, drivers and anything untracked.
    static size_t GetProcessMemoryBytes();
    static size_t GetProcessPeakMemoryBytes();

    // Export
    void ExportCSV(const std::string& path) const;
    void ExportJSON(const std::string& path) const;

    // Stats
    size_t GetTotalAllocated() const;
    size_t GetTotalFreed() const;
    size_t GetCurrentAllocated() const;
    size_t GetAllocationCount() const;
    size_t GetPeakAllocated() const;
    size_t GetTotalAllocCount() const;
    size_t GetTotalFreeCount() const;

    // Live bytes per category, for the overview breakdown.
    const std::map<std::string, size_t>& GetCategoryUsage() const { return categoryUsage; }
    const std::map<std::string, size_t>& GetCategoryPeak() const { return peakUsage; }
    const std::vector<MemAlloc>& GetAllocs() const { return allocList; }

    uint64_t GetFrameNumber() const { return frameNumber; }

    // Milliseconds since Initialize(), used for allocation timestamps.
    double Now() const;

private:
    MemoryTracker() = default;

    bool initialized = false;
    bool visible = false;
    bool captureCallstacks = true;
    int maxCallstackFrames = 32;
    bool trackingEnabled = true;
    uint64_t frameNumber = 0;
    double startTime = 0;

    std::map<void*, MemAlloc> allocations;
    std::map<std::string, size_t> budgets; // category -> bytes
    std::map<std::string, size_t> peakUsage; // category -> peak bytes
    std::map<std::string, size_t> categoryUsage; // category -> live bytes
    std::vector<MemAlloc> allocList; // allocations, ordered by recency

    // Cumulative counters. These survive ClearTracked() unless ResetCounters()
    // is called, so "how much has this session allocated" stays answerable.
    size_t totalAllocated = 0;
    size_t totalFreed = 0;
    size_t currentAllocated = 0;
    size_t peakAllocated = 0;
    size_t allocCount = 0;
    size_t freeCount = 0;

    // Leak baseline: the live key set at the last MarkLeakBaseline(), plus when
    // it was taken for display. Anything in that set and still live is a
    // candidate.
    bool leakBaselineSet = false;
    std::set<void*> leakBaselineKeys;
    uint64_t leakBaselineFrame = 0;
    double leakBaselineTime = 0;

    // Categories already reported as over budget, so the console is warned once
    // per overrun instead of once per frame.
    std::map<std::string, bool> overBudgetWarned;

    // Rolling history for the overview graphs.
    static constexpr int HISTORY_SIZE = 120;
    std::vector<float> liveBytesMBHistory;
    std::vector<float> processMBHistory;
    std::map<std::string, std::vector<float>> categoryMBHistory;

    // UI state, kept here so it survives the window being closed and reopened.
    int uiTab = 0;
    std::string categoryFilter;
    std::string tagFilter;
    bool hideSmallAllocs = true;
    float minSizeKB = 1.0f;
    bool sortBySize = true;
    void* selectedAlloc = nullptr;
    // Written from the const Export* methods so they can report their result
    // through the UI without giving up const-correctness everywhere else.
    mutable std::string lastExportMessage;

    Snapshot currentSnapshot;
    bool hasSnapshot = false;
    Snapshot baselineSnapshot;
    bool hasBaselineSnapshot = false;
    DiffResult baselineDiff;
    bool hasBaselineDiff = false;

    // Recursive: the Draw() helpers and the public getters below them both
    // take the lock, and std::mutex would deadlock on that.
    mutable std::recursive_mutex mutex;

    std::string CaptureCallstack(int skipFrames = 2);
    void UpdateCategoryPeak(const std::string& category, size_t current);
    void WarnOverBudgets();

    void DrawOverview();
    void DrawCategoryBreakdown();
    void DrawAllocationList();
    void DrawLeakReport();
    void DrawBudgetBars();
    void DrawCallstackViewer(const MemAlloc& alloc);
    void DrawSnapshotDiff();
    void DrawSettings();
    void RecordHistory();

    // Re-key an existing entry in place (used when a resource is re-tracked at
    // a new tag/size). Returns false if there was nothing to re-key.
    bool Rekey(void* ptr, size_t size, const std::string& category, const std::string& tag);
    // Find the copy of an entry in allocList, so mutations can be applied to
    // both the map and the list. Returns nullptr if the key is not tracked.
    MemAlloc* FindListEntry(void* ptr);
    // Change an entry's size, routing growth through the cumulative-allocated
    // counter and shrinkage through the cumulative-freed one, so that
    // totalAllocated == totalFreed + currentAllocated stays true.
    void ResizeEntry(void* ptr, size_t newSize);
    void UntrackKey(void* key);
    void TrackRaylibResource(void* key, size_t size, const std::string& category, const std::string& tag);
};

// ============================================================================
// SHADER RELOAD / EDITOR (must be before TechnicalToolsManager)
// ============================================================================

struct ShaderSource {
    std::string name;
    std::string vertPath;
    std::string fragPath;
    std::string vertSource;
    std::string fragSource;
    std::filesystem::file_time_type vertModTime;
    std::filesystem::file_time_type fragModTime;
    bool hasError = false;
    std::string errorLog;
};

struct ShaderUniform {
    std::string name;
    int location = -1;
    int type = 0; // GL_FLOAT, GL_FLOAT_VEC2, GL_FLOAT_VEC3, GL_FLOAT_VEC4, GL_FLOAT_MAT4, GL_INT, GL_SAMPLER_2D
    int count = 1;
    // Current value (for editing)
    float floatVal[16] = {0};
    int intVal[16] = {0};
    bool dirty = false;
};

class ShaderReloader {
public:
    static ShaderReloader& Instance();
    
    void Initialize(Engine* engine);
    void Shutdown();
    void Update(float dt);
    void Draw(); // ImGui editor window
    
    // Register a shader for hot-reload (name, vertSource, fragSource)
    // Returns true if registered, false if already exists
    bool RegisterShader(const std::string& name, const std::string& vertSource, const std::string& fragSource);
    
    // Get shader by name (for binding)
    Shader* GetShader(const std::string& name);
    
    // Force reload all shaders
    void ReloadAll();
    
    // Toggle UI
    void Toggle() { visible = !visible; }
    bool IsVisible() const { return visible; }
    
    // Settings
    void SetAutoReload(bool enabled) { autoReload = enabled; }
    bool GetAutoReload() const { return autoReload; }
    void SetWatchInterval(float seconds) { watchInterval = seconds; }
    
    // Error callback
    using ErrorCallback = std::function<void(const std::string& shaderName, const std::string& error)>;
    void SetErrorCallback(ErrorCallback cb) { errorCallback = cb; }
    
private:
    Engine* engine = nullptr;
    bool initialized = false;
    bool visible = false;
    bool autoReload = true;
    float watchInterval = 1.0f; // seconds
    float watchTimer = 0.0f;
    ErrorCallback errorCallback;
    
    struct TrackedShader {
        std::string name;
        Shader shader;
        ShaderSource source;
        std::vector<ShaderUniform> uniforms;
        bool needsReload = false;
        std::string lastError;
    };
    
    std::map<std::string, TrackedShader> shaders;
    std::string shaderDir = "shaders/";
    std::mutex mutex;
    
    void CheckForChanges();
    bool LoadShaderSource(const std::string& path, std::string& outSource, std::filesystem::file_time_type& outModTime);
    bool CompileShader(TrackedShader& tracked);
    void ExtractUniforms(TrackedShader& tracked);
    void DrawShaderEditor(TrackedShader& tracked);
    void DrawUniformEditor(ShaderUniform& uniform);
    std::string GetTypeName(int glType) const;
};

// ============================================================================
// TECHNICAL TOOLS MANAGER (orchestrates all)
// ============================================================================

class TechnicalToolsManager {
public:
    static TechnicalToolsManager& Instance();
    
    void Initialize(Engine* engine);
    void Shutdown();
    void Update(float dt);
    void Draw(); // Draw3D() + DrawUI()
    void Draw3D(); // physics debug; call inside BeginMode3D/EndMode3D
    void DrawUI(); // ImGui windows; call between ImGui::NewFrame and ImGui::Render
    
    // Input handling (call from PlayerMain)
    void HandleInput();
    
    // Access sub-systems
    Console& GetConsole() { return console; }
    Profiler& GetProfiler() { return profiler; }
    EntityInspector& GetInspector() { return inspector; }
    PhysicsDebugVisualizer& GetPhysicsDebug() { return physicsDebug; }
    InputRecorder& GetRecorder() { return recorder; }
    CaptureSystem& GetCapture() { return capture; }
    CVarSystem& GetCVarSystem() { return cvars; }
    ShaderReloader& GetShaderReloader() { return shaderReloader; }
    
    // Quick toggles
    void ToggleConsole();
    void ToggleProfiler();
    void ToggleInspector();
    void TogglePhysicsDebug();
    void ToggleShaderReloader();
    void ToggleMemoryTracker();

    bool IsAnyToolVisible() const;

    // Access memory tracker. The tracker is a singleton rather than a manager
    // member so the MEMORY_TRACKER macro and GetMemoryTracker() always reach
    // the same object -- subsystems track allocations from anywhere, not just
    // from the frame loop the manager lives on.
    MemoryTracker& GetMemoryTracker() { return MemoryTracker::Instance(); }

private:
    TechnicalToolsManager() = default;
    Engine* engine = nullptr;
    bool initialized = false;

    Console console;
    Profiler profiler;
    EntityInspector inspector;
    PhysicsDebugVisualizer physicsDebug;
    InputRecorder recorder;
    CaptureSystem capture;
    CVarSystem cvars;
    ShaderReloader shaderReloader;
    
    // Profiler overlay (internal)
    void DrawProfilerOverlay();
};

// Inline function definitions (need to be after member declarations)
inline void TechnicalToolsManager::ToggleConsole() { console.Toggle(); }
inline void TechnicalToolsManager::ToggleProfiler() { profiler.SetEnabled(!profiler.IsEnabled()); }
inline void TechnicalToolsManager::ToggleInspector() { inspector.Toggle(); }
inline void TechnicalToolsManager::TogglePhysicsDebug() { physicsDebug.Toggle(); }
inline void TechnicalToolsManager::ToggleShaderReloader() { shaderReloader.Toggle(); }
inline void TechnicalToolsManager::ToggleMemoryTracker() { MemoryTracker::Instance().Toggle(); }

inline bool TechnicalToolsManager::IsAnyToolVisible() const {
    return console.IsVisible() || inspector.IsVisible() || physicsDebug.IsVisible() || shaderReloader.IsVisible() || MemoryTracker::Instance().IsVisible();
}

// Global access
#define TECH_TOOLS TechTools::TechnicalToolsManager::Instance()
#define CONSOLE TechTools::Console::Instance()
#define PROFILER TechTools::Profiler::Instance()
#define INSPECTOR TechTools::EntityInspector::Instance()
#define PHYSICS_DEBUG TechTools::PhysicsDebugVisualizer::Instance()
#define RECORDER TechTools::InputRecorder::Instance()
#define CAPTURE TechTools::CaptureSystem::Instance()
#define CVARS TechTools::CVarSystem::Instance()
#define SHADER_RELOADER TechTools::ShaderReloader::Instance()
#define MEMORY_TRACKER TechTools::MemoryTracker::Instance()

} // namespace TechTools