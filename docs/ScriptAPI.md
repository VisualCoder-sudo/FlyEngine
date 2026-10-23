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

Visual Script also has the clickable and navigation nodes:

- **UI Button**, **UI Slider**, **UI Toggle** make the widgets. **On UI Click** (Id, then *Clicked* or *Changed*) runs when the player uses one. **Get UI** also reads Hovered, Held, Clicked and Changed. A slider or toggle keeps what the player set; the node's Value only applies when it makes the widget.
- **Free Mouse** (true/false) shows the cursor and stops the camera turning, so the player can click.
- **Build Navigation** (Min, Max, Cell size, Radius, Height) bakes the walkable map once. **Move Along Path** (object, destination, speed) walks an object there; run it from **On Update**. **Next Path Point**, **Random Walkable Point** and **Navigation Ready** are value nodes.

The **Change** node also works on UI items: in its Target picker open **UI** and pick an item (the picker lists the ids your UI nodes make). The Property list is then Text, Value, Visible, Color, Background, Font, Font Size, Anchor, Offset, Size and Image, each with a fitting editor (colour picker, checkbox, anchor and font dropdowns), and **+ New Property** sets several at once. **Get Property** reads them back the same way (type `UI/hp` as the target). A Change **overrides** the UI node: it is remembered for that id until Play stops (or a UI Remove), so it works even when it runs before the UI node makes the item, and the UI node's own fields no longer undo it when it runs again.

### Buttons, sliders and toggles

```cpp
UI::SetMouseFree(true);                                            // show the cursor, stop the first-person look
UI::Button("play", "Play", { UI::Anchor::Center, 0, 0, 220, 48 });
UI::Slider("vol", 1.0f, { UI::Anchor::Center, 0, 60, 220, 22 });
UI::Toggle("fs", "Fullscreen", false, { UI::Anchor::Center, 0, 100, 220, 24 });
// each frame
if (UI::Find("play").Clicked()) StartGame();
if (UI::Find("vol").Changed())  fly::SetVolume(UI::Find("vol").Value());
if (UI::Find("fs").Changed())   Fullscreen(UI::Find("fs").Value() > 0.5f);
```

- `Button(id, text, layout, color, textColor, fontSize, font)`: `color` is the face (it lightens under the mouse and darkens while pressed). `Slider(id, value, layout, fill, background)` is dragged by the player and holds 0..1. `Toggle(id, label, on, layout, mark, fontSize)` flips on a click; `Value() > 0.5f` is on.
- On a widget: `Hovered()`, `Held()`, `Clicked()` (pressed and released on it) and `Changed()` (a slider moved, a toggle flipped). `Clicked` and `Changed` are true for one frame, so poll them once per frame. The topmost widget under the mouse gets the click.
- `UI::SetMouseFree(true)` releases the cursor and the first-person view stops following the mouse (movement keys still work, so stop moving yourself if the menu pauses the game). `false` locks it again. Everything resets when play starts and stops.
- `UI::MouseOverUI()` is true while the mouse is over a button, slider or toggle: skip shooting then. `UI::MousePosition()` gives the mouse in layout pixels (1280 x 720).
- Not yet: stacks and automatic layout, text fields, disabled buttons, Visual Script nodes for buttons (use C++ scripts). `ShowText` is separate and still works.

## Navigation

Bake the walkable ground once, then ask for paths. It works on whatever the level is made of: floors, ramps, stairs, platforms, terrain, the city. Things that move (the player, crates, projectiles, any object with Anchored off) are not part of the map.

```cpp
fly::Nav::Build({ -60, -5, -60 }, { 60, 30, 60 });            // the box to bake, while playing
fly::Nav::Path p = fly::Nav::FindPath(bot.Position(), player.Position());
if (p.Valid()) { /* p.points: the corners from start to end */ }

fly::Nav::Agent agent(4.0f);                                    // walks a path for you, 4 m/s
agent.SetTarget(player.Position());                             // fine to call every frame
agent.Move(bot, fly::DeltaTime(), 0.75f);                       // an anchored 1.5 m cube: its middle is 0.75 m above its feet

// or move a physics body yourself:
fly::Vec3 v = agent.Step(feet, fly::DeltaTime());               // where to head, flat (y is 0)
body.SetVelocity({ v.x, body.Velocity().y, v.z });
```

