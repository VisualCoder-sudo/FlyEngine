# Architecture

How Flyengine fits together. Read this first, then [CodebaseMap.md](CodebaseMap.md) to find files, then
[Subsystems.md](Subsystems.md) for per-feature detail.

## 1. What gets built

One `CMakeLists.txt` builds everything. The important targets:

| Target | Source | What it is |
| --- | --- | --- |
| `FlyEngineCore` | ~90 files listed in `CMakeLists.txt` | Static library with nearly all engine and editor code. Everything below links it. |
| `FlyRL` | `cmake/FlySokol.cmake`, `src/Engine/RL/` | The raylib-compatible rendering layer on sokol, plus the compiled shaders. |
| `FlyImGui` | `imgui/` | Dear ImGui core (UI is driven through sokol_imgui). |
| `FlyPluginCore` | `src/Plugin/*.c`, `extern/cjson` | Plugin manifest parsing, loader, IPC, version checks. Shared by the engine and the plugin tools. |
| `FlySqlite` | `extern/sqlite` | SQLite built as a static library. Nothing in `src/` or `include/` includes `sqlite3.h` today; check before removing. |
| `Flyengine` | `src/main.cpp` | The editor. |
| `FlyPlayer` | `src/PlayerMain.cpp` | The runtime. The editor launches it for Play. Exported games are a copy of it with project data appended. |
| `flyexport` | `src/ExportMain.cpp` | Command-line Export, same code as the editor's Export dialog. |
| `flyplugin`, `flyplugin-runner` | `src/Plugin/cli/`, `src/Plugin/PluginProcessRunner.c` | Plugin CLI and the plugin process runner. |
| `*_fallback` | same sources, other `FlyRL` | OpenGL (D3D11 on Windows) copies of the editor and player. The Vulkan executables hand over to these at startup when the GPU cannot run Vulkan 1.3 plus `VK_EXT_descriptor_buffer`. |

Third-party code is vendored in-tree: `sokol/`, `extern/box3d`, `extern/{cjson,miniz,nlohmann,sqlite,ufbx}`, `imgui/`,
`curl/`, `src/Engine/Backend/{cglm,vulkanh,stb}`, `src/Engine/RL/raylib`. See [Maintenance.md](Maintenance.md) for the
rules on touching it.

## 2. Layers

```
  Editor UI (ImGui)            src/Engine/Frontend/ui*.cpp, ObjectInteractionManager, ProjectManager
  ─────────────────────────────────────────────────────────────────────────────────────────────
  Engine + Entities            Engine.cpp, ScatteredObject, Terrain, BasicTerrain, WaterBody, City, Simulation ...
  Scripting (C++ / nodes)      Scripts/, VisualScript, FlyScriptApi, CPluginAPI
  Graphics                     src/Graphics/ (lighting, shadows, sky, clouds, post, particles, occlusion, VFS)
  ─────────────────────────────────────────────────────────────────────────────────────────────
  FlyRL (raylib API on sokol)  src/Engine/RL/        include/rl/raylib.h
  Platform shim                src/Engine/Platform/  include/Engine/Platform/Platform.hpp
  ─────────────────────────────────────────────────────────────────────────────────────────────
  sokol_app / sokol_gfx / sokol_imgui  (Vulkan | OpenGL 4.1 | D3D11), Box3D, miniaudio, cglm
```

Rules that keep the layers honest:

- **Platform-specific code goes through `Platform.hpp`.** `<windows.h>` style headers are included only inside
  `src/Engine/Platform/*.cpp` and behind `#if defined(_WIN32)`. CI's Linux job exists to catch violations.
- **The engine was written against raylib's API.** Instead of rewriting call sites, `src/Engine/RL` re-implements the
  raylib subset the engine uses. When you need a raylib function that does not exist, add it to `FlyRL`, not a new
  graphics call in engine code.
- **Shaders are compiled at build time**, for all three backends at once. There is no runtime shader compilation.

## 3. The Engine and Entity model

`include/Engine.hpp` / `src/Engine/Engine.cpp`. `Engine` owns a vector of `std::unique_ptr<Entity>`. Each frame it
calls `Update(dt)` on every entity, then draws: shadows and reflections first, the opaque 3D pass, the transparent
pass, `DrawOverlay3D`, then the 2D/ImGui pass.

`Entity` (`include/Engine/Backend/Entity.hpp`) is the base class for anything in the world:

