# FlyEngine bug hunt, 2026-10-19

Five Haiku agents tried to break the engine (editor, scripting/VisualScript, distribution, Project Manager/templates, rendering/runtime). No repo code was changed. Methods were static review plus scratch-dir experiments; no GUI was launched.

Confidence: **confirmed** = reproduced or read directly, **likely** = strong code-path reasoning, not run, **speculative** = needs unusual input or untraced.
Findings come from Haiku agents and were not re-verified by the manager. Check each before fixing.
Tags: [ED] editor, [SC] scripting, [DI] distribution, [PM] project manager, [RR] rendering/runtime.

## Fix status

Fixed (compile-checked only; not run, no tests added): 1, 2, 3, 4, 5, 6, 7, 8, 9.
- 1: `IsValidProjectName` rejects `.`/`..`/leading dot/trailing dot or space/over 100 chars. 3: `DeleteFolderRecursive` refuses home, the projects dir, roots, and any folder without its own `<name>/<name>.flyproj`.
- 2: create refuses an existing project; template deps use `skip_existing`.
- 4: `make_kit.py` only wipes an empty dir or one containing `kit.txt`, and never the build dir or its parents.
- 5: `ModelsRollback` in `ScenePersistence.cpp` keeps the old groups until a load or restore succeeds.
- 6: typed non-finite numbers become 0. 7: dropped duplicates become `name (1).ext`. 8: rename rejects separators/`..` and existing targets.
- 9: project and scene saves write `<file>.tmp` then rename, and check the flush.

