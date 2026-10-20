# Making an arena shooter (a Counter-Strike style game)

How to build a round-based first-person shooter with AI bots in Flyengine, using the **Counter Guns** project as the worked example. It covers the project layout, the map, the player, the bots, the round loop, the HUD, and how to test it without playing by hand. It also lists the engine traps that cost the most time, so you can skip them.

Counter Guns lives in `~/FlyengineProjects/Counter Guns/`. Read its code alongside this guide. Read `docs/FirstGame.md` and `docs/ScriptAPI.md` first if you have not written a Flyengine script before.

Everything here was checked against the engine at the time of writing. Where something was only read from the source or inferred, the text says so.

## 1. What you are building

A round-based match: two teams, a freeze period, a timed live phase, a result screen, first team to N round wins takes the match. One human player on one team. Everyone else is a bot that patrols, spots enemies, chases, shoots, takes cover and dies.

Split the work into four parts, because they have different failure modes:

| Part | Files in Counter Guns | What it does |
|---|---|---|
| Map | `Counter Guns.flyproj`, `tools/gen_map.py`, `Scripts/CsMap.cpp` | Walls, floors, cover, ramps, spawn and waypoint markers, lighting |
| Player | `Scripts/CgPlayer.cpp` | Weapons, ammo, reload, hitscan, health, death, first-person gun model |
| Bots | `Scripts/CgBots.cpp` | Sight, patrol, chase, cover, firing with inaccuracy |
| Match and HUD | `Scripts/CounterGuns.cpp`, `Scripts/CsHud.cpp`, `Scripts/CsVisuals.hpp` | Round state machine, score, timer, kill feed, crosshair |

## 2. Project layout

```
My Game/
  My Game.flyproj        the scene: map geometry, templates, lighting
  Scripts/
    MyGame.cpp           the match script
    Bots.cpp, Player.cpp
    Hud.cpp, Visuals.hpp
    compile_flags.txt    -std=c++20 and -I path to ScriptingSDK/include (for your editor only)
  tools/gen_map.py       optional: writes the map into the .flyproj
```

Create the project from the Project Manager with the Blank template, or from a terminal:

```
Flyengine --create-project "My Game" --template Blank
```

Scripts are C++ coroutines that include `fly.hpp`. The engine compiles everything in `Scripts/` into one library, so scripts can share headers and functions across files. Standalone scripts (such as a HUD builder) must be marked "run on play" in the editor. A script that has to wait for another one polls for it, for example waits for a marker object to exist.

## 3. The player

The engine already has a first-person character controller. It is an object named `PlayerCharacter` with WASD, Space to jump, mouse look, sprint on Shift and crouch on Ctrl. Do not write your own movement. Find the body with `fly::Object::Find("PlayerCharacter")` and read its position. The camera is `fly::Game::Camera::Get()`.

What your script adds on top:

- Health, alive flag and respawn at a spawn marker.
- Weapons: a small table with damage, fire rate, magazine size, reload time, spread and range.
- A trigger. Left click is `KeyDown("Mouse Left")` for held fire and `KeyPressed("Mouse Left")` for one shot per click. The mouse button names need an engine built after the mouse-button change. An older engine ignores them.
- Hitscan. Cast a ray from the camera along the view direction, with a random cone of spread added.

### Hitscan and the ray that hits yourself

The camera sits inside the player's own capsule, so a plain `fly::Cast` from the eye hits the player first. Counter Guns wraps this in `CastSkip(origin, dir, maxDist, skipObject)` (in `CgBots.cpp`): cast, and if the hit object is the one to skip, step the origin just past it and cast again, up to four times. Use the same helper for bots, which have their own colliders.

```cpp
Hit CastSkip(Vec3 origin, Vec3 dir, float maxDist, Object skip) {
    float left = maxDist;
    for (int i = 0; i < 4; i++) {
        const Hit h = Cast(origin, dir, left);
        if (!h) return h;
        if (!skip.IsValid() || h.object != skip) return h;
        const float step = h.distance + 0.02f;   // started inside the skipped body: step past it
        left -= step;
        if (left <= 0.0f) return Hit{};
        origin = origin + dir * step;
    }
    return Hit{};
}
```

