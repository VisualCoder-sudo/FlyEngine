// ui_terrain_panel.cpp: the terrain tool panel, split out of ui.cpp.
#include "ui_internal.hpp"
#include <map>
#include <set>
#include "../../../include/Engine/ExportManager.hpp"
#include "../../../include/Engine/Backend/VisualScript.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "../../../include/Engine/Backend/ScatteredObject.hpp"
#include "../../../include/Terrain/Water/WaterBody.hpp"
#include "../../../include/Terrain/BasicTerrain.hpp"
#include "../../../include/Terrain/Foliage.hpp"
#include "../../../include/Engine/Scripts/CommandConsole.hpp"
#include "../../../include/Engine/Scripts/ScriptRuntime.hpp"
#include "../../../include/Graphics/Graphics.hpp"
#include "../../../include/Graphics/PostFX.hpp"
#include "../../../include/Engine/LoadingScreen.hpp"
#include "../include/Engine/Scripts/ScriptLauncher.hpp"
#include "../../../include/Engine/Scripts/NativeScriptHost.hpp"
#include "../include/Engine/Backend/PhysicsSimulation.hpp"
#include "../../../include/Engine/Backend/TextureManager.hpp"
#include "../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Backend/ModelImport.hpp"
#include "../include/Terrain/Terrain.hpp"
#include "../../../include/Terrain/TerrainEditor.hpp"
#include "../../../include/Terrain/TerrainRegistry.hpp"
#include "../../../include/CityGen/CityEditor.hpp"
#include "raymath.h"
#include "imgui.h"
#include "rlimgui.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <string>
#include <unordered_map>
#include <filesystem>
#include <algorithm>
#include "ui_internal.hpp"

namespace ui {

// ---------------------------------------------------------------------------
// Terrain tool panel (Cities Skylines 2 / Unity style)
// Top half: terrain tools (raise/lower/smooth/flatten + brush sliders), or
// the procedural Generate tab (region box + seed + hills/mountains/textures)
// Bottom half: terrain properties
// ---------------------------------------------------------------------------
// Terrain Tool Dropdown State (for deferred rendering on top of all panels)
bool g_terrainToolDropdownListPending = false;
Rectangle g_terrainToolDropdownListRect = { 0 };
int g_terrainToolDropdownActiveTool = -1;

// ---------------------------------------------------------------------------
static int g_terrainTab = 0; // 0 = Sculpt tab, 1 = Generate tab, 2 = Foliage tab
static int g_foliagePickType = -1; // Foliage tab: the model type a picked file is added to as a variant; -1 = it makes a new type
static float g_terrainPanelScrollY = 0.0f;
static float g_terrainPanelContentHeight = 0.0f; // measured at the end of the previous frame's draw
bool g_terrainToolDropdownOpen = false;
static Rectangle g_terrainToolDropdownAnchor = { 0 };

Rectangle GetTerrainPanelBounds() { return GetPanelBounds(PanelId::Terrain); }

static void TerrainSlider(const char* label, float x, float y, float w,
                          float& value, float mn, float mx, const char* fmt, uint64_t key) {
    // Label
    DrawTextArial(label, x, y + 4.0f, 12.0f, theme::TEXT_MUTED);

    Rectangle track = { x + 86.0f, y + 6.0f, w - 86.0f - 56.0f, 12.0f };
    Vector2 mouse = GetMousePosition();

    float t = (mx > mn) ? (value - mn) / (mx - mn) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    bool hovered = CheckCollisionPointRec(mouse, track);
    static std::unordered_map<uint64_t, bool> dragState;
    bool& dragging = dragState[key];

    if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) dragging = true;
    if (dragging && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        float nt = (mouse.x - track.x) / track.width;
        value = mn + std::clamp(nt, 0.0f, 1.0f) * (mx - mn);
        if (hovered || CheckCollisionPointRec(mouse, Rectangle{ track.x - 6, track.y - 6, track.width + 12, track.height + 12 })) MarkHand();
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) dragging = false;

    // Track
    DrawRectangleRounded(track, 0.5f, 4, theme::BG_INPUT);
    Rectangle fill = { track.x, track.y, track.width * t, track.height };
    if (fill.width >= 2.0f) DrawRectangleRounded(fill, 0.5f, 4, Fade(theme::ACCENT, 0.55f));
    // Knob
    Vector2 knob = { track.x + track.width * t, track.y + track.height * 0.5f };
    DrawCircleV(knob, hovered ? 8.0f : 7.0f, hovered ? theme::ACCENT_HOVER : theme::ACCENT);

    char buf[48];
    snprintf(buf, sizeof(buf), fmt, value);
    float tw2 = MeasureTextArial(buf, 12.0f);
    DrawTextArial(buf, x + w - tw2 - 4.0f, y + 5.0f, 12.0f, theme::TEXT);
}

