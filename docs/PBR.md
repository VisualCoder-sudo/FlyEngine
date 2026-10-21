# PBR, sky reflections and the scene-format bump

This document describes the physically based rendering (PBR) work, how it is wired through the engine, and the scene-format bug that came with it. It is written for the next person or AI who has to maintain it.

## 1. What exists

- **Shared BRDF** (`shaders/fly_common.glsl`): Cook-Torrance with GGX, Smith height-correlated visibility, Schlick Fresnel, and a multi-scatter energy-compensation term. Blocks: `lit_node_lights_pbr`, `fly_pbr`, `fly_ibl`.
- **Material maps** (`shaders/lit.glsl`, glTF metallic-roughness): `metalnessMap`, `roughnessMap`, `normalMap`, `occlusionMap`, `emissionMap`, sampler `material_smp`. Per-draw values go in the `matPbr` and `matEmissive` uniforms.
- **Image-based lighting (IBL)** baked from the atmosphere sky (`src/Engine/RL/rl_ibl.cpp`, `shaders/ibl.glsl`).
- **Per-object material** (`include/Graphics/SurfaceMaterial.hpp`): `metallic`, `roughness`, `emissive`, `emissiveStrength`, saved with the scene.
- **Terrain** uses the same BRDF and IBL (`shaders/terrain.glsl`, `shaders/terrain_paint.glsl`). **Water is deliberately untouched.**

Lighting is in Lambert-reflectance units, so the specular lobe is scaled by pi. Diffuse is the plain albedo. Output is linear light into the float HDR target; tonemapping happens later in `post.glsl`.

## 2. Backends and constraints

The renderer is sokol_gfx behind a raylib-compatible layer ("FlyRL", `src/Engine/RL`). Vulkan is the default, with an OpenGL fallback (`FlyRL_fallback`, `FlyPlayer-fallback`) and D3D11. Shaders are compiled offline by sokol-shdc (`tools/sokol-shdc/sokol-shdc -l glsl410:spirv_vk:hlsl5 --reflection --ifdef`); uniforms are looked up by name through the generated reflection.

Vulkan limits that shaped the design:
- A whole image is transitioned at once, so cube-map passes must use LOAD actions, not CLEAR.
- At most 12 samplers per shader. Texture-array syntax is unavailable, so layer textures are numbered.
- `sg_apply_bindings` with empty bindings fails validation. `Pass()` in `rl_ibl.cpp` takes a nullable `const sg_bindings*` for this reason.
- Never edit files under `sokol/`. Its `AGENTS.md` forbids LLM-generated edits to sokol headers.

## 3. IBL bake (`rl_ibl.cpp`)

API in `include/rl/raylib.h`: `BakeEnvironment(const FlyEnvironment*)`, `IsEnvironmentBaked`, `GetEnvSpecular`, `GetEnvIrradiance`, `GetEnvBrdfLut`, `GetEnvSpecularMips`.

Steps, in order:
1. A 1x1 scale pass matches the sky's brightness to the scene's `ambientSky`.
2. A capture cube (128, 6 mips) of the sky.
3. A diffuse irradiance cube (32).
4. A GGX-prefiltered specular cube (128, 6 mips).
5. A 128x128 split-sum BRDF lookup table.

Each pass draws all six cube faces at once using six MRT attachments. If that is unavailable, a one-face-per-pass path runs instead. Set `FLY_IBL_ONE_FACE=1` to force the fallback path (useful for testing it). The first successful bake logs `IBL: environment baked on ...`.

The caller is `UpdateEnvironment()` in `src/Graphics/Graphics.cpp`. It re-bakes only when the light's `EnvKey` changes, and only if `Quality().environmentLighting` is on. `UploadEnvironment()` binds the results to the lit shaders. The `envParams` uniform is: x = specular mip count minus 1, y = baked flag (0 means use the analytic ambient approximation).

The quality flag `environmentLighting` (default true) is in `include/Graphics/PostFX.hpp`, saved in the quality settings, preserved across `SetQualityTier`, and exposed as "Sky reflections" in Preferences (`src/Engine/Frontend/ui_preferences.cpp`).

## 4. Where material data flows

