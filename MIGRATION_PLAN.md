# FlyEngine: raylib (OpenGL) → sokol (Vulkan) migration

Status as of 2026-10-06: **the engine runs on sokol**. Vulkan is the default
backend; OpenGL 4.1 and D3D11 are compile-time alternatives
(`FLYENGINE_GRAPHICS_BACKEND`). raylib is no longer built.

## Decisions

| Decision | Choice |
|---|---|
| Path | Port onto sokol's GL backend first, then switch to Vulkan (done) |
| Windowing | sokol_app, driven step-by-step (`src/Engine/RL/flyapp.h`) so the engine's existing `while (!WindowShouldClose())` loops keep working |
| Platforms | Linux (Vulkan, X11/XWayland) and Windows (Vulkan, D3D11 fallback). No macOS |
| Abstraction | The earlier `gpu::IGraphicsBackend` layer was dropped. Instead, the raylib API the engine already uses is implemented on sokol (`src/Engine/RL`) |
| Shaders | Offline, sokol-shdc → GLSL 410 + SPIR-V + HLSL 5, baked into the binary. No runtime compilation |
| 2D UI | raylib's own shape/text code, running on the sokol-backed immediate-mode batch |
| Fidelity | Close match. Depth math is kept identical across backends (see below) |

## Architecture

```
engine code (unchanged raylib API calls, ~2,700 sites)
        │
include/rl/raylib.h, raymath.h, rlgl.h (subset)
        │
src/Engine/RL
  flyapp (sokol_impl.c)   stepwise sokol_app: open / poll / present / close, image readback
  rl_core.cpp             window, input state from sokol events, timing, files
  rl_gfx.cpp              device, offscreen main target + final blit, passes, pipelines, samplers
  rl_batch.cpp            rlBegin/rlVertex/rlEnd batch (shapes, text, debug lines)
  rl_shader.cpp           shaders by program name; uniforms by name via sokol-shdc reflection;
                          emulated GL texture units
  rl_textures.cpp         lazy texture creation (CPU mipmaps), filters, updates
  rl_models.cpp           meshes, DrawMesh / instancing, wireframe via edge lists,
                          small-buffer recycling pool
  raylib/                 raylib's rshapes/rtextures/rtext/rmodels (CPU parts)
        │
sokol_app + sokol_gfx (+ sokol_imgui)
```

Key points:

- **Offscreen main target.** All "screen" drawing goes into an RGBA8+depth
  target that is blitted to the swapchain in one pass at `EndDrawing()`. sokol's
  Vulkan backend allows only one swapchain pass per frame; this lets the engine
  keep interleaving screen drawing with render-to-texture passes.
- **Depth conventions.** Every vertex shader ends with `fly_clip()` and every
  render-target lookup uses `fly_rt_uv()` (`shaders/fly_common.glsl`). On
  Vulkan/D3D11 this remaps clip-space z so window depth equals OpenGL's
  `0.5*ndc+0.5` and flips V for top-down render targets. As a result the shadow
  bias constants, the road `gl_FragDepth` bias and all CPU-side projection math
  (`GetScreenToWorldRay`, frustum extraction) are unchanged from the GL version.
- **Shadows.** The shadow map is a depth-only target sampled through a
  comparison sampler (3×3 hardware PCF). A shader never samples the target it is
  rendering into: such bindings are replaced with a dummy texture.
- **Buffer recycling.** Vulkan in sokol gives every buffer its own allocation;
  streaming water chunks spent most of the frame in vkAllocateMemory. Mesh
  buffers ≤256 KB are recycled by size (rl_models.cpp).

## Done

- [x] Build cleanup: validation-layer source build, glslang, KTX, meshoptimizer,
      tinygltf, volk, VMA, vk-bootstrap, SPIRV-Cross and the `gpu::` abstraction
      removed from the build
- [x] raylib-compatible layer on sokol (window, input, 2D/3D drawing, text,
      textures, meshes, shaders, render textures, screenshots)
- [x] Shaders ported to sokol-shdc: lit / lit_road / lit_instanced, terrain,
      terrain_paint, water, rl_default, rl_blit
- [x] Graphics.cpp, TechnicalTools (shader editor now explains there is no
      runtime compile), ImGui → sokol_imgui, Terrain/Water/BasicTerrain shader loading
- [x] Player: debug overlay, Technical Tools and ImGui now render inside the
      frame (they used to be drawn after it was presented)
- [x] Water: chunk GPU buffers were never freed (pre-existing leak, fixed)
- [x] Fallback executables (`-fallback`): the Vulkan build checks the GPU before
      creating a window and hands over to an OpenGL/D3D11 build when it cannot run
- [x] `sokol-shdc` pinned by commit and SHA-256
- [x] Terrain painting updates its textures in place; buffer pool has age and size limits
- [x] Fixed on the way: Vulkan dropped every vertex buffer after the first unused
      slot (instanced draws were invisible, chunked-terrain tangents lost); an
      NVIDIA OpenGL miscompile of the instanced vertex shader; Player crash on
      exit (destruction order); texture de-duplication hash (SHA-256 had a typo
      and did not hash the file contents)
- [x] Verified on Linux/NVIDIA: editor, player and `--testwater` on GL and Vulkan;
      screenshots on both
- [x] CI: Linux builds Vulkan, the display smoke test builds GL (software GL in
      CI), Windows builds D3D11

## Not done / known gaps

- [ ] D3D11 and Windows Vulkan are untested; Windows OpenGL screenshots are not implemented (no Windows machine in this pass);
      the code paths compile-check only on Linux
- [ ] Khronos validation layers not run (not installed on the dev machine):
      install them and run a Debug build (they are picked up automatically)
- [x] City roads/instancing, terrain painting and shapes checked on GL and Vulkan
      (`--testscene`); model import not checked
- [ ] Gamepad input (sokol_app has none) — stubbed
- [ ] Cubemaps — not supported by the layer
- [ ] Partial `UpdateTextureRec` — only full-size updates
- [ ] Delete dead files: `raylib/`, `shaders/*.vert|*.frag`,
      `imgui/imgui_impl_raylib.*`, `src/Engine/Backend/{ShaderCache.cpp,RaylibBackend.cpp,
      SokolBackend.cpp,AssetPipeline.cpp,ShaderHotReloader.cpp,tinygltf_impl.cpp}`,
      `include/Engine/Backend/{ShaderCache.hpp,GraphicsBackend*.hpp,IAssetPipeline.hpp,
      IShaderHotReloader.hpp,EscapeHatches/}` and the unused vendored libraries
      under `src/Engine/Backend/` (`vulkanvaidlayer`, `vkbs`, `vmalloc`, `volk`,
      `spirvc`, `ktxsm`, `tinygltf`, `meshoptimizer`). `vulkanh/` is still used
      as a fallback for Vulkan headers.

## Vendored sokol patch

`sokol/sokol_app.h` `_sapp_vk_create_instance()`: debug builds only request
`VK_LAYER_KHRONOS_validation` when it is installed (upstream fails instance
creation otherwise). Marked `FLYENGINE PATCH`; reapply when updating sokol.
