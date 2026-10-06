// PlayerMain.cpp - Standalone game player entry point.
// Loads a project and runs it without editor UI.

#include "Engine.hpp"
#include "Engine/Scripts/CoreCLRHost.hpp"
#include "Engine/Scripts/NativeScriptHost.hpp"
#include "Engine/TechnicalTools.hpp"
#include "Engine/Frontend/ProjectManager.hpp"
#include "Engine/Backend/ScenePersistence.hpp"
#include "Engine/Backend/TextureManager.hpp"
#include "Engine/Backend/PhysicsSimulation.hpp"
#include "Engine/Backend/ScatteredObject.hpp"
#include "Engine/Backend/CameraController.hpp"
#include "Engine/Backend/CharacterController.hpp"
#include "Engine/Platform/Platform.hpp"
#include "Engine/Scripts/ScriptCompiler.hpp"
#include "Engine/Graphics.hpp"
#include "Terrain/Terrain.hpp"
#include "Terrain/Water/WaterBody.hpp"
#include "CityGen/City.hpp"
#include "raylib.h"
#include "rlgl.h"
#include "imgui.h"
#include "rlimgui.h"

// CYAN color constant (raylib uses SKYBLUE for cyan)
#ifndef CYAN
#define CYAN SKYBLUE
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <deque>

namespace fs = std::filesystem;

// ============================================================================
// Debug Stats Overlay (Ctrl+F5 to toggle)
// ============================================================================

struct DebugStats {
    // Frame timing
    double frameTimeMs = 0.0;
    double frameTimeMin = 9999.0;
    double frameTimeMax = 0.0;
    double frameTimeAvg = 0.0;
    int fps = 0;
    
    // Rendering
    int drawCalls = 0;
    int trianglesRendered = 0;
    int meshesRendered = 0;
    int materialsUsed = 0;
    int texturesBound = 0;
    // Frustum culling
    int culledEntities = 0;
    int renderedEntities = 0;
    // True when the distinct-id sets behind the two counters above hit their
    // capacity, so the numbers undercount. Surfaced as a "+" suffix.
    bool statsSaturated = false;
    
    // Scene objects
    int scatteredObjects = 0;
    int terrainChunks = 0;
    int waterBodies = 0;
    int cityBuildings = 0;

    // Scripting. There is deliberately no "active scripts" count here: the
    // script registries live on the managed and native hosts and neither
    // exposes a count, so the old field could only ever report whether a host
    // object existed. What a player actually needs to know is "did my C# load,
    // and did my plugins load", so that is what this reports.
    bool clrReady = false;
    int nativePlugins = 0;
    
    // Physics
    int physicsBodies = 0;
    int physicsContacts = 0;
    double physicsTimeMs = 0.0;
    
    // Memory. The *MB members come from the memory tracker; the categorised
    // ones stay at 0 unless something actually calls Track*, so treat a 0 as
    // "untracked" rather than "free".
    size_t textureMemoryMB = 0;
    size_t meshMemoryMB = 0;
    size_t trackedMemoryMB = 0;
    size_t processMemoryMB = 0;
    
    // Camera
    float camPosX = 0, camPosY = 0, camPosZ = 0;
    float camYaw = 0, camPitch = 0;
    
    // History for graphs (last 120 frames)
    std::deque<double> frameTimeHistory;
    std::deque<int> fpsHistory;
    std::deque<int> drawCallHistory;
    std::deque<int> triangleHistory;
    static constexpr int HISTORY_SIZE = 120;
    
    bool showStats = false;
    bool showGraphs = true;
    bool showDetails = true;
    // Mirrors Engine::IsPaused() so the title can shout about it. Entities do
    // not tick while this is set, so frame time and FPS still update but mean
    // something different -- hence the explicit banner rather than just a
    // dimmed number.
    bool paused = false;
    float timeScale = 1.0f;
    
    // Font
    Font font = {0};
    bool fontLoaded = false;
    
    // Timing
    std::chrono::high_resolution_clock::time_point frameStart;
    
    void LoadFont() {
        if (fontLoaded) return;

        // platform::ResolveFontPath() covers the local-override-then-system
        // ordering shared with the editor. The extra paths below are ones it
        // does not know about, kept as a second chance before giving up on a
        // TTF altogether.
        const std::string& resolved = platform::ResolveFontPath();
        const char* extraPaths[] = {
            "/System/Library/Fonts/Arial.ttf",
            "resources/fonts/DejaVuSans.ttf"
        };

        auto tryLoad = [this](const std::string& path) {
            if (path.empty()) return false;
            Font candidate = LoadFontEx(path.c_str(), 14, nullptr, 0);
            if (candidate.texture.id == 0) return false;
            font = candidate;
            SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
            return true;
        };

        if (tryLoad(resolved)) {
            fontLoaded = true;
            return;
        }
        for (const char* path : extraPaths) {
            if (tryLoad(path)) {
                fontLoaded = true;
                return;
            }
        }

        // Nothing loadable; raylib's built-in bitmap font always works.
        font = GetFontDefault();
        fontLoaded = true;
    }
    
