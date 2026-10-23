# Unions

A **union** merges several parts into one solid object, like Roblox Studio's solid modelling. The result is a single
mesh with one outer surface (the faces where the parts overlapped are gone). It keeps the colour of each part unless
you tell it to use one colour. Parts can also **cut** the union (Negate) or be reduced to their shared volume (Intersect),
and a union can be taken apart again (Separate).

> Written from `Csg.cpp`, `ScatteredObject.cpp`, `ScenePersistence.cpp`, `ui_explorer.cpp`, `ui.cpp`,
> `ui_properties.cpp`, `ObjectInteractionManager.cpp` and `tests/union_test.cpp`.

- [Making a union](#making-a-union)
- [Negate: cutters](#negate-cutters)
- [Intersect](#intersect)
- [Colour](#colour)
- [Separate](#separate)
- [Physics and collision](#physics-and-collision)
- [What a union can and cannot do](#what-a-union-can-and-cannot-do)
- [From scripts](#from-scripts)
- [Saving, undo, duplicate, prefabs](#saving-undo-duplicate-prefabs)
- [Internals](#internals)
- [Tests](#tests)

## Making a union

1. Select two or more **primitive** parts (cube, sphere, cylinder, wedge) in the viewport or the Explorer.
2. Right-click, open the **Union** dropdown and choose **Union**, or press **Ctrl+Shift+G**.

The dropdown is in both the viewport's right-click menu (above *Transform space*) and the Explorer's object menu. It only
lists what applies to the selection, and is left out when nothing does:

| Entry | Shown when |
| --- | --- |
| **Union** (Ctrl+Shift+G) and **Intersect** | Two or more plain primitives are selected. Imported meshes and existing unions are not accepted. |
| **Negate** / **Un-negate** | The selected primitive is not already a union. |
| **Separate** (Ctrl+Shift+U) | A union is in the selection. |

The original parts are replaced by one object named *Union*, centred on the result, with a **multicubes** icon in the
Explorer. It takes its anchored and collide flags from the first part that is not a cutter. If nothing is left (all
cutters, or the cutters remove everything) the scene is left untouched and the log says why.

## Negate: cutters

**Negate** marks a part as a cutter. A cutter is drawn as a translucent red ghost in the editor, never collides, casts no
shadow and is **not drawn in play or in an exported game**. When you make a union that includes cutters, they are
carved out of the result:

- the cutter's faces become the walls of the hole, in the **cutter's** colour,
- a cutter that sticks out past the solid does not enlarge the union.

Negate is in the Union dropdown (**Negate** / **Un-negate**). Turning it on turns **Can collide** off, turning it off
turns it back on.

## Intersect

**Intersect** (from the dropdown) makes a union of only the volume all selected parts share. The result keeps the colours
of the faces that survive. If the parts do not overlap there is nothing to build and the scene is left untouched.

## Colour

Each face keeps the colour of the part it came from. In Properties > General, a union shows:

- **Use global colour**: off by default. Turn it on and the whole union is drawn in the object's **Global colour** (the
  normal colour field, enabled only while the checkbox is on). Turn it off and the per-part colours come back; nothing is
  rebuilt, so it is instant.
- **Union**: how many parts it holds.

Transparency, material (metallic, roughness, emission) and texture are per union, not per part. The union's material
starts as the first part's. The default checker texture is laid out in world scale: one repeat per 2 m, so every part of
a union has the same cell size (it stretches with the union's size afterwards, like any object).

## Separate

**Separate** (dropdown or **Ctrl+Shift+U**) removes the union and puts the original parts back, with their names,
scripts, nodes (lights, decals, textures) and colours. They are placed where the union now is, so moving, scaling or
rotating the union first moves, scales or rotates the parts with it. Parts that were cutters come back as cutters.

Scale is applied per axis, so a union stretched along one axis and rotated by anything other than 90-degree steps
separates into parts that are slightly sheared in appearance (the parts themselves stay boxes/spheres/cylinders).

## Physics and collision

A union is one rigid body whose collider is built from the merged mesh, with the same accuracy choices as an imported
mesh (**Box**, **Hull**, **Default**, **Precise**; default **Default**). Use **Precise** for something you need holes in:
rays then pass through the hole of a Negate cut (tested), while a hull collider fills it. A union that is not anchored
falls and lands like any other object. Mass is automatic from the union's bounding size.

## What a union can and cannot do

- **Primitives only.** Imported meshes (animated ones included) and existing unions cannot be parts. (A union of unions is
  not supported; Separate first.)
- **Convex parts.** Every engine primitive is convex, which is what the one-pass evaluator relies on.
- **Faces in the same place.** Where two parts have a face exactly on top of each other (for example two cubes touching or
  overlapping with equal faces), the earlier part's face and colour is kept; there are no doubled or flickering faces.
- **Size.** Cost grows with the parts' triangle counts. Measured in the tests: 10 spheres and 3 cylinder cutters (about
  12,900 triangles) build in roughly half a second. Every undo step and play-stop restore rebuilds each union from its
  parts, so a scene full of very large unions will make those slower.
- **Negative sizes** (mirrored parts) are not supported.

## From scripts

C++ scripts can make and take apart unions too: `fly::Union(parts)`, `fly::Intersect(parts)`, `Object::Separate()`,
`SetNegate` and `SetUseGlobalColor`. The rules are the same as in the editor; see [Unions in the script API](../ScriptAPI.md#unions)
for the list and an example.

Visual Script has the same through two nodes, **Union** (with a *Do* choice of Union or Intersect, a name for the result
and a list of parts) and **Separate**, plus the `Negate` and `UseGlobalColor` properties on the Change and Get Property
nodes. See [Visual Script](VisualScript.md#objects-and-world). A typical graph: Change `VsCut.Negate = true`, Union
(`VsA`, `VsB`, `VsCut`, name `Arch`), then Change `Arch.UseGlobalColor = true`.

## Saving, undo, duplicate, prefabs

A union is saved with its parts (scene format **34**), not as a baked mesh: the mesh is rebuilt when the scene loads,
so scene files stay small and a saved union can always be separated. Undo, redo, Play/Stop restore, Ctrl+D duplicate,
prefabs and scripts that clone the object all go through the same record and `Clone()` code, so they keep unions
intact. A scene from before format 34 loads normally. A scene saved with unions needs a build that reads format 34.

If a union somehow rebuilds empty on load, the scene still loads, the parts are kept and the log says so; use
**Separate** to recover them.

## Internals

| File | Role |
| --- | --- |
| `include/Engine/Backend/Csg.hpp`, `src/Engine/Backend/Csg.cpp` | Pure geometry, no raylib. `Evaluate` builds a whole union in one pass from convex parts (Add / Negate / Intersect); `Union`, `Subtract`, `Intersect` are the classic BSP booleans for any closed solid. Polygons carry a colour. |
| `include/Engine/Backend/ScatteredObject.hpp`, `.cpp` | `MakeUnion`, `RebuildUnionMesh`, `TakeUnionParts`, `SetUseGlobalColor`, `negate`, `UnionPart` / `UnionMode`. The mesh is built here and owned by the object (`ownsProceduralMesh`), not shared. |
| `src/Engine/Frontend/ScenePersistence.cpp` | The v34 fields: `negate`, `isUnion`, then for a union `useGlobalColor`, part count and one nested object record per part (with its mode). |
| `src/Engine/Backend/VisualScript.cpp`, `src/Engine/Frontend/VisualScriptEditor.cpp` | The Visual Script side: node types `Union` (73) and `Separate` (74) (the parts are the node's target plus its extra rows), their run code, the Part rows with a picker per slot, and the `Negate` / `UseGlobalColor` properties. |
| `src/Engine/Scripts/ScriptRuntime.cpp`, `src/Engine/Backend/FlyScriptApi.cpp`, `ScriptingSDK/include/fly.hpp` | The script side: `UnionObjects` / `SeparateUnion` (they register physics bodies while playing), the `FlyNative_Union*` functions, the appended `FlyApi` members and the `fly::` wrappers. |
| `src/Engine/Frontend/ObjectInteractionManager.cpp` | `ApplyUnionRequest`: Union, Intersect and Separate run here because it owns the engine and the object list. |
| `src/Engine/Frontend/ui.cpp`, `ui_explorer.cpp`, `ui_properties.cpp` | The viewport and Explorer Union dropdowns (`UnionMenuEntries`, `RequestUnion`), the properties rows and the row icon. |

Drawing a union is the ordinary imported-model path: vertex colours multiply the object's tint, so with *Use global
colour* off the tint is white and the colours come from the mesh. With it on, the mesh colours are set to white and the
tint is the object's colour (`ApplyUnionColors` swaps them without a rebuild).

To change what a part contributes, or to allow another part kind, remember the rest of the chain: the part's polygons
come from `PartPolygons` in `ScatteredObject.cpp`, the saved record, `Clone()`, and the menu conditions in
`CanUnionSelection`.

## Tests

`tests/union_test.cpp` (ctest name `union`, needs a display like the other engine tests). It covers: the BSP booleans
and the one-pass evaluator on boxes (volumes, surface area, touching and identical faces, colour per face); union /
subtract / intersect of cubes and of a cylinder with a rotated sphere (inclusion-exclusion on mesh volumes); per-face
colours and the global colour switch; save/load, scenes from before format 34, snapshot restore and clone; Separate with a
moved, stretched and turned union; timing of a 13-part stress union; physics (a dropped union lands, rays pass through a
Negate hole and through a free cutter). The script API is covered by `tests/fly_actions_test.cpp` (wrappers against a fake table, and an engine without the union functions) and `tests/script_actions_engine_test.cpp` (the real engine: union, ray through the hole, separate, intersect, stale and unsupported inputs, and a Visual Script graph that unions, recolours and separates). The nodes' behaviour with a stubbed runtime (parts in order, names, refusals, save and load) is in `tests/visual_script_test.cpp`.

Set `FLY_UNION_SHOT=<file.png>` to also render a union next to a plain cube for a visual check.
