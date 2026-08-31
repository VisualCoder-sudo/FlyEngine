#pragma once
#include "ScatteredObject.hpp"
#include "raylib.h"
#include <string>

// Flyscript script editor: a real multi-line text editor overlay (caret,
// line numbers, syntax highlighting, scrolling) for editing a script attached
// to a scene object, or a standalone script.
//
// Update() runs every frame (keyboard/mouse), Draw() renders last inside the
// 2D pass. Only one script is edited at a time.
namespace scriptEditor {

void Init();
void Unload();

void Update();
void Draw();

void OpenObject(ScatteredObject* obj); // edit obj->script
void OpenScript(int index);            // edit flyscript::Runtime.scripts[index]
void Close();
void CloseIfTarget(const ScatteredObject* obj);
void CloseIfIndex(int index);

bool IsOpen();
bool IsCapturingKeyboard(); // the editor owns keyboard input
bool IsMouseOverWindow(Vector2 point);

} // namespace scriptEditor