static bool TerrainCheckbox(float x, float y, const char* label, bool value, uint64_t key) {
    Vector2 mouse = GetMousePosition();
    Rectangle checkRect = { x, y, 18.0f, 18.0f };
    bool hovered = CheckCollisionPointRec(mouse, checkRect);
    float t = HoverProgress(key, hovered);
    Color bg = value ? theme::SUCCESS : theme::BG_WIDGET;
    DrawRectangleRounded(checkRect, 0.2f, 4, Mix(bg, theme::ACCENT_HOVER, t));
    DrawRectangleLinesEx(checkRect, hovered ? 2.0f : 1.0f, hovered ? theme::ACCENT : theme::BORDER);
    if (value) {
        Vector2 c1 = { checkRect.x + 4.0f, checkRect.y + checkRect.height * 0.5f };
        Vector2 c2 = { checkRect.x + checkRect.width * 0.5f, checkRect.y + checkRect.height - 4.0f };
        Vector2 c3 = { checkRect.x + checkRect.width - 4.0f, checkRect.y + 4.0f };
        DrawLineEx(c1, c2, 2.0f, COL_WHITE);
        DrawLineEx(c2, c3, 2.0f, COL_WHITE);
    }
    if (hovered) MarkHand();
    DrawTextArial(label, x + 26.0f, y + 3.0f, 13.0f, theme::TEXT_MUTED);
    return hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

static void DrawToolIcon(int kind, Rectangle ir, Color col) {
    Vector2 c = { ir.x + ir.width * 0.5f, ir.y + ir.height * 0.5f };
    switch (kind) {
        case 0: { // Raise: mountain with up arrow
            Vector2 a = { c.x, ir.y + 3.0f };
            Vector2 b = { ir.x + 4.0f, ir.y + ir.height - 3.0f };
            Vector2 d = { ir.x + ir.width - 4.0f, ir.y + ir.height - 3.0f };
            DrawTriangle(a, b, d, col);
            DrawLineEx({ c.x, ir.y + 1.0f }, { c.x, ir.y + 8.0f }, 2.0f, col);   // arrow shaft
            DrawLineEx({ c.x - 3.0f, ir.y + 4.0f }, { c.x, ir.y + 1.0f }, 2.0f, col);
            DrawLineEx({ c.x + 3.0f, ir.y + 4.0f }, { c.x, ir.y + 1.0f }, 2.0f, col);
            break;
        }
        case 1: { // Lower: inverted mountain
            Vector2 a = { c.x, ir.y + ir.height - 3.0f };
            Vector2 b = { ir.x + 4.0f, ir.y + 3.0f };
            Vector2 d = { ir.x + ir.width - 4.0f, ir.y + 3.0f };
            DrawTriangle(a, b, d, col);
            break;
        }
        case 2: { // Smooth: soft wave
            Vector2 p[7] = {
                { ir.x + 2.0f, c.y + 4.0f },
                { ir.x + ir.width * 0.22f, c.y - 3.0f },
                { ir.x + ir.width * 0.40f, c.y - 5.0f },
                { c.x, c.y },
                { ir.x + ir.width * 0.60f, c.y + 5.0f },
                { ir.x + ir.width * 0.78f, c.y + 3.0f },
                { ir.x + ir.width - 2.0f, c.y - 4.0f },
            };
            for (int i = 0; i < 6; i++) DrawLineEx(p[i], p[i + 1], 2.0f, col);
            break;
        }
        case 3: { // Flatten: plateau
            Vector2 p[6] = {
                { ir.x + 2.0f, ir.y + ir.height - 4.0f },
                { ir.x + ir.width * 0.25f, ir.y + 6.0f },
                { ir.x + ir.width * 0.45f, ir.y + 6.0f },
                { ir.x + ir.width * 0.55f, ir.y + 6.0f },
                { ir.x + ir.width * 0.75f, ir.y + 6.0f },
                { ir.x + ir.width - 2.0f, ir.y + ir.height - 4.0f },
            };
            for (int i = 0; i < 5; i++) DrawLineEx(p[i], p[i + 1], 2.0f, col);
            DrawLineEx({ ir.x + 2.0f, ir.y + ir.height - 4.0f }, { ir.x + ir.width - 2.0f, ir.y + ir.height - 4.0f }, 1.0f, Fade(col, 0.5f));
            break;
        }
        case 4: { // Paint: paintbrush
            // Brush handle
            DrawLineEx({ c.x - 7.0f, c.y + 8.0f }, { c.x + 1.0f, c.y - 1.0f }, 3.0f, col);
            // Brush ferrule
            DrawRectangleRec({ c.x - 2.0f, c.y - 5.0f, 8.0f, 6.0f }, col);
            // Brush tip
            DrawLineEx({ c.x + 2.0f, c.y - 5.0f }, { c.x + 2.0f, c.y - 10.0f }, 2.0f, col);
            DrawLineEx({ c.x + 5.0f, c.y - 5.0f }, { c.x + 5.0f, c.y - 8.0f }, 2.0f, col);
            break;
        }
        case 5: { // Erode: rain droplet carving a channel
            // teardrop
            DrawCircleV({ c.x, c.y - 7.0f }, 2.2f, col);
            DrawTriangle({ c.x - 2.6f, c.y - 6.0f }, { c.x + 2.6f, c.y - 6.0f }, { c.x, c.y - 1.0f }, col);
            // channel running downhill
            DrawLineEx({ c.x - 3.0f, c.y + 1.0f }, { c.x - 5.0f, c.y + 8.0f }, 2.0f, col);
            DrawLineEx({ c.x + 3.0f, c.y + 1.0f }, { c.x + 5.0f, c.y + 8.0f }, 2.0f, col);
            DrawLineEx({ c.x - 5.0f, c.y + 8.0f }, { c.x + 5.0f, c.y + 8.0f }, 2.0f, col);
            break;
        }
        case 6: { // Noise: four-point sparkle
            DrawLineEx({ c.x - 6.5f, c.y - 6.5f }, { c.x + 6.5f, c.y + 6.5f }, 1.8f, col);
            DrawLineEx({ c.x - 6.5f, c.y + 6.5f }, { c.x + 6.5f, c.y - 6.5f }, 1.8f, col);
            DrawLineEx({ c.x, c.y - 8.0f }, { c.x, c.y + 8.0f }, 1.8f, col);
            DrawLineEx({ c.x - 8.0f, c.y }, { c.x + 8.0f, c.y }, 1.8f, col);
            DrawCircleV(c, 1.6f, col);
            break;
        }
        case 7: { // Ramp: graded slope with start/end ticks
            DrawLineEx({ ir.x + 3.0f, ir.y + ir.height - 3.0f }, { ir.x + ir.width - 3.0f, ir.y + 3.0f }, 2.5f, col);
            DrawLineEx({ ir.x + 3.0f, ir.y + ir.height - 7.0f }, { ir.x + 3.0f, ir.y + ir.height - 2.0f }, 2.0f, col);
            DrawLineEx({ ir.x + ir.width - 3.0f, ir.y + 6.0f }, { ir.x + ir.width - 3.0f, ir.y + 1.0f }, 2.0f, col);
            break;
        }
        case 8: { // None: cursor/arrow
            DrawLineEx({ c.x - 8.0f, c.y + 8.0f }, { c.x, c.y - 8.0f }, 2.5f, col);
            DrawLineEx({ c.x, c.y - 8.0f }, { c.x + 8.0f, c.y + 8.0f }, 2.5f, col);
            DrawLineEx({ c.x - 4.0f, c.y }, { c.x + 4.0f, c.y }, 2.0f, col);
            break;
        }
    }
}

// Draws the real asset icon for a terrain sculpt tool (kind matches
// BasicTerrain::Tool / TerrainToolIconType ordering), tinted with col.
// Falls back to the hand-drawn DrawToolIcon() vector glyph if the asset
// failed to load (e.g. missing from assets/EditorIcons/).
void DrawTerrainToolIcon(int kind, Rectangle ir, Color col) {
    if (kind < 0 || kind >= TTOOLICON_COUNT || g_terrainToolIcons[kind].id == 0) {
        DrawToolIcon(kind, ir, col);
        return;
    }
    Texture2D& tex = g_terrainToolIcons[kind];
    Rectangle src = { 0.0f, 0.0f, static_cast<float>(tex.width), static_cast<float>(tex.height) };
    DrawTexturePro(tex, src, ir, { 0.0f, 0.0f }, 0.0f, col);
}

void DrawTerrainToolPanel() {
    Rectangle panelRec = GetTerrainPanelBounds();
    if (panelRec.width <= 0.0f) return;
    
    // Get both terrain types
    BasicTerrain* tBasic = BasicTerrain::GetActive();
    terrain::Terrain* tChunked = nullptr;
    auto& terrainEditorState = terrain::GetTerrainEditorState();
    if (terrainEditorState.selectedTerrainLegacy) {
        tChunked = terrainEditorState.selectedTerrainLegacy;
    }
    
    // Determine active terrain (prefer explicitly selected chunked terrain, else BasicTerrain)
    bool useChunked = (tChunked != nullptr);
    BasicTerrain::Brush& brBasic = BasicTerrain::GetBrush();
    terrain::TerrainBrush& brChunked = terrainEditorState.brush;
    
    // Use a unified brush reference for UI
    struct UnifiedBrush {
        BasicTerrain::Tool tool = BasicTerrain::Tool::None;
        BasicTerrain::Shape shape = BasicTerrain::Shape::Circle;
        float radius = 25.0f;
        float strength = 40.0f;
        float hardness = 0.5f;
        float targetHeight = 0.0f;
        int paintLayer = 0;
        bool paintErase = false;
        int noiseSeed = 1337;
        float noiseScale = 40.0f;
        // Erosion settings
        float erosionThermal = 0.5f;
        float erosionHydraulic = 0.8f;
        float erosionTalusDeg = 3.0f;
        float erosionRain = 0.05f;
        float erosionEvaporation = 0.03f;
        float erosionDeposit = 0.6f;
        int erosionIterations = 6;
    };
    
    UnifiedBrush br;
    if (useChunked) {
        br.tool = static_cast<BasicTerrain::Tool>(brChunked.tool);
        br.shape = static_cast<BasicTerrain::Shape>(brChunked.shape);
        br.radius = brChunked.radius;
        br.strength = brChunked.strength;
        br.hardness = brChunked.hardness;
        br.targetHeight = brChunked.targetHeight;
        br.paintLayer = brChunked.paintLayer;
        br.paintErase = brChunked.paintErase;
        br.noiseSeed = brChunked.noiseSeed;
        br.noiseScale = brChunked.noiseScale;
        br.erosionThermal = brChunked.erosionThermal;
        br.erosionHydraulic = brChunked.erosionHydraulic;
        br.erosionTalusDeg = brChunked.erosionTalusDeg;
        br.erosionRain = brChunked.erosionRain;
        br.erosionEvaporation = brChunked.erosionEvaporation;
        br.erosionDeposit = brChunked.erosionDeposit;
        br.erosionIterations = brChunked.erosionIterations;
    } else {
        br.tool = brBasic.tool;
        br.shape = brBasic.shape;
        br.radius = brBasic.radius;
        br.strength = brBasic.strength;
        br.hardness = brBasic.hardness;
        br.targetHeight = brBasic.targetHeight;
        br.paintLayer = brBasic.paintLayer;
        br.paintErase = brBasic.paintErase;
        br.noiseSeed = brBasic.noiseSeed;
        br.noiseScale = brBasic.noiseScale;
        // BasicTerrain doesn't have erosion settings in brush, use defaults
    }

    // Safety: if a terrain is selected but tool is None, default to Raise
    if ((useChunked || tBasic) && br.tool == BasicTerrain::Tool::None && g_terrainTab == 0) {
        br.tool = BasicTerrain::Tool::Raise;
    }

    const float pad = 14.0f;

    // Shadow + body
    DrawRectangleRec({ panelRec.x + 4.0f, panelRec.y + 4.0f, panelRec.width, panelRec.height }, theme::SHADOW);
    DrawRectangleRounded(panelRec, 0.05f, 6, theme::BG_PANEL);
    DrawRectangleLinesEx(panelRec, 2.0f, theme::BORDER);

    // Title strip
    DrawRectangleRec({ panelRec.x + 1.0f, panelRec.y + 1.0f, panelRec.width - 2.0f, 33.0f }, theme::BG_TITLE);
    DrawLine(static_cast<int>(panelRec.x), static_cast<int>(panelRec.y + 34.0f),
             static_cast<int>(panelRec.x + panelRec.width), static_cast<int>(panelRec.y + 34.0f), theme::DIVIDER);
    DrawTextArial("TERRAIN", panelRec.x + pad, panelRec.y + 10.0f, 14.0f, theme::ACCENT);

    Vector2 mouse = GetMousePosition();

    // Content area (below the title strip) used for scissor clipping +
    // scrolling, same pattern as the Properties panel. Content height is
    // measured from the previous frame's pass (see the cleanup lambda
    // below) since the true height depends on which tab/tools are active.
    Rectangle contentRec = { panelRec.x + 2.0f, panelRec.y + 36.0f, panelRec.width - 4.0f, panelRec.height - 38.0f };
    if (CheckCollisionPointRec(mouse, panelRec)) {
        g_terrainPanelScrollY -= GetMouseWheelMove() * 24.0f;
    }
    float maxScroll = std::max(0.0f, g_terrainPanelContentHeight - contentRec.height);
    g_terrainPanelScrollY = std::clamp(g_terrainPanelScrollY, 0.0f, maxScroll);

    BeginScissorMode(static_cast<int>(contentRec.x), static_cast<int>(contentRec.y),
                      static_cast<int>(contentRec.width), static_cast<int>(contentRec.height));

    float innerW = panelRec.width - pad * 2.0f;
    float x = panelRec.x + pad;
    float contentTopY = panelRec.y + 44.0f;
    float y = contentTopY - g_terrainPanelScrollY;

    // Ends the scissor region, draws the tool dropdown's expanded list (if
    // open) unclipped on top of everything, and records this frame's real
    // content height for next frame's scroll clamping. Called before every
    // exit path (the early "no terrain" return and the natural end).
    auto finishPanel = [&]() {
        // Handle dropdown first (including click processing) so tool changes
        // are captured before we write back the brush state.
        if (g_terrainToolDropdownOpen) {
            constexpr int numTools = 9;
            const char* toolNames[numTools] = { "Raise", "Lower", "Smooth", "Flatten", "Paint", "Erode", "Noise", "Ramp", "None" };
            Rectangle anchor = g_terrainToolDropdownAnchor;
            Rectangle listRec = { anchor.x, anchor.y + anchor.height + 2.0f, anchor.width,
                                   static_cast<float>(numTools) * 26.0f + 8.0f };
            // 9 items can overhang the bottom of short screens: flip the list
            // above the anchor when it would not fit below.
            if (listRec.y + listRec.height > GetScreenHeight() - 8.0f) {
                listRec.y = anchor.y - listRec.height - 2.0f;
            }

            DrawRectangleRec({ listRec.x + 3.0f, listRec.y + 3.0f, listRec.width, listRec.height }, theme::SHADOW);
            DrawRectangleRounded(listRec, 0.08f, 4, theme::BG_MENU);
            DrawRectangleLinesEx(listRec, 1.0f, theme::BORDER_STRONG);

            int activeTool = static_cast<int>(br.tool);
            for (int i = 0; i < numTools; ++i) {
                Rectangle itemRec = { listRec.x + 4.0f, listRec.y + 4.0f + static_cast<float>(i) * 26.0f, listRec.width - 8.0f, 22.0f };
                bool hovered = CheckCollisionPointRec(mouse, itemRec);
                bool selected = (i == activeTool);
                const bool itemActive = hovered || selected;
                if (itemActive) DrawDropdownBox(itemRec, true);
                Rectangle iconRec = { itemRec.x + 6.0f, itemRec.y + 3.0f, 16.0f, 16.0f };
                DrawTerrainToolIcon(i, iconRec, itemActive ? theme::TEXT : theme::TEXT_MUTED);
                DrawTextArial(toolNames[i], itemRec.x + 28.0f, itemRec.y + 4.0f, 12.0f,
                              itemActive ? theme::TEXT : theme::TEXT_MUTED);
                if (hovered) MarkHand();
                if (hovered && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_clickConsumedThisFrame) {
                    auto newTool = static_cast<BasicTerrain::Tool>(i);
                    if (newTool == BasicTerrain::Tool::Paint && br.tool != BasicTerrain::Tool::Paint) {
                        int lc = useChunked ? (int)tChunked->layers.size() : BasicTerrain::GetLayerCount();
                        if (lc > 1) br.paintLayer = 1;
                        else if (lc > 0) br.paintLayer = 0;
                    }
                    br.tool = newTool;
                    g_terrainToolDropdownOpen = false;
                    g_clickConsumedThisFrame = true;
                }
            }

            if (IsKeyPressed(KEY_ESCAPE)) g_terrainToolDropdownOpen = false;
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_clickConsumedThisFrame
                && !CheckCollisionPointRec(mouse, listRec) && !CheckCollisionPointRec(mouse, anchor)) {
                g_terrainToolDropdownOpen = false;
                g_clickConsumedThisFrame = true;
            }
        }

        // If dropdown is open, capture list state for deferred rendering on top of all panels
        if (g_terrainToolDropdownOpen) {
            g_terrainToolDropdownListPending = true;
            constexpr int numTools = 9;
            Rectangle anchor = g_terrainToolDropdownAnchor;
            Rectangle listRec = { anchor.x, anchor.y + anchor.height + 2.0f, anchor.width,
                                   static_cast<float>(numTools) * 26.0f + 8.0f };
            if (listRec.y + listRec.height > GetScreenHeight() - 8.0f) {
                listRec.y = anchor.y - listRec.height - 2.0f;
            }
            g_terrainToolDropdownListRect = listRec;
            g_terrainToolDropdownActiveTool = static_cast<int>(br.tool);
        } else {
            g_terrainToolDropdownListPending = false;
        }

        EndScissorMode();
        g_terrainPanelContentHeight = (y - contentTopY) + g_terrainPanelScrollY;

        // Write back unified brush to actual brushes (AFTER dropdown click handling)
        if (useChunked) {
            brChunked.tool = static_cast<terrain::TerrainTool>(br.tool);
            brChunked.shape = static_cast<terrain::TerrainBrush::Shape>(br.shape);
            brChunked.radius = br.radius;
            brChunked.strength = br.strength;
            brChunked.hardness = br.hardness;
            brChunked.targetHeight = br.targetHeight;
            brChunked.paintLayer = br.paintLayer;
            brChunked.paintErase = br.paintErase;
            brChunked.noiseSeed = br.noiseSeed;
            brChunked.noiseScale = br.noiseScale;
            brChunked.erosionThermal = br.erosionThermal;
            brChunked.erosionHydraulic = br.erosionHydraulic;
            brChunked.erosionTalusDeg = br.erosionTalusDeg;
            brChunked.erosionRain = br.erosionRain;
            brChunked.erosionEvaporation = br.erosionEvaporation;
            brChunked.erosionDeposit = br.erosionDeposit;
            brChunked.erosionIterations = br.erosionIterations;
        } else {
            brBasic.tool = br.tool;
            brBasic.shape = br.shape;
            brBasic.radius = br.radius;
            brBasic.strength = br.strength;
            brBasic.hardness = br.hardness;
            brBasic.targetHeight = br.targetHeight;
            brBasic.paintLayer = br.paintLayer;
            brBasic.paintErase = br.paintErase;
            brBasic.noiseSeed = br.noiseSeed;
            brBasic.noiseScale = br.noiseScale;
        }
    };


    // ================= TOP HALF: TERRAIN TOOLS =================
    DrawTextArial("TERRAIN", x, y, 11.0f, theme::TEXT_DIM);
    DrawLine(static_cast<int>(x + 52.0f), static_cast<int>(y + 7.0f),
             static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
    y += 18.0f;

    // Sculpt / Generate tabs
    {
        const char* tabNames[3] = { "Sculpt", "Generate", "Foliage" };
        float tabGap = 6.0f;
        float tabW = (innerW - tabGap * 2.0f) / 3.0f;
        float tabH = 26.0f;
        for (int i = 0; i < 3; i++) {
            Rectangle tr = { x + i * (tabW + tabGap), y, tabW, tabH };
            bool sel = (g_terrainTab == i);
            bool hov = CheckCollisionPointRec(mouse, tr);
            float ht3 = HoverProgress(static_cast<uint64_t>(9600u + i), hov);
            Color bg = sel ? theme::ACCENT_SOFT : theme::BG_WIDGET;
            bg = Mix(bg, theme::BG_WIDGET_HOVER, ht3 * (sel ? 0.3f : 1.0f));
            DrawRectangleRounded(tr, 0.2f, 4, bg);
            DrawRectangleLinesEx(tr, sel ? 2.0f : 1.0f, sel ? theme::ACCENT : theme::BORDER);
            float tw3 = MeasureTextArial(tabNames[i], 12.0f);
            DrawTextArial(tabNames[i], tr.x + (tr.width - tw3) * 0.5f, tr.y + 6.0f, 12.0f,
                          sel ? theme::TEXT : theme::TEXT_MUTED);
            if (hov) MarkHand();
            if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) g_terrainTab = i;
        }
        y += tabH + 10.0f;
    }

    // The region-box preview in the viewport only makes sense while the
    // Generate tab is actually open for this terrain.
    if (tBasic) tBasic->showGenBox = (g_terrainTab == 1);
    if (g_terrainTab != 2 || !tBasic) foliage::SetPaintTarget(nullptr, -1);   // the foliage brush only lives on its tab
    // Each tab owns its own brush: off the Sculpt tab the sculpt tool is put away (and comes back with the tab), so a
    // click only ever does what the open tab says.
    {
        static BasicTerrain::Tool savedTool = BasicTerrain::Tool::None;
        static bool toolPutAway = false;
        BasicTerrain::Brush& sb = BasicTerrain::GetBrush();
        if (g_terrainTab != 0) {
            if (!toolPutAway) { savedTool = sb.tool; toolPutAway = true; }
            sb.tool = BasicTerrain::Tool::None;
            br.tool = BasicTerrain::Tool::None;   // finishPanel() writes br back
        } else if (toolPutAway) {
            sb.tool = savedTool;
            br.tool = savedTool;
            toolPutAway = false;
        }
    }

    if (g_terrainTab == 1) {
        // ================= GENERATE TAB =================
        bool hasTerrain = (tBasic != nullptr || tChunked != nullptr);
        if (!hasTerrain) {
            DrawTextArial("Select or spawn a terrain first.", x, y + 4.0f, 12.0f, theme::TEXT_MUTED);
            y += 28.0f;
        } else if (tBasic) {
            // BasicTerrain procedural generation
            if (!tBasic->genBoxInitialized) {
                tBasic->genBoxPos = { tBasic->position.x, tBasic->position.y + tBasic->GetMaxHeight() * 0.15f, tBasic->position.z };
                float fw = tBasic->GetWidth() * tBasic->GetScale();
                float fd = tBasic->GetDepth() * tBasic->GetScale();
                tBasic->genBoxSize = { fw * 0.5f, std::max(20.0f, (tBasic->GetMaxHeight() - tBasic->GetMinHeight()) * 0.4f), fd * 0.5f };
                tBasic->genBoxInitialized = true;
            }

            DrawTextArial("SEED", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            float seedF = (float)tBasic->genSeed;
            TerrainSlider("Seed", x, y, innerW - 78.0f, seedF, 0.0f, 999999.0f, "%.0f", 9610u);
            tBasic->genSeed = (int)seedF;
            Rectangle randBtn = { x + innerW - 70.0f, y - 4.0f, 70.0f, 24.0f };
            bool rndHov = CheckCollisionPointRec(mouse, randBtn);
            DrawRectangleRounded(randBtn, 0.2f, 4, rndHov ? theme::BG_WIDGET_HOVER : theme::BG_WIDGET);
            DrawRectangleLinesEx(randBtn, rndHov ? 2.0f : 1.0f, rndHov ? theme::ACCENT : theme::BORDER);
            float rndW = MeasureTextArial("Random", 11.0f);
            DrawTextArial("Random", randBtn.x + (randBtn.width - rndW) * 0.5f, randBtn.y + 6.0f, 11.0f,
                          rndHov ? theme::TEXT : theme::TEXT_MUTED);
            if (rndHov) MarkHand();
            if (rndHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) tBasic->genSeed = GetRandomValue(0, 999999);
            y += 34.0f;

            DrawTextArial("INCLUDE", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            if (TerrainCheckbox(x, y, "Hills", tBasic->genHills, 9611u)) tBasic->genHills = !tBasic->genHills;
            y += 24.0f;
            if (TerrainCheckbox(x, y, "Mountains", tBasic->genMountains, 9612u)) tBasic->genMountains = !tBasic->genMountains;
            y += 24.0f;
            if (TerrainCheckbox(x, y, "Extra Textures", tBasic->genTextures, 9613u)) tBasic->genTextures = !tBasic->genTextures;
            y += 30.0f;

            DrawTextArial("REGION BOX - POSITION", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            float posRangeX = tBasic->GetWidth() * tBasic->GetScale();
            float posRangeZ = tBasic->GetDepth() * tBasic->GetScale();
            TerrainSlider("X", x, y, innerW, tBasic->genBoxPos.x, tBasic->position.x - posRangeX, tBasic->position.x + posRangeX, "%.0f m", 9620u); y += 30.0f;
            TerrainSlider("Y", x, y, innerW, tBasic->genBoxPos.y, tBasic->GetMinHeight(), tBasic->GetMaxHeight(), "%.0f m", 9621u); y += 30.0f;
            TerrainSlider("Z", x, y, innerW, tBasic->genBoxPos.z, tBasic->position.z - posRangeZ, tBasic->position.z + posRangeZ, "%.0f m", 9622u); y += 32.0f;

            DrawTextArial("REGION BOX - SIZE", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            float maxVSize = std::max(20.0f, tBasic->GetMaxHeight() - tBasic->GetMinHeight());
            TerrainSlider("Width",  x, y, innerW, tBasic->genBoxSize.x, 4.0f, posRangeX * 2.0f, "%.0f m", 9623u); y += 30.0f;
            TerrainSlider("Height", x, y, innerW, tBasic->genBoxSize.y, 4.0f, maxVSize, "%.0f m", 9624u); y += 30.0f;
            TerrainSlider("Depth",  x, y, innerW, tBasic->genBoxSize.z, 4.0f, posRangeZ * 2.0f, "%.0f m", 9625u); y += 32.0f;

            // Generate button
            Rectangle genBtn = { x, y, innerW, 34.0f };
            bool gHov = CheckCollisionPointRec(mouse, genBtn);
            float gt = HoverProgress(9630u, gHov);
            DrawRectangleRounded(genBtn, 0.15f, 4, Mix(theme::ACCENT_SOFT, theme::ACCENT_HOVER, gt));
            DrawRectangleLinesEx(genBtn, gHov ? 2.0f : 1.0f, theme::ACCENT);
            const char* genLabel = "Generate";
            float glw = MeasureTextArial(genLabel, 13.0f);
            DrawTextArial(genLabel, genBtn.x + (genBtn.width - glw) * 0.5f, genBtn.y + 9.0f, 13.0f, theme::TEXT);
            if (gHov) MarkHand();
            if (gHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                tBasic->GenerateTerrainInBox();
            }
            y += 34.0f + 10.0f;

            // ================= EROSION (BasicTerrain) =================
            DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
                     static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
            y += 12.0f;
            DrawTextArial("EROSION", x, y, 11.0f, theme::TEXT_DIM);
            DrawLine(static_cast<int>(x + 58.0f), static_cast<int>(y + 7.0f),
                     static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
            y += 18.0f;

            static BasicTerrain::ErosionSettings es;
            float iterF = (float)es.iterations;
            TerrainSlider("Iterations", x, y, innerW, iterF, 1.0f, 60.0f, "%.0f", 9640u); es.iterations = (int)iterF; y += 30.0f;
            TerrainSlider("Thermal",   x, y, innerW, es.thermal,     0.0f, 2.0f, "%.2f", 9641u); y += 30.0f;
            TerrainSlider("Hydraulic", x, y, innerW, es.hydraulic,   0.0f, 2.0f, "%.2f", 9642u); y += 30.0f;
            TerrainSlider("Rain",      x, y, innerW, es.rain,        0.0f, 0.2f, "%.3f", 9643u); y += 30.0f;
            TerrainSlider("Evap",      x, y, innerW, es.evaporation, 0.0f, 0.1f, "%.3f", 9644u); y += 30.0f;
            TerrainSlider("Deposit",   x, y, innerW, es.deposit,     0.0f, 1.0f, "%.2f", 9645u); y += 34.0f;

            // Convert the region box to a clamped grid-cell rectangle
            auto regionCells = [&](int& rx0, int& rz0, int& rx1, int& rz1) {
                const float scaleF = tBasic->GetScale();
                const float c0x = (tBasic->genBoxPos.x - tBasic->genBoxSize.x * 0.5f - tBasic->position.x) / scaleF + tBasic->GetWidth() * 0.5f;
                const float c1x = (tBasic->genBoxPos.x + tBasic->genBoxSize.x * 0.5f - tBasic->position.x) / scaleF + tBasic->GetWidth() * 0.5f;
                const float c0z = (tBasic->genBoxPos.z - tBasic->genBoxSize.z * 0.5f - tBasic->position.z) / scaleF + tBasic->GetDepth() * 0.5f;
                const float c1z = (tBasic->genBoxPos.z + tBasic->genBoxSize.z * 0.5f - tBasic->position.z) / scaleF + tBasic->GetDepth() * 0.5f;
                rx0 = (int)std::floor(std::min(c0x, c1x));
                rx1 = (int)std::ceil(std::max(c0x, c1x));
                rz0 = (int)std::floor(std::min(c0z, c1z));
                rz1 = (int)std::ceil(std::max(c0z, c1z));
            };

            const float btnGap = 6.0f;
            const float btnW = (innerW - btnGap) * 0.5f;
            auto erosionButton = [&](const char* label, float bx, uint64_t key, bool region) {
                Rectangle eBtn = { bx, y, btnW, 28.0f };
                bool eHov = CheckCollisionPointRec(mouse, eBtn);
                float eT = HoverProgress(key, eHov);
                DrawRectangleRounded(eBtn, 0.18f, 4, Mix(theme::ACCENT_SOFT, theme::ACCENT_HOVER, eT));
                DrawRectangleLinesEx(eBtn, eHov ? 2.0f : 1.0f, eHov ? theme::ACCENT : theme::BORDER);
                float lw = MeasureTextArial(label, 11.0f);
                DrawTextArial(label, eBtn.x + (eBtn.width - lw) * 0.5f, eBtn.y + 7.0f, 11.0f,
                              eHov ? theme::TEXT : theme::TEXT_MUTED);
                if (eHov) MarkHand();
                if (eHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    if (region) {
                        int rx0, rz0, rx1, rz1;
                        regionCells(rx0, rz0, rx1, rz1);
                        tBasic->ErodeRegion(rx0, rz0, rx1, rz1, es);
                    } else {
                        tBasic->ErodeRegion(0, 0, tBasic->GetWidth() - 1, tBasic->GetDepth() - 1, es);
                    }
                }
            };
            erosionButton("Erode Region", x, 9650u, true);
            erosionButton("Erode All", x + btnW + btnGap, 9651u, false);
            y += 34.0f + 8.0f;
        } else if (tChunked) {
            // Chunked terrain - no procedural generation, but show erosion controls
            DrawTextArial("CHUNKED TERRAIN (no procedural generation)", x, y + 4.0f, 12.0f, theme::TEXT_MUTED);
            y += 28.0f;
            
            DrawTextArial("REGION BOX - POSITION", x, y, 11.0f, theme::TEXT_DIM);
            y += 14.0f;
            float halfW = tChunked->size.x * 0.5f;
            float halfD = tChunked->size.z * 0.5f;
            
            // Use proper region fields from editor state instead of reusing brush fields
            auto& terrainEditorState = terrain::GetTerrainEditorState();
            TerrainSlider("Min X", x, y, innerW, terrainEditorState.regionMinX, tChunked->center.x - halfW, tChunked->center.x + halfW, "%.0f m", 9620u); y += 30.0f;
            TerrainSlider("Max X", x, y, innerW, terrainEditorState.regionMaxX, tChunked->center.x - halfW, tChunked->center.x + halfW, "%.0f m", 9621u); y += 30.0f;
            TerrainSlider("Min Z", x, y, innerW, terrainEditorState.regionMinZ, tChunked->center.z - halfD, tChunked->center.z + halfD, "%.0f m", 9622u); y += 30.0f;
            TerrainSlider("Max Z", x, y, innerW, terrainEditorState.regionMaxZ, tChunked->center.z - halfD, tChunked->center.z + halfD, "%.0f m", 9623u); y += 30.0f;
            y += 10.0f;

            // ================= EROSION (Chunked) =================
            DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
                     static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
            y += 12.0f;
            DrawTextArial("EROSION", x, y, 11.0f, theme::TEXT_DIM);
            DrawLine(static_cast<int>(x + 58.0f), static_cast<int>(y + 7.0f),
                     static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
            y += 18.0f;

            // Use brush erosion settings for chunked terrain
            TerrainSlider("Iterations", x, y, innerW, (float&)br.erosionIterations, 1.0f, 60.0f, "%.0f", 9640u); y += 30.0f;
            TerrainSlider("Thermal",   x, y, innerW, br.erosionThermal,     0.0f, 2.0f, "%.2f", 9641u); y += 30.0f;
            TerrainSlider("Hydraulic", x, y, innerW, br.erosionHydraulic,   0.0f, 2.0f, "%.2f", 9642u); y += 30.0f;
            TerrainSlider("Rain",      x, y, innerW, br.erosionRain,        0.0f, 0.2f, "%.3f", 9643u); y += 30.0f;
            TerrainSlider("Evap",      x, y, innerW, br.erosionEvaporation, 0.0f, 0.1f, "%.3f", 9644u); y += 30.0f;
            TerrainSlider("Deposit",   x, y, innerW, br.erosionDeposit,     0.0f, 1.0f, "%.2f", 9645u); y += 34.0f;

            const float btnGap = 6.0f;
            const float btnW = (innerW - btnGap) * 0.5f;
            auto erosionButton = [&](const char* label, float bx, uint64_t key, bool region) {
                Rectangle eBtn = { bx, y, btnW, 28.0f };
                bool eHov = CheckCollisionPointRec(mouse, eBtn);
                float eT = HoverProgress(key, eHov);
                DrawRectangleRounded(eBtn, 0.18f, 4, Mix(theme::ACCENT_SOFT, theme::ACCENT_HOVER, eT));
                DrawRectangleLinesEx(eBtn, eHov ? 2.0f : 1.0f, eHov ? theme::ACCENT : theme::BORDER);
                float lw = MeasureTextArial(label, 11.0f);
                DrawTextArial(label, eBtn.x + (eBtn.width - lw) * 0.5f, eBtn.y + 7.0f, 11.0f,
                              eHov ? theme::TEXT : theme::TEXT_MUTED);
                if (eHov) MarkHand();
                if (eHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    auto& state = terrain::GetTerrainEditorState();
                    if (region) {
                        tChunked->ErodeRegion(state.regionMinX, state.regionMinZ, state.regionMaxX, state.regionMaxZ, brChunked);
                    } else {
                        // Erode all - use full terrain bounds
                        tChunked->ErodeRegion(tChunked->center.x - halfW, tChunked->center.z - halfD,
                                             tChunked->center.x + halfW, tChunked->center.z + halfD, brChunked);
                    }
                }
            };
            erosionButton("Erode Region", x, 9650u, true);
            erosionButton("Erode All", x + btnW + btnGap, 9651u, false);
            y += 34.0f + 8.0f;
        }

        // Divider between halves
        DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
                 static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
        y += 12.0f;
    } else if (g_terrainTab == 2) {
        // ================= FOLIAGE TAB =================
        if (!tBasic) {
            DrawTextArial("Select or spawn a Basic Terrain first.", x, y + 4.0f, 12.0f, theme::TEXT_MUTED);
            y += 28.0f;
        } else {
            foliage::Settings& fs = foliage::For(tBasic);
            const foliage::Settings fsBefore = fs;
            // New plants start unpainted: nothing grows until you paint it with the brush.
            auto addRule = [&](int typeId) {
                foliage::Rule nr = foliage::DefaultRuleForType(typeId);
                nr.painted = true;
                fs.rules.push_back(nr);
                const int added = (int)fs.rules.size() - 1;
                foliage::FillMask(tBasic, added, false);
                foliage::SetPaintTarget(tBasic, added);
                BasicTerrain::GetBrush().tool = BasicTerrain::Tool::None;
                ui::SetSelectedTerrain(tBasic);
            };
            // A model came back from the file dialog: it joins the type it was picked for, or starts a new type.
            {
                std::string picked;
                if (platform::PollDialogResult({ platform::DialogPurpose::PickFoliageModel }, picked) && !picked.empty()) {
                    const ::project::Info& proj = ::project::GetCurrentProject();
                    std::string stored = picked;
                    if (!proj.path.empty()) {   // copied into the project, so the project carries its plants
                        const ImportResult imp = ImportModel(picked, proj.path, proj.name);
                        if (imp.ok) stored = ResolveStoredAssetPath(imp.storedPath, proj.path);
                        else LogAlways("[foliage] import failed: %s", imp.error.c_str());
                    }
                    const int target = g_foliagePickType;
                    g_foliagePickType = -1;
                    if (target >= foliage::kBuiltinTypes && target < foliage::TypeCount()) {
                        foliage::TypeAt(target).variants.push_back({ stored, 1.0f });
                        foliage::TypeChanged(target);
                    } else {
                        foliage::Type nt;
                        nt.name = std::filesystem::path(picked).stem().string();
                        if (nt.name.empty()) nt.name = "Plant";
                        const std::string stem = nt.name;
                        for (int n = 2; foliage::FindType(nt.name) >= 0; n++) nt.name = stem + " " + std::to_string(n);
                        nt.variants.push_back({ stored, 1.0f });
                        addRule(foliage::AddType(nt));
                    }
                }
            }
            DrawTextArial("ADD a plant, then drag on the terrain to paint it.", x, y, 11.0f, theme::TEXT_DIM);
            y += 18.0f;
            if (TerrainCheckbox(x, y, "Foliage on", fs.enabled, 9700u)) fs.enabled = !fs.enabled;
            y += 26.0f;
            TerrainSlider("Wind", x, y, innerW, fs.wind, 0.0f, 2.0f, "%.2f", 9701u);
            y += 26.0f;
            TerrainSlider("Density", x, y, innerW, fs.density, 0.1f, 6.0f, "x%.2f", 9702u);
            y += 26.0f;
            TerrainSlider("Draw distance", x, y, innerW, fs.distance, 0.25f, 3.0f, "x%.2f", 9703u);
            y += 32.0f;

            auto smallButton = [&](Rectangle r, const char* label, uint64_t key) {
                bool h = CheckCollisionPointRec(mouse, r);
                float t = HoverProgress(key, h);
                DrawRectangleRounded(r, 0.18f, 4, Mix(theme::ACCENT_SOFT, theme::ACCENT_HOVER, t));
                DrawRectangleLinesEx(r, h ? 2.0f : 1.0f, h ? theme::ACCENT : theme::BORDER);
                float lw = MeasureTextArial(label, 11.0f);
                DrawTextArial(label, r.x + (r.width - lw) * 0.5f, r.y + (r.height - 11.0f) * 0.5f - 1.0f, 11.0f, h ? theme::TEXT : theme::TEXT_MUTED);
                if (h) MarkHand();
                return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
            };

            DrawTextArial("ADD", x, y, 11.0f, theme::TEXT_DIM);
            y += 16.0f;
            {
                const float gap = 5.0f, bw = (innerW - gap * 2.0f) / 3.0f;
                const int typeCount = std::min(foliage::TypeCount(), 70);
                for (int k = 0; k < typeCount; k++) {
                    const float bx = x + (float)(k % 3) * (bw + gap);
                    const float by = y + (float)(k / 3) * 28.0f;
                    std::string label = foliage::TypeAt(k).name;   // cut to fit the button
                    while (label.size() > 3 && MeasureTextArial(label.c_str(), 11.0f) > bw - 10.0f) { label.pop_back(); if (MeasureTextArial((label + "..").c_str(), 11.0f) <= bw - 10.0f) { label += ".."; break; } }
                    if (smallButton({ bx, by, bw, 24.0f }, label.c_str(), 9710u + (uint64_t)k)) addRule(k);
                }
                y += (float)((typeCount + 2) / 3) * 28.0f + 2.0f;
                if (smallButton({ x, y, innerW, 24.0f }, "Import model plant...", 9790u)) {
                    g_foliagePickType = -1;
                    BeginChooseModelFor(platform::DialogPurpose::PickFoliageModel, "Choose a plant model");
                }
                y += 32.0f;
            }

            int removeIndex = -1;
            for (size_t ri = 0; ri < fs.rules.size(); ri++) {
                foliage::Rule& r = fs.rules[ri];
                const uint64_t base = 9800u + (uint64_t)ri * 16u;
                DrawLine(static_cast<int>(x), static_cast<int>(y), static_cast<int>(x + innerW), static_cast<int>(y), theme::DIVIDER);
                y += 8.0f;
                if (TerrainCheckbox(x, y, foliage::TypeAt(r.type).name.c_str(), r.enabled, base)) r.enabled = !r.enabled;
                if (smallButton({ x + innerW - 64.0f, y - 2.0f, 64.0f, 22.0f }, "Remove", base + 1u)) removeIndex = (int)ri;
                y += 28.0f;
                if (!r.enabled) continue;
                if (foliage::TypeAt(r.type).builtin < 0) {   // a model type: its models and look
                    foliage::Type& ty = foliage::TypeAt(r.type);
                    const foliage::Type before = ty;
                    const uint64_t tk = 14000u + (uint64_t)ri * 256u;
                    int removeVariant = -1;
                    for (size_t vi = 0; vi < ty.variants.size(); vi++) {
                        foliage::Variant& v = ty.variants[vi];
                        const uint64_t vk = tk + (uint64_t)vi * 4u;
                        std::string label = std::filesystem::path(v.model).filename().string();
                        const float room = innerW - (ty.variants.size() > 1 ? 30.0f : 0.0f);
                        while (label.size() > 3 && MeasureTextArial(label.c_str(), 11.0f) > room) label.pop_back();
                        DrawTextArial(label.c_str(), x, y + 3.0f, 11.0f, foliage::TypeVariantLoaded(r.type, (int)vi) ? theme::TEXT_MUTED : theme::TEXT_DIM);
                        if (ty.variants.size() > 1 && smallButton({ x + innerW - 24.0f, y, 24.0f, 20.0f }, "X", vk)) removeVariant = (int)vi;
                        y += 22.0f;
                        TerrainSlider("Chance", x, y, innerW, v.weight, 0.0f, 10.0f, "%.1f", vk + 1u); y += 26.0f;
                    }
                    if (removeVariant >= 0) ty.variants.erase(ty.variants.begin() + removeVariant);
                    if (smallButton({ x, y, innerW, 22.0f }, "Add another model...", tk + 250u)) {
                        g_foliagePickType = r.type;
                        BeginChooseModelFor(platform::DialogPurpose::PickFoliageModel, "Choose a plant model");
                    }
                    y += 28.0f;
                    TerrainSlider("Height", x, y, innerW, ty.height, 0.1f, 40.0f, "%.1f m", tk + 240u); y += 26.0f;
                    TerrainSlider("Sink", x, y, innerW, ty.sink, -0.5f, 2.0f, "%.2f m", tk + 241u); y += 26.0f;
                    if (TerrainCheckbox(x, y, "Casts shadows", ty.castsShadow, tk + 242u)) ty.castsShadow = !ty.castsShadow;
                    y += 24.0f;
                    if (TerrainCheckbox(x, y, "Sways in wind", ty.wind, tk + 243u)) ty.wind = !ty.wind;
                    y += 24.0f;
                    if (TerrainCheckbox(x, y, "Two-sided", ty.twoSided, tk + 244u)) ty.twoSided = !ty.twoSided;
                    y += 28.0f;
                    // New models and the look flags load the meshes again; the other sliders only place the plants again.
                    bool models = before.variants.size() != ty.variants.size() || before.wind != ty.wind || before.twoSided != ty.twoSided;
                    bool placed = before.height != ty.height || before.sink != ty.sink || before.castsShadow != ty.castsShadow;
                    for (size_t vi = 0; vi < ty.variants.size() && vi < before.variants.size(); vi++) {
                        models = models || before.variants[vi].model != ty.variants[vi].model;
                        placed = placed || before.variants[vi].weight != ty.variants[vi].weight;
                    }
                    if (models || placed) foliage::TypeChanged(r.type, models);
                }
                {
                    const float gap = 5.0f, bw = (innerW - gap) / 2.0f;
                    if (smallButton({ x, y, bw, 22.0f }, r.painted ? "Grow on: Brush" : "Grow on: Layer", base + 10u)) {
                        r.painted = !r.painted;
                        if (!r.painted && foliage::PaintTarget(tBasic)) foliage::SetPaintTarget(nullptr, -1);
                    }
                    if (r.painted) {
                        int target = -1;
                        const bool on = foliage::PaintTarget(tBasic, &target) && target == (int)ri;
                        if (smallButton({ x + bw + gap, y, bw, 22.0f }, on ? "Painting: ON" : "Paint", base + 11u)) {
                            foliage::SetPaintTarget(on ? nullptr : tBasic, on ? -1 : (int)ri);
                            if (!on) { BasicTerrain::GetBrush().tool = BasicTerrain::Tool::None; ui::SetSelectedTerrain(tBasic); }
                        }
                    }
                    y += 28.0f;
                }
                if (r.painted) {
                    auto& pb = br;   // the shared brush sliders (the panel writes br back at the end)
                    const float gap = 5.0f, bw = (innerW - gap) / 2.0f;
                    if (smallButton({ x, y, bw, 22.0f }, "Fill all", base + 12u)) foliage::FillMask(tBasic, (int)ri, true);
                    if (smallButton({ x + bw + gap, y, bw, 22.0f }, "Clear all", base + 13u)) foliage::FillMask(tBasic, (int)ri, false);
                    y += 28.0f;
                    TerrainSlider("Brush size", x, y, innerW, pb.radius, 2.0f, 100.0f, "%.0f m", base + 14u); y += 26.0f;
                    TerrainSlider("Brush strength", x, y, innerW, pb.strength, 5.0f, 100.0f, "%.0f", base + 15u); y += 26.0f;
                    TerrainSlider("Brush hardness", x, y, innerW, pb.hardness, 0.0f, 1.0f, "%.2f", 12000u + (uint64_t)ri); y += 26.0f;
                    DrawTextArial("Drag on the terrain. Ctrl erases, Shift paints faster.", x, y, 10.0f, theme::TEXT_DIM);
                    y += 18.0f;
                } else {
                    float layerF = (float)r.layer;
                    TerrainSlider("Layer", x, y, innerW, layerF, -1.0f, 3.0f, "%.0f", base + 2u); r.layer = (int)std::lround(layerF); y += 26.0f;
                    TerrainSlider("Painted", x, y, innerW, r.minWeight, 0.0f, 1.0f, "%.2f", base + 3u); y += 26.0f;
                }
                TerrainSlider("Density", x, y, innerW, r.density, 0.0f, foliage::TypeAt(r.type).builtin == (int)foliage::Kind::Grass ? 3000.0f : (foliage::TypeAt(r.type).builtin == (int)foliage::Kind::Flowers || foliage::TypeAt(r.type).height < 0.8f ? 300.0f : 60.0f), "%.1f", base + 4u); y += 26.0f;
                TerrainSlider("Min size", x, y, innerW, r.minScale, 0.2f, 3.0f, "%.2f", base + 5u); y += 26.0f;
                TerrainSlider("Max size", x, y, innerW, r.maxScale, 0.2f, 4.0f, "%.2f", base + 6u); y += 26.0f;
                if (r.maxScale < r.minScale) r.maxScale = r.minScale;
                TerrainSlider("Max slope", x, y, innerW, r.maxSlope, 0.0f, 90.0f, "%.0f deg", base + 7u); y += 26.0f;
                TerrainSlider("Distance", x, y, innerW, r.viewDistance, 10.0f, 2000.0f, "%.0f m", base + 8u); y += 26.0f;
                TerrainSlider("Fade start", x, y, innerW, r.fadeStart, 0.0f, 0.95f, "%.2f", 12100u + (uint64_t)ri * 2u); y += 26.0f;
                TerrainSlider("Far keep", x, y, innerW, r.farKeep, 0.0f, 1.0f, "%.2f", 12101u + (uint64_t)ri * 2u); y += 26.0f;
                TerrainSlider("LOD 1 at", x, y, innerW, r.lod1, 0.0f, 1.0f, "%.2f", 12200u + (uint64_t)ri * 2u); y += 26.0f;
                TerrainSlider("LOD 2 at", x, y, innerW, r.lod2, 0.0f, 1.0f, "%.2f", 12201u + (uint64_t)ri * 2u); y += 26.0f;
                if (r.lod2 < r.lod1) r.lod2 = r.lod1;
                float seedF = (float)r.seed;
                TerrainSlider("Seed", x, y, innerW, seedF, 0.0f, 999.0f, "%.0f", base + 9u); r.seed = (int)seedF; y += 30.0f;
            }
            if (removeIndex >= 0) { foliage::SetPaintTarget(nullptr, -1); fs.rules.erase(fs.rules.begin() + removeIndex); }
            if (!fs.rules.empty()) {
                const std::string total = std::to_string(foliage::InstanceCount()) + " plants placed, " + std::to_string(foliage::DrawnCount()) + " drawn";
                DrawTextArial(total.c_str(), x, y, 11.0f, theme::TEXT_DIM);
                y += 20.0f;
            }

            // Rules changed: the plants are placed again. (Dragging a slider re-places them every frame, which is cheap:
            // only the cells near the camera are rebuilt.)
            bool changed = fs.rules.size() != fsBefore.rules.size() || fs.enabled != fsBefore.enabled || fs.density != fsBefore.density;
            for (size_t i = 0; !changed && i < fs.rules.size(); i++) changed = !foliage::SameRule(fs.rules[i], fsBefore.rules[i]);
            if (changed) foliage::Invalidate(tBasic);
        }

        DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
                 static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
        y += 12.0f;
    } else {
    // Tool selector: a full-width dropdown (Raise/Lower/Smooth/Flatten/Paint/
    // Erode/Noise/Ramp/None) instead of a 9-button icon grid, to save vertical
    // space now that this panel only gets half the column height.
    {
        constexpr const char* toolNames[9] = { "Raise", "Lower", "Smooth", "Flatten", "Paint", "Erode", "Noise", "Ramp", "None" };
        int activeTool = static_cast<int>(br.tool);
        Rectangle box = { x, y, innerW, 30.0f };
        bool hov = CheckCollisionPointRec(mouse, box);
        DrawDropdownBox(box, hov || g_terrainToolDropdownOpen);

        Rectangle iconRec = { box.x + 8.0f, box.y + 7.0f, 16.0f, 16.0f };
        DrawTerrainToolIcon(activeTool, iconRec, theme::TEXT);
        DrawTextArial(toolNames[activeTool], box.x + 32.0f, box.y + 8.0f, 13.0f, theme::TEXT);

        // Chevron
        float cx = box.x + box.width - 18.0f;
        float cy = box.y + box.height * 0.5f;
        Color chevCol = theme::TEXT;
        if (g_terrainToolDropdownOpen) {
            DrawLineEx({ cx - 5.0f, cy + 2.0f }, { cx, cy - 3.0f }, 2.0f, chevCol);
            DrawLineEx({ cx, cy - 3.0f }, { cx + 5.0f, cy + 2.0f }, 2.0f, chevCol);
        } else {
            DrawLineEx({ cx - 5.0f, cy - 2.0f }, { cx, cy + 3.0f }, 2.0f, chevCol);
            DrawLineEx({ cx, cy + 3.0f }, { cx + 5.0f, cy - 2.0f }, 2.0f, chevCol);
        }

        if (hov) MarkHand();
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !g_clickConsumedThisFrame) {
            g_terrainToolDropdownOpen = !g_terrainToolDropdownOpen;
            g_clickConsumedThisFrame = true;
        }
        // Anchor is captured in screen space (post-scroll), matching where
        // the closed box is actually drawn this frame; the expanded list is
        // rendered from this rect after the scissor region ends (see
        // finishPanel above).
        g_terrainToolDropdownAnchor = box;

        y += box.height + 10.0f;
    }

    // Brush shape: Circle | Square
    DrawTextArial("Shape", x, y + 7.0f, 12.0f, theme::TEXT_MUTED);
    for (int i = 0; i < 2; i++) {
        Rectangle sr = { x + 86.0f + i * 78.0f, y, 70.0f, 26.0f };
        bool sel = (static_cast<int>(br.shape) == i);
        bool hov = CheckCollisionPointRec(mouse, sr);
        float st2 = HoverProgress(static_cast<uint64_t>(9400u + i), hov);
        Color bg = sel ? theme::ACCENT_SOFT : theme::BG_WIDGET;
        bg = Mix(bg, theme::BG_WIDGET_HOVER, st2 * (sel ? 0.3f : 1.0f));
        DrawRectangleRounded(sr, 0.25f, 4, bg);
        DrawRectangleLinesEx(sr, sel ? 2.0f : 1.0f, sel ? theme::ACCENT : theme::BORDER);
        const char* nm = (i == 0) ? "Circle" : "Square";
        float nw = MeasureTextArial(nm, 12.0f);
        DrawTextArial(nm, sr.x + (sr.width - nw) * 0.5f, sr.y + 6.0f, 12.0f,
                      sel ? theme::TEXT : theme::TEXT_MUTED);
        if (hov) MarkHand();
        if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            br.shape = static_cast<BasicTerrain::Shape>(i);
        }
    }
    y += 34.0f;

    // Brush sliders
    TerrainSlider("Size",     x, y, innerW, br.radius,       4.0f, 200.0f, "%.0f m", 9201u); y += 32.0f;
    TerrainSlider("Strength", x, y, innerW, br.strength,     2.0f, 200.0f, "%.0f",   9202u); y += 32.0f;
    TerrainSlider("Hardness", x, y, innerW, br.hardness,     0.0f, 1.0f,   "%.2f",   9203u); y += 32.0f;

    // Target height only matters when flattening (row always reserved)
    terrain::Terrain* activeChunked = tChunked;
    BasicTerrain* activeBasic = tBasic;
    if ((activeBasic && br.tool == BasicTerrain::Tool::Flatten) ||
        (activeChunked && br.tool == BasicTerrain::Tool::Flatten)) {
        float minH = activeBasic ? activeBasic->GetMinHeight() : activeChunked->minHeight;
        float maxH = activeBasic ? activeBasic->GetMaxHeight() : activeChunked->maxHeight;
        TerrainSlider("Target H", x, y, innerW, br.targetHeight, minH, maxH, "%.0f m", 9204u);
    }

    // Noise tool settings (seed + feature size) shown only while Noise is active
    if (br.tool == BasicTerrain::Tool::Noise) {
        float seedF = (float)br.noiseSeed;
        TerrainSlider("Seed", x, y, innerW, seedF, 0.0f, 999999.0f, "%.0f", 9205u); y += 30.0f;
        br.noiseSeed = (int)seedF;
        TerrainSlider("Feature", x, y, innerW, br.noiseScale, 4.0f, 200.0f, "%.0f m", 9206u); y += 30.0f;
    }
    y += 32.0f;

    // Shortcut hint
    if (br.tool == BasicTerrain::Tool::Paint) {
        DrawTextArial("Click: stamp   Drag: paint   [ ] size   Ctrl erase", x, y + 2.0f, 10.0f, theme::TEXT_DIM);
    } else if (br.tool == BasicTerrain::Tool::Ramp) {
        DrawTextArial("Drag: grade start -> cursor   [ ] width", x, y + 2.0f, 10.0f, theme::TEXT_DIM);
    } else if (br.tool == BasicTerrain::Tool::Erode) {
        DrawTextArial("Click/Drag: erode soil   [ ] size", x, y + 2.0f, 10.0f, theme::TEXT_DIM);
    } else if (br.tool == BasicTerrain::Tool::Noise) {
        DrawTextArial("Click/Drag: add noise   [ ] size", x, y + 2.0f, 10.0f, theme::TEXT_DIM);
    } else {
        DrawTextArial("Click: stamp   Drag: paint   [ ] size   Ctrl invert", x, y + 2.0f, 10.0f, theme::TEXT_DIM);
    }
    y += 24.0f;

    // ================= PAINT LAYERS (when Paint tool active) =================
    if (br.tool == BasicTerrain::Tool::Paint) {
        DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
                 static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
        y += 12.0f;

        DrawTextArial("LAYERS", x, y, 11.0f, theme::TEXT_DIM);
        DrawLine(static_cast<int>(x + 52.0f), static_cast<int>(y + 7.0f),
                 static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
        y += 18.0f;

        int layerCount = 0;
        Texture2D* layerTexs = nullptr;
        const std::string* layerNames = nullptr;

        if (activeBasic) {
            layerCount = BasicTerrain::GetLayerCount();
            layerTexs = BasicTerrain::GetLayerTextures();
            layerNames = BasicTerrain::GetLayerNames();
        } else if (activeChunked) {
            layerCount = (int)activeChunked->layers.size();
            // Chunked terrain layers don't have static texture access - skip thumbnails for now
            layerTexs = nullptr;
            layerNames = nullptr;
            // Could allocate a temporary array from layer names
        }

        if (layerCount == 0) {
            DrawTextArial("No texture layers.", x, y, 11.0f, theme::TEXT_MUTED);
            y += 18.0f;
        } else {
            float thumbSize = 40.0f;
            float btnW = (innerW - 5.0f * 3.0f) / 4.0f;
            for (int i = 0; i < 4; i++) {
                Rectangle lr = { x + i * (btnW + 5.0f), y, btnW, thumbSize + 20.0f };
                bool active = (br.paintLayer == i);
                bool available = (i < layerCount);
                bool hov = CheckCollisionPointRec(mouse, lr) && available;
                float ht2 = HoverProgress(static_cast<uint64_t>(9500u + i), hov);

                Color bg = active ? theme::ACCENT_SOFT : theme::BG_WIDGET;
                bg = Mix(bg, theme::BG_WIDGET_HOVER, ht2 * (active ? 0.3f : 1.0f));
                DrawRectangleRounded(lr, 0.12f, 4, bg);
                DrawRectangleLinesEx(lr, active ? 2.0f : 1.0f, active ? theme::ACCENT : theme::BORDER);

                if (available && layerTexs && layerTexs[i].id > 0) {
                    Rectangle src = { 0, 0, (float)layerTexs[i].width, (float)layerTexs[i].height };
                    Rectangle dst = { lr.x + 2.0f, lr.y + 2.0f, thumbSize - 4.0f, thumbSize - 4.0f };
                    DrawTexturePro(layerTexs[i], src, dst, {0,0}, 0.0f, WHITE);
                } else if (available) {
                    DrawRectangleRec({lr.x + 2.0f, lr.y + 2.0f, thumbSize - 4.0f, thumbSize - 4.0f},
                        Fade(theme::ACCENT, 0.3f));
                } else {
                    DrawRectangleRec({lr.x + 2.0f, lr.y + 2.0f, thumbSize - 4.0f, thumbSize - 4.0f},
                        theme::BG_INPUT);
                }

                const char* label = "Layer";
                if (layerNames && available) label = layerNames[i].c_str();
                else if (available) label = "Layer";
                float lw = MeasureTextArial(label, 9.0f);
                DrawTextArial(label, lr.x + (lr.width - lw) * 0.5f, lr.y + thumbSize - 2.0f, 9.0f,
                    available ? (active ? theme::TEXT : theme::TEXT_MUTED) : theme::TEXT_DIM);

                if (hov) MarkHand();
                if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    br.paintLayer = i;
                }
            }
            y += thumbSize + 22.0f;

            // Erase toggle
            Rectangle eraserRect = { x, y, innerW, 26.0f };
            bool eHov = CheckCollisionPointRec(mouse, eraserRect);
            float et = HoverProgress(9510u, eHov);
            Color eBg = br.paintErase ? theme::ACCENT_SOFT : theme::BG_WIDGET;
            eBg = Mix(eBg, theme::BG_WIDGET_HOVER, et * (br.paintErase ? 0.3f : 1.0f));
            DrawRectangleRounded(eraserRect, 0.18f, 4, eBg);
            DrawRectangleLinesEx(eraserRect, br.paintErase ? 2.0f : 1.0f,
                br.paintErase ? theme::ACCENT : theme::BORDER);
            const char* eLabel = br.paintErase ? "Erase Mode: ON" : "Erase Mode: OFF";
            float ew = MeasureTextArial(eLabel, 12.0f);
            DrawTextArial(eLabel, eraserRect.x + (eraserRect.width - ew) * 0.5f, eraserRect.y + 6.0f, 12.0f,
                br.paintErase ? theme::ACCENT : theme::TEXT_MUTED);
            if (eHov) MarkHand();
            if (eHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                br.paintErase = !br.paintErase;
            }
            y += 32.0f;
        }
    }

    // Divider between halves
    DrawLine(static_cast<int>(panelRec.x + pad), static_cast<int>(y),
             static_cast<int>(panelRec.x + panelRec.width - pad), static_cast<int>(y), theme::DIVIDER);
    y += 12.0f;
    } // end Sculpt tab (g_terrainTab == 0)

    // ================= BOTTOM HALF: PROPERTIES =================
    DrawTextArial("PROPERTIES", x, y, 11.0f, theme::TEXT_DIM);
    DrawLine(static_cast<int>(x + 74.0f), static_cast<int>(y + 7.0f),
             static_cast<int>(x + innerW), static_cast<int>(y + 7.0f), theme::DIVIDER);
    y += 20.0f;

    // Determine which terrain to show properties for
    BasicTerrain* propBasic = tBasic;
    terrain::Terrain* propChunked = tChunked;
    bool hasTerrain = (propBasic != nullptr || propChunked != nullptr);

    if (!hasTerrain) {
        DrawTextArial("No terrain selected.", x, y + 14.0f, 13.0f, theme::TEXT_MUTED);
        DrawTextArial("Pick tools above, then click any terrain to edit.",
                      x, y + 36.0f, 11.0f, theme::TEXT_DIM);
        finishPanel();
        return;
    }

    auto infoRow = [&](const char* k, char* vbuf, size_t vsz) {
        DrawTextArial(k, x, y, 12.0f, theme::TEXT_MUTED);
        float vw = MeasureTextArial(vbuf, 12.0f);
        DrawTextArial(vbuf, x + innerW - vw, y, 12.0f, theme::TEXT);
        (void)vsz;
        y += 21.0f;
    };

    char buf[96];
    if (propBasic) {
        snprintf(buf, sizeof(buf), "%d x %d", propBasic->GetWidth(), propBasic->GetDepth());
        infoRow("Grid", buf, sizeof(buf));

        snprintf(buf, sizeof(buf), "%.0f x %.0f m", propBasic->GetWidth() * propBasic->GetScale(), propBasic->GetDepth() * propBasic->GetScale());
        infoRow("World Size", buf, sizeof(buf));

        snprintf(buf, sizeof(buf), "%.0f, 0, %.0f", propBasic->position.x, propBasic->position.z);
        infoRow("Position", buf, sizeof(buf));

        snprintf(buf, sizeof(buf), "%.0f m .. %.0f m", propBasic->GetMinHeight(), propBasic->GetMaxHeight());
        infoRow("Height Range", buf, sizeof(buf));
        y += 4.0f;

        // Wireframe toggle
        if (TerrainCheckbox(x, y, "Show Wireframe", propBasic->showWireframe, 9301u)) {
            propBasic->showWireframe = !propBasic->showWireframe;
        }
        y += 28.0f;

        // Reset flat button
        Rectangle resetBtn = { x, y, 110.0f, 26.0f };
        bool rHov = CheckCollisionPointRec(mouse, resetBtn);
        float rt = HoverProgress(9302u, rHov);
        DrawRectangleRounded(resetBtn, 0.18f, 4, Mix(theme::BG_WIDGET, theme::BG_WIDGET_PRESSED, rt));
        DrawRectangleLinesEx(resetBtn, rHov ? 2.0f : 1.0f, rHov ? theme::DANGER : theme::BORDER);
        float rbw = MeasureTextArial("Reset Flat", 12.0f);
        DrawTextArial("Reset Flat", resetBtn.x + (resetBtn.width - rbw) * 0.5f, resetBtn.y + 6.0f, 12.0f,
                      rHov ? theme::DANGER_HOVER : theme::TEXT_MUTED);
        if (rHov) MarkHand();
        if (rHov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
            propBasic->GenerateFlat(0.0f);
        }

        // Hint about selection state
        DrawTextArial(propBasic->editActive ? "Editing: LMB drag sculpt" : "Click terrain to edit",
                      x + 126.0f, y + 6.0f, 11.0f,
                      propBasic->editActive ? theme::SUCCESS : theme::TEXT_DIM);
    } else if (propChunked) {
        snprintf(buf, sizeof(buf), "%d x %d chunks", propChunked->gridWidth, propChunked->gridDepth);
        infoRow("Grid", buf, sizeof(buf));

        snprintf(buf, sizeof(buf), "%.0f x %.0f m", propChunked->size.x, propChunked->size.z);
        infoRow("World Size", buf, sizeof(buf));

        snprintf(buf, sizeof(buf), "%.0f, 0, %.0f", propChunked->center.x, propChunked->center.z);
        infoRow("Position", buf, sizeof(buf));

        snprintf(buf, sizeof(buf), "%.0f m .. %.0f m", propChunked->minHeight, propChunked->maxHeight);
        infoRow("Height Range", buf, sizeof(buf));
        y += 4.0f;

        // Wireframe toggle
        if (TerrainCheckbox(x, y, "Show Wireframe", propChunked->showWireframe, 9301u)) {
            propChunked->showWireframe = !propChunked->showWireframe;
        }
        y += 28.0f;

        // Chunk bounds toggle
        if (TerrainCheckbox(x, y, "Show Chunk Bounds", propChunked->showChunkBounds, 9302u)) {
            propChunked->showChunkBounds = !propChunked->showChunkBounds;
        }
        y += 28.0f;

        // LOD colors toggle
        if (TerrainCheckbox(x, y, "Show LOD Colors", propChunked->showLODColors, 9303u)) {
            propChunked->showLODColors = !propChunked->showLODColors;
        }
        y += 28.0f;

        // Hint about selection state
        DrawTextArial("Editing: LMB drag sculpt",
                      x + 126.0f, y + 6.0f, 11.0f,
                      theme::SUCCESS);
    }

    finishPanel();
}

} // namespace ui