- `Build(min, max)` or `Build(Settings)`: `cellSize` (0.5 m), `agentRadius` (0.4), `agentHeight` (1.8), `maxStep` (0.5, the highest step walked up) and `maxSlope` (45 degrees). Gaps narrower than three cells (1.5 m at the defaults) are closed by the agent's radius. A smaller cell is more exact and slower: a 160 x 160 m level at 0.5 m bakes in about 70 ms. Keep the box no bigger than the level; over 4 million squares is refused. Baking again replaces the map; bake after you move or add level geometry.
- The map has a square for every spot an agent can stand: ground that is flat enough, with room for `agentHeight` above, and `agentRadius` away from walls and ledges. Two squares are joined when the step between them is small enough and nothing thin (a wall under half a metre thick) stands between them. Several floors above each other are fine.
- `FindPath(from, to, allowPartial = false)`: both ends are moved onto the nearest walkable ground (a few squares sideways, two agent heights up or down). Empty when there is no way. With `allowPartial` an unreachable goal gives the path to the closest point that can be reached, and `Path::partial` is true. The path is straightened: only the corners remain.
- `Agent(speed, repathSeconds = 0.5, arriveDistance = 0.4)`: `SetTarget`, `Step(position, dt)`, `Move(...)`, `Arrived()`, `HasPath()`, `NextPoint()`, `Stuck()`, `ClearTarget()`. It asks for a new path every `repathSeconds` and when the target has moved over a metre, so it follows a moving target and goes round new obstacles after the next bake. Keep `arriveDistance` no bigger than `cellSize`.
- `Agent::Move(Vec3& feet, dt)` steps and moves `feet` with `MoveAlong` (so the agent slides along walls and never leaves the map; `feet.y` follows the ground) and returns the flat velocity it really moved at (slower while sliding along a wall, zero when blocked or when `feet` is nowhere near the map). `Move(Object body, dt, feetToCentre = 0)` does the same for an anchored object moved by its position; `feetToCentre` is how far its position sits above its feet (half the height for a cube whose middle is its position). For objects physics moves, use `Step` and `SetVelocity`.
- Before heading for the next corner the agent checks that the straight line from where it really stands is clear; if not it goes back to the corner it came from or asks for a new path (at most every 0.25 s). If it wants to move but gets nowhere for `SetStuckTime(seconds)` (1 s; 0 turns it off) it asks for a new path; if that does not help, `Stuck()` is true until it moves again (a target that keeps moving does not reset it). It never picks another target by itself: do that when `Stuck()` says so.
- `MoveAlong(from, delta, out)`: moves a point by the flat part of `delta` over walkable ground, sliding along walls and ledges instead of crossing them; `out` is always on the map and `out.y` is the ground height. A `from` up to two squares off the map is pulled back onto it. Use it for anything else that pushes an agent (knock-back, keeping bots apart). False (and `out = from + delta`) when there is no map or no ground near `from`. `LineClear(from, to)`: true when an agent at `from` can walk straight to `to`. `CanMove()`: false on an engine too old for these two.
- `Nearest(p, out)`, `IsWalkable(p)`, `RandomPoint(center, radius, out)` (a place to patrol to), `Ready()`, `Clear()`.
- `ShowDebug()` draws the squares (green, orange at the edges) and the last path in the 3D view. Use it to see why a bot will not go somewhere.
- Not yet: doors and other changes while playing (bake again), off-mesh links such as jumps and ladders, crowds avoiding each other, and a Visual Script node.

## Unions

Solid modelling from a script, the same as the editor's **Union** dropdown (see [guide/Unions.md](guide/Unions.md)). Only primitive parts (cube, sphere, cylinder, wedge) can be unioned.

- `Object fly::Union(parts)`: merges two or more objects into one solid and removes the parts (their handles become invalid). Every face keeps its part's colour. A part marked `SetNegate(true)` is carved out of the result. Returns an invalid `Object`, with nothing changed, when it cannot be built: fewer than two parts, a stale handle, the same part twice, an imported mesh or an existing union among them, or nothing left after the cuts.
- `Object fly::Intersect(parts)`: like `Union`, but keeps only the volume all the parts share.
- `Object::Separate()`: removes a union and returns its original parts as a `std::vector<Object>` (names, scripts and nodes restored, placed where the union now is); the union's handle becomes invalid. Empty if the object is not a union.
- `Object::IsUnion()`, `UnionPartCount()`.
- `Object::SetNegate(bool)` / `IsNegate()`: marks a primitive as a cutter. A cutter is not drawn in play and never collides, so make it a cutter before you union it (or leave it as an invisible helper). `SetNegate` turns `CanCollide` off or on with it.
- `Object::SetUseGlobalColor(bool)` / `UseGlobalColor()` (unions only): off, every face keeps its part's colour; on, the whole union is drawn in the object's own `Color()`.
- All of these do nothing (and return false / invalid / empty) on an engine from before unions: check `Object::IsValid()` on the result.

Made while playing, the union gets a physics body and is removed again when play stops, like any object a script creates. Its collider is built from the merged mesh with **Default** accuracy; set `SetCollisionAccuracy(CollisionAccuracy::Precise)` when you need a carved hole to be a real hole for rays and bodies (a hull collider fills it).

```cpp
fly::Object post = fly::Object::Create("Cube");
fly::Object beam = fly::Object::Create("Cube");
fly::Object hole = fly::Object::Create("Cylinder");
post.SetColor({ 200, 60, 40 });  beam.SetColor({ 40, 90, 200 });
hole.SetNegate(true);
fly::Object arch = fly::Union({ post, beam, hole });   // post and beam keep their colours; the hole is cut through
if (arch) {
    arch.SetCollisionAccuracy(fly::CollisionAccuracy::Precise);
    arch.SetUseGlobalColor(true);                       // or paint it all one colour
    arch.SetColor({ 120, 120, 120 });
}
```

Not yet: Visual Script nodes for unions (the editor and C++ scripts only).

## Prefabs

Save a group in the editor: right-click it in the Explorer > **Save as Prefab**. It is written to `assets/Prefabs/<name>.flyprefab` (nested groups, scripts and flags included). Drag the file from the Assets panel into the viewport to place a copy.

- `Object Prefab::Spawn(file, position)`: adds a copy with its pivot (middle of its footprint, lowest point) at `position`. `file` is project-relative: `"assets/Prefabs/Tree.flyprefab"`. Returns the prefab's first object, or an invalid `Object` if the file is missing. Copies spawned while playing get physics and their `runOnPlay` scripts at once, and are removed when play stops.

## Foliage

Not scripted. Set up per terrain in the terrain panel's **Foliage** tab; saved as `foliage.cfg` beside `basicterrain.bt`.

Besides the six built-in plants, **Import model plant...** makes a plant type from your own 3D model (obj, glb, gltf, fbx). The model is copied into the project. Use **Add another model...** on the type to give it variants: each plant picks one at random by its Chance, so one rule grows a mix of different trees or rocks. A model is scaled to the type's Height; its colours come from the vertex colours, the material colour and the diffuse texture (sampled per vertex, plants are drawn untextured).
