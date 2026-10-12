// Windowed smoke test for the sokol-backed raylib layer (needs a display and a
// working GPU/driver, so it is not registered with ctest; run it by hand:
//   build/rl_smoke_test
// Verifies behaviour that is easy to break and only visible at runtime:
//   * UpdateTexture updates in place, and a second update in the same frame is
//     applied on the next frame instead of being dropped;
//   * heavy mesh churn across the buffer pool's age limit stays correct.
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { ++g_failures; std::printf("FAIL: "); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static Color CenterPixel() {
    const char* path = "rl_smoke_shot.png";
    TakeScreenshot(path);
    Image img = LoadImage(path);
    Color c = GetImageColor(img, img.width / 2, img.height / 2);
    UnloadImage(img);
    std::remove(path);
    return c;
}

static void DrawFullscreen(Texture2D tex) {
    BeginDrawing();
    ClearBackground(BLACK);
    DrawTexturePro(tex, { 0, 0, (float)tex.width, (float)tex.height },
                   { 0, 0, (float)GetScreenWidth(), (float)GetScreenHeight() }, { 0, 0 }, 0.0f, WHITE);
    EndDrawing();
}

static const Color kRed = { 255, 0, 0, 255 }, kBlue = { 0, 0, 255, 255 }, kYellow = { 255, 255, 0, 255 };

static bool Near(Color a, Color b) {
    return abs(a.r - b.r) < 4 && abs(a.g - b.g) < 4 && abs(a.b - b.b) < 4;
}

int main() {
    InitWindow(256, 256, "rl_smoke_test");
    SetTargetFPS(0);

    // --- texture updates -------------------------------------------------
    std::vector<Color> px(16 * 16, Color{ 255, 0, 0, 255 });
    Image img = { px.data(), 16, 16, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 };
    Texture2D tex = LoadTextureFromImage(img);

    DrawFullscreen(tex);
    CHECK(Near(CenterPixel(), kRed), "initial texture should be red");

    std::vector<Color> blue(16 * 16, Color{ 0, 0, 255, 255 });
    UpdateTexture(tex, blue.data());                 // first update: re-creates as updatable
    DrawFullscreen(tex);
    CHECK(Near(CenterPixel(), kBlue), "first update should show blue");

    std::vector<Color> green(16 * 16, Color{ 0, 255, 0, 255 });
    std::vector<Color> yellow(16 * 16, Color{ 255, 255, 0, 255 });
    UpdateTexture(tex, green.data());                // frame N: applied immediately
    UpdateTexture(tex, yellow.data());               // frame N, second update: deferred, must not be lost
    DrawFullscreen(tex);
    DrawFullscreen(tex);                             // frame N+1 shows the deferred update
    CHECK(Near(CenterPixel(), kYellow), "second same-frame update must not be dropped");

    // --- mesh churn (buffer pool) ----------------------------------------
    Material mat = LoadMaterialDefault();
    int drawn = 0;
    for (int frame = 0; frame < 900; ++frame) {
        BeginDrawing();
        ClearBackground(BLACK);
        Camera3D cam = { { 0, 3, 6 }, { 0, 0, 0 }, { 0, 1, 0 }, 45.0f, CAMERA_PERSPECTIVE };
        BeginMode3D(cam);
        for (int i = 0; i < 8; ++i) {
            const int res = 2 + (frame * 7 + i * 13) % 40;     // sizes keep changing
            Mesh m = GenMeshPlane(4.0f, 4.0f, res, res);
            DrawMesh(m, mat, MatrixIdentity());
            UnloadMesh(m);
            ++drawn;
        }
        EndMode3D();
        EndDrawing();
    }
    CHECK(drawn == 900 * 8, "mesh churn loop did not complete");

    // --- instanced drawing -----------------------------------------------
    {
        Shader sh = LoadShaderProgram("lit_instanced");
        Material im = LoadMaterialDefault();
        im.shader = sh;
        im.maps[MATERIAL_MAP_DIFFUSE].color = Color{ 255, 0, 0, 255 };
        // Full ambient light from every side, a sun straight down with no shadow cascades (w = 0).
        const float one[4] = { 1, 1, 1, 1 }, down[4] = { 0, -1, 0, 0 };
        const float deep = -1000.0f;
        SetShaderValue(sh, GetShaderLocation(sh, "ambientSky"), one, SHADER_UNIFORM_VEC4);
        SetShaderValue(sh, GetShaderLocation(sh, "ambientGround"), one, SHADER_UNIFORM_VEC4);
        SetShaderValue(sh, GetShaderLocation(sh, "sunDir"), down, SHADER_UNIFORM_VEC4);
        SetShaderValue(sh, GetShaderLocation(sh, "waterSurfaceY"), &deep, SHADER_UNIFORM_FLOAT);
        Mesh cube = GenMeshCube(1.0f, 1.0f, 1.0f);
        Matrix xf[3] = {
            MatrixMultiply(MatrixScale(1, 1, 1), MatrixTranslate(-3, 0, 0)),
            MatrixMultiply(MatrixScale(2, 2, 2), MatrixTranslate(0, 0, 0)),
            MatrixMultiply(MatrixScale(1, 4, 1), MatrixTranslate(3, 0, 0)),   // tall, like a building
        };
        // The city packs a per-instance tint into the matrix's last row (m3, m7, m11).
        xf[2].m3 = 1.0f; xf[2].m7 = 1.0f; xf[2].m11 = 1.0f;
        // The persistent-buffer path (used by the city): column-major float16 per instance.
        std::vector<float16> packed(3);
        for (int i = 0; i < 3; ++i) packed[(size_t)i] = MatrixToFloatV(xf[i]);
        const unsigned int instBuf = rlLoadVertexBuffer(packed.data(), (int)(packed.size() * sizeof(float16)), false);
        CHECK(instBuf != 0, "rlLoadVertexBuffer failed");

        for (int mode = 0; mode < 2; ++mode) {
            const char* what = mode == 0 ? "DrawMeshInstanced" : "DrawMeshInstancedBuffer";
            BeginDrawing();
            ClearBackground(BLACK);
            Camera3D cam = { { 0, 0, 10 }, { 0, 0, 0 }, { 0, 1, 0 }, 45.0f, CAMERA_PERSPECTIVE };
            BeginMode3D(cam);
            if (mode == 0) DrawMeshInstanced(cube, im, xf, 3);
            else DrawMeshInstancedBuffer(cube, im, instBuf, 3);
            EndMode3D();
            EndDrawing();
            const char* path = "rl_smoke_inst.png";
            TakeScreenshot(path);
            Image shot = LoadImage(path);
            std::remove(path);
            auto px = [&](int x, int y) { return GetImageColor(shot, x, y); };
            CHECK(px(128, 128).r > 150 && px(128, 128).g < 40, "%s: centre cube missing", what);
            CHECK(px(220, 128).r > 150 && px(220, 128).g < 40, "%s: right cube missing", what);
            CHECK(px(220, 80).r > 150 && px(220, 80).g < 40, "%s: right cube lost its height scale", what);
            CHECK(px(36, 128).r > 150 && px(36, 128).g < 40, "%s: left cube missing", what);
            CHECK(px(128, 244).r < 20 && px(128, 12).r < 20, "%s: geometry smeared beyond its cubes", what);
            CHECK(px(128, 60).r < 20, "%s: geometry smeared above the centre cube", what);
            UnloadImage(shot);
        }
        rlUnloadVertexBuffer(instBuf);
    }

    // --- recycled buffers must never show another mesh's data -------------
    // A buffer released in a frame may still be referenced by a draw recorded earlier
    // in that frame, so it must not be refilled until the next frame. Each mesh below
    // is a full-screen quad of one colour; every draw must show its own colour.
    {
        auto makeQuad = [](Color c) {
            Mesh m = { 0 };
            m.vertexCount = 4;
            m.triangleCount = 2;
            m.vertices = (float*)MemAlloc(4 * 3 * sizeof(float));
            m.texcoords = (float*)MemAlloc(4 * 2 * sizeof(float));
            m.normals = (float*)MemAlloc(4 * 3 * sizeof(float));
            m.colors = (unsigned char*)MemAlloc(4 * 4);
            m.indices = (unsigned short*)MemAlloc(6 * sizeof(unsigned short));
            const float v[12] = { -1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0 };
            const float t[8] = { 0, 0, 1, 0, 1, 1, 0, 1 };
            const float n[12] = { 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1 };
            const unsigned short idx[6] = { 0, 1, 2, 0, 2, 3 };
            for (int i = 0; i < 12; ++i) { m.vertices[i] = v[i]; m.normals[i] = n[i]; }
            for (int i = 0; i < 8; ++i) m.texcoords[i] = t[i];
            for (int i = 0; i < 4; ++i) { m.colors[i * 4] = c.r; m.colors[i * 4 + 1] = c.g; m.colors[i * 4 + 2] = c.b; m.colors[i * 4 + 3] = 255; }
            for (int i = 0; i < 6; ++i) m.indices[i] = idx[i];
            UploadMesh(&m, false);
            return m;
        };
        Material qm = LoadMaterialDefault();
        const Color palette[4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 0, 255 } };
        int wrong = 0;
        for (int round = 0; round < 40; ++round) {
            // Within one frame: draw a mesh, release it, create a same-sized mesh and draw that.
            // The first quad covers the left half of the screen, the second the right half.
            BeginDrawing();
            ClearBackground(BLACK);
            Camera3D cam = { { 0, 0, 3 }, { 0, 0, 0 }, { 0, 1, 0 }, 90.0f, CAMERA_PERSPECTIVE };
            BeginMode3D(cam);
            Mesh a = makeQuad(palette[round % 4]);
            DrawMesh(a, qm, MatrixTranslate(-1.2f, 0, 0));
            UnloadMesh(a);                                        // released while its draw is pending
            Mesh b = makeQuad(palette[(round + 1) % 4]);          // same size: would reuse a's buffers
            DrawMesh(b, qm, MatrixTranslate(1.2f, 0, 0));
            UnloadMesh(b);
            EndMode3D();
            EndDrawing();
            const char* path = "rl_smoke_recycle.png";
            TakeScreenshot(path);
            Image shot = LoadImage(path);
            std::remove(path);
            const Color left = GetImageColor(shot, 64, 128), right = GetImageColor(shot, 192, 128);
            const Color wantL = palette[round % 4], wantR = palette[(round + 1) % 4];
            if (!Near(left, wantL) || !Near(right, wantR)) ++wrong;
            UnloadImage(shot);
        }
        CHECK(wrong == 0, "recycled mesh buffers showed another mesh's data in %d of 40 frames", wrong);
    }

    // The scene after churn still renders correctly.
    DrawFullscreen(tex);
    CHECK(Near(CenterPixel(), kYellow), "texture still correct after mesh churn and instancing");

    // Model import: a glTF loaded from disk must produce meshes that draw.
    {
        const char* glb = FLY_SOURCE_DIR "/src/Engine/Backend/tinygltf/models/box01.glb";
        Model mdl = LoadModel(glb);
        CHECK(mdl.meshCount > 0, "LoadModel(box01.glb) produced no meshes");
        if (mdl.meshCount > 0) {
            BeginDrawing();
            ClearBackground(BLACK);
            Camera3D cam = { { 0, 0, 6 }, { 0, 0, 0 }, { 0, 1, 0 }, 45.0f, CAMERA_PERSPECTIVE };
            BeginMode3D(cam);
            DrawModel(mdl, Vector3{ 0, 0, 0 }, 1.0f, WHITE);
            EndMode3D();
            EndDrawing();
            const char* path = "rl_smoke_model.png";
            TakeScreenshot(path);
            Image shot = LoadImage(path);
            std::remove(path);
            int lit = 0;
            for (int y = 0; y < shot.height; y += 4)
                for (int x = 0; x < shot.width; x += 4) {
                    const Color c = GetImageColor(shot, x, y);
                    if (c.r + c.g + c.b > 30) ++lit;
                }
            CHECK(lit > 20, "imported model drew nothing (%d lit samples)", lit);
            UnloadImage(shot);
        }
        UnloadModel(mdl);
    }

    UnloadTexture(tex);
    CloseWindow();
    std::printf(g_failures ? "rl_smoke_test: %d FAILED\n" : "rl_smoke_test: all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