Headshots are a damage multiplier when the hit point is in the top part of the target's collider (Counter Guns uses 2.5 times).

### Feedback

Without feedback, working guns look broken. Add at least: an ammo counter, a hit marker on the crosshair, impact particles or a decal (`fly::Emit`, `fly::Decal`), and a kill feed line. Sounds and tracers are not in Counter Guns yet and are the first thing a player misses.

## 4. The map

### Build the map in the scene file, not from a script

This is the most important rule in this guide.

A script that calls `Object::Create("Cube")` while the game is running gets a physics body with a default 1.5 metre cube collider. `SetSize`, `SetAnchored` and `SetCanCollide` change fields on the object, and in engine builds before the collider fix they never rebuilt the collider. The visible wall looked right while the collision was a 1.5 metre cube at the wrong place, so the player fell through the floor and walked through walls.

The engine now rebuilds the body when those setters are called during Play. Even so, prefer scene-file geometry for anything solid, for two reasons: the colliders are correct at load with no extra work, and the map exists before any script runs, so scripts never race against it. Use script-created objects for things that come and go (bullets, effects, bot parts).

Counter Guns generates its scene records with `tools/gen_map.py`, which writes the arena into the `.flyproj`. The generator owns only objects whose names start with its own prefixes (`Arena_`, `WP_`, `TeamA_Spawn_` and so on) and keeps everything else (the baseplate, template objects) untouched. Rerun it after every layout change:

```
python3 -I tools/gen_map.py
```

Two things the generator had to get right, because the loader is strict:

1. The scene trailer needs two integers per object after the model count (hidden, locked). Without them any scene with three or more objects is rejected and the engine prints `Starting empty scene` with no other warning.
2. Wedge ramps must have collision accuracy "Default", not "Box", or they collide as a block.

If you write scene records by hand, copy a record from a project the editor saved, and check the player log for `Reading the scene: N object(s)`.

### Layout principles

- Draw the map on paper first, in metres, with named spawn rooms, a mid area and two or more lanes. Counter Guns' arena is 96 by 88 metres, symmetric in both axes, so it is fair for both teams.
- Walls 5 metres high, doorways at least 6 metres wide for bots, cover boxes 1.2 to 2.5 metres high, ramps to platforms.
- Keep a written layout (`DESIGN.md`) with coordinates for every named marker. The map script, the bot script and any tools all read the same names.

### Markers

Spawn points, patrol waypoints, doorways and cover positions are invisible, non-colliding objects with stable names: `TeamA_Spawn_1`, `WP_Mid_N`, `Door_A_W`, `Cover_...`. Code finds them with `Object::Find(name)`.

Place waypoints in open floor. Counter Guns had waypoints inside platforms and inside pillars, which made bots wander onto platforms and aim at points they could not reach. Check every marker against the solid boxes. A grid search over the layout (0.25 m cells, body radius 0.4 m) is a cheap way to prove that every spawn can reach every waypoint.

### Look

Scripts have flat colours, transparency, and the environment values (time of day, sun, fog, bloom, clouds). There is no material or texture API for scripts. Use a small palette, a warm and a cool team colour, and set the environment once in a map script.

## 5. The bots

Each bot is checked every frame for four behaviours, in this priority order, and fires whenever it can see an enemy.

| Behaviour | Does | Starts when |
|---|---|---|
| Cover | Walk to a spot behind a cover marker that the enemy cannot see past, 3.5 m from the marker | Health is under 45 and an enemy threatens it |
| Chase | Walk to the last place it saw an enemy, for up to 4 s after losing sight | It lost sight but still remembers |
| Patrol | Walk to a random nearby waypoint, then pick another | It has seen nothing |
| Fire | After a reaction delay (0.45 s from first sight), shoot with a cone of spread | It can see an enemy; closer than 22 m it stops to shoot, farther it keeps advancing |

Speeds in Counter Guns: patrol 2.6 m/s, chase 4.2, cover 5.0. Sight range is 60 m. These are tuning constants at the top of `CgBots.cpp`.

### Sight

A bot sees an enemy when the distance is under its view range, the enemy is inside its field of view cone, and a ray from the bot's eye to the target's centre hits the target's collider first. Reject an invalid target first. An invalid `Object` handle compares equal to any terrain hit, so a stale target would "see" through walls.

