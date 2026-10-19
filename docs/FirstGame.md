# Your first game

A walk through making a small game in Flyengine with a C++ script. Takes about 15 minutes.

## 1. Make the project

1. Start Flyengine. In the Project Manager, type a name under **NEW PROJECT**.
2. Optionally press **Choose from template**. Without one the project is empty. **Coin Dash**, **Physics Playground**
   and **Campfire Valley** are finished samples (sources in `assets/templates/`). Open one and read its script first.
3. Press **Create Project**.

## 2. Add a script

Scripts are C++ coroutines. Create `Scripts/Hello.cpp` in the project folder:

```cpp
#include "fly.hpp"

struct Hello : fly::Script {
    fly::Task Run() override {
        fly::Object crate = fly::Object::Create("Cube");
        crate.SetPosition({0, 5, 0});
        for (;;) {
            co_await fly::Wait(1.0f);
            fly::Emit("Sparks", crate.Position());
        }
    }
};
FLY_SCRIPT(Hello)
```

Press **Play**. The editor compiles the script and runs it in `FlyPlayer`.

## 3. React to the world

```cpp
fly::Vec3 eye = fly::Game::Camera::Get().position;
fly::Hit h = fly::Cast(eye, {0, -1, 0}, 50.0f);
if (h) fly::Decal(h.point, h.normal, 0.5f);

for (const fly::Touch& t : fly::Touches())      // contacts that began this frame
    if (t.speed > 4.0f) fly::Emit("Sparks", t.point, t.normal);
```

## 4. Make it move smoothly

```cpp
fly::Tween<fly::Vec3> rise(door.Position(), door.Position() + fly::Vec3{0, 3, 0}, 1.5f, fly::Ease::Back);
while (!rise.Done()) { door.SetPosition(rise.Step(fly::DeltaTime())); co_await fly::NextFrame(); }
```

## 5. Dress the world

Select a BasicTerrain, open the **Foliage** tab and add grass, flowers, trees, pines, bushes and rocks. Each rule grows
on a painted terrain layer, so painting a layer grows plants there. Wind sway is global.

## 6. Export

Use the export option described in the README. All project files, including `foliage.cfg`, are packed.

See `docs/ScriptAPI.md` for every function.
