# Subsystems

One section per feature area: what it does, which files own it, and what is easy to break. User-facing behaviour is
described in the [README](../README.md); this file is for the person changing the code.

- [Rendering layer (FlyRL)](#rendering-layer-flyrl)
- [Graphics pipeline](#graphics-pipeline)
- [Scene objects and nodes](#scene-objects-and-nodes)
- [Terrain, foliage and water](#terrain-foliage-and-water)
- [City generator](#city-generator)
- [Physics and character](#physics-and-character)
- [Scripting](#scripting)
- [Visual scripting](#visual-scripting)
- [Plugins](#plugins)
- [Export](#export)
- [FlyCloud and the console](#flycloud-and-the-console)
- [Editor UI](#editor-ui)
- [Audio](#audio)
- [Automation and debugging hooks](#automation-and-debugging-hooks)

## Rendering layer (FlyRL)

`src/Engine/RL/` implements the raylib API the engine uses on top of sokol_app and sokol_gfx. Anything that touched
OpenGL objects in raylib (textures, meshes, shaders, render targets) is reimplemented; CPU-side raylib code is vendored
in `src/Engine/RL/raylib/`.

Differences from raylib that matter when editing engine code:

- Shaders are looked up by **program name** (`LoadShaderProgram("water")`), not loaded from source. Uniforms are set by
  name through generated reflection (`cmake/GenShaderRegistry.cmake` scrapes the names from sokol-shdc's headers).
- Everything draws into an offscreen target that is copied to the window once per frame.
- Extras: float render targets (`LoadRenderTextureEx`), full-screen passes (`DrawFullscreen`), 3D textures
  (`LoadTexture3D`), projection jitter (`SetProjectionJitter`), depth textures read through a non-filtering sampler.
- `GenTextureMipmaps` runs on the CPU before first use. Wireframe draws edges as lines. No gamepad API in sokol_app, so
  `rl_gamepad.cpp` polls evdev (Linux) or XInput (Windows) itself.
- Backend is a compile-time choice (`FLYENGINE_GRAPHICS_BACKEND`). `rl_fallback.c` checks the GPU at startup and starts
  the `-fallback` executable when Vulkan 1.3 + `VK_EXT_descriptor_buffer` is missing. `FLYENGINE_FORCE_FALLBACK=1`
  forces it.

Vulkan constraints (full list in [PBR.md](PBR.md)): whole images transition at once so cube-map passes need LOAD
actions; at most 12 samplers per shader; `sg_apply_bindings` with empty bindings fails validation.

After touching `src/Engine/RL`, run a **Debug** build: sokol's validation layer aborts on invalid GPU usage.

## Graphics pipeline

Owned by `src/Graphics/` and `PostFX`. Frame order: shadow cascades, water reflection pass, opaque pass, transparent
pass, post chain, 2D/ImGui. Settings:

- `RenderQuality` (`include/Graphics/PostFX.hpp`) holds quality flags, saved in `graphics.cfg`. `SetQualityTier`
  must preserve independent flags such as `environmentLighting`.
- Lighting items (Sun, Time, weather and so on) are Explorer items saved with the scene. Scripts change them through
  `fly::Game::Environment`; a playtest restores the whole Lighting section on Stop.
- Per-object lights, decals and tiled textures are `ObjectNode`s evaluated in `Graphics.cpp` (`NodeLight`,
  `NodeSurface`). Shadow-casting lights are capped globally by the Lighting `maxShadowCasters`.
- Image-based lighting: `UpdateEnvironment()` in `Graphics.cpp` re-bakes only when the light's `EnvKey` changes.
  Set `FLY_IBL_ONE_FACE=1` to test the one-face-per-pass fallback.
- Material and BRDF details: [PBR.md](PBR.md).

## Scene objects and nodes

`ScatteredObject` is the workhorse. It holds the transform, colour, `SurfaceMaterial`, `useModelMaterial`, collision
accuracy and flags (`canCollide`, anchored), attached scripts, and a list of `ObjectNode`s. `Clone` must copy every new
field, or duplicate/paste will silently drop it (the PBR work had to do this for `surface` and `useModelMaterial`).

`ObjectNode` (`include/Engine/Backend/ObjectNode.hpp`) is one plain struct for every node kind: SpotLight, PointLight,
SurfaceLight, Decal, Texture. Fields a kind does not use are ignored. To add a kind, add the enum value, a default in
`MakeNode`, the name in `ObjectNodeTypeName`, the draw path in `Graphics.cpp`, the editor UI in `ui_nodes.cpp`, and the
save/load code with a scene-version gate (see [Maintenance.md](Maintenance.md#add-a-saved-field)).

Unions: `ScatteredObject::MakeUnion` merges convex primitive parts into one object whose mesh is built by `csg::Evaluate`
(`Backend/Csg.cpp`) and owned by the object (not in the shared model cache). The parts stay in `UnionParts()` so the
union can be separated and saved; the mesh is rebuilt on load, on clone and on every undo/play restore. A part marked
`negate` is a cutter. The editor operations run in `ObjectInteractionManager::ApplyUnionRequest`. Full details:
[guide/Unions.md](guide/Unions.md).

Models: `ModelImport.cpp` loads glTF/GLB, OBJ, PLY and (through `FbxModel.cpp` and `extern/ufbx`) FBX.
`ModelAnimator.cpp` plays skeletal clips. `ModelGroup` is the editor grouping; members stay in the flat object list.

Undo is `src/Engine/Frontend/UndoSystem.cpp`, capped at 100 entries / 256 MB. Undo entries refer to selection by index.

## Terrain, foliage and water

Two terrain systems exist side by side:

- **`terrain::Terrain`** — chunked, splatmap-painted, LOD-geomorphed. Split by concern across `TerrainMesh`,
  `TerrainSculpt`, `TerrainPhysics`, `TerrainIO`, `TerrainCollider`, `TerrainEditor`; the split classes reach private
  state through the `*Access` friend classes in `Terrain.hpp`. `TerrainRegistry` owns the `terrain.terrain` container
  and the TERRAIN# slot numbering. Slots renumber on delete but names never change, because scripts and physics key off
  the Explorer name.
- **`BasicTerrain`** — brush-based terrain (`Tool` enum: Raise, Lower, Smooth, Flatten, Paint, Erode, Noise, Ramp).
  `Tool` order must match `TerrainToolIconType` and the tool-name table in `ui.cpp` (they are indexed by
  `static_cast<int>`). Saved as `.bt`.

Terrain collision is a Box3D height field (`TerrainCollider.cpp`). `tests/terrain_collision_test.cpp` covers the
player on slopes, steps and walls.

Known open items (from [BUGS.md](../BUGS.md)): `BasicTerrain::BuildDenseMesh` uses 16-bit indices, so grids over about
256x256 break when the adaptive mesh bails out; the `Terrain` constructor shadows `chunkResolution`;
`TerrainMesh.cpp`, `TerrainEditor.cpp` and most of `TerrainSculpt.cpp` were not reviewed in depth.

Foliage (`Foliage.cpp`, `foliage.cfg`): rules per painted layer; placement is procedural per cell, drawn instanced with
wind and LOD.

Water (`Terrain/Water/`): `WaterBody` generates chunked meshes; noise and ripples are in `WaterNoise`/`WaterRipple`.
Water is intentionally not PBR. `--testwater` and `tests/water_test.cpp` stress it.

## City generator

`src/CityGen/City.cpp` is the largest first-party file. The data model is in `include/CityGen/City.hpp`: `CityParams`,
`RoadNode`, `RoadEdge`, `Block`, `Building`, `PlacedBuilding`, `District`, `BusStop`, `BusLine`, `SignalPhase`.
Pipeline: generate or edit the road graph, derive blocks, fill blocks with buildings/parks/plazas, build meshes
(`CityGeometry.cpp`, `CityMeshes.cpp`), draw instanced (`CityInstancing.cpp`), run traffic and transit. The editor panel
is `CityEditor.cpp`. Saved as `city.city`, a text format with optional trailing tags so old files keep loading.

Be careful with: loading untrusted or corrupt files (the load path validates indices and sizes; `city_corrupt_load`
tests it), float parameters that are not clamped on load, and the road, lot and traffic math, which `BUGS.md` says was
never audited. Terrain fitting (*Snap city to terrain*, *Shape terrain to city*) depends on `BasicTerrain`.

## Physics and character

`phys::Simulation` creates Box3D bodies for scene objects when play starts and keeps `bodyMap`/`bodyToObject`.
After changing an object's size, anchored flag, `canCollide` or collision accuracy, call `RebuildBodyForObject`.
Before freeing an object call `RemoveObject`. Events (`ContactEvent`) feed script `Touches` and impact dust.
`CharacterController.cpp` is the player body (the object named `PlayerCharacter`). `tests/character_jump_test.cpp` and
`object_collision_test.cpp` guard the common regressions.

## Scripting

Pieces:

| Piece | File | Job |
| --- | --- | --- |
| SDK | `ScriptingSDK/include/fly.hpp`, `src/fly_module.cpp` | C++ API (`fly::Script`, `fly::Object`, coroutines `co_await fly::NextFrame()`), compiled into every script library. |
| ABI | `ScriptingSDK/include/FlyScriptABI.h` | Plain C function table `FlyApi` and `FlyScript_GetModule`. `FLY_SCRIPT_ABI_VERSION` is 1. |
| Engine side of ABI | `src/Engine/Backend/FlyScriptApi.cpp` | The exported `FlyNative_*` functions. |
| Compiler | `Scripts/ScriptCompiler.cpp`, `ScriptDiagnostics.cpp` | Finds a compiler (`FLYENGINE_CXX`, clang++/g++, or MSVC via vswhere), builds `Scripts/.build/`, parses errors into the on-screen error panel. |
| Host | `Scripts/NativeScriptHost.cpp` | Loads the library, resumes coroutines each frame, watches timestamps and hot-reloads. Running scripts restart on reload; state is not kept. |
| World context | `Scripts/ScriptRuntime.cpp` | Object list, camera, physics and engine references the API reads and writes. Guarded by `RUNTIME_MAGIC`. |
| Launcher | `Scripts/ScriptLauncher.cpp` | Creates script stubs in `Scripts/` and opens them in an external editor. Unsaved stubs are deleted on close, but only if untouched. |
| Console | `Scripts/CommandConsole.cpp` | The `` ` `` console. |

The `FlyNative_*` symbols are exported from the executables so native plugins can resolve them (`ENABLE_EXPORTS ON`).
CI fails if fewer than 100 are exported.

### Add a script API function

1. Append the function pointer to the end of the `FlyApi` struct in `FlyScriptABI.h`. Append only, never reorder
   (a comment on the struct says so). Old script libraries were built against a shorter struct.
2. Implement `FlyNative_X` in `FlyScriptApi.cpp`, declare it in `include/Engine/Scripts/FlyScriptApi.hpp`, and assign it in
   `MakeScriptApi()` in `Scripts/NativeScriptHost.cpp`, which builds the table passed to `FlyScript_GetModule`.
3. Wrap it in `fly.hpp`. Gate the wrapper on `Api()->size >= offsetof(FlyApi, X) + sizeof(void*)` (see `HaveActions`,
   `HaveDebug`), so a script built against a newer SDK degrades on an older engine instead of calling garbage.
4. Document it in [ScriptAPI.md](ScriptAPI.md). Add a test (`tests/script_actions_engine_test.cpp` is the model).

## Visual scripting

`Backend/VisualScript.cpp` (graph model, evaluation) and `Frontend/VisualScriptEditor.cpp` (node editor). The graph is
stored in the scene, runs on play in the editor and in exported players. Event nodes (OnUpdate, OnStop), flow nodes
(Branch, Sequence, Delay, Repeat, DoOnce), actions, and value nodes evaluated on demand. A value node wired into two
inputs evaluates once, so both inputs see the same value (one Random for both). `tests/visual_script_test.cpp` has 500+ checks. A Code Snippet node
shares state across instances (documented limitation), and a broken snippet fails the whole script build.

## Plugins

Two separate mechanisms, easy to confuse:

- **Native plugins** (`CPluginAPI.h`, API 1.1): plain C/C++ shared libraries in
  `<project>/plugins/nat/<name>/build/`. The editor `dlopen`s them in-process (unsandboxed), calls `on_load`,
  `on_update`, `on_unload`, and reloads when the file changes. Function table is built in `Scripts/CPluginAPI.cpp`;
  `Frontend/PluginHost.cpp` supplies editor-only services (spawn, remove, top-down camera, scene-swap events). A plugin
  built against a newer API than the engine is refused. The tutorial plugin is `assets/tutorial/HelloPlugin`.
- **Plugin tooling** (`src/Plugin/*.c`): manifest parsing (`id.json`, `plugin.json`), loader, identity validation,
  IPC, version compatibility, and a Linux sandbox (`PluginSandbox.c`). The sandbox is **not wired in** for in-process
  native plugins (see `BUGS.md` #11). `flyplugin` is the CLI; `flyplugin-runner` runs a plugin in a child process.

## Export

`File > Export` and `flyexport` share `src/Engine/Export/ExportManager.cpp`. The result is a copy of `FlyPlayer` with a
**pack archive** appended (`PackArchive.hpp`, footer magic `FLYPACK1`). At startup the player reads the last 64 bytes,
and if it finds an archive serves all files from it through `Graphics/Vfs.cpp` under the synthetic root
`/__embedded__`. Settings are stored in `<project>/export.cfg`.

- No scripts or plugins: just copy the player and append data.
- With scripts or plugins: they are compiled into a relinked player using a **runtime kit** from
  `packaging/make_kit.py` (kit in `kit/` beside the editor or `FLYENGINE_KIT_DIR`). Build the kit from a Release
  build with `-DFLYENGINE_ENABLE_FLYCLOUD=OFF`.
- Windows target from Linux: only for projects without code, using a Windows `FlyPlayer.exe` in
  `players/windows-x64/` or `FLYENGINE_PLAYERS_DIR`. `PeIcon.cpp` writes the icon into the PE file from any host.
- By default assets go into an `Assets` folder beside the game (list in `__meta/assets.txt`); `--embed-assets`
  packs them in. Exports exclude `*.pem` and `*.key`; FlyCloud pushes also exclude `fcloud_config.json`.
- Env vars for testing: `FLY_EXIT_AFTER_FRAMES`, `FLY_SCREENSHOT`, `FLY_CONTENT_PROMPT`, `FLY_ASSETS_DIR`.
- `packaging/test_export.sh <project> [kit]` is the end-to-end check.

## FlyCloud and the console

`Backend/fcloudint.cpp` is the FlyCloud client (vendored libcurl, miniz for ZIP, nlohmann/json). Commands are reached
through the editor console (`fcloud ...`; run `fcloud --help`). Configuration is `fcloud_config.json` (see
`fcloud_config.example.json`); it is never overwritten when corrupt and is excluded from pushes and exports. Build with
`-DFLYENGINE_ENABLE_FLYCLOUD=OFF` to drop libcurl entirely (commands become no-ops). Live calls were never reviewed
(`BUGS.md`).

## Editor UI

`Frontend/ui.cpp` plus `ui_*.cpp`, one file per panel area (explorer, properties, assets, terrain panel, lighting, nodes,
groups, layout, preferences, theme, top bar, tutorial). `ui_internal.hpp` is shared private state. `ui.hpp` declares the
public `ui::` API, including `MenuAction` and `TransformTool`. `ObjectInteractionManager` handles viewport picking,
gizmos, spawn, delete, duplicate. New panel: add a `ui_<name>.cpp`, list it in `FlyEngineCore` in `CMakeLists.txt`,
declare what it needs in `ui_internal.hpp`, and call its draw function from `ui.cpp`.

Rename rejects separators and `..`; project names pass `IsValidProjectName`; folder delete refuses anything without its
own `<name>/<name>.flyproj`. Keep these guards when refactoring `ProjectManager.cpp`.

## Audio

`Audio.cpp` uses miniaudio (`Backend/miniaudio_impl.c`) and stb_vorbis. `AudioAssets.cpp` decodes and caches sounds
(768 MB cache budget, decode outside the lock). `SoundRays.cpp` traces rays for 3D occlusion and balances sound
between the ears. Tests: `audio_test`, `audio_assets_test`, `soundrays_test`, `sound_rays_engine_test`.

## Automation and debugging hooks

- `FlyPlayer --automation <file>`: scripted input and checks (`Backend/Automation.hpp` documents the command list:
  `key`, `mouse`, `look`, `dump`, `screenshot`, `expect`, `compare`, `quit`). Scripts and golden images are in
  `tests/automation/`.
- Player flags: `--noscripts`, `--nohotreload`, `--fixed-dt`, `--log-stdout`, `--logfile`, `--noshadows`, `--quality`.
- Editor: `--testscene`, `--testwater`, `--create-project <name> [path] [--template <t>]`.
- `crashreporter::Install()` plus the terminate handler in `main.cpp` write fatal errors to a log. Debug builds on
  Linux use the Vulkan validation layer when installed.
- The `TechTools` memory tracker and physics debug visualiser are in `TechnicalTools.cpp`.