### Aim

Pick a constant cone angle, not an angle that grows with range per metre. Counter Guns first added `distance * 0.002` radians per metre to a base spread, so the error grew with the square of the range and bots at 30 metres missed by 3.6 metres against a 0.8 metre body. A constant cone of 0.03 radians standing and about 2.2 times that moving gives roughly 0.9 metres of error at 30 metres, so bots hit sometimes at 15 to 30 metres and rarely beyond 40.

Add a reaction delay (0.45 s in Counter Guns) and cap the fire rate, otherwise bots are unbeatable.

### Movement

There is no navmesh. Counter Guns bots walk straight lines between waypoints. That works only if the layout is open enough. Two cheap fixes that matter a lot:

- Stuck detection: every 0.5 s, if a bot that wants to move has moved less than 0.1 m, pick a nearby waypoint and walk there for up to 3 s before resuming the chase.
- Waypoints in doorways, so a bot aims for the gap instead of the wall next to it.

A real navigation helper would be the biggest improvement to this part.

### Bot bodies

Each bot is an invisible anchored collider plus a few visual parts (head, body, arms) as separate objects. Create them by cloning template objects that exist in the scene file, not with `Object::Create`. Clones copy size, collision and anchoring from the template, so they get the right body immediately. Put the templates far away (Counter Guns uses y = -500) and make them non-colliding for visual parts.

## 6. The round loop

One script owns the match as a state machine, updated every frame:

```
Freeze  (4 s, movement allowed, shooting off)
  -> Live  (120 s)
       ends when a team has no one alive, or the timer reaches 0
  -> Over  (5 s, show result, update score)
  -> next round, or the match is over when someone has enough round wins, then reset
```

Counter Guns' `CounterGuns.cpp` is about 150 lines for this. The shape to copy:

```cpp
struct MyGame : fly::Script {
    fly::Task Run() override {
        // wait for markers, then set everything up
        for (int i = 0; i < 60 && !fly::Object::Find("WP_Mid_N"); i++) co_await fly::NextFrame();
        SetupPlayer(); SetupBots(); StartRound();
        for (;;) {
            const float dt = fly::DeltaTime();
            UpdatePhase(dt);                 // Freeze / Live / Over
            UpdatePlayer(dt, live);
            UpdateBots(dt, live);
            UpdateHud();
            co_await fly::NextFrame();
        }
    }
};
FLY_SCRIPT(MyGame)
```

Keep shared state in one struct (`State& S()`), as Counter Guns does, so the player, bot and HUD files see the same data.

## 7. The HUD

`fly::UI` makes text, panels, bars and images by id, anchored to a screen corner and drawn over the 3D view. Sizes are in pixels of a 1280 by 720 screen and scale to the window. Counter Guns builds a score strip, a timer, a five-line kill feed, a crosshair, a hit marker (hidden after 0.15 s), a health bar and an ammo counter. Widgets are removed when play starts and stops, so build them from a script that runs on play.

## 8. First-person gun model

The engine does not draw a gun for you. Counter Guns builds one from eight cloned parts (receiver, barrel, muzzle, magazine, grip, stock, sight, stripe) and moves them every frame to sit in front of the camera. Three rules:

1. Compute one camera basis (right, up, forward) and place every part at `eye + right * x + up * y + forward * z` from the same basis.
2. Set each part's rotation from that same basis. Mixing a yaw-only rotation with an offset that includes pitch makes the parts rotate separately, and the gun appears to break apart when you look up or down.
3. The engine's Euler angles go through raylib's `MatrixRotateXYZ`, which negates each angle and composes Rx times Ry. Solve the angles for the full basis (`EulerForBasis` in `CsVisuals.hpp`) instead of guessing signs. A basis built as `cross(forward, up)` is a mirror image of a rotation, so no rotation matches it. Build the right vector so the basis has determinant +1, or flip one axis of the part (a symmetric box looks the same).

Hiding a part by setting its transparency did not hide the clones. Move hidden parts far away instead (Counter Guns parks them at y = -800).

## 9. Testing without playing by hand

