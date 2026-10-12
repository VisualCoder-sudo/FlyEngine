# Third-Party Notices

FlyEngine is built on the open-source software below. Thank you to all of the authors and contributors.
Each library remains under its own license; the full license texts ship with the sources in the listed locations.

## Windowing, rendering, input

| Library | Author | License | Location |
|---|---|---|---|
| [sokol](https://github.com/floooh/sokol) | Andre Weissflog | zlib/libpng | `sokol/` |
| [raylib](https://www.raylib.com/) (API and CPU-side modules) | Ramon Santamaría and contributors | zlib/libpng | `raylib/`, `src/Engine/RL/raylib/` |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut and contributors | MIT | `imgui/` |
| [cglm](https://github.com/recp/cglm) | Recep Aslantas | MIT | `src/Engine/Backend/cglm/` |

## Physics

| Library | Author | License | Location |
|---|---|---|---|
| [Box3D](https://github.com/erincatto/box3d) | Erin Catto | MIT | `extern/box3d/` |

## Assets, models, textures, shaders

| Library | Author | License | Location |
|---|---|---|---|
| [ufbx](https://github.com/ufbx/ufbx) | Samuli Raivio (bqqbarbhg) | MIT / public domain | `extern/ufbx/` |
| [TinyGLTF](https://github.com/syoyo/tinygltf) | Syoyo Fujita and contributors | MIT | `src/Engine/Backend/tinygltf/` |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | Arseny Kapoulkine | MIT | `src/Engine/Backend/meshoptimizer/` |
| [KTX-Software](https://github.com/KhronosGroup/KTX-Software) (incl. Basis Universal, Zstandard) | The Khronos Group and contributors | Apache-2.0 (bundled components under their own licenses) | `src/Engine/Backend/ktxsm/` |
| [glslang / SPIRV-Tools](https://github.com/KhronosGroup/glslang) | The Khronos Group, Google, LunarG and contributors | BSD-3-Clause / Apache-2.0 | `src/Engine/Backend/spirvc/` |
| [stb](https://github.com/nothings/stb) (image, image_write, image_resize2, truetype, rect_pack, perlin, vorbis, ...) | Sean Barrett and contributors | MIT / public domain | `src/Engine/Backend/stb/`, `src/Engine/RL/raylib/external/` |
| [QOI](https://github.com/phoboslab/qoi) | Dominic Szablewski | MIT | `src/Engine/RL/raylib/external/qoi.h` |
| [cgltf](https://github.com/jkuhlmann/cgltf) | Johannes Kuhlmann | MIT | `src/Engine/RL/raylib/external/` |
| [M3D](https://gitlab.com/bztsrc/model3d) | bzt | MIT | `src/Engine/RL/raylib/external/` |
| [par_shapes](https://github.com/prideout/par) | Philip Rideout | MIT | `src/Engine/RL/raylib/external/` |
| [tinyobjloader-c](https://github.com/syoyo/tinyobjloader-c) | Syoyo Fujita | MIT | `src/Engine/RL/raylib/external/` |
| vox_loader | Johann Nadalutti | MIT | `src/Engine/RL/raylib/external/` |

## Audio

| Library | Author | License | Location |
|---|---|---|---|
| [miniaudio](https://github.com/mackron/miniaudio) | David Reid | public domain / MIT-0 | `src/Engine/RL/raylib/` |
| [dr_libs](https://github.com/mackron/dr_libs) (dr_wav, dr_mp3, dr_flac) | David Reid | public domain / MIT-0 | `raylib/src/external/` |

## Data, storage, networking, archives

| Library | Author | License | Location |
|---|---|---|---|
| [SQLite](https://www.sqlite.org/) | D. Richard Hipp and contributors | public domain | `extern/sqlite/` |
| [nlohmann/json](https://github.com/nlohmann/json) | Niels Lohmann | MIT | `extern/nlohmann/` |
| [cJSON](https://github.com/DaveGamble/cJSON) | Dave Gamble and contributors | MIT | `extern/cjson/` |
| [miniz](https://github.com/richgel999/miniz) | Rich Geldreich and contributors | MIT | `extern/miniz/` |
| [libcurl](https://curl.se/) | Daniel Stenberg and contributors | curl license (MIT-style) | `curl/` |
| [OpenSSL](https://www.openssl.org/) (Linux, FlyCloud TLS; system library) | The OpenSSL Project | Apache-2.0 | system |

## Vendored but not currently used by the engine sources

[Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) and Vulkan validation layers (Khronos, Apache-2.0),
[volk](https://github.com/zeux/volk) (Arseny Kapoulkine, MIT),
[vk-bootstrap](https://github.com/charles-lunarg/vk-bootstrap) (Charles Giessen, MIT),
[VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndProcessors/VulkanMemoryAllocator) (AMD, MIT).
