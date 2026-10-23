# C++ coding: game scripts

Game logic in Flyengine is written in C++20. Every `.cpp` file in your project's `Scripts/` folder is compiled by the
editor into one shared library, loaded, and **hot-reloaded** when you save. A script is a class whose `Run()` function
is a **coroutine**: it runs a little each frame and uses `co_await` to wait.

For editor tools (windows, menus, editor modes) use [native plugins](CPluginAPI.md). For no-code logic use
[Visual Script](VisualScript.md).

> Written from the source (`ScriptingSDK/include/fly.hpp`, `src/Engine/Scripts/`). Not built or run. Open questions are
> at the end.

- [Your first script](#your-first-script)
- [The script model](#the-script-model)
- [Waiting](#waiting)
- [Objects](#objects)
- [World services](#world-services)
- [Math helpers](#math-helpers)
- [Input, sound and text](#input-sound-and-text)
- [Attaching and running scripts](#attaching-and-running-scripts)
- [Compiling, errors and hot reload](#compiling-errors-and-hot-reload)
- [Playtests are temporary](#playtests-are-temporary)
- [Console](#console)
- [Compatibility between SDK and engine](#compatibility-between-sdk-and-engine)
- [Internals](#internals)
- [Open questions](#open-questions)

## Your first script

Create `Scripts/Spinner.cpp` in the project folder:

```cpp
#include "fly.hpp"

struct Spinner : fly::Script {
    fly::Task Run() override {
        fly::Object self = fly::Object::Self();
        while (true) {
            fly::Vec3 r = self.Rotation();
            r.y += 45.0f * fly::DeltaTime();
            self.SetRotation(r);
            co_await fly::NextFrame();
        }
    }
};
FLY_SCRIPT(Spinner)
```

Attach it to an object (see [Attaching and running scripts](#attaching-and-running-scripts)) and press **Play**. More
samples: `ScriptingSDK/samples/SampleScripts.cpp` and three complete projects in `ScriptingSDK/samples/projects/`
(Coin Dash, Physics Playground, Campfire Valley). Step-by-step tutorials: [FirstGame.md](../FirstGame.md) and
[ArenaShooter.md](../ArenaShooter.md).

## The script model

- A script is a `struct` deriving from `fly::Script` with `fly::Task Run() override`.
- `FLY_SCRIPT(ClassName)` at namespace scope registers it under its C++ name. Use it once per class.
- `Run()` starts suspended. The engine resumes it once per frame unless it is waiting. `co_return;` ends the script.
- If `Run()` throws, the exception is caught by the host (`Task::Step` rethrows it; how the host reports it was not read).
- **State is lost on hot reload.** Running scripts are destroyed and started again from the top of `Run()`.
- `fly::Print("score = ", score)` prints every argument to the editor log. With a single string argument it prints the
  string.
- `fly::DeltaTime()` is the frame time in seconds.

## Waiting

| Statement | Resumes |
| --- | --- |
| `co_await fly::NextFrame();` | next frame |
| `co_await fly::Wait(2.0f);` | after 2 seconds of game time |
| `co_await fly::WaitFrames(10);` | after skipping 10 frames |

Do not loop without a `co_await`: the game waits for `Run()` to yield.

## Objects

`fly::Object` is a handle to a scene object. A stale handle (the object was deleted) is rejected by the engine: getters
return zero values and setters do nothing. Test a handle with `if (obj)`.

```cpp
fly::Object self  = fly::Object::Self();            // the object the script is attached to (invalid if standalone)
fly::Object door  = fly::Object::Find("Door");      // by name; "Model.Part" searches inside a model
fly::Object crate = fly::Object::Create("Cube");    // "Cube", "Sphere", "Cylinder" or "Wedge"
```

| Group | Members |
| --- | --- |
| Transform | `Position/SetPosition`, `Size/SetSize`, `Rotation/SetRotation` (Euler degrees), `Origin/SetOrigin` |
| Motion | `Velocity/SetVelocity`, `AngularVelocity/SetAngularVelocity`, `AddForce` (a push; call each frame for a steady one), `AddImpulse` (a one-time kick) |
| Look | `Color/SetColor` (`fly::Color3`, 0-255), `Transparency/SetTransparency` |
| Physics flags | `Anchored/SetAnchored`, `CanCollide/SetCanCollide`, `Mass/SetMass`, `GetCollisionAccuracy/SetCollisionAccuracy` (`Box`, `Hull`, `Default`, `Precise`) |
| Lifetime | `Destroy()` (the handle is invalid afterwards), `Clone()`, `Name()`, `SetName()` |
| Unions | `IsUnion()`, `UnionPartCount()`, `Separate()`, `SetNegate/IsNegate`, `SetUseGlobalColor/UseGlobalColor`; free functions `fly::Union(parts)` and `fly::Intersect(parts)`. See [Unions in the script API](../ScriptAPI.md#unions) |
| Scripts | `StartScript(typeName = nullptr)`, `StopScript()`: start the object's attached script or a named one |

New scene objects start **anchored**, which makes physics treat them as static (`ScatteredObject.hpp`); the samples call
`SetAnchored(false)` on spawned cubes so gravity moves them. See [Physics](Physics.md) and
[Object Properties](ObjectProperties.md).

## World services

All under `fly::Game::` unless noted. The full list with signatures is in the header; the later gameplay additions
(casts, joints, camera, particles, decals, tweens, events, UI, navigation, prefabs, foliage) are in
[ScriptAPI.md](../ScriptAPI.md).

| Area | Calls |
| --- | --- |
| `Game::IsPlaying()` | true while the game is running |
| `Lighting` | `GlobalShadows/SetGlobalShadows`, `ShadowQuality/SetShadowQuality`, `Ambient/SetAmbient` |
| `Environment` | `TimeOfDay`, `DayLength`, `Overcast`, `Rain`, `WetGround`, `Fog`, `CloudCover`, `WindSpeed`, `WindDirection`, `Exposure`, `SunIntensity`, `Bloom`, each with a `Set...` partner. Ranges are in the header comments. Setting one inserts the Lighting item it belongs to. |
| `Rendering` | `Grid/SetGrid`, `Wireframe/SetWireframe` |
| `Camera` | `FOV/SetFOV`; `Camera::Get()` / `SetView(position, target, fov)` for the full view |
| `Physics` | `Gravity`, `Friction`, `Restitution`, each with a `Set...` partner |
| Queries (`fly::`) | `Cast(origin, direction, maxDistance)`, `OverlapSphere(center, radius)`, `Touches()` |
| Effects | `Emit`, `Emitter`, `Decal` |
| Others | `Joint`, `Tween<T>`, `Events`, `UI`, `Nav`, `Prefab::Spawn` |

See [Lighting and Graphics](LightingGraphics.md) for what the environment values do, and [Physics](Physics.md) for the
physics values.

## Math helpers

`fly::Vec3` (x, y, z with `+ - *`, `+=`, `-=`), `Dot`, `Length`, `Distance`, `Normalize`, `Cross`, and `Lerp` for floats
and `Vec3`.

## Input, sound and text

- Keyboard: `fly::KeyDown("W")` (held), `fly::KeyPressed("Space")` (went down this frame). Keys are names (strings).
  The exact list of accepted names was not checked.
- Gamepad: `GamepadAvailable(pad)`, `GamepadDown(button, pad)`, `GamepadPressed(button, pad)`, `GamepadAxis(axis, pad)`.
- Sound: `fly::PlaySound("file", volume)`, `fly::SetVolume(volume)`. File names resolve against the project, such as
  `assets/audio/x.wav`.
- Screen text: `fly::ShowText(text, "Top Center", seconds)`.
- Debug text: `fly::DebugText(key, text)`.
- `fly::TimeSincePlay()` is seconds since Play started.

## Attaching and running scripts

- In the Explorer, right-click an object to attach scripts. An object can carry several (`script` plus
  `extraScripts`).
- Scripts marked **run on play** start when Play or Run starts, in the editor and in `FlyPlayer`
  (`ScatteredObject::runOnPlay`, default `true`).
- A standalone script (no object) can be started from the console with `run <ClassName>`.
- The player's body is the object named `PlayerCharacter`.

## Compiling, errors and hot reload

- Compiler lookup: `FLYENGINE_CXX` if set. On Linux, `clang++` then `g++`. On Windows, `cl` (from a developer prompt),
  then `clang++` or MinGW `g++`, then Visual Studio found with `vswhere`.
- Flags on the GCC/Clang path include `-std=c++20 -shared -O2 -g` (`ScriptCompiler.cpp`).
- Output goes to `Scripts/.build/`. The editor also writes `Scripts/compile_flags.txt` so clangd-based editors find
  `fly.hpp`.
- A failed build shows each error in a panel over the viewport. Click one to open the file at that line in VS Code or
  Rider. **The previous library stays loaded** while the build is broken.
- Saving a script triggers a rebuild and reload. Running scripts restart.
- Hot reload can be turned off in the player with `--nohotreload`, and scripts with `--noscripts`.

## Playtests are temporary

When Play starts, the editor snapshots the scene objects and the world settings scripts can change (lighting, grid,
wireframe, field of view, gravity, friction, restitution). **Stop** puts back anything that differs: objects scripts
created disappear and changes to existing objects are undone. Cities are not snapshotted. A playtest restores the whole
Lighting section when it stops.

## Console

The editor console (`` ` ``) takes commands such as `Cube.Position = (0, 5, 0)`, `Physics.Gravity`,
`Environment.Rain = 0.6`, `run Spinner`, `scripts` and `rebuild`. Type `help` for the list.

## Compatibility between SDK and engine

Scripts talk to the engine only through a plain C table of function pointers
(`ScriptingSDK/include/FlyScriptABI.h`, `FLY_SCRIPT_ABI_VERSION` 1). C++ exceptions, allocations and coroutine frames
never cross it, so a script built with any compiler works with an engine built by another.

The wrappers in `fly.hpp` check the size of the table before calling newer functions (for example `HaveActions()`).
On an **older engine, queries find nothing and actions do nothing** instead of crashing.

## Internals

See [Subsystems.md](../Subsystems.md#scripting) for the table of files. In short:

| Piece | File |
| --- | --- |
| SDK | `ScriptingSDK/include/fly.hpp`, `ScriptingSDK/src/fly_module.cpp` |
| ABI | `ScriptingSDK/include/FlyScriptABI.h` (append new entries at the **end** of `FlyApi`; never reorder) |
| Engine side | `src/Engine/Backend/FlyScriptApi.cpp` (the exported `FlyNative_*` functions) |
| Compiler | `src/Engine/Scripts/ScriptCompiler.cpp`, `ScriptDiagnostics.cpp` |
| Host | `src/Engine/Scripts/NativeScriptHost.cpp` (loads, resumes coroutines, hot reload) |
| Console | `src/Engine/Scripts/CommandConsole.cpp` |

To add an API function, follow "Add a script API function" in Subsystems.md.

## Open questions

1. **Exception handling.** What the host does when `Run()` throws (log and stop that script, or more) was not read.
2. **Key names.** The accepted strings for `KeyDown` / `KeyPressed` and gamepad buttons/axes were not checked. Where is
   the authoritative list?
3. **Object defaults.** I did not verify what `Object::Create` sets for `anchored`. The samples call
   `SetAnchored(false)` explicitly, and `ScatteredObject.hpp` says new objects start anchored.
