#pragma once
// Scripted input and checks for unattended runs: FlyPlayer --automation <file>.
//
// The file is one command per line, "<frame> <command> [args]"; `#` starts a comment. Frames count from 1, the first
// frame the game loop runs. Commands:
//   key down|up <Name>          hold or release a key (A..Z, 0..9, Space, Enter, Escape, Tab, Shift, Ctrl, Up, Down,
//                               Left, Right, F1..F12)
//   mouse down|up left|right|middle
//   look <dx> <dy>              relative mouse movement for that one frame, in pixels (turns the camera)
//   dump [ObjectName]           print the frame, camera, PlayerCharacter and optionally one named object
//   log <text>                  print a line
//   screenshot <file>           save the frame once it is drawn
//   expect exists|missing <Name>               fail the run if the object is missing / still present
//   expect pos <Name> <x> <y> <z> [tolerance]  fail if the object is farther than tolerance (default 0.1) from the point
//   expect dist <A> <B> <max>                  fail if the two objects are more than max apart
//   compare <actual.png> <golden.png> [tol]    after the frame is drawn, fail if the mean per-channel difference
//                                              exceeds tol (default 2, of 255). A missing golden is created from actual.
//   quit                        end the run
// Everything is printed to stdout with an "[automation]" prefix, so a run can be read from a terminal or a pipe.
#include <string>
#include <vector>

struct Camera3D;
class ScatteredObject;

namespace automation {

// Reads the script. Returns false (and prints why) if the file is missing or a line is malformed.
bool Load(const std::string& path);

bool Active();

// Applies the commands due on this frame. Call once at the top of the game loop, before the engine updates.
// Returns true when the run should end: a `quit` command, or one frame after the last command.
bool BeforeUpdate(int frame, const Camera3D& camera, const std::vector<ScatteredObject*>& objects);

// Saves any screenshots asked for this frame. Call after the frame has been drawn.
void AfterRender();

// Number of failed expect/compare checks so far. The player exits with status 1 when this is not zero.
int Failures();

}   // namespace automation
