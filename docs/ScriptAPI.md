# Script API: gameplay additions

All in `ScriptingSDK/include/fly.hpp`. On an older engine, queries find nothing and actions do nothing.

## Queries

| Call | Result |
|---|---|
| `Hit Cast(origin, direction, maxDistance = 100)` | First hit: `hit`, `point`, `normal`, `distance`, `object` (invalid for terrain/city). `if (hit)` works. |
| `std::vector<Object> OverlapSphere(center, radius, maxCount = 64)` | Scene objects whose bounds touch the sphere. |
| `std::vector<Touch> Touches(maxCount = 128)` | Contacts that began since the last physics update: `a`, `b`, `point`, `normal`, `speed`, `Involves(o)`, `Other(o)`. |

## Joints

`Joint::Weld(a, b, anchor)`, `Joint::Hinge(a, b, anchor)`, `Joint::Ball(a, b, anchor)`, `Joint::Rope(a, b, anchorA, anchorB)`, then `Destroy()`.

## Camera

`Game::Camera::Get()` returns `View{position, target, fov}`. The camera sits at the player's eye, so this is also the player position. `SetView(position, target, fov = 0)` moves it; call each frame to keep hold.

## Particles and decals

Presets: `"Smoke"`, `"Fire"`, `"Sparks"`, `"Dust"`, `"Splash"`, `"Debris"`, `"Steam"`.

- `Emit(preset, position, direction = {0,1,0}, count = 0, scale = 1)`: one-shot burst.
- `Emitter::Start(preset, position, direction, scale)`, then `Move(...)`, `Stop()`: continuous.
- `Decal(position, normal, size, ColorRGBA, seconds = 30)`: mark on a surface. Ring of 256.

Hard physics impacts already kick up dust by themselves.

## Tween and events

- `Tween<float|Vec3>(from, to, seconds, Ease)` with `Step(dt)`, `Value()`, `Done()`, `Restart()`. `Ease`: `Linear In Out InOut Bounce Back`.
- `Events::On(name, fn(float))` returns a subscription id. `Events::Off(id)`. `Events::Fire(name, value)`. Handlers run immediately and are lost on hot-reload.

## Foliage

Not scripted. Set up per terrain in the terrain panel's **Foliage** tab; saved as `foliage.cfg` beside `basicterrain.bt`.

Besides the six built-in plants, **Import model plant...** makes a plant type from your own 3D model (obj, glb, gltf, fbx). The model is copied into the project. Use **Add another model...** on the type to give it variants: each plant picks one at random by its Chance, so one rule grows a mix of different trees or rocks. A model is scaled to the type's Height; its colours come from the vertex colours, the material colour and the diffuse texture (sampled per vertex, plants are drawn untextured).