- `Update`, `Draw`, `DrawOverlay3D`, `IsTransparent`
- `IsEditorOnly()` — return true to be skipped in the player (the editor's gizmo and camera entities do).
- `IsVisible(frustum)` / `GetCullBounds()` — frustum culling. Physics ignores the result.
- `alive = false` — the engine removes the entity at the end of the frame.

Things to know:

- `AddEntity` during an update is deferred (`pendingEntities`), so it is safe to add entities from inside `Update`.
- `Engine::MAX_FRAME_DT` (0.05 s) caps the delta so one stall cannot blow up physics or scripts.
- Pause (`SetPaused`) freezes entity updates but keeps rendering and input. `StepFrame()` queues single steps.
  `SetTimeScale` scales dt for every entry point.
- `isPlayerBuild` (`SetPlayerBuild`) is how editor-only code paths turn off in `FlyPlayer`.

Main entity types:

| Entity | Header | Role |
| --- | --- | --- |
| `ScatteredObject` | `Backend/ScatteredObject.hpp` | A scene object: cube/sphere/cylinder/wedge or an imported model. Transform, colour, `SurfaceMaterial`, collision settings, scripts, `ObjectNode` children (lights, decals, textures). |
| `ModelGroup` | same header | Editor grouping. Members stay in the flat object list (physics and scripts iterate that). The group adds selection, transform and persistence only. |
| `Terrain` | `Terrain/Terrain.hpp` | Chunked splatmap terrain with LOD geomorphing. Saved in `terrain.terrain`. |
| `BasicTerrain` | `Terrain/BasicTerrain.hpp` | The simpler heightmap terrain with brushes (raise, lower, smooth, flatten, paint, erode, noise, ramp). Saved as `.bt`. |
| `WaterBody` | `Terrain/Water/WaterBody.hpp` | Water surface with noise waves and ripples. |
| `City` | `CityGen/City.hpp` | Procedural city (roads, blocks, buildings, traffic). |
| `phys::Simulation` | `Backend/PhysicsSimulation.hpp` | Box3D world. Creates and syncs bodies for the object list. |
| `NativeScriptHost` | `Scripts/NativeScriptHost.hpp` | Loads and ticks script libraries and plugins. Owns `ScriptRuntime`. |
| `ObjectInteractionManager` | `Frontend/ObjectInteractionManager.hpp` | Editor selection, gizmos, picking, spawn, delete. Editor only. |
| `CameraController` | `Backend/CameraController.hpp` | Editor fly camera and top-down mode. |

## 4. Process flows

### Editor (`src/main.cpp`)

1. Benchmark and crash-reporter install, terminate handler.
2. Command-line modes (`--testwater`, `--testscene`, `--create-project`) bypass the UI.
3. Splash, then `project::ShowProjectManager()` (its own window) returns the project to open.
4. The editor creates the `Engine`, loads the scene, adds the entities above, and runs. The ImGui panels are drawn
   from `src/Engine/Frontend/ui*.cpp`.
5. **Play** snapshots the scene and world settings, saves a temporary `<Project>_temp.flyproj`, and starts the
   `FlyPlayer` beside the editor. **Stop** restores the snapshot. See `tests/play_snapshot_test.cpp`.

### Player (`src/PlayerMain.cpp`)

Parses arguments (`-play <project>`, `--fps`, `--res`, `--automation`, `--noscripts`, `--quality`, ...; run
`FlyPlayer --help`), loads the project through `ProjectManager.cpp`, builds the same world without the editor
entities, auto-starts "run on play" scripts, and runs a custom loop using `UpdateFrame` / `RenderFrame`.

An **exported game** is the same executable with a pack archive appended. At startup it detects the archive and
serves every file through the virtual file system (`Graphics/Vfs.cpp`). See [Subsystems.md](Subsystems.md#export).

### Scripts and Play

Scripts are `fly::Script` classes (C++20 coroutines) compiled from `<project>/Scripts/*.cpp` into one shared library
by `ScriptCompiler`. `NativeScriptHost` loads it, calls `FlyScript_GetModule`, resumes coroutines once per frame, and
rebuilds and reloads when a source file changes. The boundary is a plain C ABI, so the compiler used for scripts need
not match the one that built the engine. See [Subsystems.md](Subsystems.md#scripting).

## 5. Graphics pipeline in one paragraph

Scene lighting is computed in linear light into a float HDR target. Cascaded shadow maps (`Shadows.cpp`), planar
water reflections (`Reflections.cpp`), the physical sky and volumetric clouds (`Atmosphere.cpp`, `Clouds.cpp`), and
image-based lighting baked from the sky (`rl_ibl.cpp`) feed the lit, terrain and water shaders. `PostFX.cpp` then
runs a chain of full-screen passes (AO, bloom, fog, tone mapping, anti-aliasing) and draws the result into the
window. Per-object lights, decals and tiled textures come from `ObjectNode`s. Quality tiers live in `RenderQuality`
(`include/Graphics/PostFX.hpp`). Details of the material and IBL path are in [PBR.md](PBR.md).

## 6. Physics

Box3D (`extern/box3d`) is wrapped by `phys::Simulation` (`PhysicsSimulation.cpp`) and `PhysicsCollision.cpp`.
Objects get bodies on `StartPlay()`. Terrain collision is a Box3D height field built in `TerrainCollider.cpp`.
`CharacterController.cpp` drives the player's body. The editor can delete objects mid-play, so
`Simulation::RemoveObject` must be called **before** an object is freed or `bodyMap` keeps a dangling pointer.

## 7. Where state lives on disk

| What | Where | Format doc |
| --- | --- | --- |
| Project and scene | `<project>/<name>.flyproj` | [FileFormats.md](FileFormats.md) |
| Terrain | `terrain.terrain`, `*.bt`, `foliage.cfg` | [FileFormats.md](FileFormats.md) |
| City | `city.city` | [FileFormats.md](FileFormats.md) |
| Scripts | `<project>/Scripts/*.cpp`, build output in `Scripts/.build/` | [Subsystems.md](Subsystems.md#scripting) |
| Plugins | `<project>/plugins/nat/<name>/build/` | [Subsystems.md](Subsystems.md#plugins) |
| Export settings | `<project>/export.cfg` | [Subsystems.md](Subsystems.md#export) |
| User settings | `graphics.cfg` (quality), `loading.cfg` (loading screen, also per project) | `Platform::ConfigDir()` is the base on Linux (`$XDG_CONFIG_HOME` or `~/.config`) |