    void FrameStart() {
        // Collect render stats from PREVIOUS frame's graphics module
        drawCalls = gfx::GetDrawCallCount();
        trianglesRendered = gfx::GetTriangleCount();
        meshesRendered = gfx::GetMeshCount();
        materialsUsed = gfx::GetMaterialCount();
        texturesBound = gfx::GetTextureCount();
        culledEntities = gfx::GetCulledEntityCount();
        renderedEntities = gfx::GetRenderedEntityCount();
        statsSaturated = gfx::GetStatsSaturated();
        
        frameStart = std::chrono::high_resolution_clock::now();
    }
    
    void FrameEnd() {
        auto end = std::chrono::high_resolution_clock::now();
        frameTimeMs = std::chrono::duration<double, std::milli>(end - frameStart).count();
        
        frameTimeMin = std::min(frameTimeMin, frameTimeMs);
        frameTimeMax = std::max(frameTimeMax, frameTimeMs);
        
        // Rolling average over last 60 frames
        frameTimeHistory.push_back(frameTimeMs);
        if (frameTimeHistory.size() > HISTORY_SIZE) frameTimeHistory.pop_front();
        
        double sum = 0;
        for (double v : frameTimeHistory) sum += v;
        frameTimeAvg = frameTimeHistory.empty() ? 0 : sum / frameTimeHistory.size();
        
        fps = (int)(1000.0 / frameTimeMs);
        fpsHistory.push_back(fps);
        if (fpsHistory.size() > HISTORY_SIZE) fpsHistory.pop_front();
        
        drawCallHistory.push_back(drawCalls);
        if (drawCallHistory.size() > HISTORY_SIZE) drawCallHistory.pop_front();
        
        triangleHistory.push_back(trianglesRendered);
        if (triangleHistory.size() > HISTORY_SIZE) triangleHistory.pop_front();
    }
    
    void Toggle() {
        showStats = !showStats;
        if (showStats && !fontLoaded) LoadFont();
    }
    
    // One row of the overlay. Building the whole panel as a list first and then
    // measuring and drawing from that same list is the point: the old code ran
    // two hand-written passes that each spelled out the layout, so adding a
    // line to the draw pass without editing the measure pass left the
    // background panel the wrong height. One list cannot drift.
    struct Row {
        std::string text;
        Color color;
        float fontSize;
        int spaceBefore = 0;
        int spaceAfter = 0;
    };
    
