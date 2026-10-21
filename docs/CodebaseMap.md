# Codebase map

Where things live. Headers are in `include/`, implementations in `src/`, mirroring each other:
`include/Engine/Backend/X.hpp` is usually implemented by `src/Engine/Backend/X.cpp` (a few are under a different
folder, for example `ScatteredObject.hpp` lives in `Backend/` and `ObjectInteractionManager.cpp` in `Frontend/`; use
`git ls-files | grep Name` when unsure).

## Top level

| Path | Purpose |
| --- | --- |
| `CMakeLists.txt`, `CMakePresets.json`, `cmake/` | Build. `cmake/FlySokol.cmake` defines FlyRL, the shader list and the sokol-shdc download. `cmake/GenShaderRegistry.cmake` generates the shader-uniform registry. |
| `src/` | First-party source. |
| `include/` | First-party headers. `include/rl/` is the raylib-compatible public API (`raylib.h`, `raymath.h`, `rlgl.h` subset). |
| `shaders/` | sokol-shdc GLSL. Only the `.glsl` files are built; `*.vert` and `*.frag` are legacy and unused. |
| `ScriptingSDK/` | What game scripts compile against: `include/fly.hpp` (C++ API), `include/FlyScriptABI.h` (C ABI), `src/fly_module.cpp`, `samples/`. |
| `assets/` | Editor icons, logo, preset and terrain textures, project templates (`templates/`), the plugin tutorial (`tutorial/HelloPlugin`). Installed to the data dir. |
| `tests/` | Test executables and `tests/CMakeLists.txt`. |
| `docs/` | This documentation. |
| `packaging/` | `.desktop` and MIME files, AppImage and tarball scripts, `make_kit.py` (export runtime kit), `test_export.sh`. |
| `tools/` | `guard_raylib_funcs.py` (used once when vendoring raylib modules), `sokol-shdc/` (downloaded; not in git). |
| `resources/` | Windows resource files (`app.rc`, `player.rc`). |
| `BUGS.md` | Bug-hunt report with fix status and the still-open list. Read it before assuming an area is clean. |
| `THIRD_PARTY_NOTICES.md` | Licences of vendored code. Update when adding a dependency. |
| `.github/workflows/build.yml` | CI. |
| `build/`, `export/`, `Captures/`, `Recordings/`, `Scripts/`, `flyengine.flyproj` | Local artefacts. Not source. |

