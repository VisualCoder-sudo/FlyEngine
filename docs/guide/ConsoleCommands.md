# Console commands

The editor console (`` ` ``, or the bar at the bottom) runs one command per line. The AI assistant's **Build** mode uses the same
commands: every line of a `fly-console` block is one of these. Names are case-insensitive for shapes, properties and groups; object
names are case-sensitive.

## Building the scene

| Command | What it does |
| --- | --- |
| `spawn <Shape> [Name] [Prop=value ...]` | Creates an object. `Shape` is `Cube`, `Sphere`, `Cylinder` or `Wedge`. |
| `water [Name] [Prop=value ...]` | Adds a water body (a lake, sea or pool). See [Water and terrain](#water-and-terrain). |
| `terrain [Name] [Prop=value ...]` | Adds a terrain (ground you can sculpt, flat or hilly). |
| `city [Name] [Prop=value ...]` | Generates a city: `Position=(x,z)`, `Grid=(x,z)` blocks 2..64 (default 6x6), `CellSize` 15..200 m (default 40), `Organic=true` for curved streets, `Seed=n`. |
| `node <Object> <Type> [Prop=value ...]` | Adds a light, decal or texture to an object. See [Lights, decals and textures](#lights-decals-and-textures). |
| `delnode <Object> <NodeName>` | Removes one of an object's nodes. |
| `union <New> <Part> <Part> ... [intersect]` | Merges 2 or more plain parts (Cube, Sphere, Cylinder, Wedge) into one shape. Parts with `Negate=true` are carved out; `intersect` keeps only the shared volume. The parts are removed. |
| `separate <Union>` | Turns a union back into its parts. |
| `prefab <file.flyprefab> [Position=(x,y,z)]` | Adds a saved prefab from the project. |
| `delete <Name>` | Removes the object, water body, terrain or city. |
| `clone <Name> [NewName]` | Copies an object with its look and settings. Without a new name, a number is added (`Wall2`). |
| `rename <Old> <New>` | Renames an object. |

- A spawned object starts **anchored** (static scenery). Use `Anchored=false` for things that should fall and collide.
- Names are unique. A name that is already taken is an error, so a later line can rely on it. With no name, the shape name is used
  (`Cube`, then `Cube2`, ...). Put a name with spaces in quotes: `spawn Cube "My Box"`.
- Nothing is created if any property in the line is wrong.
- The default size is 1.5 on every axis. The scene is Y-up: `Position=(x, y, z)` with `y` as height, in the same units as `Size`.
  The ground of a new scene is at `y = 0`, so a box 2 tall that stands on it has `y = 1`.

```
spawn Cube Floor Position=(0,-0.5,0) Size=(30,1,30) Color=(90,140,90)
spawn Cube Wall1 Position=(0,2,-15) Size=(30,4,1) Color=(200,200,200)
spawn Sphere Ball Position=(0,6,0) Size=(1,1,1) Color=(220,40,40) Anchored=false
```

## Water and terrain

| Command | Properties |
| --- | --- |
| `water` | `Position=(x,y,z)` (default `(0,0,0)`), `Size=(x,y,z)` (only x and z, the length and width, matter; default `(100,1,100)`), `Height=n` (water level, default 0), `Color=(r,g,b)` or `(r,g,b,a)`, `Transparency=0..1`, `PlayerCollision=true/false` (true: the player floats and swims, false: falls through) |
| `terrain` | `Position=(x,y,z)` (its centre), `Size=(width,depth)` in metres, 50 to 4000 (default `(1000,1000)`), `Height=n` (flat ground level, default 0), `Hills=n` (random hills up to about n metres high, 0 = flat), `Seed=n` (different hills) |

- Names are shared with objects, so a water body or terrain cannot reuse the name of a cube. With no name they are `Water` / `Terrain`
  (then `Water2`, ...). `delete` and `rename` work on them too. `clone` and `Name.Prop = value` do not (delete and make a new one).
- Water is a flat surface at its `Height`. Ground (terrain or cubes) that is lower than that is under water, so for a lake use a terrain
  with `Hills` and a water body at a `Height` between the valleys and the hill tops.
- Terrain is large (the default is 1 km by 1 km). Give small scenes a smaller `Size`.

```
terrain Ground Size=(300,300) Hills=8 Seed=3
water Lake Position=(0,0,0) Size=(80,1,60) Height=2 Color=(20,90,200,150)
```

## Lights, decals and textures

Lights, decals and textures are **child nodes** of an object and move with it: `node <Object> <Type> [Prop=value ...]`.
`Type` is `PointLight`, `SpotLight`, `SurfaceLight`, `Decal` or `Texture`. A light that is not part of anything needs a carrier:
make a tiny invisible object and attach the light to it.

| Property | For | Meaning |
| --- | --- | --- |
| `Name="..."` | all | Node name (default is the type). |
| `Enabled=true/false` | all | |
| `Color=(r,g,b)` | lights | 0..255 (default white). |
| `Brightness=n` | lights | 0..100 (default 1). |
| `Range=n` | lights | Metres (default 16). |
| `Angle=n` | Spot, Surface | Cone half-angle in degrees, 1..89 (default 45). |
| `Face=Front/Back/Left/Right/Top/Bottom` | spot, surface, decal, texture | The side of the object it faces (spot default Bottom, surface Top). |
| `Falloff=Linear/InverseSquare` | lights | |
| `Shadows=true/false` | lights | Cast shadows (default true). |
| `Texture=path` | Decal, Texture | Image inside the project, for example `Textures/brick.png`. |
| `Tint=(r,g,b)`, `Transparency=0..1` | Decal, Texture | |
| `Tile=(x,y)`, `Offset=(x,y)` | Texture | World size of one tile, and shift in tiles. |

A doorway: `spawn Cube Wall Size=(4,3,0.3)`, `spawn Cube Door Size=(1,2,1) Position=(0,-0.5,0) Negate=true`, `union House Wall Door`.

```
spawn Cube Lamp Size=(0.3,0.3,0.3) Position=(0,4,0) Transparency=1 CanCollide=false
node Lamp PointLight Color=(255,200,120) Brightness=3 Range=20
spawn Cube Sign Position=(5,2,0) Size=(4,2,0.2)
node Sign Decal Texture=Textures/logo.png Face=Front
```

Things the console cannot do yet: import meshes, sculpt or paint terrain, edit a city's roads, group objects, change the sky and
lighting beyond what the property commands above cover, and build visual scripts. Say so rather than guessing.

## Object properties

`<Object>.<Property>` shows a property, `<Object>.<Property> = <value>` sets it. `spawn` accepts the same names as `Prop=value`.

| Property | Value |
| --- | --- |
| `Position`, `Size`, `Rotation`, `Origin`, `Velocity`, `AngularVelocity` | `(x, y, z)`. `Rotation` is Euler angles in degrees. |
| `Color` | `(r, g, b)`, each 0-255 |
| `Transparency` | 0 (solid) to 1 (invisible) |
| `Anchored`, `CanCollide` | `true` or `false` |
| `Mass` | a number |
| `CollisionAccuracy` | `Box`, `Hull`, `Default` or `Precise` |
| `Negate` | `true` makes a Cube/Sphere/Cylinder/Wedge a cutter: carved out of a `union` it joins, never collides, not drawn in play |
| `UseGlobalColor` | Unions only. `true` paints the whole union in its `Color` instead of each part's own |

## World settings

`Lighting`, `Environment`, `Rendering`, `Camera` and `Physics` take the same `Group.Property [= value]` form, for example
`Environment.Rain = 0.6`, `Environment.TimeOfDay = 18`, `Physics.Gravity = 9.8`, `Camera.FOV = 70`. The property names are listed in
the scripting docs (`fly::Game::Environment`, `fly::Game::Lighting`, ...).

## Other commands

`help`, `print <text>`, `scripts` (list compiled script classes), `run <Class>` / `stop <Class>` (start or stop a standalone
script), `rebuild` (recompile `Scripts/*.cpp`).

Console changes to the scene are not saved until you save the project; Ctrl+Z undoes them.