    void DrawOverlay() {
        if (!showStats) return;
        if (!fontLoaded) LoadFont();
        
        const int margin = 16;
        const int graphMargin = 20;
        const int panelW = 400;
        const int x = 15;
        const int y = 15;
        const int lineH = 20;
        const int graphH = 95;
        const int graphGap = 10;
        
        std::vector<Row> rows;
        auto line = [&](Color c, float size, const char* fmt, auto... args) {
            char buf[256];
            std::snprintf(buf, sizeof(buf), fmt, args...);
            rows.push_back({ buf, c, size, 0, 0 });
        };
        auto section = [&](const char* name) {
            rows.push_back({ name, LIME, 13.0f, 4, 2 });
        };
        // Vertical breathing room between groups. Carried on the next row so it
        // is part of the measured height rather than applied only at draw time.
        auto gap = [&](int px) {
            if (!rows.empty()) rows.back().spaceAfter += px;
        };
        
        // Title
        rows.push_back({ "DEBUG STATS  (Ctrl+F5 to close)", LIME, 16.0f, 0, 6 });
        
        // Frame timing. Always shown: when you collapse the details to get the
        // graphs more room, the one number you still want is the frame time.
        Color fpsCol = fps >= 55 ? LIME : (fps >= 30 ? YELLOW : RED);
        line(fpsCol, 15, "FPS: %d   |   Frame: %.2f ms", fps, frameTimeMs);
        if (paused) {
            line(YELLOW, 13, "== PAUSED ==  entities are not ticking; press . to step, P to resume");
        } else if (timeScale != 1.0f) {
            line(YELLOW, 13, "Time scale:  %.2fx", timeScale);
        }
        gap(6);
        line(WHITE, 13, "Avg: %.2f ms   Min: %.2f ms   Max: %.2f ms", frameTimeAvg, frameTimeMin, frameTimeMax);
        
        if (showDetails) {
            gap(6);
            section("RENDERING");
            line(SKYBLUE, 13, "Draw Calls:  %d", drawCalls);
            line(WHITE, 13, "Triangles:   %d  (%.2f K)", trianglesRendered, trianglesRendered / 1000.0f);
            line(WHITE, 13, "Meshes:      %d", meshesRendered);
            // These two are distinct resources touched this frame, and they come
            // from the engine's own draw sites rather than from inside raylib,
            // so they are a lower bound. Say so rather than implying they are
            // exact.
            line(WHITE, 13, "Materials:   %d%s", materialsUsed, statsSaturated ? "+" : "");
            line(WHITE, 13, "Textures:    %d%s", texturesBound, statsSaturated ? "+" : "");
            // Frustum culling stats
            line(LIME, 13, "Rendered:    %d", renderedEntities);
            line(ORANGE, 13, "Culled:      %d", culledEntities);
            if (renderedEntities + culledEntities > 0) {
                float pct = (float)culledEntities / (renderedEntities + culledEntities) * 100.0f;
                line(WHITE, 13, "Cull Rate:   %.1f%%", pct);
            }
            
            gap(6);
            section("SCENE");
            line(ORANGE, 13, "Objects:     %d", scatteredObjects);
            line(WHITE, 13, "Terrain:     %d chunks", terrainChunks);
            line(WHITE, 13, "Water:       %d bodies", waterBodies);
            line(WHITE, 13, "City:        %d buildings", cityBuildings);
            // There is no active-script count to show -- the registries live on
            // the script hosts and expose no count. Reporting "did my C# load"
            // and "how many plugins loaded" is the part a player can act on.
            line(WHITE, 13, "C# Runtime:  %s", clrReady ? "ready" : "not loaded");
            line(WHITE, 13, "Plugins:     %d loaded", nativePlugins);
            
            gap(6);
            section("PHYSICS");
            line(PINK, 13, "Bodies:      %d   |   Contacts: %d", physicsBodies, physicsContacts);
            line(WHITE, 13, "Step Time:   %.2f ms", physicsTimeMs);
            
            gap(6);
            // F9 opens the full Memory Tracker window, so say so here -- this
            // panel is the glanceable summary, that one is the drill-down.
            section("MEMORY");
            line(VIOLET, 13, "Tracked:     %zu MB", trackedMemoryMB);
            line(WHITE, 13, "Process RSS: %zu MB", processMemoryMB);
            line(WHITE, 13, "Textures:    %zu MB", textureMemoryMB);
            line(WHITE, 13, "Meshes:      %zu MB", meshMemoryMB);
            
            gap(6);
            section("CAMERA");
            line(LIGHTGRAY, 13, "Pos:  %.1f,  %.1f,  %.1f", camPosX, camPosY, camPosZ);
            line(WHITE, 13, "Yaw:  %.1f   Pitch:  %.1f", camYaw, camPitch);
        }
        
        int graphsHeight = 0;
        if (showGraphs) {
            section("GRAPHS (last 120 frames)");
            rows.back().spaceBefore += 10;
            graphsHeight = 4 * graphH + 3 * graphGap;
        }
        
        // Controls hint, pinned to the bottom of the panel.
        const int hintSpace = 8 + lineH + margin;
        
        // Measure from the same list that gets drawn.
        int contentH = 0;
        for (const Row& r : rows) contentH += r.spaceBefore + lineH + r.spaceAfter;
        const int panelH = margin + contentH + graphsHeight + hintSpace;
        
        DrawRectangleRounded({ (float)x, (float)y, (float)panelW, (float)panelH }, 0.03f, 8, {15, 15, 20, 220});
        DrawRectangleRoundedLinesEx({ (float)x, (float)y, (float)panelW, (float)panelH }, 0.03f, 8, 2.0f, {80, 180, 80, 255});
        
        int cy = y + margin;
        for (const Row& r : rows) {
            cy += r.spaceBefore;
            DrawTextEx(font, r.text.c_str(), { (float)x + margin, (float)cy }, r.fontSize, 1.0f, r.color);
            cy += lineH + r.spaceAfter;
        }
        
        // Graphs
        if (showGraphs) {
            int graphX = x + graphMargin;
            int graphW = panelW - 2 * graphMargin;
            DrawGraph("Frame Time (ms)", frameTimeHistory, graphX, cy, graphW, graphH, 0, 33.3);
            cy += graphH + graphGap;
            DrawGraph("FPS", fpsHistory, graphX, cy, graphW, graphH, 0, 120);
            cy += graphH + graphGap;
            DrawGraph("Draw Calls", drawCallHistory, graphX, cy, graphW, graphH, 0, 200);
            cy += graphH + graphGap;
            DrawGraph("Triangles (K)", triangleHistory, graphX, cy, graphW, graphH, 0, 500);
        }
        
        DrawTextEx(font, "F6: Graphs   F7: Details   F8: Reset Min/Max   P: Pause   .: Step",
                  { (float)x + margin, (float)(y + panelH - margin - lineH) }, 11.0f, 1.0f, DARKGRAY);
    }
    
    // Color gradient helper - green to yellow to red based on value (0-1)
    Color GetGradientColor(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        if (t < 0.5f) {
            // Green to Yellow
            float lt = t * 2.0f;
            return {(unsigned char)(255 * lt), 255, 0, 255};
        } else {
            // Yellow to Red
            float lt = (t - 0.5f) * 2.0f;
            return {255, (unsigned char)(255 * (1.0f - lt)), 0, 255};
        }
    }
    
    void DrawGraph(const char* label, const std::deque<double>& history, int x, int y, int w, int h, double minVal, double maxVal) {
        if (history.size() < 2) return;
        
        double range = maxVal - minVal;
        if (range <= 0) range = 1;
        
        // Background with rounded corners
        DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.05f, 6, {25, 25, 35, 220});
        DrawRectangleRoundedLinesEx({(float)x, (float)y, (float)w, (float)h}, 0.05f, 6, 1.0f, {70, 70, 90, 255});
        
