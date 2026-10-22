# Distribution: exporting a game

Exporting turns a project into a game people can run without the editor. The result is a copy of
[`FlyPlayer`](FlyPlayer.md) with your project's data appended to it. This page covers **exporting a game**. Packaging the
editor itself (AppImage, tarball) is not covered here.

> Written from `src/Engine/Export/ExportManager.cpp`, `include/Engine/ExportManager.hpp`, `include/Engine/PackArchive.hpp`,
> `src/ExportMain.cpp`, `src/PlayerMain.cpp` and the README. Not built or run. Open questions are at the end.

- [Quick start](#quick-start)
- [The Export dialog and `export.cfg`](#the-export-dialog-and-exportcfg)
- [Assets: beside the game or inside it](#assets-beside-the-game-or-inside-it)
- [Scripts and plugins in an export](#scripts-and-plugins-in-an-export)
- [Targets](#targets)
- [Icon](#icon)
- [From the command line: `flyexport`](#from-the-command-line-flyexport)
- [What is excluded](#what-is-excluded)
- [Loading screen](#loading-screen)
- [Testing an export](#testing-an-export)
- [The archive format](#the-archive-format)
- [Internals](#internals)
- [Open questions](#open-questions)

## Quick start

1. Open your project. **File > Export...** (the editor saves the project first).
2. Choose a target, name, window title, size, fullscreen and FPS cap, and optionally an icon.
3. Export. The finished game lands in the output folder you chose.
4. Ship the executable **and** the `Assets` folder next to it (unless you chose to embed assets).

A project **without** C++ scripts or plugins needs only the `FlyPlayer` that sits next to the editor. A project **with**
them needs a [runtime kit](#scripts-and-plugins-in-an-export).

## The Export dialog and `export.cfg`

Settings are stored in `<project>/export.cfg`, one `key value` pair per line. A missing file gives defaults taken from the
project (name) and the current quality settings.

| Key | Meaning | Default / limits |
| --- | --- | --- |
| `target` | Target id: `linux-arch`, `linux-debian`, `windows-x64` | The target native to this machine |
| `outputDir` | Where the finished game goes | |
| `exeName` | File name without extension. Sanitised: only letters, digits, `-`, `_`, `.`; Windows device names such as `CON` and `COM1` are rejected | Project name |
| `windowTitle` | Window title | Project name |
| `icon` | Square PNG, project-relative or absolute | none |
| `linuxLauncher` | Also write `<name>.desktop` and `<name>.png` | off |
| `windowWidth`, `windowHeight` | Initial window size | 1280 x 720; minimum 320 x 240 |
| `fullscreen` | Start fullscreen | off |
| `fpsCap` | Frame cap, 0 = unlimited | The current Preferences value (the editor's default was 180); clamped 0 to 1000 |
| `assetsFolder` | Write assets beside the game instead of packing them in | on |

The graphics settings saved under *Preferences > Rendering* **at export time** go into the game. An exported game ignores
the machine's `graphics.cfg` and `loading.cfg` and writes nothing next to itself.

## Assets: beside the game or inside it

By default the project's `assets/` (models, textures, sounds) are written to an **`Assets` folder beside the game**, and
the game needs it. If it is missing or incomplete, the game opens with *"All content not present, Continue Playing?"* and
you can continue or quit.

To pack everything into one executable, untick *Assets in a folder beside the game* or pass `--embed-assets` to
`flyexport`. In that mode nothing else ships: no `.so` or `.dll`, no source, no compiler needed on the player's machine.

The list of what belongs in the folder is stored in the game as `__meta/assets.txt`, one line per file:
`<size><TAB><path>` after a `folder Assets` line.

For automated runs: `FLY_CONTENT_PROMPT=continue|quit` answers the question; `FLY_ASSETS_DIR=<dir>` points the game at
another folder.

## Scripts and plugins in an export

C++ scripts and plugins are **compiled into** the game, not shipped as libraries.

- A project **without** code: the player is copied and the data appended.
- A project **with** scripts or plugins: the sources are compiled with the flags the player was built with and linked into
  a copy of the player using a **runtime kit**.
  - Make the kit with `packaging/make_kit.py <build_dir> <kit_dir>` (static libraries, the link command, compile flags and
    headers).
  - Put it in a folder named `kit` next to the editor, or set `FLYENGINE_KIT_DIR`.
  - Build the kit from a **Release** build configured with `-DFLYENGINE_ENABLE_FLYCLOUD=OFF`, so the game has no libcurl
    or OpenSSL dependency.
- If a project has code and no kit is found, Export fails with a message saying so.
- **Scripts** are everything under `Scripts/`.
- **Plugins** need their **source**: `plugins/<name>/src/*.cpp`, optionally `plugins/<name>/include`. The older
  `plugins/nat/<name>` and `plugins/lib/<name>` layouts work too. Keep the entry point `extern "C"`
  (`CP_REGISTER_PLUGIN` inside an `extern "C"` block). A folder with a built `.so`/`.dll` but no source is reported as
  binary-only and cannot be linked in. See [CPluginAPI](CPluginAPI.md#plugins-in-exported-games).
- Visual Script graphs are part of the scene and run in an exported game. Code Snippet nodes are generated into
  `Scripts/.visualscript/snippets.cpp`; whether Export compiles that file in is an [open question](#open-questions).

## Targets

| Target | Available when | Notes |
| --- | --- | --- |
| Linux (Arch) | The editor runs on Arch | The build happens natively. |
| Linux (Debian) | The editor runs on Debian | The build happens natively. |
| Windows (x86-64) | A 64-bit Windows `FlyPlayer.exe` is in `players/windows-x64/` next to the editor, or in `FLYENGINE_PLAYERS_DIR` | Only for projects **without** C++ scripts or plugins, because compiling them needs that OS's compiler. The editor checks that the file really is a 64-bit Windows executable. Writes `<name>.exe`. |

Building for the *other* Linux distribution (it needs a container) is not implemented; it shows as disabled. A target
that is not available has a reason string that the dialog shows.

## Icon

Pick a square PNG, 256x256 or larger, under *Icon*. The window and taskbar use it on Windows and X11. For a Windows
target it is also written into the `.exe` itself (`PeIcon.cpp`; works from any host, but the Windows `FlyPlayer.exe` must
have been built with `resources/player.rc`). Linux executables cannot carry an icon: tick *Also write .desktop + .png*
(`--launcher`) to get `<name>.png` and `<name>.desktop` beside the game. Native Wayland windows have no window-icon API,
so there the `.desktop` file is what applies.

## From the command line: `flyexport`

```sh
flyexport <project> --out <dir> [--name N] [--title T] [--icon game.png] [--launcher]
          [--fps N] [--size WxH] [--fullscreen] [--embed-assets | --assets-folder]
          [--player <FlyPlayer>] [--target <id>]
```

`--icon` is resolved relative to your shell, not the project. `--assets-folder` is the default. `--player` builds on a
specific player executable. `flyexport` shares the exporter code with the editor.

## What is excluded

Export never packs:

- `plugins/` (their sources are compiled in instead; the folder is not shipped)
- `export.cfg` and `compile_flags.txt`
- `fcloud_config.json` (it holds a server URL and a plaintext API key)
- `*.pem` and `*.key` files
- `*_temp.flyproj`
- source files under `Scripts/` (`.cpp .cc .cxx .h .hpp .hxx`; they are compiled in)

## Loading screen

Opening a project shows a loading screen in both the editor and the player (`src/Engine/LoadingScreen.cpp`). The player
shows a percentage by default. **Preferences > Loading** restyles it (percentage, progress bar, stage text, log lines,
title with `{name}`, background and accent colour). The choices are per machine in `<config dir>/loading.cfg`. *Use this
look for this project only* writes `<project>/loading.cfg`, which then wins for that project's player. Player flags:
`--noloading`, `--loading`, `--loadinglogs`.

## Testing an export

`packaging/test_export.sh <project> [kit]` exports a project, runs the result alone in an empty folder, and checks that it
exits cleanly, has no library dependencies beyond the system's, and writes nothing next to itself.

Environment variables that help automated checks: `FLY_EXIT_AFTER_FRAMES=<n>` quits after n frames,
`FLY_SCREENSHOT=<file>` saves the last frame.

## The archive format

The data is an archive appended to a copy of the player (`include/Engine/PackArchive.hpp`):

```
[original executable][blob 0][blob 1]...[index][footer]
```

- Blobs are raw-deflate streams, or stored bytes.
- The 64-byte footer is at the very end, so detecting an archive only needs the last 64 bytes. It holds the index offset
  and size, the archive size, an index CRC, the version (1), and the magic `FLYPACK1`.
- Each index entry has a UTF-8 relative path, offset, stored and original sizes, a CRC32, and a method (0 stored, 1 deflate).
- At startup the player finds the archive and serves files from it through a virtual file system
  (`include/Graphics/Vfs.hpp`) under the synthetic root `/__embedded__`.
- Identical files share their stored bytes (`aliasOf`).

## Internals

| File | Role |
| --- | --- |
| `include/Engine/ExportManager.hpp`, `src/Engine/Export/ExportManager.cpp` | `exporter::Config`, `Targets()`, `Run()`, `LoadConfig/SaveConfig`, runtime-kit linking. |
| `include/Engine/PackArchive.hpp` | `pack::BuildExecutable` and the format. |
| `include/Engine/PeIcon.hpp` | Writes the icon into a Windows PE file. |
| `src/ExportMain.cpp` | The `flyexport` tool. |
| `src/PlayerMain.cpp` | `ReadEmbeddedConfig` reads `__meta/export.cfg` from the archive. |
| `packaging/make_kit.py`, `packaging/test_export.sh` | Kit builder and end-to-end test. |

See also [Subsystems.md](../Subsystems.md#export).

## Open questions

1. **Visual Script Code Snippets in exports.** `ExportManager.cpp` has no explicit mention of `.visualscript`. Does a
   project that relies on Code Snippet nodes export correctly, given `WriteSnippetSources` deliberately does not create
   a `Scripts` folder for exported players?
2. **Targets from a non-Arch/Debian distribution.** The doc says Arch and Debian are offered "when the editor runs on that
   distribution". What happens on Fedora, Ubuntu or others? I did not read `Targets()`.
3. **Packaging the editor.** You chose game export only. `packaging/` also holds AppImage, tarball and desktop-file
   scripts; I left them out. Say if they should get a section.
4. **FlyCloud publishing.** Not covered, per your choice.
