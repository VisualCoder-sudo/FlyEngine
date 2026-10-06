# Flyengine

A 3D game engine and editor built on [sokol](https://github.com/floooh/sokol)
(Vulkan by default; OpenGL and D3D11 also supported) and
[Box3D](https://github.com/erincatto/box3d).

Terrain with splatmap painting and LOD geomorphing, water bodies, rigid-body
physics, glTF/OBJ/FBX/PLY model import, a PBR terrain shader, an ImGui editor,
and a C# scripting host (Windows).

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

Debian/Ubuntu:

```sh
sudo apt install build-essential cmake ninja-build pkg-config \
    libasound2-dev libx11-dev libxrandr-dev libxi-dev libxcursor-dev \
    libxinerama-dev libxkbcommon-dev libgl1-mesa-dev libglib2.0-dev \
    libvulkan-dev
# Optional: Vulkan validation layers (used automatically by Debug builds)
sudo apt install vulkan-validationlayers
```

Arch:

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
| `FLYENGINE_ENABLE_CSHARP` | `ON` | C# scripting host. **Windows only** - see below. |
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

Shaders are compiled into the binary. The binary locates `assets/` on its own, so it works from any
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

sokol_app (the windowing layer) talks X11. Under a Wayland session this works
through **XWayland**, which GNOME, KDE Plasma and Sway enable by default. If
your session has XWayland disabled, enable it.

There is no native Wayland backend to select; this is a property of
sokol_app's Linux backend, not of Flyengine.

---

## Platform differences

Everything below is deliberate, not unfinished.

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

### C# scripting

**Windows only.** The CoreCLR host embeds `coreclr.dll` and is guarded by
`#if defined(_WIN32)`; on other platforms `LoadCoreCLR()` reports that
scripting is unavailable and the editor runs with it disabled.

This is a known gap, not a claim that it works. The managed assembly itself
builds on any platform:

```sh
./ScriptingSDK/build_sdk.sh [project_path]
```

The Linux CLR host is the next piece of work.

### Fonts

`arial.ttf` dropped in the working directory wins (so an in-tree build can be
tweaked), then the platform's candidates: Arial on Windows, DejaVu Sans and
Liberation Sans on Linux.

### The scripting ABI

The C# side P/Invokes `FlyNative_*` through the process's own exported symbols.
On Linux that means the executable must export its dynamic symbol table, which
CMake does with `ENABLE_EXPORTS ON`. Without it the build and link succeed and
every script call fails on the first tick - CI checks the export count
explicitly for this reason.

---

## Shaders

`shaders/*.glsl` are written in sokol-shdc's annotated GLSL and compiled at
build time for OpenGL (GLSL 410), Vulkan (SPIR-V) and D3D11 (HLSL 5), then
linked into the binary. A shader error is therefore a build error, on every
backend at once. There is no runtime shader compilation; edit the `.glsl` file
and rebuild. `shaders/fly_common.glsl` holds the helpers that keep depth and
render-target conventions identical across backends.

The old `shaders/*.vert` / `*.frag` files are no longer used.

---

## Tests

```sh
cmake -S . -B build -G Ninja -DFLYENGINE_BUILD_TESTS=ON && cmake --build build
ctest --test-dir build                 # headless: math, projection depth, memory tracker, texture hash
build/tests/rl_smoke_test              # needs a display + GPU: textures, mesh churn, instancing
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

---

## Project layout

```
CMakeLists.txt            the single build definition for both platforms
include/Engine/Platform/  the cross-platform shim (see below)
src/Engine/Platform/      its implementation
shaders/                  sokol-shdc GLSL, compiled into the binary at build time
src/Engine/RL/            raylib-compatible API implemented on sokol (see below)
include/rl/               its public headers (raylib.h, raymath.h, rlgl.h subset)
sokol/                    vendored sokol headers (sokol_app.h carries one marked patch)
assets/                   editor icons, logo, preset and terrain textures
packaging/                .desktop entry, MIME XML, tarball script
ScriptingSDK/             the C# SDK and its build scripts
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

## Credits

* VisualCoder-sudo
* [sokol](https://github.com/floooh/sokol) by Andre Weissflog
* [raylib](https://www.raylib.com/) by Ramon Santamaría and contributors (API and CPU-side modules)
* [Box3D](https://github.com/erincatto/box3d) by Erin Catto
* [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut
* [ufbx](https://github.com/ufbx/ufbx) by Ulti