        // Y-axis labels (min, mid, max) - positioned inside with padding
        const int labelPad = 8;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f", maxVal);
        DrawTextEx(font, buf, {(float)x + labelPad, (float)y + 4}, 10, 1.0f, {150, 150, 150, 255});
        std::snprintf(buf, sizeof(buf), "%.1f", (minVal + maxVal) * 0.5);
        DrawTextEx(font, buf, {(float)x + labelPad, (float)y + h * 0.5f - 6}, 10, 1.0f, {120, 120, 120, 255});
        std::snprintf(buf, sizeof(buf), "%.1f", minVal);
        DrawTextEx(font, buf, {(float)x + labelPad, (float)y + h - 16}, 10, 1.0f, {150, 150, 150, 255});
        
        // Label (top-left)
        DrawTextEx(font, label, {(float)x + labelPad + 2, (float)y + 2}, 11, 1.0f, LIME);
        
        // Current value (top-right)
        if (!history.empty()) {
            double current = history.back();
            std::snprintf(buf, sizeof(buf), "Current: %.2f", current);
            DrawTextEx(font, buf, {(float)x + w - 115, (float)y + 2}, 10, 1.0f, WHITE);
        }
        
        // Graph drawing area (inside the border with padding)
        const int innerPad = 30; // left padding for Y labels
        const int rightPad = 8;
        const int topPad = 18;
        const int bottomPad = 8;
        int gx = x + innerPad;
        int gy = y + topPad;
        int gw = w - innerPad - rightPad;
        int gh = h - topPad - bottomPad;
        
        // Grid lines (horizontal) - within graph area
        for (int i = 1; i < 4; ++i) {
            float gy_line = gy + (float)i / 4.0f * gh;
            DrawLine(gx, (int)gy_line, gx + gw, (int)gy_line, {40, 40, 50, 100});
        }
        
        // Target line for frame time (16.67ms = 60 FPS)
        if (maxVal > 16.67 && minVal < 16.67) {
            float ty = gy + gh - (float)((16.67 - minVal) / range) * gh;
            DrawLine(gx, (int)ty, gx + gw, (int)ty, {0, 255, 0, 100});
            DrawTextEx(font, "60 FPS", {(float)gx + gw - 38, ty - 12}, 9, 1.0f, {0, 255, 0, 180});
        }
        
        // Draw graph with gradient coloring - clipped to graph area
        for (size_t i = 1; i < history.size(); ++i) {
            float x1 = gx + (float)(i - 1) / (HISTORY_SIZE - 1) * gw;
            float x2 = gx + (float)i / (HISTORY_SIZE - 1) * gw;
            float y1 = gy + gh - (float)((history[i - 1] - minVal) / range) * gh;
            float y2 = gy + gh - (float)((history[i] - minVal) / range) * gh;
            y1 = std::clamp(y1, (float)gy, (float)(gy + gh));
            y2 = std::clamp(y2, (float)gy, (float)(gy + gh));
            
            // Color based on value (higher = more red)
            float t1 = (float)((history[i - 1] - minVal) / range);
            float t2 = (float)((history[i] - minVal) / range);
            Color c1 = GetGradientColor(t1);
            Color c2 = GetGradientColor(t2);
            
            // Draw gradient line (approximate with middle color)
            Color midColor = {
                (unsigned char)((c1.r + c2.r) * 0.5f),
                (unsigned char)((c1.g + c2.g) * 0.5f),
                (unsigned char)((c1.b + c2.b) * 0.5f),
                255
            };
            DrawLine((int)x1, (int)y1, (int)x2, (int)y2, midColor);
            
            // Fill area under curve (subtle) - clipped
            DrawTriangle(
                {(float)x1, y1},
                {(float)x2, y2},
                {(float)x2, (float)gy + gh},
                {midColor.r, midColor.g, midColor.b, 30}
            );
        }
        
