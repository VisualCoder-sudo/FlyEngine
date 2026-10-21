# Maintenance guide

Practical notes for keeping Flyengine healthy: build and test routines, recipes for common changes, rules for
vendored code, and the hazards that have bitten before.

## Build

Presets (`CMakePresets.json`) use Ninja and put output in `build/<preset>`:

```sh
cmake --preset release && cmake --build --preset release     # build/release
cmake --preset debug   && cmake --build --preset debug       # build/debug: sokol validation + Vulkan validation layer
cmake --preset ci                                             # Release without FlyCloud (no libcurl)
```

Plain CMake works too (`cmake -S . -B build -G Ninja`). Options are in the README's *Build options* table. The ones
you will use most: `FLYENGINE_GRAPHICS_BACKEND` (VULKAN, GL, D3D11), `FLYENGINE_BUILD_TESTS`,
`FLYENGINE_ENABLE_FLYCLOUD`, `FLYENGINE_SOKOL_SHDC`.

Facts worth remembering:

- Building `Flyengine` also builds `FlyPlayer` (a dependency). The editor starts the `FlyPlayer` next to it for Play,
  so a stale player makes new features "not show up" in Play.
- GCC Release builds use `-O2`, not `-O3`: GCC 12 miscompiles the engine at `-O3` (the viewport renders flat clear
  colour). Do not "optimise" that back.
- Libraries are linked statically on purpose, so an exported game is one executable.
- `ccache` is used automatically when found. LTO is on in Release (`FLYENGINE_ENABLE_LTO`).
- sokol-shdc is pinned to a commit and SHA-256 in `cmake/FlySokol.cmake` and must match the vendored sokol headers.
  Upgrade the commit and all hashes together, and rebuild every shader.
- Much of the Windows-only code was changed without being compiled locally (see BUGS.md); CI's Windows job is the
  check. Treat Windows-only changes as untested until it passes.

## Testing

```sh
cmake --preset release -DFLYENGINE_BUILD_TESTS=ON && cmake --build --preset release
ctest --test-dir build/release              # headless tests
ctest --test-dir build/release -L needs_display   # tests that open a window (need a display + GPU)
```

- Tests link `FlyEngineCore`, so they run the same code as the editor. A few (`math_backend_parity`,
  `projection_depth_range`) deliberately do **not**, so they keep working while the engine is mid-change.
- Tests labelled `needs_display` share `RESOURCE_LOCK display` so they run one at a time even with `ctest -j`
  (tiling window managers resize side-by-side windows and break screenshot measurements).
- Image tests write PNGs (`sky_*.png`, `pbr_<api>_*.png`, ...). Look at them after a rendering change; passing
  numbers do not prove it looks right.
- Run rendering changes in a **Debug** build (validation on) and once with `FLY_IBL_ONE_FACE=1` if the IBL bake changed.
- Pass counts quoted in other docs (PBR.md: 36/36 with a display; BUGS.md: 23/23 headless) are snapshots. Trust the
  current output.
- Known failures that predate current work are listed in [PBR.md](PBR.md) §6. Check there before chasing a red test.
- A/B check for anything touching collisions or loading: build a copy with your changes reverted, run the same
  `FlyPlayer --automation` script (for example `tests/automation/smoke.auto`) in both, compare player heights.
- Add a new test: create `tests/<name>_test.cpp`, add `add_executable` + `target_link_libraries(... FlyEngineCore FlyRL)`
  + `add_test` to `tests/CMakeLists.txt`, and add `set_tests_properties(<name> PROPERTIES LABELS needs_display)` if it
  opens a window. Register the fallback variant if the test is backend-sensitive (see `pbr_test_fallback`).

## CI

`.github/workflows/build.yml`: Linux on Ubuntu 24.04 and Arch (configure, build, count `FlyNative_*` exports ≥ 100,
install, smoke-test the install from an unrelated cwd), Wayland/XWayland under `xvfb-run`, Windows/MSVC with D3D11
(export table via `dumpbin`, static CRT check), tarball and Windows zip artifacts. A shader error is a build error on
every job.

## Recipes

### Add a saved field

1. Add the member (and a default) to the owning struct or class.
2. Write it at the **end** of the record in `WriteObjectRecord` (or the relevant section writer).
3. Read it in `ReadObjectRecord` under `if (version >= N)` where N is the new version. Provide a default otherwise.
4. Bump **only** `kSceneFormatVersion` in `ScenePersistence.hpp`. Do not touch `kMaxSceneVersion`; it follows.
5. If the type is cloned, copy the field in `Clone`. Check Play snapshot/restore and prefab save (they reuse the record
   code).
6. Add a compatibility test: save, load, compare; and load a previous-version fixture (see `tests/pbr_test.cpp`).

### Add a shader

1. Create `shaders/<name>.glsl` with an `@module <name>` that equals the file stem and one or more `@program`s.
   Shared code goes in an include file and `@include fly_common.glsl`.
2. Add it to `FLY_SHADERS` in `cmake/FlySokol.cmake` (include-only files go in `FLY_SHADER_INCLUDES`).
3. Reference the program by name with `LoadShaderProgram("<program>")`; check `src/Engine/RL/rl_programs.hpp` and `rl_shader.cpp` for how programs are registered. Uniforms are set by
   name, so the registry generator must see them; rebuild, don't hand-edit generated files.
4. Keep depth, clip-space and render-target conventions by using the helpers in `fly_common.glsl`; the same shader
   runs on Vulkan, OpenGL and D3D11. Mind the 12-sampler limit.
