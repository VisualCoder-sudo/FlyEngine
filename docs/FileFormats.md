# File formats

Everything Flyengine saves, where the code is, and the compatibility rules. When in doubt, read the writer and the
reader side by side; they are kept next to each other.

## Project file: `<name>.flyproj`

Text. Header lines, then the scene embedded after a marker. Example (abridged from `flyengine.flyproj`):

```
FLYENGINE_PROJECT 1
Name = "GroupCrashTest"
Template = "Blank"
Created = 0
Modified = 0
---SCENE---
SIMPLE_ENGINE_BUILD 7
3
0 "Cube 1" 0 0.75 0 ...
```

- Header (`Name`, `Template`, `Created`, `Modified`) is preserved when the scene is re-saved
  (`project::SaveProjectFile`).
- Code: `src/Engine/Frontend/ProjectManager.cpp`. Saves write `<file>.tmp` then rename, and check the flush, so a
  crash mid-save does not truncate the project.
- Pressing Play writes `<Project>_temp.flyproj` for `FlyPlayer` to load.
- Project folder name must equal the project name; the folder contains `<name>/<name>.flyproj`. Several safety checks
  (delete, create) depend on that.

A project folder also holds `Scripts/`, `assets/`, `plugins/`, `export.cfg`, `loading.cfg`, terrain files and
`foliage.cfg`. Templates in `assets/templates/<Name>/` supply a `.flyproj`, `data.json`, `thumbnail.png`, and a `deps/`
folder copied into new projects.

## Scene: `SIMPLE_ENGINE_BUILD <version>`

Text stream: signature, version, object count, then one record per object, followed by version-gated trailing
sections (model groups, Visual Script graph from v24, and so on).

- **Version constant:** `kSceneFormatVersion` in `include/Engine/Backend/ScenePersistence.hpp` (currently **34**).
  The editor writer, the editor reader and `FlyPlayer`'s loader (`kMaxSceneVersion` in `ProjectManager.cpp`) all use it.
  Never hard-code a second copy: that exact mistake made FlyPlayer reject v33 scenes and fall through the world
  (see [PBR.md](PBR.md) §5).
- **Writer:** `WriteObjectRecord` and `SaveSceneToStream` in `src/Engine/Frontend/ScenePersistence.cpp`.
- **Reader:** `ReadObjectRecord` and `LoadSceneFromStream` in the same file. Fields are read only
  `if (version >= N)`.
- **Other users of the same record code:** `SnapshotSceneToMemory` / `RestoreSceneFromMemory` (Play snapshot),
  `SnapshotObjectRecords` / `DiffObjectRecords` (what changed during play), `AppendSnapshot`, and
  `SavePrefabToFile` (prefabs). A field you forget in the record is therefore also lost in Play-restore and prefabs.
- Reading rejects: wrong signature, version < 1, version > `kSceneFormatVersion`, count over `kMaxSceneCount`.
  A newer-version file logs `[scene] made with a newer Flyengine ...` and is not loaded.
- Scenes older than v29 get a Baseplate object appended (the floor became an ordinary object in v29).
- Model paths are stored relative to the project folder (`baseDir`) so folders stay portable.

Version landmarks confirmed in code: v24 Visual Script graph, v26 model-tree extras, v29 baseplate as an ordinary
object, v33 PBR (`metallic roughness r g b emissiveStrength useModelMaterial` appended per object), v34 unions (appended
after the PBR fields: `negate isUnion`; for a union also `useGlobalColor partCount`, then per part a line with its mode
(0 Add, 1 Negate, 2 Intersect) followed by that part's own full object record, recursively; the union's mesh is not
stored, `RebuildUnionMesh` rebuilds it on load; see [guide/Unions.md](guide/Unions.md)). Other `version >= N`
gates (18, 19, 23, 25, 30, ...) are in `ScenePersistence.cpp`; grep for them before changing the section they guard.

## Terrain

### `terrain.terrain` (chunked `Terrain`)

Binary container owned by `TerrainRegistry` (`include/Terrain/TerrainRegistry.hpp`):

```
TerrainFileHeader { magic "FLYTERR1", version, terrainCount }
for each terrain, in slot order (1-based):
    TerrainBlockHeader { mark "TER", terrainIndex, nameLen, dataSize }
    name bytes (UTF-8)       <- authoritative Explorer name
    data bytes               <- Terrain::WriteToStream() payload
```

Slot numbers are ordering only: deleting a terrain renumbers later slots but never changes names. Structs are in
`include/Terrain/TerrainTypes.hpp` (`TerrainHeader`, `TerrainChunkHeader`, `TerrainLayerHeader`). The reader validates
header fields, chunk sizes and allocation bounds; those strict checks mean a file written by an older or foreign build
with mismatching sizes is **rejected**. No real `.terrain` fixture exists in the repo to prove old saves still load, so
test with a real old project before tightening them. `tests/terrain_io_fuzz_test.cpp` fuzzes the reader.

### `.bt` (`BasicTerrain`)

Written and read by `BasicTerrainIO.cpp`. Templates keep samples in `assets/templates/*/deps/basicterrain.bt`.

### `foliage.cfg`

Foliage rules and settings (`include/Terrain/Foliage.hpp`: `Variant`, `Type`, `Rule`, `Settings`).

## City: `city.city`

Text with optional trailing tags so older files keep loading. Reader validates node and edge indices, counts, and
sidecar block sizes (4 GB cap). Add new data as a new trailing tag, never by changing existing fields.
`tests/city_corrupt_load_test.cpp` and `city_loading_test.cpp` cover it. Float parameters (`cellSize`, `laneWidth`, ...)
are not clamped on load yet.

## Pack archive (exported games): `FLYPACK1`

Appended to a copy of the player. Full spec in the header comment of `include/Engine/PackArchive.hpp`:

```
[original executable][blob 0][blob 1]...[index][64-byte footer]
footer: u64 indexOffset | u64 indexSize | u64 archiveSize | u32 indexCrc | u32 version | 24 zero bytes | "FLYPACK1"
index : u32 count, per entry: u16 pathLen | path | u64 offset | u64 storedSize | u64 originalSize | u32 crc32 | u8 method
```

Little-endian; offsets relative to the first blob; method 0 = stored, 1 = deflate; identical files can alias an earlier
entry. Reserved internal paths: `__meta/graphics.cfg`, `__meta/assets.txt`. `kVersion` is 1; bump it if the layout
changes and make the reader reject unknown versions.

## Other files

| File | Written by | Notes |
| --- | --- | --- |
| `export.cfg` | Export dialog | Window title/size, fullscreen, FPS cap, icon, assets-folder mode. |
| `graphics.cfg` | Preferences > Rendering | `gfx::LoadQualitySettingsFromText` parses it. Embedded copy in exports. |
| `loading.cfg` | Loading screen settings | Per user and per project. |
| `fcloud_config.json` | FlyCloud client | Atomic save. See `fcloud_config.example.json`. Never commit real keys. |
| `id.json`, `plugin.json` | Plugin authors | Parsed by `src/Plugin/ManifestParser.c`. |
| `Scripts/compile_flags.txt` | Script compiler | Lets clangd find `fly.hpp`. Regenerated. |
| `Scripts/.build/` | Script compiler | Build output. Safe to delete. |