        // Target line for FPS (60)
        if (maxVal >= 60 && minVal <= 60) {
            float ty = gy + gh - (float)((60.0 - minVal) / range) * gh;
            DrawLine(gx, (int)ty, gx + gw, (int)ty, {0, 255, 0, 100});
        }
    }
    
    void DrawGraph(const char* label, const std::deque<int>& history, int x, int y, int w, int h, int minVal, int maxVal) {
        if (history.size() < 2) return;
        
        int range = maxVal - minVal;
        if (range <= 0) range = 1;
        
        // Background with rounded corners
        DrawRectangleRounded({(float)x, (float)y, (float)w, (float)h}, 0.05f, 6, {25, 25, 35, 220});
        DrawRectangleRoundedLinesEx({(float)x, (float)y, (float)w, (float)h}, 0.05f, 6, 1.0f, {70, 70, 90, 255});
        
        // Y-axis labels
        const int labelPad = 8;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d", maxVal);
        DrawTextEx(font, buf, {(float)x + labelPad, (float)y + 4}, 10, 1.0f, {150, 150, 150, 255});
        std::snprintf(buf, sizeof(buf), "%d", (minVal + maxVal) / 2);
        DrawTextEx(font, buf, {(float)x + labelPad, (float)y + h * 0.5f - 6}, 10, 1.0f, {120, 120, 120, 255});
        std::snprintf(buf, sizeof(buf), "%d", minVal);
        DrawTextEx(font, buf, {(float)x + labelPad, (float)y + h - 16}, 10, 1.0f, {150, 150, 150, 255});
        
        // Label
        DrawTextEx(font, label, {(float)x + labelPad + 2, (float)y + 2}, 11, 1.0f, LIME);
        
        // Current value
        if (!history.empty()) {
            int current = history.back();
            std::snprintf(buf, sizeof(buf), "Current: %d", current);
            DrawTextEx(font, buf, {(float)x + w - 115, (float)y + 2}, 10, 1.0f, WHITE);
        }
        
        // Graph drawing area
        const int innerPad = 30;
        const int rightPad = 8;
        const int topPad = 18;
        const int bottomPad = 8;
        int gx = x + innerPad;
        int gy = y + topPad;
        int gw = w - innerPad - rightPad;
        int gh = h - topPad - bottomPad;
        
        // Grid lines
        for (int i = 1; i < 4; ++i) {
            float gy_line = gy + (float)i / 4.0f * gh;
            DrawLine(gx, (int)gy_line, gx + gw, (int)gy_line, {40, 40, 50, 100});
        }
        
        // Draw graph with gradient
        for (size_t i = 1; i < history.size(); ++i) {
            float x1 = gx + (float)(i - 1) / (HISTORY_SIZE - 1) * gw;
            float x2 = gx + (float)i / (HISTORY_SIZE - 1) * gw;
            float y1 = gy + gh - (float)(history[i - 1] - minVal) / range * gh;
            float y2 = gy + gh - (float)(history[i] - minVal) / range * gh;
            y1 = std::clamp(y1, (float)gy, (float)(gy + gh));
            y2 = std::clamp(y2, (float)gy, (float)(gy + gh));
            
            float t1 = (float)(history[i - 1] - minVal) / range;
            float t2 = (float)(history[i] - minVal) / range;
            Color c1 = GetGradientColor(t1);
            Color c2 = GetGradientColor(t2);
            
            Color midColor = {
                (unsigned char)((c1.r + c2.r) * 0.5f),
                (unsigned char)((c1.g + c2.g) * 0.5f),
                (unsigned char)((c1.b + c2.b) * 0.5f),
                255
            };
            DrawLine((int)x1, (int)y1, (int)x2, (int)y2, midColor);
            
            // Fill area
            DrawTriangle(
                {(float)x1, y1},
                {(float)x2, y2},
                {(float)x2, (float)gy + gh},
                {midColor.r, midColor.g, midColor.b, 30}
            );
        }
        
        // Target line for FPS (60)
        if (maxVal >= 60 && minVal <= 60) {
            float ty = gy + gh - (float)((60.0 - minVal) / range) * gh;
            DrawLine(gx, (int)ty, gx + gw, (int)ty, {0, 255, 0, 100});
        }
    }
};

// Global debug stats instance
static DebugStats g_debugStats;

struct PlayerOptions {
    std::string projectPath;
    int targetFPS = 60;
    bool noScripts = false;
    bool noCacheRecompile = false;
    bool fullscreen = false;
    int width = 1280;
    int height = 720;
    bool hotReload = true;
    size_t maxMemoryBytes = 0; // 0 = unlimited
};

static void PrintUsage(const char* exeName) {
    std::printf(
        "Usage: %s -play <project> [options]\n"
        "\n"
        "Options:\n"
        "  --capfps30           Cap framerate at 30 FPS\n"
        "  --capfps60           Cap framerate at 60 FPS (default)\n"
        "  --noscripts          Disable C# scripting entirely\n"
        "  --nocacherecompile   Skip shader cache recompilation on startup\n"
        "  --fullscreen         Start in fullscreen mode\n"
        "  --res WxH            Window resolution (e.g., --res 1920x1080)\n"
        "  --nohotreload        Disable script hot-reload\n"
        "  --maxmem4G           Budget texture/mesh memory at 4GB (reported, not enforced)\n"
        "\n"
        "Example:\n"
        "  %s -play MyProject --capfps60 --fullscreen\n",
        exeName, exeName);
}

static bool ParseResolution(const std::string& s, int& outW, int& outH) {
    auto pos = s.find('x');
    if (pos == std::string::npos) return false;
    try {
        outW = std::stoi(s.substr(0, pos));
        outH = std::stoi(s.substr(pos + 1));
        return outW > 0 && outH > 0;
    } catch (...) {
        return false;
    }
}

static PlayerOptions ParseArgs(int argc, char* argv[]) {
    PlayerOptions opts;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-play" || arg == "--play") {
            if (i + 1 < argc) opts.projectPath = argv[++i];
        } else if (arg == "--capfps30") {
            opts.targetFPS = 30;
        } else if (arg == "--capfps60") {
            opts.targetFPS = 60;
        } else if (arg == "--noscripts") {
            opts.noScripts = true;
            std::printf("[Player] --noscripts flag detected, C# scripting disabled\n");
        } else if (arg == "--nocacherecompile") {
            opts.noCacheRecompile = true;
        } else if (arg == "--fullscreen") {
            opts.fullscreen = true;
        } else if (arg == "--res") {
            if (i + 1 < argc) ParseResolution(argv[++i], opts.width, opts.height);
        } else if (arg == "--nohotreload") {
            opts.hotReload = false;
        } else if (arg == "--maxmem4G") {
            opts.maxMemoryBytes = 4ull * 1024 * 1024 * 1024;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage(argv[0]);
            std::exit(0);
        }
    }
    return opts;
}

