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

## In-game UI

Panels, text, bars and images drawn over the 3D view, made by id. Sizes and offsets are in pixels of a 1280 x 720 screen and scale with the window height. Every widget is removed when play starts and stops.

```cpp
UI::Text("score", "Score: 0", { UI::Anchor::TopRight, -20, 20 });
UI::Bar("hp", 1.0f, { UI::Anchor::BottomLeft, 20, -20, 240, 22 }, { 200, 40, 40 });
// each frame
UI::SetText("score", "Score: " + std::to_string(score));
UI::SetValue("hp", health / 100.0f);
```

- `Layout{anchor, x, y, w, h}`: `Anchor` is `TopLeft TopCenter TopRight Left Center Right BottomLeft BottomCenter BottomRight Fill`. x and y move the widget right and down from the anchor, so a right-anchored widget wants a negative x. `Fill` covers the screen with x and y as the margin. Text ignores w and h; an image with w and h 0 keeps its own size.
- `Panel(id, layout, color)`, `Text(id, text, layout, color, fontSize = 24, font = "")`, `Bar(id, value, layout, fill, background)`, `Image(id, file, layout, tint)`. Calling one again with the same id updates it.
- Fonts: `font` is `""` or `"Sans"` (the default, a smooth system font), `"Serif"`, `"Mono"`, `"Pixel"` (the blocky built-in one), or a font file from the project: drop a `.ttf` / `.otf` into `assets/Fonts` and use its file name (`"Roboto.ttf"`; a full path like `"assets/Fonts/Roboto.ttf"` works too). The Visual Script UI Text node lists them in its Font dropdown. Put the file in the project so it ships with the game; Sans/Serif/Mono come from the machine and differ between systems. Letters, digits and Latin-1 accents are available. A font that cannot be loaded falls back to Pixel.
- `SetText`, `SetFont`, `SetValue` (0..1), `SetColor`, `Show` / `Hide`, `Remove`. Unknown ids are ignored.

**Changing items directly.** `UI::Find(id)` (and everything that makes an item, such as `UI::Text`) gives a `UI::Widget`. Read any part of it, change any part of it, and chain the calls:

```cpp
UI::Widget hp = UI::Find("hp");
hp.SetValue(hp.Value() - 0.1f).SetColor({ 220, 60, 60 });
UI::Find("score").SetText("Score: " + std::to_string(score)).SetFontSize(32).SetOffset(-30, 24);
if (UI::Find("tip")) UI::Find("tip").Hide();
```

- Read: `Exists()` (or `if (w)`), `Text()`, `Value()`, `Visible()`, `GetLayout()`, `FontSize()`, `Color()`, `Background()`.
- Change: `SetText`, `SetValue`, `SetColor` (a bar's background stays), `SetBackground`, `SetFont`, `SetFontSize`, `SetImage`, `SetAnchor`, `SetOffset(x, y)`, `SetSize(w, h)`, `SetLayout(layout)`, `Show` / `Hide`, `Remove()`.
- A widget only holds the id, so it is cheap to keep. If the item is gone, reads are empty and changes do nothing.

In Visual Script, **Set UI** changes Text, Value, Visible, Color, Background, Font, Font Size, Anchor, Offset, Size or Image, and the **Get UI** node reads Exists, Text, Value, Visible, Color, Background, Font Size, Anchor, Offset or Size.

The **Change** node also works on UI items: in its Target picker open **UI** and pick an item (the picker lists the ids your UI nodes make). The Property list is then Text, Value, Visible, Color, Background, Font, Font Size, Anchor, Offset, Size and Image, each with a fitting editor (colour picker, checkbox, anchor and font dropdowns), and **+ New Property** sets several at once. **Get Property** reads them back the same way (type `UI/hp` as the target). A Change **overrides** the UI node: it is remembered for that id until Play stops (or a UI Remove), so it works even when it runs before the UI node makes the item, and the UI node's own fields no longer undo it when it runs again.

Not yet: buttons and mouse input, stacks, custom fonts. `ShowText` is separate and still works.

## Prefabs

Save a group in the editor: right-click it in the Explorer > **Save as Prefab**. It is written to `assets/Prefabs/<name>.flyprefab` (nested groups, scripts and flags included). Drag the file from the Assets panel into the viewport to place a copy.

- `Object Prefab::Spawn(file, position)`: adds a copy with its pivot (middle of its footprint, lowest point) at `position`. `file` is project-relative: `"assets/Prefabs/Tree.flyprefab"`. Returns the prefab's first object, or an invalid `Object` if the file is missing. Copies spawned while playing get physics and their `runOnPlay` scripts at once, and are removed when play stops.

## Foliage

Not scripted. Set up per terrain in the terrain panel's **Foliage** tab; saved as `foliage.cfg` beside `basicterrain.bt`.

Besides the six built-in plants, **Import model plant...** makes a plant type from your own 3D model (obj, glb, gltf, fbx). The model is copied into the project. Use **Add another model...** on the type to give it variants: each plant picks one at random by its Chance, so one rule grows a mix of different trees or rocks. A model is scaled to the type's Height; its colours come from the vertex colours, the material colour and the diffuse texture (sampled per vertex, plants are drawn untextured).