5. Build with `-DFLYENGINE_GRAPHICS_BACKEND=GL` as well: the pipeline is compiled for all three but only runs on one.

### Add an ImGui panel or editor setting

Add `src/Engine/Frontend/ui_<name>.cpp`, list it in `FlyEngineCore` in `CMakeLists.txt`, share state through
`ui_internal.hpp`, call it from `ui.cpp`. Persistent user settings go through the existing preferences code in
`ui_preferences.cpp`; render quality flags belong in `RenderQuality` (`PostFX.hpp`) and its text save/load.

### Add a new entity type

Subclass `Entity`, implement `Update`/`Draw`, override `IsEditorOnly()` if it must not exist in the player, add the
`.cpp` to `FlyEngineCore`, and spawn it from `main.cpp`/`PlayerMain.cpp` or a `MenuAction`. If it saves with the
scene, follow *Add a saved field*. If it needs physics, register it with `phys::Simulation` and unregister **before**
destruction.

### Add a script API function

See [Subsystems.md](Subsystems.md#add-a-script-api-function).

### Add a template project

Create `assets/templates/<Name>/` with `<Name>.flyproj`, `data.json` (name, description, author), `thumbnail.png`,
and `deps/` (copied into new projects). It appears under *Choose from template* automatically.
`tests/make_sample_projects.cpp` regenerates some `.flyproj` and terrain files.

### Update a vendored library

1. Replace the files, keeping local patches. `sokol_app.h` carries one marked patch; raylib modules carry
   `#if !defined(RL_SOKOL_BACKEND)` wrappers (`tools/guard_raylib_funcs.py` re-applies them).
2. Update `THIRD_PARTY_NOTICES.md`.
3. Rebuild everything and run all tests, including the window tests.

## Vendored code

- **Never edit `sokol/`.** Its `AGENTS.md` forbids LLM-generated edits to sokol headers, and `sokol_app.h` already
  carries one marked local patch. Put workarounds in FlyRL.
- **Do not delete the top-level `raylib/` folder** (kept intentionally). The raylib code actually built lives in
  `src/Engine/RL/raylib/`.
- `curl/` ships without its `docs/` folder; the root `CMakeLists.txt` works around curl's unconditional
  `find_package(Perl)`/`add_subdirectory(docs)`. Keep that workaround if you re-vendor.
- Large generated or third-party files (`ufbx.c`, `sqlite3.c`, `stb_*`, `vulkan.hpp`) should not be reformatted.

## Hazards (things that have already caused bugs)

- **Two copies of a constant.** The scene-version mismatch (FlyPlayer rejecting v33) came from a hard-coded duplicate.
  Share constants through one header.
- **Dangling physics bodies.** Free objects only after `Simulation::RemoveObject`.
- **`Clone` and record code must list every field.** Otherwise duplicate, paste, prefab and Play-restore lose data.
- **Enum order coupled to tables.** `BasicTerrain::Tool` must match `TerrainToolIconType` and the tool-name table in
  `ui.cpp`.
- **Platform headers.** `<windows.h>` only in `src/Engine/Platform/*.cpp` and behind `_WIN32`; the Linux CI job is the
  guard. Windows `min`/`max` macros are `#undef`ed in `Terrain.hpp`; `NOMINMAX` is defined globally.
- **Shader changes affect three backends.** A shader that links but samples depth wrongly on one backend is a real
  failure mode (`projection_depth_range_test` exists for the CPU side).
- **Vulkan validation.** Cube-map passes need LOAD (not CLEAR); empty `sg_apply_bindings` fails; 12 samplers max.
- **Script ABI.** Append-only. Never reorder or remove `FlyApi` members.
- **Hot reload resets script state.** Do not promise persistence across a rebuild.
- **Untrusted files.** Terrain, city, scene and pack readers are fuzz- or corruption-tested; keep bounds checks when
  editing them, and prefer rejecting a file to guessing.
- **Windows UTF-8.** There is no UTF-8 `activeCodePage` manifest yet; non-ASCII user or project names break many
  places on Windows. Use `U8Path` for file paths.
- **Export safety.** `packaging/make_kit.py` only wipes an empty directory or one containing `kit.txt`. Keep that.
- **Project deletion.** `DeleteFolderRecursive` refuses home, the projects dir, roots, and any folder lacking its own
  `<name>/<name>.flyproj`. Keep every one of those checks.

## Open work

[BUGS.md](../BUGS.md) is the authoritative list. At the time this was written, the main open items were: per-instance
Code Snippet state; tutorial plugin build blocks the UI thread; no UTF-8 manifest on Windows; `BasicTerrain` 16-bit
index overflow on large grids; unaudited city road/lot/traffic math; unreviewed `TerrainMesh`/`TerrainEditor`/most of
`TerrainSculpt`; sandbox not wired for in-process native plugins; `IsSecureOrLocalUrl` userinfo trick; stale-asset
re-export edge case. Those findings came from automated review and were not all re-verified; check before fixing.

## Documentation upkeep

- Update [FileFormats.md](FileFormats.md) whenever you change a saved format or bump a version.
- Update [CodebaseMap.md](CodebaseMap.md) when adding or moving files; keep the `FlyEngineCore` source list in
  `CMakeLists.txt` and this map in sync.
- Script-facing changes belong in [ScriptAPI.md](ScriptAPI.md) and the README's scripting section.
- Keep the README for users and build instructions; keep the `docs/` files for maintainers.