The engine has built-in tools for unattended runs. Use them from the first day. Counter Guns went three rounds of "it works" without anyone walking into a wall, and the flying player bug was found only when a person played it.

### Read the log

Script `Print` goes to the in-engine console (F12). Copy it to the terminal or a file:

```
FlyPlayer -play "My Game" --log-stdout
FlyPlayer -play "My Game" --logfile run.log
```

### Script the input

`--automation file.txt` replays a list of commands on exact frame numbers. Each line is `<frame> <command>`:

```
# walk forward, turn, fire, check
30  dump
31  key down W
90  key up W
100 look 300 0
110 mouse down left
115 mouse up left
120 dump PlayerCharacter
125 screenshot /tmp/shot.png
130 quit
```

Commands: `key down|up <Name>`, `mouse down|up left|right|middle`, `look <dx> <dy>` (mouse pixels, one frame), `dump [ObjectName]` (camera and `PlayerCharacter` positions), `log <text>`, `screenshot <file>`, `quit`. Output is on stdout with an `[automation]` prefix.

Add `--fixed-dt 0.016667` so each frame advances the game by exactly a sixtieth of a second. Without it frames run uncapped, so a script of 400 frames was less than a second of game time and the round was still in its freeze period.

Full run:

```
FlyPlayer -play "My Game" --nohotreload --fixed-dt 0.016667 --log-stdout --automation test.txt
```

Use `--nohotreload`, because saving a script during a test restarts the match. The run still opens a window. Under a display-less machine, run it through `xvfb-run`.

### What to check

- Walk forward into the spawn wall and confirm the position stops (`dump` before and after).
- Fire and confirm the ammo goes down (read the HUD in a screenshot, or add `fly::DebugText("ammo", ...)`).
- Take a screenshot at four yaw angles to check the gun model.
- Stand in a bot's view and confirm health drops.
- Let a round run to its end and confirm the next round starts and the score updates.

### Watch state live

`fly::DebugText(key, text)` shows a line of text in the top-left corner, one row per key. `fly::MouseDelta(dx, dy)` reads the mouse movement. Use them while developing and remove them before shipping.

## 10. Traps, in order of cost

1. **Script-created solids collide wrong** (old engines). Use scene-file geometry. See section 4.
2. **No visible feedback**, so working guns look dead. Add hit markers, particles and a kill feed first.
3. **Mirrored rotation basis** breaks any multi-part model. See section 8.
4. **Scene records missing the trailer** make the loader silently start an empty scene. Check the log for the object count.
5. **Waypoints inside solids** make bots wander and stall.
6. **Aim error that grows with range per metre.** Use a constant cone.
7. **Invalid target handles match any terrain hit.** Check `IsValid()` before using one.
8. **"Play" in the editor and `FlyPlayer -play` took different paths.** Test with the same launch you will use. If they behave differently, that is an engine bug to report.
9. **Hot reload resets round state and leaves old bot parts in the scene.** Restart Play instead.
10. **Only screenshots and the kill feed are visible from outside.** Test with `--automation` and `dump` so a failing step is a number in a log, not a guess.

## 11. A suggested order of work

1. Blank project. Create a floor and four walls in the scene file. Walk around in Play and confirm you collide with everything.
2. Add spawn markers and respawn the player at one.
3. Add weapons, hitscan, ammo, reload and a crosshair. Test with `--automation`.
4. Build the first-person gun model. Check four yaw angles and one pitch.
5. Add one bot that stands still and can be shot. Add health, death and a kill feed line.
6. Make that bot patrol, see and shoot. Tune aim last.
7. Scale to two teams and the round loop. Then build the real map.
8. Add the HUD polish, then sounds, tracers and recoil.

Get step 1 and step 3 working before you do anything else. Every later part depends on them, and both are easy to break without noticing.

## 12. Known limits at the time of writing

- No navmesh. Bots walk straight lines.
- No material or texture API for scripts, only flat colours.
- The `.flyproj` scene format is not documented. The generator in Counter Guns is the best reference.
- `--automation` needs a window. It does not run without a display.
- Counter Guns itself is not finished: the bot-versus-player hit path, bot models standing on the floor, and ramps were not confirmed in play when this was written.
