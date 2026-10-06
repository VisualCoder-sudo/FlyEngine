#include "../../../include/Engine/Backend/Benchmark.hpp"
#include "../../../include/Engine/Frontend/ProjectManager.hpp"
#include "../../../include/Engine/Platform/Platform.hpp"
#include "raylib.h"
#include "raymath.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace benchmark {
namespace {

constexpr int kWinWidth  = 640;
constexpr int kWinHeight = 360;

namespace theme {
    constexpr Color BG            = Color{ 24, 26, 32, 255 };
    constexpr Color PANEL         = Color{ 30, 32, 38, 255 };
    constexpr Color BORDER        = Color{ 72, 77, 88, 255 };
    constexpr Color BORDER_STRONG = Color{ 104, 110, 122, 255 };
    constexpr Color ACCENT        = Color{ 0, 190, 200, 255 };
    constexpr Color ACCENT_HOVER  = Color{ 40, 210, 220, 255 };
    constexpr Color ACCENT_ACTIVE = Color{ 0, 150, 160, 255 };
    constexpr Color BUTTON_BG     = Color{ 48, 51, 60, 255 };
    constexpr Color BUTTON_HOVER  = Color{ 63, 67, 78, 255 };
    constexpr Color BUTTON_ACTIVE = Color{ 40, 43, 51, 255 };
    constexpr Color TEXT          = Color{ 232, 232, 238, 255 };
    constexpr Color TEXT_MUTED    = Color{ 156, 161, 172, 255 };
    constexpr Color TEXT_DIM      = Color{ 122, 128, 140, 255 };
    constexpr Color WARNING       = Color{ 235, 175, 45, 255 };
    constexpr Color DANGER        = Color{ 216, 84, 84, 255 };
}

Font g_font = { 0 };
bool g_customFont = false;

void LoadBenchmarkFont() {
    g_font = GetFontDefault();
    g_customFont = false;

    // ResolveFontPath() returns the first font that exists, already preferring
    // one in the working directory over a system install. Previously this
    // probed a hand-built list that had drifted from the one the editor uses.
    const std::string& fontPath = platform::ResolveFontPath();
    if (fontPath.empty()) return;

    g_font = LoadFontEx(fontPath.c_str(), 32, nullptr, 0);
    if (g_font.baseSize > 0) {
        SetTextureFilter(g_font.texture, TEXTURE_FILTER_BILINEAR);
        g_customFont = true;
    }
}

void UnloadBenchmarkFont() {
    if (g_customFont) {
        UnloadFont(g_font);
        g_customFont = false;
    }
}

void DrawTextCentered(const char* text, float centerX, float y, float size, Color color) {
    Vector2 textSize = MeasureTextEx(g_font, text, size, 1.0f);
    DrawTextEx(g_font, text, Vector2{ centerX - (textSize.x * 0.5f), y }, size, 1.0f, color);
}

void DrawTextLeft(const char* text, float x, float y, float size, Color color) {
    DrawTextEx(g_font, text, Vector2{ x, y }, size, 1.0f, color);
}

bool DrawButton(Rectangle rec, const char* text, bool isPrimary) {
    Vector2 mouse = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mouse, rec);
    bool pressed = hovered && IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    bool clicked = hovered && IsMouseButtonReleased(MOUSE_BUTTON_LEFT);

    Color bg;
    if (isPrimary) {
        bg = pressed ? theme::ACCENT_ACTIVE : (hovered ? theme::ACCENT_HOVER : theme::ACCENT);
    } else {
        bg = pressed ? theme::BUTTON_ACTIVE : (hovered ? theme::BUTTON_HOVER : theme::BUTTON_BG);
    }

    DrawRectangleRounded(rec, 0.18f, 4, bg);
    DrawRectangleLinesEx(rec, 1.0f, hovered ? theme::BORDER_STRONG : theme::BORDER);

    Color textColor = isPrimary ? Color{ 15, 17, 21, 255 } : theme::TEXT;
    Vector2 textSize = MeasureTextEx(g_font, text, 15.0f, 1.0f);
    DrawTextEx(g_font, text,
               Vector2{ rec.x + (rec.width - textSize.x) * 0.5f, rec.y + (rec.height - textSize.y) * 0.5f },
               15.0f, 1.0f, textColor);

    if (hovered) {
        SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    }

    return clicked;
}

} // namespace

