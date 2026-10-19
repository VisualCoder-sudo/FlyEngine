# Hello Plugin

The sample plugin from Flyengine's "Get started" tutorial.

## How plugins work
- A plugin is a shared library built against `include/CPluginAPI.h` (a plain C ABI, so C or C++ both work).
- Put it in `<project>/plugins/nat/<name>/build/` (`.so` on Linux, `.dll` on Windows).
- The editor loads it at startup and reloads it whenever the file changes.
- It exports `CP_GetPluginEntry` (use `CP_REGISTER_PLUGIN`) and gets `on_load`, `on_update`, `on_unload`
  plus a table of engine functions (entities, input, physics, audio, ImGui, files, ...).
- The editor does not compile plugins. Build them yourself:
  `cc -shared -fPIC -Iinclude -o build/hello_plugin.so src/hello_plugin.c`

## Where to get plugins
Flyengine has no plugin store. A plugin is just a folder: copy its folder into your project's
`plugins/nat/` and (if it ships source) build it. Plugins you find on the engine's GitHub page
(https://github.com/VisualCoder-sudo/FlyEngine) or get from other developers work the same way.
Only install plugins you trust: they run with the editor's permissions.
