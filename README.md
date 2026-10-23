# Flyengine

A 3D game engine and editor built on [sokol](https://github.com/floooh/sokol)
(Vulkan by default; OpenGL and D3D11 also supported) and
[Box3D](https://github.com/erincatto/box3d).

Terrain with splatmap painting and LOD geomorphing, water bodies, rigid-body
physics, glTF/OBJ/FBX/PLY model import, physically based materials lit by the
sky (metallic/roughness, normal maps, image-based lighting), a procedural
[city maker](#city-maker) with traffic, an ImGui editor, and
[C++ scripting](#c-scripting) with hot reload.

Builds from **one `CMakeLists.txt` on Linux and Windows**.

---

## Building

### Requirements

CMake 3.25 or newer, and a C++20 compiler (GCC 12+, Clang 15+, or MSVC 2022).

sokol, Box3D and a vendored libcurl are built from the tree as part of the
project, so there is nothing else to clone. The shader compiler
([sokol-shdc](https://github.com/floooh/sokol-tools-bin)) is downloaded into
`tools/sokol-shdc/` on the first configure; set `FLYENGINE_SOKOL_SHDC` to use a
local copy instead.

The default **Vulkan** backend needs a Vulkan 1.3 driver that supports
`VK_EXT_descriptor_buffer` (current NVIDIA, AMD and Intel drivers do). On a
machine without one, the Vulkan executables start the OpenGL (D3D11 on Windows)
`-fallback` executables installed next to them, so the same install works
everywhere. You can also build only OpenGL with
`-DFLYENGINE_GRAPHICS_BACKEND=GL`.

#### Linux

Debian/Ubuntu Linux:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libasound2-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev \
    libxinerama-dev libxkbcommon-dev libgl1-mesa-dev libglib2.0-dev \
    libvulkan-dev
# Optional: Vulkan validation layers (used automatically by Debug builds)
sudo apt install vulkan-validationlayers
```

Arch Linux:

```sh
sudo pacman -S base-devel cmake ninja pkgconf \
    alsa-lib libx11 libxrandr libxi libxcursor libxinerama libxkbcommon \
    mesa glib2 vulkan-icd-loader vulkan-headers
# Optional: sudo pacman -S vulkan-validation-layers
```

#### Windows

Visual Studio 2022 or later with the "Desktop development with C++" workload, or
any MSVC toolchain plus Ninja. CMake and Ninja come with the VS installer.

The Vulkan build needs the [LunarG Vulkan SDK](https://vulkan.lunarg.com/)
(for `vulkan-1.lib`). Without it, configure with
`-DFLYENGINE_GRAPHICS_BACKEND=D3D11`, which needs nothing extra.

### Build

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/Flyengine          # Linux
build\Flyengine.exe        # Windows
```

The editor starts `FlyPlayer` from its own directory for Play, so rebuild both
(`cmake --build build` does) - an old `FlyPlayer` next to a new `Flyengine`
will not show newer features such as the sky. The `Flyengine` target depends
on `FlyPlayer`, so building the editor alone from an IDE rebuilds both.

For Visual Studio, open the folder in the IDE and build - `CMakeSettings.json`
is configured for it.

### Build options

| Option | Default | Effect |
| --- | --- | --- |
| `FLYENGINE_GRAPHICS_BACKEND` | `VULKAN` | `VULKAN`, `GL` (OpenGL 4.1 core) or `D3D11` (Windows only). Chosen at compile time; shaders are compiled for all three, so switching needs no shader changes. |
| `FLYENGINE_BUILD_FALLBACK_BACKEND` | `ON` | With the Vulkan backend, also builds `Flyengine-fallback` / `FlyPlayer-fallback` on OpenGL (D3D11 on Windows). The Vulkan executables check the GPU at startup and hand over to these when it cannot run Vulkan 1.3 + `VK_EXT_descriptor_buffer`. Set `FLYENGINE_FORCE_FALLBACK=1` in the environment to force it. |
| `FLYENGINE_VULKAN_VALIDATION` | `OFF` | Use `VK_LAYER_KHRONOS_validation` in release builds too. Debug builds use it automatically when it is installed, and always run sokol's own validation layer. |
| `FLYENGINE_SOKOL_SHDC` | downloaded | Path to the `sokol-shdc` shader compiler. |
| `FLYENGINE_ENABLE_FLYCLOUD` | `ON` | Builds the FlyCloud integration (vendored libcurl, miniz, nlohmann/json). Turning it off drops the libcurl build, which is the slowest part of a cold compile. |
| `FLYENGINE_ENABLE_DESKTOP_VALIDATION` | `OFF` | Validate `packaging/*.desktop` and the MIME XML at build time. |
| `FLYENGINE_BUILD_TESTS` | `OFF` | Build the `tests/` targets. |
| `FLYENGINE_DATA_DIR` | `<prefix>/share/flyengine` | Where the binary looks for `assets/`. Also the install destination, so the two cannot drift apart. |

---

## Running

```sh
Flyengine                 # splash screen, then the project manager
Flyengine /path/to/proj   # open a project folder
Flyengine /path/to/proj.flyproj
```

### Where the engine finds its data

Shaders are compiled into the binary. The binary locates the `assets/` directory on its own, so it works from any
working directory - including a `.desktop` launcher, which starts with `$HOME`
as the CWD. The first of these that contains the file wins:

1. `$FLYENGINE_DATA_DIR`
2. the current directory, then up to four parent directories
3. the executable's own directory
4. `<exe>/../share/flyengine`, `<exe>/../share`, `<exe>/../lib/flyengine`,
   `<exe>/../../share/flyengine`
5. the `FLYENGINE_DATA_DIR` compiled in at configure time

To point a build at a different data tree without reinstalling:

```sh
FLYENGINE_DATA_DIR=/path/to/FlyEngine ./build/Flyengine
```

If nothing matches, the engine logs the exact list of roots it tried rather
than silently rendering nothing.

---

## Installing on Linux

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --build build
sudo cmake --install build
```

Then refresh the desktop caches:

```sh
sudo update-desktop-database /usr/local/share/applications
sudo gtk-update-icon-cache -f -t /usr/local/share/icons/hicolor
sudo update-mime-database /usr/local/share/mime
```

This installs the binary, `assets/` and `shaders/` under
`/usr/local/share/flyengine`, a `.desktop` entry, the hicolor icon set, and a
MIME type so double-clicking a `.flyproj` file opens the editor.

### AppImage

`packaging/build_appimage.sh` compiles inside a `debian:12` container (needs podman) and writes
`export/Flyengine-x86_64.AppImage`, a single file that runs on most distros:

```sh
sh packaging/build_appimage.sh
./export/Flyengine-x86_64.AppImage
```

### Relocatable tarball

`packaging/make_tarball.sh` builds a runtime-only tarball that can be unpacked
anywhere:

```sh
./packaging/make_tarball.sh            # build/ -> dist/
sudo tar -xzf dist/flyengine-linux-x86_64.tar.gz -C /
```

The unpacked binary runs in place (`./usr/local/bin/Flyengine`) without being
installed, because the data-root search is relative to the executable.

---

## Wayland

sokol_app (the windowing layer) talks to X11, Under a Wayland session this works
through **XWayland**, which GNOME, KDE Plasma and Sway Desktop Environemnts enable by default. If
your session has XWayland disabled, enable it.

There is no native Wayland backend to select; this is a property of
sokol_app's Linux backend, not of Flyengine.

---

## Platform differences

Everything below is on purpose, not unfinished.

### File dialogs

The engine never calls a Win32 dialog. On Linux it shells out to
**`zenity`**, falling back to **`kdialog`**. If neither is installed, an ImGui
modal asks for the path.

Install one for native dialogs:

```sh
sudo apt install zenity        # or: sudo pacman -S zenity
```

Dialogs are **non-blocking**. A request is started with
`platform::Begin*Dialog()` and answered later by `platform::PollDialogResult()`
on the frame after it completes, so the render loop never stalls. Results are
tagged with a `DialogPurpose` so several subsystems can poll every frame
without consuming each other's results.

### C++ scripting

Game scripts are C++20 files in a project's `Scripts/` folder. The editor
compiles them into one shared library with the system compiler and hot-reloads
it when a file changes (running scripts restart; their state is not kept).

```cpp
#include "fly.hpp"

struct Spinner : fly::Script {
    fly::Task Run() override {
        fly::Object self = fly::Object::Self();
        while (true) {
            fly::Vec3 r = self.Rotation();
            r.y += 45.0f * fly::DeltaTime();
            self.SetRotation(r);
            co_await fly::NextFrame();   // or fly::Wait(seconds), fly::WaitFrames(n)
        }
    }
};
FLY_SCRIPT(Spinner)
```

Compiler lookup: `FLYENGINE_CXX` if set, then `clang++` / `g++` on Linux;
`cl` (developer prompt), `clang++` / MinGW `g++`, then Visual Studio found via
`vswhere` on Windows. Compiler output goes to the editor log. A failed build
lists each error in a panel over the viewport (click one to open the file at
that line in VS Code or Rider) while the previous library stays loaded. Builds
land in `Scripts/.build/`, and a `Scripts/compile_flags.txt` is written so
clangd-based editors find `fly.hpp`. See `ScriptingSDK/samples/` for more.

**Playtests are temporary.** When Play starts the editor snapshots the scene
objects and the world settings scripts can change (lighting, grid, wireframe,
field of view, gravity, friction, restitution). When Stop is pressed, anything
that differs is put back: objects scripts created disappear and changes to
existing objects are undone. Cities are not snapshotted, since nothing at play
time changes them. `FlyPlayer` starts the scripts marked "run on play" as soon
as the scene loads; the player's body is the object named `PlayerCharacter`.

Scripts change the time, weather, sky and picture through `fly::Game::Environment`
(`SetTimeOfDay`, `SetDayLength`, `SetRain`, `SetOvercast`, `SetWetGround`, `SetFog`,
`SetCloudCover`, `SetWindSpeed`, `SetWindDirection`, `SetExposure`, `SetSunIntensity`,
`SetBloom`, and the matching getters). Setting something inserts the Lighting item it
belongs to; a playtest puts the whole Lighting section back when it stops.

The editor console (`` ` ``) takes commands such as `Cube.Position = (0, 5, 0)`,
`Physics.Gravity`, `Environment.Rain = 0.6`, `run Spinner`, `scripts` and `rebuild`; type `help`.

### Gameplay API

Ray casts with hit info, sphere overlaps, touch events, joints, camera control, particles, decals, tweens and events,
clickable in-game UI (buttons, sliders, toggles) and navigation (`fly::Nav`: bake the walkable ground, find paths, follow them).
See [docs/ScriptAPI.md](docs/ScriptAPI.md), and [docs/FirstGame.md](docs/FirstGame.md) for a tutorial.
See [docs/PBR.md](docs/PBR.md) for the PBR / sky-reflection pipeline, the scene-format version rule, and the related tests.

### Sample projects

`assets/templates/` holds Coin Dash, Physics Playground and Campfire Valley. Each folder has a `<Name>.flyproj`, a
`data.json` (name, description, author), a `thumbnail.png` and a `deps/` folder whose contents are copied into the new
project (scripts, terrain). Every folder there is offered under **Choose from template** in the New Project panel.
`make_sample_projects` regenerates the `.flyproj` and terrain files.

### Particles

Smoke, fire, sparks, dust, splash, debris and steam, drawn as soft billboards from a 6000 particle pool. Scripts start
them with `fly::Emit` / `fly::Emitter`; hard physics impacts add dust and scuffs on their own.

### Foliage

Terrain panel, **Foliage** tab: rules grow grass, flowers, bushes, trees, pines and rocks on a painted layer, with
density, size, slope and altitude limits. Placement is procedural per cell, drawn instanced with wind sway and LOD,
and saved as `foliage.cfg`.

### Fonts

`arial.ttf` dropped in the working directory wins (so an in-tree build can be
tweaked), then the platform's candidates: Arial on Windows, DejaVu Sans and
Liberation Sans on Linux.

### The scripting ABI

Script libraries talk to the engine only through plain C
(`ScriptingSDK/include/FlyScriptABI.h`): the engine passes a table of
`FlyNative_*` function pointers to the library's `FlyScript_GetModule` entry
point and gets back the list of script classes. C++ exceptions, allocations and
coroutine frames never cross it, so scripts built with any compiler work with an
engine built by any other.

The same `FlyNative_*` functions are exported from the executable for native
plugins. On Linux that needs `ENABLE_EXPORTS ON`; CI checks the export count.

### Native plugins

Plugins are plain C or C++ shared libraries built against `include/Engine/Plugin/CPluginAPI.h`
(API 1.1). Put the library in `<project>/plugins/nat/<name>/build/`; the editor
loads it at startup and reloads it when the file changes. A plugin exports
`CP_GetPluginEntry` (use `CP_REGISTER_PLUGIN`) and gets `on_load`, `on_update`
and `on_unload` callbacks plus the engine function table (entities, input,
physics, audio, ImGui, files, ...). The editor does not compile plugins; build
them yourself, e.g. `cc -shared -fPIC -I<FlyEngine>/include -o build/x.so x.c`.

**Plugin Manager.** Preferences > Plugins lists every plugin in `<project>/plugins/`, loaded or not:
its `id.json` details, API version, editor modes and, for one that failed, why. Each has an Enabled
checkbox (kept in `plugins/disabled.txt`; a disabled plugin is unloaded but not deleted), Reload,
Show and Remove (deletes its folder, after a confirmation). *Import plugin...* copies a plugin folder
(the one holding `id.json` and `build/`) into the project, *Import library...* installs a single
`.so`/`.dll`, and *Rescan* picks up folders added by hand. An import never overwrites an installed plugin.

API 1.1 added `ui_set_accent_color`, which re-skins the editor's accent colour
(editor only). A plugin built against a newer API than the engine is refused.

## Exporting a game

**File > Export...** packs a project into an executable: FlyPlayer with the project's scene appended to it (compressed),
and the project's C++ scripts and plugins compiled *into* it. By default the project's `assets/` (models, textures,
sounds) are written to an **`Assets` folder beside the game**, which must be shipped with it; if it is missing or
incomplete the game opens with "All content not present, Continue Playing?" and carries on or quits as you choose.
Untick *Assets in a folder beside the game* (or pass `--embed-assets` to `flyexport`) to pack them into the single
executable instead. (Automated runs can answer the question with `FLY_CONTENT_PROMPT=continue|quit`; `FLY_ASSETS_DIR`
points the game at another folder.) In single-file mode nothing else
ships: no `.so`/`.dll`, no source, no compiler needed on the player's machine. The editor saves the project first;
the window title, size, fullscreen and **FPS cap** are chosen in the dialog and stored in `<project>/export.cfg`
(the graphics settings are the ones saved under Preferences > Rendering at export time).

From a terminal: `flyexport <project> --out <dir> [--name N] [--title T] [--icon game.png] [--fps N] [--size WxH] [--fullscreen] [--embed-assets]`.

**Icon.** Pick a square PNG (256x256 or larger) under *Icon* in the dialog; it is stored in `export.cfg`. The game's window
and taskbar use it on Windows and X11. For a Windows target it is also written into the `.exe` itself, so Explorer shows it
(the Windows `FlyPlayer.exe` must be built with `resources/player.rc`; the replacement works from any host, see
`include/Engine/PeIcon.hpp`). Linux executables cannot carry an icon; tick *Also write .desktop + .png* (`--launcher`) to get `<name>.png` and
`<name>.desktop` beside the game for launchers (off by default: one file). Native Wayland windows have no window-icon API; there the `.desktop` file is what applies.

How it works:

- The data is an archive appended to a copy of the player (`include/Engine/PackArchive.hpp` describes the format).
  At startup the player finds it at the end of its own file and serves every project/engine file from it through a
  virtual file system (`include/Graphics/Vfs.hpp`, hooked into raylib's file functions in `rl_core.cpp`); the files sit under the
  synthetic root `/__embedded__`. An exported game ignores this machine's `graphics.cfg`/`loading.cfg` and writes nothing next
  to itself. For testing, `FLY_EXIT_AFTER_FRAMES=<n>` makes any player quit after n frames (`FLY_SCREENSHOT=<file>` saves the last one).
- A project **without** C++ code only needs the `FlyPlayer` next to the editor. A project **with** scripts or plugins is
  relinked: its sources are compiled with the flags the player was built with and linked into a copy of it, using a
  *runtime kit* made by `packaging/make_kit.py <build_dir> <kit_dir>` (static libraries, the link command, compile
  flags and headers). Put the kit in a folder named `kit` next to the editor, or set `FLYENGINE_KIT_DIR`. Make the kit from
  a Release build configured with `-DFLYENGINE_ENABLE_FLYCLOUD=OFF`, so the game has no libcurl/OpenSSL dependency.
- Plugins are compiled in, so they need their **source**: `plugins/<name>/src/*.cpp` (optionally `plugins/<name>/include`).
  Keep the entry point as `extern "C"` (`CP_REGISTER_PLUGIN` inside an `extern "C"` block). Scripts are everything under `Scripts/`.
- Targets: Linux (Arch) and Linux (Debian) are offered when the editor runs on that distribution (the build happens
  natively). **Windows (x86-64)** from a non-Windows editor works for projects *without* C++ scripts or plugins: put a
  64-bit Windows `FlyPlayer.exe` (built on Windows or by CI) in `players/windows-x64/` next to the editor (or point
  `FLYENGINE_PLAYERS_DIR` at the folder) and the target enables itself; Export appends the data to it and writes
  `<name>.exe`. The editor checks the file really is a 64-bit Windows executable, and refuses a project with scripts or
  plugins for that target, because compiling them needs that OS's compiler. Building for the *other* Linux distribution
  (needs a container) is not implemented yet and shows as disabled.

`packaging/test_export.sh <project> [kit]` exports a project, runs the result alone in an empty folder and checks that it
exits cleanly, has no library dependencies beyond the system's, and writes nothing next to itself.

---

## Sound

`fly::PlaySound("sounds/boom.ogg", 0.8f)` and the Visual Script **Play Sound** node play a file from the project
(WAV, MP3, FLAC or OGG Vorbis). `fly::SetVolume` is the master volume. Clips are decoded once and cached; up to 64
sounds play at once, the oldest giving way. Sound stops when Play ends.

The engine side is `include/Engine/Audio.hpp` (`audio::Play`, `PlayAt` for a sound placed in the world, `Stop`, loop,
pitch, pan), mixed by [miniaudio](https://github.com/mackron/miniaudio). The sound device opens on the first sound; a
machine without one just plays nothing. Exported games read their sounds from the archive.

Choosing a sound from outside the project in a Play Sound node copies it into `assets/audio/` (`audioassets` in
`include/Engine/Backend/AudioAssets.hpp`). The same sound is stored once: importing identical content again, under any
name, reuses the copy. Like model imports, a new copy is pending until you save the project; closing without saving
deletes it. Every save also deletes imported sounds that nothing in the project (the scene, your C++ scripts) mentions any
more. Typing a full path into the node, or dropping a sound onto the Asset Browser, imports it the same way. Only files the engine
imported itself are ever deleted; sounds you put in the folder by hand are left alone.

**Sounds from objects and sound rays.** A Play Sound node can name an **Object**: the sound then comes from that object, follows
it as it moves, and fades out over the node's **Range** (metres). **Loop** keeps it going (a fan's hum) until a **Stop Sound**
node, the object's destruction, or the end of Play stops it. Two switches use *sound rays* (they need an object, and only run
while the game is playing):

- **Muffle**: rays from the object to you. The share a wall blocks, and how thick it is, close a low-pass filter and lower the
  volume, smoothly, so a fan behind a wall sounds dull and comes back clear when you step out. Close to the object (about two
  of its sizes, at least 1.5 m) nothing muffles it, fading in over the next 3 m. Things right at either end (your own body,
  the floor under the object, its mount) are not walls. If the straight line is blocked, a few ways round it are tried (a door,
  a corner, over a low wall); an open one leaves the sound clearer than the wall alone.
- **Echo**: rays out of the object in every direction. How near the walls are and how enclosed the room is set the echo's delay,
  how long it rings and how loud it is; open ground has none. The delay is fixed when the sound starts.

**Stop Sound** links itself: wire it after a Play Sound with ordinary flow wires (Play Sound -> Stop Sound, or Play Sound -> Delay -> Stop
Sound) and it takes that sound's file and object, and follows later changes to it. You can also link it by hand to a different Play Sound:
drag a wire from a Play Sound's *Sound* output (the dot on its File row) to a
Stop Sound's *Sound* input. It then stops exactly that sound (same file, same object), shows them read-only, and follows any later
change to the Play Sound. Unwired, it stops the file and object you pick in it. Only that pairing can be wired.

`FLY_SOUND_DEBUG=1` logs what the rays measure once a second per sound, including what is blocking it.

A 3D sound keeps the same loudness whichever way you face: it is balanced between your ears (the near ear louder, the far one never
silent) rather than dropping when it is straight ahead or behind you. Water and anything without a collider do not block rays.

## Shaders

`shaders/*.glsl` are written in sokol-shdc's annotated GLSL and compiled at
build time for OpenGL (GLSL 410), Vulkan (SPIR-V) and D3D11 (HLSL 5), then
linked into the binary. A shader error is therefore a build error, on every
backend at once. There is no runtime shader compilation; edit the `.glsl` file
and rebuild. `shaders/fly_common.glsl` holds the helpers that keep depth and
render-target conventions identical across backends.

The old `shaders/*.vert` / `*.frag` files are no longer used.

---

## Sky, light and the picture

The scene is lit in linear light into a float render target and turned into the
picture by a chain of full-screen passes (`src/Graphics/PostFX.cpp`,
`shaders/post.glsl`). What a scene looks like is set in the Explorer's
**Lighting** section and saved with the scene; how much of the machine it may
use is set in **Preferences > Rendering** and kept per machine. A new project
starts with the atmosphere sky, clouds, a clear Weather item and the Picture item
already in its Lighting section.

- **Sky** (`src/Graphics/Atmosphere.cpp`, `shaders/sky.glsl`): one colour, or an
  *Atmosphere*: air round a planet that scatters the sun's light (after
  Hillaire, 2020). Blue overhead, pale horizon, red sunsets and the blue of dusk
  come out of a few numbers (what the air scatters, how thick it is, haze,
  ozone), and the sunlight and the sky's light on the scene take their colour
  from the same tables. The presets (*Earth, Clear, Hazy, Dusty, Green, Violet,
  Thin*) only set those numbers. The sky has a sun disc, a moon with phases
  opposite the sun, and stars that turn with the time.
- **Clouds** (`src/Graphics/Clouds.cpp`, `shaders/clouds.glsl`): a layer of
  volumetric cloud in the atmosphere sky that can be flown through and above.
  Cover, thickness, base height, size and wind are settings; the Weather item's
  cloud cover and rain add to it. The clouds' shadow drifts over the ground and
  shows as rays in haze. The noise they are shaped from is built on worker
  threads the first time clouds are wanted (about 3 s; clouds appear when it is done).
- **Fog**: density and a height it thins over. Under an atmosphere the sun lights
  it and the sun's shadow map cuts shafts through it; distance fades into the
  air's own colour whether or not there is fog.
- **Picture**: tone curve (*Neutral* keeps colours as given, *ACES*, *AgX*),
  exposure with an eye that adapts to the view (atmosphere sky only), bloom,
  contrast, saturation, temperature, vignette and film grain.
- **Shadows** reach further through cascades: up to three wider shadow maps
  beyond the one round the camera, redrawn every few frames.
- **Materials** are physically based (`shaders/fly_common.glsl`: GGX specular,
  Lambert diffuse, the glTF metallic/roughness model). Objects, terrain, roads
  and city buildings share it; water keeps its own shading. An object's
  **Material** group in Properties sets metallic, roughness and glow; an imported
  model keeps its file's materials and maps (base colour, normal, metalness,
  roughness, occlusion, emission from glTF and FBX; normal maps from OBJ) until
  *Model's own* is unticked. Normal-mapped meshes without tangents get them
  generated on import.
- **Sky reflections** (`src/Engine/RL/rl_ibl.cpp`, `shaders/ibl.glsl`): the sky
  and ground are baked into cube maps (diffuse light, and reflections blurred
  per roughness), scaled to the scene's ambient light so the sky's brightness
  settings keep their meaning. The bake is redrawn only when the light changes
  (the sun moves half a degree, the ambient a few percent). A GPU that takes six
  colour attachments draws each cube mip in one pass (14 passes a bake);
  `FLY_IBL_ONE_FACE=1` forces one face per pass.

Scenes saved before these existed load as they were: a sky of one colour and
the neutral picture.

**Preferences > Rendering** has four tiers (*Low* to *Ultra*) and the parts they
set: render scale, anti-aliasing (off, FXAA, temporal), bloom, ambient
occlusion, cloud and fog quality, shadow cascades. *Sky reflections* is a
separate switch there (off: a flat sky and ground colour instead of the baked
environment). The player takes
`--quality low|medium|high|ultra`. Ctrl+Shift+F2 in the editor shows what each
pass costs on the CPU side.

Cost of a 70 x 70 city (20,000 buildings, with traffic) at 2560 x 1440 on an
RTX 4060, Release build, in ms per frame (`build/tests/city_lod_test 70 <tier>
sky 2560 1440`, the mean of its three views; runs vary by a few tenths):

| | Sky of one colour | Atmosphere + clouds |
|---|---|---|
| before these passes existed | 2.0 | |
| Low | 1.9 | 2.7 |
| Medium | | 3.9 |
| High (default) | 3.1 | 4.5 |
| Ultra | | 5.1 |

Known limits: temporal anti-aliasing (bicubic history, variance clipping, 16 jitter samples) reprojects by the camera's motion only, so
fast-moving things can leave a faint trail; the chunked `terrain::Terrain` and
the water take an averaged cloud shadow, not the drifting one; terrain does not
receive the sun's shadow map (it never did).

---

## Terrain collision

*Basic Terrain* is solid in Play and in the Player. Starting play builds a Box3D
height-field collider from each terrain's current heightmap (aligned with the drawn
mesh, including its position), so objects land and rest on it and raycasts, including
script raycasts, hit it. The big y = 0 floor slab stays as a safety net but sits at or
below the lowest terrain point, so valleys below y = 0 are not blocked. Sculpting during
Play does not update the collider; stop and play again. The chunked `terrain::Terrain` has
no collider yet.

The player (`CharacterController`, kinematic mode) is a capsule moved with Box3D's
character mover, so it collides with terrain, the city and other bodies alike:

- walks up and down slopes and stays on the ground over bumps, and does not slide while
  standing or walking on anything up to 75 degrees; steeper faces act as walls and are
  slid down;
- steps onto ledges up to 0.4 m without jumping;
- spawns above a terrain that covers the origin.

The limits are the constants at the top of `src/Engine/Backend/CharacterController.cpp`
(`WALKABLE_NORMAL_Y`, `STEP_HEIGHT`). `build/tests/terrain_collision_test` checks the
collider against the mesh and the behaviour above.

## Loading screens

Opening a project shows a loading screen in both programs (`src/Engine/LoadingScreen.cpp`),
driven by the real loading steps (script build, textures, scene objects, terrain, city, water,
lighting, plugins):

- the **editor** shows a percentage, what it is doing now, and the log lines the load writes
  (script build output, `[terrain] loaded ...`, `[city] loaded ...`);
- the **player** shows a percentage (the stage and log lines can be turned on).

**Preferences > Loading** switches each one on or off and restyles it: percentage, progress bar,
stage text, log lines (and how many), title (`{name}` is the project's name), background and accent
colour, with a live preview. The choices are kept per machine in `<config dir>/loading.cfg`. For a
game, *Use this look for this project only* writes the player's look into `<project>/loading.cfg`,
which then wins over the machine's for that project's player. Player flags: `--noloading`,
`--loading`, `--loadinglogs`.

---

## Editor bottom panel

The bottom of the editor is one panel, docked flush between the explorer and the properties
panel and above the command console, with two tabs:

- **Assets** - the asset browser.
- **Output** - the log (script builds, physics, project loading), coloured by severity and kept
  at the newest line unless you scroll up. **Clear** empties it.

When something is written to the log while the Assets tab is showing, a small red dot appears at
the top right of the **Output** tab's label; opening the tab clears it.

---

## City maker

`City` menu -> *New regular city* / *New organic city*, then open the *City Editor* panel.
Cities are saved with the scene (`city.city`, a text format with optional trailing
tags, so older files keep loading). Code lives in `src/CityGen/` and `include/CityGen/`.

- **Layout:** grid or organic road graph; blocks become buildings, parks or concrete
  (*Paint block*). Roads are drawn, bent, deleted and snapped with *Draw road*.
- **Road tools:** per-node height, slopes, bridges, per-road width/lanes/presets,
  one-way roads, speed limits and turn lanes. *Elevate road* drags nodes up and down.
- **Junctions:** plain, crosswalks, traffic lights (editable phases, yellow/all-red,
  offsets), stop signs and roundabouts. Roundabouts have a configurable island
  radius, roadway width (0 = auto), splitter islands and a grass or concrete island.
  Buildings keep clear of them.
- **Buildings:** four styles (Modern, Brick, Industrial, Suburban), roof/shape
  variety (flat, gable, shed, tower), short buildings, footprint variety, floor
  counts with facade windows aligned to floors (stepped towers lay out whole floors per tier). *Insert building* places buildings
  by hand, with an optional *NoCollision* mode.
- **Terrain:** *City panel > Terrain* fits the city to a *Basic Terrain*: *Snap city to terrain*
  puts every road node on the ground (with an optional maximum grade), *Limit grades* only
  relaxes steep roads, and *Shape terrain to city* lowers or raises the ground to just under
  the roads, pads and parks and blends it back into the natural ground (not under bridges).
  *Restore terrain* undoes the last shaping. With *Keep terrain under the city* on (default)
  the terrain is reshaped automatically after Snap and Limit grades.
- **Parks and plazas:** big blocks become parks automatically, or paint blocks as *Park* or
  *Plaza* with the Paint block tool. A park gets sandy paths from its middle to each side, a
  paved hub with a fountain (bigger parks), benches and lamps beside the paths and trees on a
  jittered grid. A plaza is a paved square with a fountain, a ring of benches, lamps and corner
  trees. Pedestrians stroll to parks, walk in to the fountain or a spot on the grass, sit a
  while and walk back (*People* stats count park visits).
- **Big cities:** *Detail distance* (style settings, default 450 m, 0 = off) is a distance level of detail.
  City blocks farther than that draw all their buildings as plain boxes in one batch per tile
  (street furniture is only drawn within a third of it, roads beyond three times it are skipped),
  and far pedestrians are simulated at a quarter rate and not drawn. A 70 x 70 city (20,000
  buildings) goes from 25.8 ms to 6.3 ms per frame from the air and from 7.8 ms to 2.4 ms at street
  level; a 140 x 140 city (80,000 buildings) runs at about 5 ms. Big cities still build on a
  worker thread (about 1.4 s for 70 x 70, 12 s for 140 x 140) while the old geometry keeps drawing.
  A build that runs longer than 5 seconds shows "City attempting to load [n%]" with a progress bar
  in the viewport (also in the player).
- **Hills:** *Max road grade* (style settings, default 15 %) is the steepest slope a road ramp
  may have at its steepest point. When you raise a node with the *Elevate road* tool (or edit
  its *Height*), the node stays exactly where you put it and its neighbours follow, so the hill
  spreads out gradually instead of ramping up sharply. 0 turns it off. Saved with the city.
- **Lighting:** besides *Time*, the Explorer's Lighting **+** menu inserts *Sun* (direction,
  height, intensity, colour), *Ambient*, *Sky*, *Clouds*, *Fog*, *Weather* and *Picture* (see
  "Sky, light and the picture" above). The terrain and the water
  follow them too. The sun follows the time of day (rises at 6:00, peaks at the Sun item's
  height at noon, sets at 18:00, orange near the horizon, shadows move with it; untick *Follow
  the time of day* to pin it); at night the moon lights the scene. *Weather* has cloud cover (dimmer, softer sun, grey sky, haze),
  rain (falling streaks, wet dark roads with puddles that mirror the sky, pedestrians with
  umbrellas) and wet ground; *Clear / Overcast / Rain / Storm* are one-click presets.
- **People:** pedestrians walk to destinations, wait for the walk signal, queue at bus stops,
  ride buses and get off at another stop.
- **Street furniture:** street lights and trees.
- **Traffic (Play mode):** cars follow each other (IDM) with randomised speed and
  braking, turn gradually through junctions, obey signals, stop signs and
  right-of-way, and show indicators. Pedestrians use sidewalks and crosswalks.
  Drive on the left is an option. Cars beyond *Traffic detail distance* switch to
  a cheap constant-speed model drawn as boxes (0 = always detailed).
- **Cars:** round rolling wheels and a slanted windshield. *Car colours* sets spawn
  chances (default Black 55%, Grey 25%, White 15%, Other 5%); add your own colours,
  or tick *All* for equal chances.
- **Districts:** the *Districts* tool places downtowns, suburbs and industrial districts
  (several at once, draggable); building height fades smoothly from a district's centre
  and each district can set the building style. *Auto* puts a downtown in the middle.
  Blocks can be painted into a district.
- **Day and night:** the game time is a scene-wide setting, not a city setting: select
  *Lighting > Time* in the Explorer (optionally with a running day length). The sun and sky
  dim, and windows, lamps, car and bus lights glow at night; lamps and headlights light the
  road, sidewalk and facades around them. Street lamps come on one by one as it gets dark
  (a few flicker), and the tall downtown buildings get neon shop signs. The time is saved
  with the scene.
- **Routed traffic:** cars drive to destinations along the fastest route (downtown attracts
  more trips); *Rush hours* scales the car count with the hour.
- **Public transit:** bus stops and lines, generated automatically (*Auto-generate transit*) or
  by hand (*Transit* tool: click a road to place a stop on that kerb, click stops to build a
  line). Buses follow their line, stop and wait at each stop, and share the road with cars.
- **Street furniture:** lamps, trees, benches, hydrants, bollards and street signs.
- **Physics:** in Play (and in the Player) the city gets ground and building
  collisions (*Building collision* toggle).

`build/tests/city_geometry_test` (with `-DFLYENGINE_BUILD_TESTS=ON`) checks the
block/outline geometry headlessly.

---

## Tests

```sh
cmake -S . -B build -G Ninja -DFLYENGINE_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build                 # headless: math, projection depth, memory tracker, texture hash, script diagnostics
build/tests/rl_smoke_test              # needs a display + GPU: textures, mesh churn, instancing
build/tests/sky_test                   # needs a display + GPU: a day of sky, clouds, cascades, anti-aliasing (sky_*.png)
build/tests/new_project_test           # needs a display + GPU: a new project starts with sky, clouds, weather and picture, in editor and player
build/tests/loading_test               # needs a display + GPU: loading screen settings, progress, pictures (loading_*.png)
build/tests/terrain_collision_test     # needs a display + GPU: terrain collider alignment, player on slopes/steps/walls, bodies resting on terrain
build/tests/character_jump_test        # needs a display + GPU: one jump press after landing from any height gives a full jump
build/Flyengine --testscene [frames] sky   # walks the lighting panels, Preferences and the bottom tabs, saving a picture of each
build/Flyengine --testscene [frames]   # terrain tools + city + shapes; run from a Debug build
build/Flyengine --testwater [objects] [frames]
```

`rl_smoke_test` and `--testscene` render for real and read the frame back, so
they are not part of `ctest` (CI machines have no GPU). Debug builds enable
sokol's validation layer, which aborts on invalid GPU usage, so run these from
a Debug build after touching `src/Engine/RL`.

---

## Continuous integration

`.github/workflows/build.yml` runs on every push and pull request:

- **Linux** on `ubuntu:24.04` and `archlinux:base-devel` - the two distribution
  families the project targets. Configures, builds, checks the `FlyNative_*`
  export count, installs, and smoke-tests the installed binary from an unrelated
  working directory.
- **Wayland/XWayland** - builds the OpenGL backend and starts the engine under
  `xvfb-run` (Mesa software GL), failing if sokol_app cannot open a display,
  which is the failure mode when XWayland is missing.
- **Windows/MSVC** - builds the D3D11 backend, checks the export table with `dumpbin`, and fails
  on a dynamic CRT dependency (the build is configured for a static CRT so
  `VCRUNTIME140.dll` is not needed at runtime).
- **Tarball** - produces `flyengine-linux-x86_64.tar.gz` as a build artifact.
- **Windows zip** - the Windows job also produces `flyengine-windows-x86_64.zip` (D3D11, static CRT): unzip anywhere and run
  `Flyengine/bin/Flyengine.exe`. Its `bin/FlyPlayer.exe` is the base for Windows exports.

---

## Project layout

```
CMakeLists.txt            the single build definition for both platforms
include/Engine/Platform/  the cross-platform shim (see below)
src/Engine/Platform/      its implementation
shaders/                  sokol-shdc GLSL, compiled into the binary at build time
src/CityGen/              the procedural city maker (include/CityGen/)
src/Terrain/              terrain, sculpting, persistence; TerrainCollider.cpp builds the Box3D height field
src/Engine/PostFX.cpp     HDR scene target, tone mapping, bloom, AO, fog, anti-aliasing, quality tiers
src/Engine/Atmosphere.cpp physical sky, sun, moon and stars (shaders/sky.glsl)
src/Engine/Clouds.cpp     volumetric clouds and their shadow
src/Engine/LoadingScreen.cpp  the editor's and the player's loading screens
src/Engine/RL/            raylib-compatible API implemented on sokol (see below)
include/rl/               its public headers (raylib.h, raymath.h, rlgl.h subset)
sokol/                    vendored sokol headers (sokol_app.h carries one marked patch)
assets/                   editor icons, logo, preset and terrain textures
packaging/                .desktop entry, MIME XML, tarball script
ScriptingSDK/             the C++ script SDK (fly.hpp, the C ABI, samples)
extern/box3d/             vendored Box3D
```

### Rendering layer

The engine was written against raylib's API. Rather than rewriting every call
site, `src/Engine/RL` implements the part of that API the engine uses on top of
sokol_app and sokol_gfx. raylib's CPU-side modules (shapes, image loading, text,
model loaders) are reused from `src/Engine/RL/raylib/`; everything that touched
OpenGL objects (textures, meshes, shaders, render targets) is reimplemented.
Notable differences from raylib:

- shaders are looked up by program name (`LoadShaderProgram("water")`), not
  loaded from GLSL source; uniforms are still set by name;
- everything draws into an offscreen target that is copied to the window once
  per frame, which lets the engine interleave screen and render-texture
  drawing on every backend;
- a few additions raylib has no word for: float render targets
  (`LoadRenderTextureEx`), full-screen passes (`DrawFullscreen`), 3D textures
  (`LoadTexture3D`), a sub-pixel shift of the projection (`SetProjectionJitter`),
  and depth textures read as plain values through a `nonfiltering` sampler;
- `GenTextureMipmaps` builds mipmaps on the CPU, before the texture's first use;
- wireframe draws mesh edges as lines (no polygon-mode support in sokol);
- gamepads are not supported (sokol_app has no gamepad API).

### The platform shim

`include/Engine/Platform/Platform.hpp` is the only place the rest of the engine
should reach for anything platform-specific. It covers:

- **dialogs** - non-blocking `zenity`/`kdialog`/ImGui-prompt
- **processes** - `RunProcessCapture`, `LaunchDetached`
- **locations** - `ExecutablePath`, `UserHomeDir`, `ConfigDir`, `DocumentsDir`,
  `DataSearchRoots`, `ResolveAsset`, `ResolveShader`
- **desktop** - `RevealInFileManager`, `OpenWithDefaultApp`, font discovery
- **language** - `FLY_API` (export decoration) and `FLY_TRY`/`FLY_CATCH`
  (structured exception guards, which are real `__try`/`__except` on MSVC and
  degrade to a plain scope on GCC/Clang, which have no SEH)

`Window.h`-style headers are included *only* inside
`src/Engine/Platform/*.cpp` and behind `#if defined(_WIN32)`.

---

Maintainer documentation (architecture, code map, file formats, recipes) is in [docs/README.md](docs/README.md).

---

## Credits

* VisualCoder-sudo
* [sokol](https://github.com/floooh/sokol) by Andre Weissflog
* [raylib](https://www.raylib.com/) by Ramon Santamaría and contributors (API and CPU-side modules)
* [Box3D](https://github.com/erincatto/box3d) by Erin Catto
* [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut
* [ufbx](https://github.com/ufbx/ufbx) by Ulti
* [cglm](https://github.com/recp/cglm) by Recep Aslantas
* [stb](https://github.com/nothings/stb) by Sean Barrett and contributors
* [miniaudio](https://github.com/mackron/miniaudio) and dr_libs by David Reid
* [QOI](https://github.com/phoboslab/qoi) by Dominic Szablewski
* [SQLite](https://www.sqlite.org/) by D. Richard Hipp and contributors
* [nlohmann/json](https://github.com/nlohmann/json) by Niels Lohmann
* [cJSON](https://github.com/DaveGamble/cJSON) by Dave Gamble and contributors
* [miniz](https://github.com/richgel999/miniz) by Rich Geldreich and contributors
* [libcurl](https://curl.se/) by Daniel Stenberg and contributors
* [OpenSSL](https://www.openssl.org/) by The OpenSSL Project

Full list with licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