static fs::path ResolveProjectDir(const std::string& input) {
    fs::path p = input;
    if (!fs::exists(p)) {
        // Try as .flyproj file
        if (p.extension() != ".flyproj") {
            p += ".flyproj";
        }
    }
    p = fs::absolute(p);
    // If it's a .flyproj file, return its parent directory
    if (p.extension() == ".flyproj") {
        return p.parent_path();
    }
    // Otherwise assume it's a project directory
    return p;
}

int main(int argc, char* argv[]) {
    PlayerOptions opts = ParseArgs(argc, argv);

    // Auto-detect project from working directory if not specified
    if (opts.projectPath.empty()) {
        fs::path cwd = fs::current_path();
        // Check if CWD or parent contains a .flyproj file
        fs::path projDir = cwd;
        bool found = false;
        for (int i = 0; i < 3 && !found; ++i) {
            for (auto& entry : fs::directory_iterator(projDir)) {
                if (entry.is_regular_file() && entry.path().extension() == ".flyproj") {
                    found = true;
                    break;
                }
            }
            if (!found && projDir.has_parent_path()) {
                projDir = projDir.parent_path();
            }
        }
        if (found) {
            opts.projectPath = projDir.string();
            std::printf("[Player] Auto-detected project: %s\n", projDir.string().c_str());
        }
    }

    if (opts.projectPath.empty()) {
        std::fprintf(stderr, "No Project Found!\n");
        PrintUsage(argv[0]);
        return 1;
    }

    fs::path projectInput = opts.projectPath;
    // Make absolute to handle relative paths correctly
    projectInput = fs::absolute(projectInput);
    bool isSpecificFlyproj = projectInput.extension() == ".flyproj";
    
    fs::path projectDir;
    if (isSpecificFlyproj) {
        projectDir = projectInput.parent_path();
    } else {
        projectDir = ResolveProjectDir(opts.projectPath);
    }
    
    if (!fs::exists(projectDir)) {
        std::fprintf(stderr, "Project not found: %s\n", projectDir.string().c_str());
        return 1;
    }

    // Read project info to get window title
    project::Info info;
    if (isSpecificFlyproj) {
        if (!project::ReadProjectHeader(projectInput.string(), info)) {
            info.name = projectInput.stem().string();
            info.path = projectDir.string();
        } else {
            info.path = projectDir.string();
        }
    } else {
        if (!project::ReadProjectHeader(projectDir.string(), info)) {
            info.name = projectDir.filename().string();
            info.path = projectDir.string();
        } else {
            info.path = projectDir.string();
        }
    }

    // Initialize shader cache (skip recompile if requested)
    // ShaderCache is initialized by graphics system

    // Create engine in player mode
    Engine engine(opts.width, opts.height, info.name + " / FlyEngine C++ Runtime", opts.targetFPS);
    engine.SetPlayerBuild(true);

    // Configure window
    if (opts.fullscreen) {
        int monitor = GetCurrentMonitor();
        SetWindowSize(GetMonitorWidth(monitor), GetMonitorHeight(monitor));
        ToggleFullscreen();
    }

    // Core gameplay entities
    std::vector<ScatteredObject*> rawObjectPtrs;
    std::vector<std::unique_ptr<ModelGroup>> sceneModels;
    auto sim = std::make_unique<phys::Simulation>(rawObjectPtrs);
    phys::Simulation& simRef = *sim;
    engine.AddEntity(std::move(sim));

    // CharacterController (player movement + camera)
    auto charController = std::make_unique<CharacterController>(engine, &engine.GetCamera(), simRef);
    CharacterController* charControllerPtr = charController.get();
    engine.AddEntity(std::move(charController));
    
    // Add player body to simulation's object list
    if (charControllerPtr && charControllerPtr->GetPlayerBody()) {
        rawObjectPtrs.push_back(charControllerPtr->GetPlayerBody());
    }

    // CoreCLR Host (C# scripting)
    CoreCLRHost* coreClrHostPtr = nullptr;
    if (!opts.noScripts) {
        std::printf("[Player] Initializing CoreCLR host...\n");
        scriptCompiler::EnsureBuilt(info.path);
        auto coreClrHost = std::make_unique<CoreCLRHost>();
        coreClrHostPtr = coreClrHost.get();
        if (coreClrHost->Initialize(info.path)) {
            engine.AddEntity(std::move(coreClrHost));
            std::printf("[Player] CoreCLR host initialized successfully\n");
        } else {
            std::printf("[Player] CoreCLR host initialization failed\n");
            coreClrHostPtr = nullptr; // Prevent dangling pointer
        }
    } else {
        std::printf("[Player] Skipping CoreCLR (--noscripts)\n");
    }

    // NativeScript Host (C/C++ plugin scripting) - always enabled unless explicitly disabled
    bool noNativeScripts = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--no-native-scripts") {
            noNativeScripts = true;
            break;
        }
    }
    
    NativeScript::NativeScriptHost* nativeScriptHostPtr = nullptr;
    if (!noNativeScripts) {
        std::printf("[Player] Initializing NativeScript host...\n");
        fflush(stdout);
        auto nativeScriptHost = std::make_unique<NativeScript::NativeScriptHost>();
        nativeScriptHostPtr = nativeScriptHost.get();
        nativeScriptHost->SetEngine(&engine);
        if (nativeScriptHost->Initialize(info.path)) {
            engine.AddEntity(std::move(nativeScriptHost));
            std::printf("[Player] NativeScript host initialized successfully\n");
            fflush(stdout);
        } else {
            std::printf("[Player] NativeScript host initialization failed\n");
            fflush(stdout);
        }
    } else {
        std::printf("[Player] Skipping NativeScript (--no-native-scripts)\n");
        fflush(stdout);
    }

    // Load scene
    textureManager::Init(info.path);
    terrain::Terrain* loadedTerrain = nullptr;
    project::Info loaded = info;
    if (isSpecificFlyproj) {
        // Load from specific .flyproj file (e.g., temp file)
        if (!project::OpenProjectFileFromPath(projectInput.string(), engine, rawObjectPtrs, sceneModels, loaded, &simRef, &loadedTerrain)) {
            std::printf("Starting empty scene\n");
        }
    } else {
        // Load from project directory (default <folder>/<folder>.flyproj)
        if (!project::OpenProjectFile(projectDir.string(), engine, rawObjectPtrs, sceneModels, loaded, &simRef, &loadedTerrain)) {
            std::printf("Starting empty scene\n");
        }
    }

    // Auto-start physics simulation (no editor Play button in player)
    simRef.StartPlay();

    // Ensure CharacterController body exists in physics world
    if (charControllerPtr) {
        charControllerPtr->EnsurePhysicsBody();
    }

    // Bind world to script runtime
    if (coreClrHostPtr && coreClrHostPtr->IsReady()) {
        Camera3D& cam = engine.GetCamera();
        coreClrHostPtr->BindWorld(rawObjectPtrs, sceneModels, cam, simRef, engine);
        coreClrHostPtr->SyncStandaloneScripts();
    }
    
    // NativeScript host uses CPluginAPI directly (no ScriptRuntime needed)
    // FlyPluginAPI_SetEngine was already called during NativeScriptHost::Initialize

    // Initialize Technical Tools (console, profiler, inspector, physics debug, recorder, capture, memory tracker)
    TechTools::TechnicalToolsManager::Instance().Initialize(&engine);
    std::printf("[Player] Technical Tools ready. F1=Inspector, F2=Physics Debug, F3=Record, F4=Playback, F5=Screenshot, F6=Video, F9=Memory Tracker, F12=Console\n");

    // Apply the --maxmem4G budget. This is a reported limit, not an enforced
    // one: the tracker flags when texture+mesh usage crosses it, but it does
    // not start refusing loads, because deciding what to evict is the resource
    // owner's call and needs the full scene context the tracker does not have.
    if (opts.maxMemoryBytes > 0) {
        MEMORY_TRACKER.SetBudget("texture", opts.maxMemoryBytes);
        MEMORY_TRACKER.SetBudget("mesh", opts.maxMemoryBytes);
        std::printf("[Player] Memory budget set: %llu MB for textures and meshes "
                    "(reported, not enforced -- press F9 to watch it)\n",
                    (unsigned long long)(opts.maxMemoryBytes / (1024 * 1024)));
    }

    // Initialize ImGui for the player build (rlImGuiSetup creates the context)
    IMGUI_CHECKVERSION();
    rlImGuiSetup();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // no .ini config persistence
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.Fonts->AddFontDefault();

    // Physics debug geometry is world-space, so it draws through the scene
    // camera right after the 3D pass.
    engine.onDrawOverlay3D = [&]() {
        BeginMode3D(engine.GetCamera());
        TechTools::TechnicalToolsManager::Instance().Draw3D();
        EndMode3D();
    };

    // Debug stats overlay, Technical Tools windows and ImGui. These used to be
    // drawn after the engine had already presented the frame (EndDrawing), so
    // they never reached the screen reliably; drawing them from the engine's
    // 2D overlay hook keeps them inside the frame.
    engine.onDrawOverlay2D = [&]() {
        if (g_debugStats.showStats) {
            g_debugStats.DrawOverlay();
        }
        TechTools::TechnicalToolsManager::Instance().DrawUI();
        ImGui::Render();
        rlImGuiRender();
    };

    // Custom run loop with debug stats
    std::printf("[Player] Press Ctrl+F5 to toggle debug stats overlay, F9 for the memory tracker\n");
    
    while (!WindowShouldClose()) {
        g_debugStats.FrameStart();
        
        // Handle Technical Tools input
        TechTools::TechnicalToolsManager::Instance().HandleInput();
        
        // Update Technical Tools
        TechTools::TechnicalToolsManager::Instance().Update(fminf(GetFrameTime(), Engine::MAX_FRAME_DT));
        
        // ImGui frame for Technical Tools (also calls ImGui::NewFrame())
        rlImGuiNewFrame(GetFrameTime());
        
        // Handle debug stats toggle (Ctrl+F5)
        if (IsKeyDown(KEY_LEFT_CONTROL) && IsKeyPressed(KEY_F5)) {
            g_debugStats.Toggle();
        }
        if (g_debugStats.showStats) {
            if (IsKeyPressed(KEY_F6)) g_debugStats.showGraphs = !g_debugStats.showGraphs;
            if (IsKeyPressed(KEY_F7)) g_debugStats.showDetails = !g_debugStats.showDetails;
            if (IsKeyPressed(KEY_F8)) {
                g_debugStats.frameTimeMin = 9999.0;
                g_debugStats.frameTimeMax = 0.0;
            }
        }
        
        // Pause and single-step. These live outside the showStats block on
        // purpose: pausing is a gameplay/debug action, not a panel preference,
        // and a paused frame has to be reachable with the panel closed.
        if (IsKeyPressed(KEY_P)) engine.TogglePause();
        if (IsKeyPressed(KEY_PERIOD)) {
            // Stepping from a running game implies "hold still, then advance",
            // so pause first rather than queueing a step nobody can see.
            if (!engine.IsPaused()) engine.SetPaused(true);
            engine.StepFrame();
        }
        if (IsKeyPressed(KEY_ESCAPE) && engine.IsPaused()) engine.SetPaused(false);
        g_debugStats.paused = engine.IsPaused();
        g_debugStats.timeScale = engine.GetTimeScale();
        
        // Collect scene stats before update
        g_debugStats.scatteredObjects = (int)rawObjectPtrs.size();
        g_debugStats.clrReady = coreClrHostPtr && coreClrHostPtr->IsReady();
        g_debugStats.nativePlugins = nativeScriptHostPtr
            ? (int)nativeScriptHostPtr->GetPlugins().size() : 0;
        
        // Terrain chunk count is exact -- Terrain::GetChunkCount() is just
        // gridWidth * gridDepth. The old code hardcoded 1 with a comment
        // claiming the API did not exist; it does.
        g_debugStats.terrainChunks = loadedTerrain ? loadedTerrain->GetChunkCount() : 0;
        g_debugStats.waterBodies = (int)WaterBody::GetInstances().size();
        {
            // Buildings hang off a city's blocks, not off the city directly, and
            // a block is either a park or a parcel of buildings, so this is a
            // sum over every block of every live city.
            int buildings = 0;
            for (const city::City* c : city::GetCityRegistry().GetCities()) {
                if (!c) continue;
                for (const city::Block& b : c->GetBlocks()) {
                    buildings += (int)b.buildings.size();
                }
            }
            g_debugStats.cityBuildings = buildings;
        }
        
        // Physics stats
        g_debugStats.physicsBodies = simRef.GetBodyCount();
        g_debugStats.physicsContacts = simRef.GetContactCount();
        g_debugStats.physicsTimeMs = simRef.GetLastStepTimeMs();
        
        // Camera stats
        Camera3D& cam = engine.GetCamera();
        g_debugStats.camPosX = cam.position.x;
        g_debugStats.camPosY = cam.position.y;
        g_debugStats.camPosZ = cam.position.z;
        // Calculate yaw/pitch from camera target
        Vector3 forward = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        g_debugStats.camYaw = atan2f(forward.x, forward.z) * RAD2DEG;
        g_debugStats.camPitch = asinf(forward.y) * RAD2DEG;
        
        // Memory stats from the memory tracker. These are the tracked resource
        // categories, which is a different number from textureManager's
        // "approximate" figure: the tracker only counts what something actually
        // called Track* on, so a category at 0 means nothing was tracked there,
        // not that usage is 0.
        g_debugStats.textureMemoryMB = MEMORY_TRACKER.GetCurrentUsage("texture") / (1024 * 1024);
        g_debugStats.meshMemoryMB = MEMORY_TRACKER.GetCurrentUsage("mesh") / (1024 * 1024);
        g_debugStats.trackedMemoryMB = MEMORY_TRACKER.GetCurrentAllocated() / (1024 * 1024);
        g_debugStats.processMemoryMB = TechTools::MemoryTracker::GetProcessMemoryBytes() / (1024 * 1024);
        
        // Update engine (runs entity updates, physics, etc.)
        float dt = fminf(GetFrameTime(), Engine::MAX_FRAME_DT);
        engine.UpdateFrame(dt);
        
        // Render frame: the overlay callbacks above draw the debug stats,
        // Technical Tools and ImGui before the frame is presented.
        engine.RenderFrame();
        
        g_debugStats.FrameEnd();
    }
    
    // Shutdown Technical Tools
    TechTools::TechnicalToolsManager::Instance().Shutdown();

    // Shutdown ImGui
    rlImGuiShutdown();

    textureManager::Shutdown();
    return 0;
}