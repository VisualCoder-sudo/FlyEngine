# Visual Script

Visual Script is the node-graph way to make things happen in your game without writing C++. You wire **event** nodes
(Start, On Update, On Key Pressed, ...) to **action** nodes (Change, Create Object, Play Sound, ...), and feed them values
from **value** nodes (Get Variable, Add, Random Range, ...). The graphs are saved in the scene and run when you press
Play, in the editor and in exported games.

For logic that is easier as code, use [C++ scripts](CppCoding.md), or embed C++ inside a graph with a
[Code Snippet](#code-snippets) node.

> Written from `include/Engine/Backend/VisualScript.hpp`, `src/Engine/Backend/VisualScript.cpp` and the node help text.
> The editor UI (`VisualScriptEditor.cpp`) was not read in detail, so mouse and menu steps are described only where the
> code states them. Open questions are at the end.

- [Where it lives](#where-it-lives)
- [Two kinds of wires](#two-kinds-of-wires)
- [Nodes](#nodes)
- [Targets, properties and values](#targets-properties-and-values)
- [Variables](#variables)
- [Code Snippets](#code-snippets)
- [Sound options](#sound-options)
- [Playing and stopping](#playing-and-stopping)
- [Internals](#internals)
- [Open questions](#open-questions)

## Where it lives

- The editor is the **Visual Script** tab of the bottom panel (next to Assets and Output).
- A scene can hold **several** scripts (graphs); each has a name (default "Script 1"). **All of them run on Play.**
  The manager lets you create, rename, remove and switch between them, and "Open existing" for scripts already in the
  scene.
- Graphs are saved inside the scene file as a `VSCRIPT` block.

## Two kinds of wires

- **Exec wires** carry the flow: they connect an output pin of one node to the single input of the next. An event node's
  output may feed several nodes (fan-out). **End** may take several incoming wires. Connecting to a node replaces any
  wire already entering it.
- **Value wires** carry data into a **value input pin**. Value nodes have no exec pins; they are evaluated when an input
  pin asks for them. A value node wired into two inputs is evaluated once per read, so both see the same value (one
  Random Range for both).
- An input pin you do not wire falls back to a literal typed on the node.

## Nodes

Names are as shown in the editor (`TypeName`). Descriptions follow the in-editor tooltips (`NodeHelp`).

### Events (start a flow)

| Node | What it does |
| --- | --- |
| Start | Runs once when Play starts. Connect the first node of the script here. |
| On Update | Runs every frame while playing. |
| On Stop | Runs once when Play stops, just before everything is put back. |
| On Key Pressed | Runs once each time the chosen key is pressed (not while held). |
| Every N Seconds | Runs repeatedly, every N seconds. |
| On UI Click | Runs when a button is pressed or a toggle flipped (*Clicked*), or a slider moved (*Changed*). |
| On Gamepad Button Pressed | Runs when the chosen gamepad button is pressed. |

Keys offered: Space, Enter, W, A, S, D, Q, E, R, F, Up, Down, Left, Right, 1, 2, 3, 4, Shift, Ctrl.

### Flow control

| Node | What it does |
| --- | --- |
| Branch | True/False outputs. The condition can be wired, or a variable name typed in the field. |
| Sequence | Runs its outputs one after another, top to bottom. |
| Delay | Waits the given seconds, then continues. |
| Repeat | Runs the loop output N times, then continues from *Done*. |
| Do Once | Lets the flow through the first time only. |
| End | Ends the chain. |

### Objects and world

| Node | What it does |
| --- | --- |
| Change | Sets properties of an object (colour, position, size and more). **+ New Property** sets several in one node; each extra property has its own value pin. |
| Set World Property | Sets a world setting (see below). Put back when Play stops. |
| Create Object | Makes a new object of a chosen shape. *Then* continues; *Setup* runs a chain on the new object **before** it gets its physics body, so it appears configured. In a Setup chain, an empty target means the new object. |
| Clone | Copies an object. Its *Setup* output works like Create Object's. |
| Destroy | Removes an object. |
| Teleport | Moves an object to `(x, y, z)`. |
| Set Velocity, Add Force, Add Impulse | Motion. *Add Force* is a steady push (use every frame); *Add Impulse* is a one-time kick. |
| Run Script | Starts the C++ script attached to an object. |
| Object Exists | True if an object with that name is in the scene. |
| Raycast | Gives the name of the first object hit along a line (empty if none). |
| Get Property | Reads a property of an object. |

### Output

| Node | What it does |
| --- | --- |
| Print | Writes a message to the console log, tagged `[script]`. |
| Show Text | Shows text on screen at a chosen spot for N seconds (0 = until replaced; empty text clears). |
| Play Sound, Stop Sound, Set Volume | Audio. See [Sound options](#sound-options). |

### In-game UI

UI Text, UI Panel, UI Bar, UI Image, UI Button, UI Slider, UI Toggle make a widget by **id**; running a node again with
the same id updates that widget. **Set UI** changes a widget that exists (Text, Bar value, Visible, Colour, Background,
Font, Font Size, Anchor, Offset, Size, Image), **Get UI** reads one (including *Exists* and *Clicked*), **Remove UI**
removes one (an empty id removes all). **Free Mouse** (`true`) shows the cursor and stops the camera turning so the
player can click UI; the widgets are useless without it. Widgets are placed by anchor, offset and size; sizes are in
pixels of a 1280 x 720 screen and scale with window height (see [ScriptAPI.md](../ScriptAPI.md#in-game-ui)). Change and
Get Property can also target a UI item as `UI/<id>`.

### Navigation

| Node | What it does |
| --- | --- |
| Build Navigation | Scans the level and builds the walkable map. Run it once after the level exists. Min/Max is the box to scan; Radius and Height size the walker. |
| Move Along Path | Walks an object toward a destination around walls and over steps. Run it every frame (On Update). The object moves by its position, so anchor it. |
| Next Path Point | The next corner on the way between two positions, as `(x, y, z)`; empty if no way. |
| Random Walkable Point | A random walkable position within a radius of a center. |
| Navigation Ready | True once a map has been built. |

### Values

| Group | Nodes |
| --- | --- |
| Variables | Set Variable, Add To Variable, Get Variable |
| Logic | Compare (`== != < > <= >=`), And, Or, Not |
| Maths | Add, Subtract, Multiply, Divide (divide by zero gives 0), Math Function (Min, Max, Abs, Round, Floor, Ceil, Sqrt, Pow, Modulo, Sin, Cos; trig in degrees), Clamp, Random Range, Lerp |
| Vectors | Make Vec3, Vec3 Part, Vec3 Math (`+`, `-`, `scale`), Distance, Normalize |
| Text | Join Text |
| Time | Time (*Delta Time* or *Time Since Play*) |
| Input | Is Key Down, Is Gamepad Button Down, Gamepad Axis (-1..1, triggers rest at -1) |

Vectors are written as text, `"(x, y, z)"`.

## Targets, properties and values

- **Target** is an object path such as `Workspace/Group1/Cube`, or a world group. The editor has a target browser that
  lists the scene's groups and loose objects. Paths with `#2` suffixes pick duplicates by number.
- Change/Get Property properties: `Color`, `Position`, `Size`, `Rotation`, `Transparency`, `Anchored`, `CanCollide`, `Mass`.
- **Set World Property** groups and properties:

| Group | Properties |
| --- | --- |
| Lighting | `GlobalShadows`, `ShadowQuality`, `Ambient` |
| Environment | `TimeOfDay`, `DayLength`, `Overcast`, `Rain`, `WetGround`, `Fog`, `CloudCover`, `WindSpeed`, `WindDirection`, `Exposure`, `SunIntensity`, `Bloom` |
| Rendering | `Grid`, `Wireframe` |
| Camera | `FOV` |
| Physics | `Gravity`, `Friction`, `Restitution` |

These match the `fly::Game::` calls in [C++ coding](CppCoding.md#world-services).

- **Values** use console syntax: `(255, 0, 0)` for a colour or vector, `true`/`false`, plain numbers.

## Variables

Variables are named values. Set Variable stores one, Add To Variable adds to it, Get Variable reads
it. A variable never set counts as `0`. An unwired Branch tests the variable named in its field.

## Code Snippets

A **Code Snippet** node holds C++: the body of a `fly::Script::Run` coroutine, started when the flow reaches the node.
You choose 0 to 6 value inputs and 0 to 6 value outputs. Inside the snippet:

- `In(i)`, `InNum(i)`, `InBool(i)`, `InVec3(i)` read what was wired into input pin `i` when the flow reached the node.
- `Out(i, value)` sets output pin `i` for the nodes wired after it.

How it works: the snippets are written to `Scripts/.visualscript/snippets.cpp` as one hidden class per node (named
`VSnip_...`), compiled together with your other scripts, and started by the node. Compile errors point at
`Code Snippet <Graph>/<n>` with line numbers counted from the snippet's own first line.

**Known limitation:** pin values are kept per node, not per running instance. If the same node is reached again while an
earlier run is still going, or the graph runs on several objects, the runs share and overwrite the same In/Out values.
A snippet that does not compile fails the whole script build. See [C++ coding](CppCoding.md#compiling-errors-and-hot-reload).

## Sound options

Play Sound and Stop Sound keep extra settings: `object` (the sound comes from that object and follows it), `loop`,
`range` (metres; full volume close up, silent at this distance; objects only, default 30), `muffle` (walls between the
object and listener muffle it) and `echo` (the room adds an echo). Wire a Play Sound's **Sound** output into a Stop
Sound's **Sound** input to stop exactly that sound; if nothing is wired, the editor links a Stop Sound to the nearest
Play Sound above it. Sound files are paths relative to the project.

## Playing and stopping

- **Play** runs every Start chain. **Tick** runs On Update chains and pending Delays every frame. **Stop** runs On Stop
  chains and drops pending state.
- Changes graphs make to objects and world settings are undone on Stop (see
  [C++ coding](CppCoding.md#playtests-are-temporary)).
- In an exported game the graphs run as well. Whether Code Snippets work in an export is covered in
  [Distribution](Distribution.md).

## Internals

| File | Role |
| --- | --- |
| `include/Engine/Backend/VisualScript.hpp` | Node enum (append new types before `Count_`), `Node`, `Graph`, save/load, runtime entry points. |
| `src/Engine/Backend/VisualScript.cpp` | Graph model, evaluation, node help text, snippet generation. |
| `src/Engine/Frontend/VisualScriptEditor.cpp` | The node editor (`vscript::DrawEditor`). |
| `src/Engine/GameUI.cpp`, `Navigation.cpp` | The UI and navigation nodes call these. |
| `tests/visual_script_test.cpp` | The graph test (over 500 checks, per Subsystems.md). |

To add a node type: add an enum value before `Count_`, then `TypeName`, `NodeHelp`, pin counts (`DataInputCount`,
`OutputCount`, labels), evaluation, editor UI, and save/load. Do not renumber existing values: scenes store them.

## Open questions

1. **Gamepad numbering.** The header says the gamepad nodes use "gamepad 0"; the tooltips say "gamepad 1". Which should
   the guide tell users?
2. **Variable scope.** I did not read whether variables are shared across scripts (graphs) or per graph, and whether they
   reset on Stop.
3. **Editor controls.** Right-click menus, shortcuts and how nodes are inserted ("Insert Node") were not read from
   `VisualScriptEditor.cpp`. Do you want a step-by-step editor walkthrough? If so, that needs a pass through that file.
