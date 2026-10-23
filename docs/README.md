# Flyengine documentation

Maintainer documentation. The top-level [README](../README.md) covers building, running and using the engine; these
files explain how the code is organised and how to change it safely.

| Read this | When |
| --- | --- |
| [Architecture.md](Architecture.md) | You are new to the codebase. Targets, layers, the Engine/Entity model, editor and player flows. |
| [CodebaseMap.md](CodebaseMap.md) | You need to find where something lives (every first-party directory, key files, shaders, tests). |
| [Subsystems.md](Subsystems.md) | You are changing a feature: rendering layer, terrain, water, city, physics, scripting, plugins, export, FlyCloud, UI, audio. |
| [FileFormats.md](FileFormats.md) | You are touching anything that is saved or loaded, or bumping a version. |
| [Maintenance.md](Maintenance.md) | You are building, testing, adding a feature (recipes), updating a dependency, or hunting a regression. Also lists hazards. |
| [PBR.md](PBR.md) | Materials, sky reflections, scene-format v33 and the bug it caused. Contains the known-failing-tests list. |
| [guide/Unions.md](guide/Unions.md) | Unions: merge parts into one solid, Negate/Intersect, global colour, Separate, and how they are saved and built. |
| [ScriptAPI.md](ScriptAPI.md) | Gameplay additions in `fly.hpp` (casts, joints, particles, tween, UI, ...). |
| [FirstGame.md](FirstGame.md), [ArenaShooter.md](ArenaShooter.md) | Tutorials for game authors. |
| [../BUGS.md](../BUGS.md) | Bug-hunt report: what was fixed and what is still open. |

## Orientation in five lines

1. `Flyengine` (editor, `src/main.cpp`) and `FlyPlayer` (runtime, `src/PlayerMain.cpp`) both link `FlyEngineCore`.
2. Rendering goes through **FlyRL**, a raylib-compatible layer on sokol (`src/Engine/RL`), with offline-compiled shaders.
3. The world is a list of `Entity` objects owned by `Engine`; scene objects are `ScatteredObject`.
4. Games are scripted in C++ coroutines (hot-reloaded shared library over a C ABI) or visual node graphs.
5. Export appends a compressed archive to a copy of `FlyPlayer`.

## Caveats about this documentation

Written from reading the source, the README, `PBR.md`, `BUGS.md` and the build files. The project was **not built or
run** while writing it. Statements about behaviour come from code and comments; items `BUGS.md` marks as unverified
are carried over as unverified. If something here disagrees with the code, the code wins; fix the doc.