Second pass (4 Sonnet fixers, reviewed and merged; compile-checked, `tests/visual_script_test.cpp` passes 534 checks per the scripting fixer, nothing else run):
- Fixed: 10, 11 (comments only, sandbox NOT wired in), 12, 13 (files with `\` in the name are skipped on export), 14 (warns on non-https instead of refusing), 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35 (manifests never run through `flyplugin validate`), 36, 37 (logs only, no dialog), 38.
- Also fixed minors: CLI validation in `flyexport`, `test_export.sh` default build dir, failed shader compile no longer cached, zero-size picking ray, `RemoveByName` selection.
- Not fixed: stale assets on re-export, `appimagetool` pinning (comment only), `FindClip` prefix, pre-init screenshots, explorer rename UTF-8 truncation, group-delete double undo (a fixer's change was reverted: it would have lost the group-restoring undo step), tutorial `cc` build still blocks the UI thread.
- Behaviour changes to know about: a VisualScript value node wired into two inputs now evaluates once (one Random value for both); exports exclude `*.pem`/`*.key`; `fcloud delete --previous --all` needs `-y`; an edited shader that fails to compile keeps the previous one.
Everything else below is still open.

## Critical / data loss

1. **[PM] Project name `..` or `.` deletes `$HOME` contents.** `ProjectManager.cpp:758-764` (`IsValidProjectName` allows them), `:746-750`, `:293-299`, `:1275-1281`, `:1369`. Folder resolves to `~/FlyengineProjects/..`; "Delete project from disk" runs `fs::remove_all` on it. The `remove_all` step was reproduced on a fake home (`precious.txt` deleted). Full UI flow not run. Fix: reject `.`/`..`/leading-dot names; verify the folder is a direct child of the projects dir before any `remove_all`.
2. **[PM] Creating a project with an existing name overwrites it.** `ProjectManager.cpp:1296-1310`, `:1465-1478` (`std::ios::trunc`), `:1433`. "Start Tutorial" (`:1346-1351`) can overwrite a `Tutorial` folder lacking a `.flyproj`. Confirmed by code.
3. **[PM] Delete can remove a parent folder holding a lone `.flyproj`.** `ProjectManager.cpp:206-229`, `:1275-1281`. Opening `~/Desktop` (one `.flyproj`) then "Delete from disk" removes it all. Likely.
4. **[DI] `packaging/make_kit.py:61` `shutil.rmtree(kit, ignore_errors=True)` with no check.** Reproduced with a fake ninja: unrelated `precious.txt` deleted.
5. **[ED] Failed scene load or undo restore leaves dangling group pointers.** `ScenePersistence.cpp:527` (`models.clear()` before parsing), `:1132`; `ObjectInteractionManager.cpp:714-716, 775-787, 1089`. Live objects keep freed `parentModel`. Likely crash. Repro: group objects, open a truncated scene, click/delete a grouped object.
6. **[ED] Inspector accepts `nan`/`inf`; scene then never reloads.** `ui_properties.cpp:325,331` (`strtof`), writer `ScenePersistence.cpp` ~61-72 emits `nan`, reader `>>` fails. Confirmed (stream behaviour probed).
7. **[ED] Dropping a same-named file overwrites the existing asset silently.** `ui_assets.cpp:384` (`overwrite_existing`). The move path checks `exists`; this one doesn't.
8. **[ED] Asset rename overwrites and accepts separators / `..` / absolute paths.** `ui_assets.cpp:273-283`. Rename to `../../x` leaves the project.
9. **[PM][ED] Saves are not atomic.** `ProjectManager.cpp:965-972, 996-1003`; `ScenePersistence.cpp:342-347`. Truncate-then-write, no temp+rename. Scene save also returns `file.good()` before the stream flushes (`:329`), so disk-full reports success.

## Security

10. **[DI] `fcloud_config.json` (plaintext `api_key`) is packed into exported games.** `ExportManager.cpp:66-81` (exclude list lacks it), staging `:672-689`. Confirmed by running `flyexport` and reading the pack. Likely also uploaded in FlyCloud push zips (`FlyCloudModule.hpp:527-536`).
11. **[PM] Plugin manifest validation and sandbox are not on the engine load path.** `NativeScriptHost.cpp:346-375` `dlopen`s every `plugins/{nat,lib}/*/build/*.so`. `PluginSandbox`, `PluginIdentityValidator`, `flyplugin-runner`, `PluginIPC` have no callers; `flyplugin run` is a stub. Headers say "sandboxed"; the tutorial says "run with the editor's permissions". Confirmed by grep.
12. **[DI] Newline in `--title` injects `Exec=` into `.desktop` and keys into `export.cfg`.** `ExportManager.cpp:754-755, 813-814`. Confirmed.
13. **[DI] Asset name `a\..\..\b.txt` written literally on Linux, treated as traversal on a Windows host.** `ExportManager.cpp:797`; `Vfs.cpp:252-253` normalises `\` to `/` so it is always "missing". CLI part confirmed; Windows not run.
14. **[DI] FlyCloud:** no https check on `server_url` (`FlyCloudModule.hpp:239`); `fcloudint.cpp:84` logs the full `--apikey` command to the UI log; `.temp_export.zip` (with the key) can be left behind on iterator throw (`:531`). `make_appimage.sh:58` downloads `continuous` appimagetool with no checksum.
15. **[PM] Latent:** `PluginLoader.c:154,160,220` builds `so_path` from id.json `Name` with no validation (`../../x`). No current callers. `ManifestParser.c:344-370` never binds identity to manifest; declared mismatch/duplicate error codes are never returned.

## Crash / hang

16. **[SC] Value-wire evaluation is exponential in shared inputs; cycles hang.** `VisualScript.cpp` ~996, ~1018, `ConnectData` ~390. No memoisation; depth guard 64 allows ~2^64 calls. Measured: 16 nodes 13 ms, 20 nodes 209 ms, 24 nodes 3.5 s. Editor accepts diamonds and 2-node data cycles. Confirmed.
17. **[SC] Setup-chain cycle recurses until stack overflow.** `VisualScript.cpp` ~1151, ~1259, ~1277. `kMaxSteps` is per `Emit` call. Segfault reproduced with `VLINK 1 2 / VLINKP 2 3 1 / VLINKP 3 2 1`. Needs a hand-edited scene (editor allows one exec input per node).
18. **[SC] Script calling its own object's `StopScript()` destroys the executing instance.** `NativeScriptHost.cpp` ~992, ~958; `fly_module.cpp` `DestroyInstance`; `fly.hpp:182`. Likely.
19. **[SC] Destroy inside Create Object / Clone Setup frees an object still in use.** `VisualScript.cpp` ~1273, ~1156; `ScriptRuntime.cpp` ~148; `Engine.cpp` ~371. Likely use-after-free.
20. **[SC] Plugin `on_load` failure leaves registrations pointing into an unloaded library.** `NativeScriptHost.cpp` ~497-505, ~618-627; `PluginHost.cpp` ~67-119. Missing `RemoveModesOf` confirmed; crash likely.
21. **[ED] Unchecked counts from scene files throw uncaught.** `ScenePersistence.cpp:530, 563, 571, 597, 604, 1063, 1075, 1082`. `reserve(count)` with a huge count throws `length_error`; no try/catch on the editor load path (`ObjectInteractionManager.cpp:1089`). Likely.
22. **[RR] Texture byte size overflows `int`.** `rtextures.c:5412`; used in `rl_textures.cpp:172-177`. Images above ~23170x23170 RGBA8 wrap negative, then `pending.assign` reads out of bounds. Likely.
23. **[RR] Vulkan screenshot readback ignores create/allocate results.** `sokol_impl.c` ~530-537. Confirmed; also stalls GPU with `vkDeviceWaitIdle` per screenshot.
24. **[ED] `g_renameModel` has no `GroupExists` guard** (`ui_explorer.cpp:148-154`, `ui.cpp:55,679-681`). Possible write through freed pointer. Speculative.

## Major logic

25. **[RR] Shader hot reload leaves live shaders pointing at a destroyed `sg_shader`.** `rl_shader_hotreload.cpp:324`, `rl_shader.cpp:123-129`, `rl_gfx.cpp:247-248`, `rl_batch.cpp` ~140. Default/blit/user shaders keep the dead handle; 2D draws likely vanish after the first shader save. Comment at `:7` claims "next draw rebuilds it". Likely.
26. **[RR] Queued batch draws can reference a recycled texture id.** `UnloadTexture` (`rl_textures.cpp:207-212`) doesn't flush the batch; id reused LIFO (`rl_gfx.cpp:374-376`). Draw(A); Unload(A); Load(B); Draw(B) renders both as B. Likely.
27. **[ED] Inspector edits never create undo steps.** `ui_properties.cpp:293-380, 1434-1485`. Spawn, type Pos X, Ctrl+Z removes the object. Confirmed.
28. **[SC] Failed plugin hot reload permanently disables that plugin for the session.** `NativeScriptHost.cpp` ~562-624, `CheckHotReload` ~639 skips `!loaded`. A half-copied `.so` is enough. `plugin->entry` left dangling. Confirmed.
29. **[SC] Renaming a visual script in the editor orphans its Explorer entry.** `VisualScriptEditor.cpp` ~1014 calls `RenameCurrent` only (Explorer paths use `RenameKey`). "Open in Visual Script" then creates an empty graph under the old name. Confirmed.
30. **[SC] Scene load doesn't de-duplicate graph names.** `VisualScript.cpp` ~709-731. Real `MakeClassName` maps `-` to `_`, so `a-b` and `a_b` collide; duplicate `VSnip_a_b_<id>` classes fail the whole script build. Test harness stubs `MakeClassName` as identity, hiding it. Likely.
31. **[SC] `UI::SetColor` free function resets a bar's background.** `fly.hpp:635` vs doc comment `:631`. Confirmed.
32. **[SC] `docs/FirstGame.md:21` `fly::Object::Create("Box")` creates nothing.** Only Cube/Sphere/Cylinder/Wedge are valid (`ScriptRuntime.cpp` ~116-124). The tutorial crate never appears. Confirmed.
33. **[DI] Folder-name case sensitivity.** `scripts/` and `Plugins/` sources ship as plain source and are never compiled; `Assets/` stays in the archive. `ExportManager.cpp:72,76,283,285,699`. Confirmed.
34. **[DI] FlyCloud ignores zip return codes**, so unreadable files drop silently and upload reports SUCCESS (`FlyCloudModule.hpp:530,536`). `delete --previous --all` has no confirm though help says `-y` is for no prompt (`:626-627`).
35. **[PM] Tutorial sample plugin isn't a valid plugin by the project's own spec.** `assets/tutorial/HelloPlugin` lacks `id.json`, `plugin.json`, `icon.png`; `flyplugin validate` fails. `InstallTutorialPlugin` also runs `cc` synchronously on the UI thread (20 s timeout).
36. **[PM] Missing template yields a silent blank project with a wrong header.** `ProjectManager.cpp:1483-1489`.
37. **[PM] Scene sidecar loss (`city.city`, `basicterrain.bt`, `foliage.cfg`) is silent.** `ScenePersistence.cpp:733-740, 833-846`.
38. **[RR] Scene sky/sun colours aren't clamped; sky wraps dark.** `ScenePersistence.cpp:866-870`, `Lighting.cpp:163-166` (1.2 becomes 306, wraps to 50). Confirmed.

## Minor

**Editor**
- Group delete records two undo steps; first Ctrl+Z does nothing visible (`ui_explorer.cpp:582-606, 758`; `ObjectInteractionManager.cpp:1208`).
- Undo restores selection by name; duplicate names select the wrong object (`ObjectInteractionManager.cpp:625-636, 755-770`).
- Unknown group member saved as index 0; duplicate indices not rejected on load (`ScenePersistence.cpp:194, 1002, 610`).
- Explorer rename truncates silently at 63 bytes and can cut UTF-8 (`ui.cpp:64`).
- Fold state keyed by name path; same-name siblings share state (`ui_explorer.cpp:195-212`).
- Empty groups never cleaned up (`ui_groups.cpp`); drop-beside-itself reorders to end (`ui_groups.cpp:24-44`).
- Inspector size/position unchecked, applied to all selected (`ui_properties.cpp:1448-1456`). Zero/negative size may reach mass code (speculative).
- Undo history unbounded; each entry a full scene text (`UndoSystem.cpp`).
- `ScatteredObject.cpp:495-505` vs `:192-198` clone/destructor texture refcount asymmetry (speculative).
- `tests/explorer_tree_test` and `prefab_test` not registered with ctest (`tests/CMakeLists.txt:250-256`).

**Scripting / UI**
- Shrinking Sequence outputs leaves exec wires (`VisualScriptEditor.cpp` ~622). Node positions lose precision on save (`VisualScript.cpp` ~634-657). `RemoveByName` doesn't adjust `g_current` (~113-120).
- Snippet inputs/outputs are per class, not per instance (`ScriptRuntime.cpp` ~381-391). One broken Code Snippet fails every C++ script build (`WriteSnippetSources` ~1426-1486). Snippets not written if Play pressed within 0.8 s of an edit (`VisualScriptEditor.cpp` ~1000-1040).
- `ParseUiColor` NaN cast to `unsigned char` is UB (~795-800). Font lookup stats the disk every frame (`GameUI.cpp`). Animation clock is wall time, only advanced when drawn (`ScatteredObject.cpp` ~613-623). `ModelAnimator::FindClip` compares a 32-byte prefix. Windows `build.bat` doesn't escape `%` (speculative).
- Test gaps: no tests for diamonds/cycles, Setup recursion, free `SetColor`, `FindClip`/`Advance`.

**Distribution**
- Re-export keeps deleted assets in `Assets/` (`ExportManager.cpp:794-803`). Failure after exe write leaves a runnable partial exe (`:791-802`).
- CLI unvalidated: `--size 0x0`, `--fps abc`, unknown flags silent, `--launcher` without `--icon` silent no-op (`ExportMain.cpp:30`, `ExportManager.cpp:508, 808`).
- `/tmp/fly_export_<seconds>` work dir collides and is `remove_all`'d (`ExportManager.cpp:624-650`). `PackArchive.cpp:153-154` deletes old output before rename. `packaging/test_export.sh:13` default build dir `build-vk` doesn't exist.

**Project Manager / plugins**
- Failed plugin counts as loaded on retry (`PluginLoader.c`). `VersionCompat.c`: `1..0` parses as 1.0.0 (`:43`), `-1.0.0` accepted, unparseable API version counts compatible (`:218-221`), `*` wildcard never matches, prerelease compared with `strcmp`.
- Header check doesn't validate the scene (`ProjectManager.cpp:176-200`). Strict version with no forward-compat message (`:52, :185`). Name rules lack length limit, Windows reserved names, trailing dot/space (`:758-764`). Trailing slash breaks canonical lookup (`:206-210`). Recent list: names untruncated, path truncated byte-by-byte per frame. Template `.flyproj` headers use display names (cosmetic). Manifest parser unchecked I/O (`ManifestParser.c:126-140`, speculative). Template thumbnails (~3.5 MB) decoded on every open.

**Rendering / runtime**
- Pipeline cache keeps entries for destroyed shaders (`rl_gfx.cpp:455-509`). Failed shader compile cached until restart (`rl_shader.cpp:87-101`). Pre-init screenshot requests never served (`rl_misc.cpp:121-127`).
- Physics accumulator not reset between play sessions (`PhysicsSimulation.cpp:330-342`). Pending physics events may hold removed objects (~900-914, speculative). Hull cache keyed by raw pointer (~160-167, speculative).
- Audio: oversized clips get a misleading "not a valid WAV" error (`Audio.cpp:137, 113`); unbounded clip cache; one failed `ma_engine_init` latches `failed` forever (`:290-291`); decode under global mutex.
- Zero-size window gives a NaN picking ray (`rl_misc.cpp:43-44`, speculative). Shader registry generator fails silently if shdc output changes (`GenShaderRegistry.cmake`). `IsWindowFocused` always true; `GetMonitorWidth/Height` ignore index; `EndDrawing` spin-waits the last ~1 ms (`rl_core.cpp`).
- Test gaps: `character_jump`, `lighting`, `loading`, `weather` tests open a window and aren't ctest-registered. `build/release` has no test binaries, so no existing test was run.

## Checked, no finding
Template JSON, scripts, sidecars and asset references (all 6 templates); template-name traversal; recent entries for deleted dirs; export name sanitizing (`--name ../../escape`); symlinks not packed; missing flyproj/icon/bad out path fail cleanly; `fly.hpp` signatures vs `docs/ScriptAPI.md`; physics fixed timestep; character jump logic; audio voice destruction order.

## Not covered
Terrain/foliage/city generation (grep only), water, GUI runtime behaviour, Windows paths, live FlyCloud calls, AppImage build.