bool RunStartupBenchmark() {
    InitWindow(kWinWidth, kWinHeight, "Flyengine / Inital Benchmark");
    // Unlock FPS so we can test the maximum render capability of the GPU/CPU.
    SetTargetFPS(0);
    SetExitKey(KEY_NULL);

    project::ApplyWindowIcon();
    LoadBenchmarkFont();

    Camera3D camera{};
    camera.position   = Vector3{ 10.0f, 10.0f, 10.0f };
    camera.target     = Vector3{ 0.0f, 0.0f, 0.0f };
    camera.up         = Vector3{ 0.0f, 1.0f, 0.0f };
    camera.fovy       = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    constexpr float kWarmupDuration = 0.15f;
    constexpr float kTestDuration   = 1.20f;
    constexpr float kTotalDuration  = kWarmupDuration + kTestDuration;
    constexpr int kMinFpsRequired = 60;

    double startTime = GetTime();
    int sampleFrames = 0;
    double sampleStartTime = 0.0;
    bool warmupFinished = false;
    float avgFps = 0.0f;
    bool benchmarkFinished = false;

    // --- Phase 1: Benchmark Execution ---
    while (!WindowShouldClose() && !benchmarkFinished) {
        double now = GetTime();
        double elapsed = now - startTime;
        float dt = GetFrameTime();

        if (!warmupFinished && elapsed >= kWarmupDuration) {
            warmupFinished = true;
            sampleStartTime = now;
            sampleFrames = 0;
        }

        if (warmupFinished) {
            sampleFrames++;
            double testElapsed = now - sampleStartTime;
            if (testElapsed >= kTestDuration) {
                avgFps = (testElapsed > 0.0) ? static_cast<float>(sampleFrames / testElapsed) : 0.0f;
                benchmarkFinished = true;
            }
        }

        float progress = static_cast<float>(elapsed / kTotalDuration);
        if (progress > 1.0f) progress = 1.0f;

        float currentFps = (dt > 0.00001f) ? (1.0f / dt) : 0.0f;

        // Camera orbit for 3D load
        float orbitAngle = static_cast<float>(now) * 1.5f;
        camera.position.x = sinf(orbitAngle) * 12.0f;
        camera.position.z = cosf(orbitAngle) * 12.0f;
        camera.position.y = 8.0f + sinf(orbitAngle * 0.5f) * 2.0f;

        BeginDrawing();
            ClearBackground(theme::BG);

            // 3D rendering workload
            BeginMode3D(camera);
                DrawGrid(20, 1.0f);
                for (int x = -3; x <= 3; ++x) {
                    for (int z = -3; z <= 3; ++z) {
                        float dist = sqrtf(static_cast<float>(x * x + z * z));
                        float height = sinf(static_cast<float>(now * 4.0f) + dist) * 1.5f + 1.5f;
                        Vector3 pos{ static_cast<float>(x * 2), height * 0.5f, static_cast<float>(z * 2) };
                        Color cubeCol = ColorFromHSV(fmodf((dist * 30.0f + static_cast<float>(now) * 60.0f), 360.0f), 0.7f, 0.9f);
                        DrawCube(pos, 1.2f, height, 1.2f, cubeCol);
                        DrawCubeWires(pos, 1.2f, height, 1.2f, Fade(BLACK, 0.4f));
                    }
                }
                DrawSphere(Vector3{ 0.0f, 3.5f, 0.0f }, 1.5f, theme::ACCENT);
                DrawSphereWires(Vector3{ 0.0f, 3.5f, 0.0f }, 1.55f, 16, 16, Fade(WHITE, 0.5f));
            EndMode3D();

            // 2D Overlay
            DrawRectangle(0, 0, kWinWidth, 60, Fade(theme::PANEL, 0.88f));
            DrawLine(0, 60, kWinWidth, 60, theme::BORDER);
            DrawTextLeft("Flyengine Hardware Benchmark", 20, 14, 18, theme::TEXT);
            DrawTextLeft("Testing graphics rendering and frame rate stability...", 20, 36, 12, theme::TEXT_MUTED);

            // Bottom status bar & progress
            DrawRectangle(0, kWinHeight - 80, kWinWidth, 80, Fade(theme::PANEL, 0.92f));
            DrawLine(0, kWinHeight - 80, kWinWidth, kWinHeight - 80, theme::BORDER);

            // Progress bar
            Rectangle barBg{ 20, static_cast<float>(kWinHeight - 60), static_cast<float>(kWinWidth - 40), 12 };
            DrawRectangleRounded(barBg, 0.5f, 4, theme::BUTTON_BG);
            if (progress > 0.01f) {
                Rectangle barFill{ barBg.x, barBg.y, barBg.width * progress, barBg.height };
                DrawRectangleRounded(barFill, 0.5f, 4, theme::ACCENT);
            }
            DrawRectangleLinesEx(barBg, 1.0f, theme::BORDER);

            std::string fpsText = "Current: " + std::to_string(static_cast<int>(currentFps + 0.5f)) + " FPS";
            DrawTextLeft(fpsText.c_str(), 20, kWinHeight - 38, 13, theme::TEXT_MUTED);

            std::string progText = std::to_string(static_cast<int>(progress * 100.0f)) + "%";
            Vector2 progSize = MeasureTextEx(g_font, progText.c_str(), 13, 1.0f);
            DrawTextLeft(progText.c_str(), kWinWidth - 20 - progSize.x, kWinHeight - 38, 13, theme::TEXT_MUTED);

        EndDrawing();
    }

    // If user closed the window during benchmark, abort startup
    if (WindowShouldClose() && !benchmarkFinished) {
        UnloadBenchmarkFont();
        CloseWindow();
        return false;
    }

    // If the computer achieved at least 60 FPS, pass immediately
    if (avgFps >= kMinFpsRequired) {
        UnloadBenchmarkFont();
        CloseWindow();
        return true;
    }

    // --- Phase 2: Warning Dialog Window ---
    // If benchmark failed (< 60 FPS), show the warning window
    SetTargetFPS(60);
    SetWindowTitle("Flyengine - Hardware Warning");

    bool userChoiceMade = false;
    bool proceed = false;

    while (!userChoiceMade && !WindowShouldClose()) {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);

        // Keyboard shortcuts
        if (IsKeyPressed(KEY_Y) || IsKeyPressed(KEY_ENTER)) {
            proceed = true;
            userChoiceMade = true;
        } else if (IsKeyPressed(KEY_N) || IsKeyPressed(KEY_ESCAPE)) {
            proceed = false;
            userChoiceMade = true;
        }

        BeginDrawing();
            ClearBackground(theme::BG);

            // Dialog Panel Box
            Rectangle panelRec{ 40, 30, static_cast<float>(kWinWidth - 80), static_cast<float>(kWinHeight - 60) };
            DrawRectangleRounded(panelRec, 0.08f, 6, theme::PANEL);
            DrawRectangleLinesEx(panelRec, 1.0f, theme::BORDER_STRONG);

            // Title Banner
            DrawRectangleRounded(Rectangle{ panelRec.x, panelRec.y, panelRec.width, 42 }, 0.08f, 6, Color{ 38, 41, 48, 255 });
            DrawLine(static_cast<int>(panelRec.x), static_cast<int>(panelRec.y + 42),
                     static_cast<int>(panelRec.x + panelRec.width), static_cast<int>(panelRec.y + 42), theme::BORDER);

            DrawTextLeft("Performance Warning", panelRec.x + 20, panelRec.y + 12, 18, theme::WARNING);

            // Warning Prompt (Exact text required)
            const char* line1 = "Your computer failed to meet the benchmark,";
            const char* line2 = "This may cause Flyengine to be unstable,";
            const char* line3 = "Are you sure you want to proceed?";

            float textStartY = panelRec.y + 64.0f;
            DrawTextCentered(line1, kWinWidth * 0.5f, textStartY, 16.0f, theme::TEXT);
            DrawTextCentered(line2, kWinWidth * 0.5f, textStartY + 26.0f, 16.0f, theme::TEXT);
            DrawTextCentered(line3, kWinWidth * 0.5f, textStartY + 52.0f, 16.0f, theme::WARNING);

            // Benchmark Stats
            std::string statsStr = "Measured Benchmark: " + std::to_string(static_cast<int>(avgFps + 0.5f)) +
                                  " FPS  |  Target Requirement: 60+ FPS";
            DrawTextCentered(statsStr.c_str(), kWinWidth * 0.5f, textStartY + 96.0f, 13.0f, theme::TEXT_DIM);

            // Buttons: Yes and No
            constexpr float btnWidth  = 130.0f;
            constexpr float btnHeight = 36.0f;
            float btnY = panelRec.y + panelRec.height - 56.0f;

            Rectangle noBtnRec { (kWinWidth * 0.5f) - btnWidth - 16.0f, btnY, btnWidth, btnHeight };
            Rectangle yesBtnRec{ (kWinWidth * 0.5f) + 16.0f, btnY, btnWidth, btnHeight };

            if (DrawButton(noBtnRec, "No", false)) {
                proceed = false;
                userChoiceMade = true;
            }

            if (DrawButton(yesBtnRec, "Yes", true)) {
                proceed = true;
                userChoiceMade = true;
            }

        EndDrawing();
    }

    UnloadBenchmarkFont();
    CloseWindow();

    return proceed;
}

} // namespace benchmark