| Stage | File |
|---|---|
| Struct | `include/Graphics/SurfaceMaterial.hpp` |
| Object fields `surface`, `useModelMaterial` | `include/Engine/Backend/ScatteredObject.hpp`, `.cpp` (copied in `Clone`) |
| Primitive shapes, batched by material | `src/Engine/ShapeBatch.cpp` (`ApplySurfaceMaterial`, `QueueShape`) |
| Model import (FBX PBR values and maps) | `src/Engine/Backend/FbxModel.cpp`, `ModelImport.cpp` |
| glTF factors, default material (rough 0.6, metal 0) | `src/Engine/RL/raylib/rmodels.c` |
| Upload to shader | `DrawMeshInternal` in `src/Engine/RL/rl_models.cpp` (`matPbr`, `matEmissive`) |
| Sampler and uniform lookup | `src/Engine/RL/rl_shader.cpp` |
| Mipmaps, trilinear, tangent generation | `ScatteredObject::PreparePbrMaterials()` |
| Editor UI (Material group) | `ui_properties.cpp`, `ui_internal.hpp`, `ui.cpp` |

A model object uses the file's own material while `useModelMaterial` is true. Editing Metallic or Roughness in the Properties panel sets it to false so the object's values apply.

`matPbr` is {metal, rough, normal-map-bound, any-other-map-bound}. Map samplers that are unbound read white, so an object with no maps behaves as a pure value material.

## 5. Scene format

The scene format version is `kSceneFormatVersion` in `include/Engine/Backend/ScenePersistence.hpp` (currently **33**). The editor writer, the editor reader (`src/Engine/Frontend/ScenePersistence.cpp`) and FlyPlayer's project loader (`src/Engine/Frontend/ProjectManager.cpp`, `kMaxSceneVersion`) all use this one constant.

v33 appends this to each object record: `metallic roughness r g b emissiveStrength useModelMaterial`. It is read only when version >= 33, so v32 scenes still load (checked in `tests/pbr_test.cpp`).

**When you bump the format: change only `kSceneFormatVersion`, add a version-gated read, and add a compatibility check to a test.**

### The bug this caused (fixed)

Symptom: in FlyPlayer, collisions were broken and the player fell through the world.

Cause: `ProjectManager.cpp` had its own hard-coded `kMaxSceneVersion = 32` while the editor already wrote v33. Pressing Play wrote `<Project>_temp.flyproj` as v33, FlyPlayer rejected it as "made with a newer Flyengine", and fell back to "Starting empty scene". Physics was fine; there was just nothing loaded.

Fix: the shared constant above. The `new_project` and `loading` tests would have caught this and are now registered in ctest.

## 6. Tests

All of these are in `tests/CMakeLists.txt`. Tests that open a window carry the `needs_display` label.

- `pbr` and `pbr_fallback` (`tests/pbr_test.cpp`): material save/load/clone, v32 compatibility, a sphere grid, sky/ground split, normal-map detail, glow, and env-off. Output images are named `pbr_<api>_*` so the two runs do not collide.
- `object_collision` (`tests/object_collision_test.cpp`): the player walks into a wall of each shape (cube, sphere, cylinder, wedge) and of an OBJ model at each collision accuracy (including a normal-mapped OBJ, which exercises tangent generation), top-down rays hit the objects, and dropped bodies come to rest on the floor.
- Window tests that were previously unregistered and now run in ctest: `character_jump`, `new_project`, `loading`, `play_snapshot`, `navigation_engine`, `game_ui_draw`, `script_actions_engine`.
- Every `needs_display` test has `RESOURCE_LOCK display`, so they run one at a time even under `ctest -j`. Reason: a tiling window manager resizes side-by-side windows and breaks the screenshot measurements.

Run: `ctest --test-dir build/release -j4` (needs a display). Status when this was written: 36/36 pass.

### Known failures that predate this work

Same result in a baseline build without these changes:
- `terrain_collision_test` line 130 expects the y=0 safety floor that commit `261e8a1` removed from `PhysicsSimulation.cpp`.
- `sound_rays_engine_test` line 108.
- `terrain_tabs_test` segfaults.
- `city_surface_audit` aborts.
- `rl_smoke_test`: instanced draws and a `.glb` load fail.
- `make_sample_projects`: scripts are not copied to the deps folder.
- The FlyPlayer Island and CampfireValley templates fall through terrain when `basicterrain.bt` is under `deps/`. Projects created through New Project copy it to the project root and are fine.

## 7. How to verify a rendering or collision change

1. Build both backends (Vulkan default, plus the fallback targets) and run `ctest`.
2. For anything that might affect collisions or loading, A/B against a baseline: build a copy of the tree with your changes reverted to HEAD, run the same test executables and FlyPlayer `--automation` scripts (for example `walk.auto`) in both, and compare player heights. Sideways drift is expected, because the player reads the real desktop mouse.
3. For shader or bake changes, also build Debug so sokol validation is on, and run once with `FLY_IBL_ONE_FACE=1`.

## 8. Not verified

The editor's Material panel (Metallic, Roughness, Glow swatch, "Model's own") was not checked by eye, only through code paths and tests.