Vendored, do not edit casually (see [Maintenance.md](Maintenance.md#vendored-code)): `sokol/`, `raylib/` (kept on
purpose, do not delete), `curl/`, `imgui/`, `extern/*`, `src/Engine/Backend/{cglm,vulkanh,stb}`,
`src/Engine/RL/raylib/`.

## `src/Engine/` — core, runtime and editor

| Path | Purpose |
| --- | --- |
| `Engine.cpp` | Main loop, entity list, update and draw order. |
| `CameraController.cpp` | Editor fly and top-down camera. |
| `PhysicsSimulation.cpp`, `PhysicsCollision.cpp` | Box3D wrapper, collision shapes, raycasts, joints, contact events. |
| `TextureManager.cpp` | Texture cache, mip and LOD handling. |
| `ShapeBatch.cpp` | Batches primitive shapes by material. |
| `CityMeshes.cpp`, `CityInstancing.cpp` | City mesh building and instanced draws. |
| `Navigation.cpp` | Walkable-ground bake and pathfinding (`fly::Nav`). |
| `GameUI.cpp` | In-game UI widgets scripts create (`fly::UI`). |
| `LoadingScreen.cpp` | Editor and player loading screens. |
| `FrameStats.cpp`, `TechnicalTools.cpp` | Frame timing, memory tracker, physics debug visualiser, debug tools. `TechnicalTools.cpp` is large (about 4000 lines). |
| `Audio.cpp` | miniaudio playback, 3D sound. |
| `Backend/` | Scene objects and non-UI services: `ScatteredObject`, `ModelImport`, `FbxModel`, `ModelAnimator`, `CharacterController`, `VisualScript`, `FlyScriptApi` (the `FlyNative_*` functions), `Automation`, `Benchmark`, `CrashReporter`, `AudioAssets`, `SoundRays`, `fcloudint` (FlyCloud). |
| `Frontend/` | Editor: `ui.cpp` and the `ui_*.cpp` panels, `ObjectInteractionManager`, `ProjectManager`, `ScenePersistence`, `UndoSystem`, `VisualScriptEditor`, `PluginHost`, `ExportDialog`. `ui_internal.hpp` is the shared private header. |
| `Scripts/` | `NativeScriptHost`, `ScriptRuntime`, `ScriptCompiler`, `ScriptDiagnostics`, `ScriptLauncher` (open scripts in an external editor), `CommandConsole`, `CPluginAPI.cpp` (plugin function table). |
| `Export/` | `ExportManager`, `PackArchive`, `PeIcon`. |
| `Platform/` | `Platform.cpp`, `FileDialog.cpp`, `ProcessRunner.cpp`. The only place OS headers are allowed. |
| `RL/` | FlyRL, see below. |

### `src/Engine/RL/` (FlyRL)

| File | Purpose |
| --- | --- |
| `sokol_impl.c` | The single translation unit that defines sokol's implementation. |
| `rl_core.cpp` | Window, frame, input, file functions (hooked to the VFS). |
| `rl_gfx.cpp` | Draw state, render targets, passes, pipelines. |
| `rl_batch.cpp` | Immediate-mode batching. |
| `rl_models.cpp` | Mesh and model upload and draw (`DrawMeshInternal` sets the material uniforms). |
| `rl_textures.cpp` | Texture creation and upload. |
| `rl_shader.cpp` | Program lookup by name, uniforms by name, samplers. |
| `rl_shader_hotreload.cpp` | Shader reload support. |
| `rl_ibl.cpp` | Environment bake (irradiance, specular, BRDF LUT). |
| `rl_gamepad.cpp`, `rl_misc.cpp`, `rl_raymath.c`, `rl_fallback.c` | Gamepad stub, misc helpers, math, and the Vulkan-to-fallback handoff. |
| `sokol_imgui_impl.cpp` | ImGui rendering and input through sokol_imgui. |
| `rl_internal.hpp`, `rl_programs.hpp` | Private shared declarations and the program table. |
| `raylib/` | Vendored raylib CPU-side modules (shapes, textures, text, models). Functions the sokol layer replaces are wrapped in `#if !defined(RL_SOKOL_BACKEND)`. |

## `src/Graphics/`

`Graphics.cpp` (lighting setup, node lights, environment update/upload), `Lighting.cpp`, `Shadows.cpp` (cascades),
`Reflections.cpp` (planar water reflections), `Atmosphere.cpp` (sky, sun, moon, stars), `Clouds.cpp`, `PostFX.cpp`
(HDR target, tone mapping, bloom, AO, fog, AA, quality tiers), `Particles.cpp` (6000-particle pool), `Occlusion.cpp`,
`Vfs.cpp` (virtual file system for exported games).

## `src/Terrain/`

| File | Purpose |
| --- | --- |
| `Terrain.cpp`, `TerrainMesh.cpp`, `TerrainIO.cpp`, `TerrainSculpt.cpp`, `TerrainPhysics.cpp`, `TerrainCollider.cpp`, `TerrainEditor.cpp` | The chunked `Terrain`. `Terrain.hpp` declares friend access classes (`TerrainSculptAccess`, `TerrainPhysicsAccess`, `TerrainMeshAccess`, `TerrainIOAccess`) so these files can touch private state. |
| `TerrainRegistry` (header-only) | Ordered list of all terrains and the `terrain.terrain` container format. |
| `BasicTerrain.cpp`, `BasicTerrainIO.cpp`, `BasicTerrainErode.cpp` | The brush-based `BasicTerrain` and its `.bt` format. |
| `Foliage.cpp` | Procedural foliage rules and instanced drawing. `foliage.cfg`. |
| `Water/` | `WaterBody`, `WaterNoise`, `WaterRipple`. |

## `src/CityGen/`

`City.cpp` (about 6600 lines: params, road graph, blocks, lots, buildings, traffic, transit, save and load),
`CityGeometry.cpp` (meshes), `CityEditor.cpp` (the City Editor panel). Many `tests/city_*` files cover it.

## `src/Plugin/`

C code shared with the CLI tools: `ManifestParser.c`, `PluginLoader.c`, `PluginIPC.c`, `PluginIdentityValidator.c`,
`PluginSandbox.c` (Linux seccomp/cgroups), `VersionCompat.c` (SemVer and API-version checks), `PluginProcessRunner.c`,
`cli/FlyPluginCLI.c`. Public headers: `include/Engine/Plugin/`.

## Entry points

`src/main.cpp` (editor), `src/PlayerMain.cpp` (player), `src/ExportMain.cpp` (`flyexport`), `src/SceneTest.cpp` and
`src/WaterStressTest.cpp` (built into the core library and reached through `--testscene` and `--testwater`).

## Shaders

| File | Used for |
| --- | --- |
| `fly_common.glsl` | Shared helpers: depth and render-target conventions, the PBR BRDF (`lit_node_lights_pbr`, `fly_pbr`, `fly_ibl`). Include file, not a program. |
| `fly_shadow.glsl`, `fly_atmosphere.glsl` | Include files for shadow sampling and sky maths. |
| `lit.glsl` | Opaque and transparent lit geometry. |
| `terrain.glsl`, `terrain_paint.glsl` | Terrain and the splat paint overlay. |
| `water.glsl` | Water. Deliberately not PBR. |
| `sky.glsl`, `clouds.glsl`, `ibl.glsl` | Sky, volumetric clouds, environment bake. |
| `post.glsl`, `outline.glsl` | Post chain and selection outline. |
| `rl_default.glsl`, `rl_blit.glsl` | Immediate-mode drawing and the final blit. |

## Tests

`tests/CMakeLists.txt` registers them. See [Maintenance.md](Maintenance.md#testing) for how to run each kind.
Groups by name prefix: `city_*` (city generation), `terrain_*`, `water`, `audio_*`, `game_ui_*`, `navigation_*`,
`script_*`, `pbr`, `prefab`, `explorer_tree`, `version_compat`, `rl_smoke`, `sky`, `loading`, `new_project`,
`play_snapshot`. `tests/automation/` holds `--automation` scripts for FlyPlayer.
