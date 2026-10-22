# Object Properties

The **Properties** panel edits whatever is selected in the viewport or the Explorer. For a scene object it shows
General, Material, Water interactions, Position, Size, Rotation, Origin and velocity sections. Lights, decals and
repeating textures are **child nodes** of an object and have their own panel. A selected water body shows a different set
of properties (see [Water](Water.md)).

> Written from `src/Engine/Frontend/ui_properties.cpp`, `ui_nodes.cpp`, `ScatteredObject.hpp` and `ObjectNode.hpp`. Not
> built or run. The Explorer's right-click menus and Lighting panel were not read here. Open questions are at the end.

- [Selecting and multi-edit](#selecting-and-multi-edit)
- [General](#general)
- [Material](#material)
- [Water interactions](#water-interactions)
- [Transform: Position, Size, Rotation, Origin](#transform-position-size-rotation-origin)
- [Velocity](#velocity)
- [Scripts on an object](#scripts-on-an-object)
- [Child nodes: lights, decals, textures](#child-nodes-lights-decals-textures)
- [Groups and models](#groups-and-models)
- [Object properties from code](#object-properties-from-code)
- [Internals](#internals)
- [Open questions](#open-questions)

## Selecting and multi-edit

With nothing selected the panel says "Select an object in the viewport or the Explorer to edit it here." When several
objects are selected the title shows how many (for example "3 selected"). Most edits in the General, Material and Water
sections are applied to **every** selected object, not only the primary one. Edits push an undo step.

## General

| Property | Meaning |
| --- | --- |
| Colour | The object's colour. Disabled while the game is playing. |
| Anchored | When on, physics treats the object as static: gravity, forces and impulses are ignored. **New objects start anchored.** Turn it off to let an object fall. |
| Can collide | When off, the object ignores all collisions, including the ground. |
| Collision | Collision accuracy: **Box**, **Hull**, **Default**, **Precise**. Greyed out for plain cubes, which are always Box. |
| Transparency | 0 is opaque, 1 is invisible. |
| Mass | Automatic by default (derived from the object's size and shape). Typing a value switches to a manual mass; tick **Auto** to go back. Changing the size resets a manual mass to automatic. |
| Mesh | The model file name, for imported models. |
| Animation | For imported models with skeletal clips: shows `current/total  name`. Click to go to the next clip, right-click for the previous, and the button beside it pauses or plays. |
| Texture | A texture file for the object, and a **Preset** drop-down of textures that ship with the editor. Presets are copied into the project's `PresetTextures` folder so the scene saves a portable path. The **X** button clears it. |

How accurate each collision mode is (what shape is built for each) is described in `include/Engine/PhysicsCollision.hpp`
(`Box`, `Hull`, `Default`, `Precise`); see [Physics](Physics.md).

Imported model formats: glTF/GLB, OBJ, PLY, IQM, M3D, VOX and FBX (through ufbx, baked to its bind pose, so
FBX models have no animation clips). Skeletal clips play the chosen clip; the chosen clip, loop, play and speed are saved
with the scene.

## Material

A physically based material. Details of how it is lit: [Lighting and Graphics](LightingGraphics.md) and
[PBR.md](../PBR.md).

| Property | Meaning |
| --- | --- |
| Model's own | Only for imported models. On (the default): the model uses its file's materials and maps. Off: the object's own values below replace them on every part (maps still modulate them). |
| Metallic | 0 is plastic, wood or stone; 1 is bare metal (the colour tints the reflection). |
| Roughness | 0 is a mirror, 1 is fully matte. Default 0.6. |
| Glow | Emissive strength (0 to 1000; 0 gives off no light, 1 is as bright as a white surface in full sun) and the glow colour. A non-zero glow always applies. |

Editing Metallic or Roughness on a model switches it to its own material, otherwise the edit would not show. Primitive
shapes always use these values.

## Water interactions

Three switches for how the object behaves while it is in a water body (applied by the physics simulation):

| Switch | Meaning |
| --- | --- |
| Floats | Buoyancy. Off means the object sinks. |
| Foam | Leaves a wake or foam trail. |
| Splash | Splash and spray on entering the water. |

All default to on.

## Transform: Position, Size, Rotation, Origin

Collapsible groups of three numbers each:

| Group | Fields | Notes |
| --- | --- | --- |
| Position | Pos X, Pos Y, Pos Z | World position of the object's centre. |
| Size | Width, Height, Length | Minimum 0.01. A new size resets automatic mass. |
| Rotation | Rot X, Rot Y, Rot Z | Euler angles in degrees. |
| Origin | Origin X, Origin Y, Origin Z | Editor pivot: a local offset from the object's centre that the move and rotate gizmos attach to. |

## Velocity

*Linear velocity* (Vel X/Y/Z) and *Angular velocity* (Ang X/Y/Z). Before Play they are the **starting** velocities, applied
when Play begins. During Play the fields show and edit the live physics body.

## Scripts on an object

An object carries one primary script and any number of extra scripts (right-click it in the Explorer to attach them).
**Run on play** (default on) starts them when Play or Run starts, in the editor and in `FlyPlayer`. See
[C++ coding](CppCoding.md#attaching-and-running-scripts).

## Child nodes: lights, decals, textures

Right-click an object in the Explorer to insert a child node. A node moves with its object. Selecting one shows the node
panel over the properties area, headed by the node type and "on <object name>".

Common to all nodes: **name**, **Enabled**, and (except for Point Light) a **Face** (Front, Back, Left, Right, Top,
Bottom: the object's local -Z, +Z, -X, +X, +Y, -Y). A light's face is "Points out of"; a decal or texture's face is
"Projects onto".

### Lights (SpotLight, PointLight, SurfaceLight)

| Setting | Range | Applies to |
| --- | --- | --- |
| Colour | any | all lights |
| Brightness | 0 to 20 | all lights |
| Range (metres) | 0.5 to 200 | all lights |
| Cone angle (degrees) / Spread angle | 5 to 89 | Spot / Surface |
| Size (fraction of the face) | 0.05 to 1 | Surface (emitter rectangle) |
| Falloff | Linear or Inverse square (default) | all lights |
| Cast shadows | on/off | all lights; the object and player shadow the light |
| Shadow resolution | 128 to 4096 | when Cast shadows is on |

At most N lights cast shadows at once; N is the Lighting setting `maxShadowCasters` (shown in the panel under
*Lighting > Time*). New nodes start with a Spot light facing Bottom and a Surface light facing Top.

### Decal and Texture

| Setting | Meaning |
| --- | --- |
| Image | A file chosen with **Choose file...** (png, jpg, jpeg, bmp, tga, webp). Files outside the project are copied to `assets/NodeTextures/`. A **Preset texture...** drop-down offers the editor's presets. **Clear** removes it. |
| Tint | Colour multiplier. |
| Transparency | 0 is opaque, 1 is invisible. |
| Studs per tile (X, Y) | Texture only: world size of one tile (minimum 0.05; Decal defaults to 1, Texture to 4). |
| Offset (X, Y, in tiles) | Texture only: shift across the face. |

A **Decal** covers the whole face with the image. A **Texture** node repeats the image across the face and keeps its size
when the part is resized.

## Groups and models

Objects can be grouped. A group (`ModelGroup`) can nest inside another, can be hidden or locked in the Explorer (the
flags apply to everything below it), and members stay in the flat scene list so physics and scripts still see them.
Plugins can mark a group as theirs; see [CPluginAPI](CPluginAPI.md#editor-only-features-api-11-and-12). The Explorer's
eye and lock icons hide an object (not drawn) or lock it (cannot be picked in the viewport).

## Object properties from code

| Property | C++ (`fly::Object`) | Visual Script (Change) |
| --- | --- | --- |
| Position, Size, Rotation | `Position/SetPosition`, `Size`, `Rotation` | `Position`, `Size`, `Rotation` |
| Colour | `Color/SetColor` | `Color` |
| Transparency | `Transparency/SetTransparency` | `Transparency` |
| Anchored, Can collide | `Anchored`, `CanCollide` | `Anchored`, `CanCollide` |
| Mass | `Mass/SetMass` | `Mass` |
| Collision accuracy | `Get/SetCollisionAccuracy` | not in the Change list |
| Origin, velocities | `Origin`, `Velocity`, `AngularVelocity` | `Set Velocity` node |

See [C++ coding](CppCoding.md#objects) and [Visual Script](VisualScript.md#targets-properties-and-values). Metallic,
roughness and glow are not exposed to scripts in `fly::Object`.

## Internals

| File | Role |
| --- | --- |
| `include/Engine/Backend/ScatteredObject.hpp` | The object: transform, physics flags, `surface`, `waterInteraction`, scripts, `nodes`. `Clone()` must copy every new field or duplicate and paste drop it. |
| `include/Engine/Backend/ObjectNode.hpp` | One plain struct for every node kind; fields a kind does not use are ignored. |
| `include/Graphics/SurfaceMaterial.hpp` | `metallic`, `roughness`, `emissive`, `emissiveStrength`. |
| `src/Engine/Frontend/ui_properties.cpp` | The Properties panel. |
| `src/Engine/Frontend/ui_nodes.cpp` | The child node panel. |

To add a node kind, see "Scene objects and nodes" in [Subsystems.md](../Subsystems.md#scene-objects-and-nodes). To add a
saved field, follow the recipe in [Maintenance.md](../Maintenance.md#add-a-saved-field); scenes carry a version number.

## Open questions

1. **Collision accuracy semantics.** The names are clear (Box, Hull, Default, Precise) but I did not read what Default
   does per shape or how Precise is built. Should the guide describe this, and is Precise meant for static geometry only?
2. **Explorer menus.** Which child node kinds the right-click menu offers, and how scripts are attached from it, were not
   read from `ui_explorer.cpp`.
3. **Check "Model's own" wording.** The panel says "Model's own"; the code comment says it keeps "its file's materials".
   I describe it as above; confirm that wording is right for users.
