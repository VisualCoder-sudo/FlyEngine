#pragma once

#include <string>

// Bridges the in-editor console bar to FlyEngine::FlyCloudModule.
namespace fcloud {

// Dispatches a "fcloud ..." command against the currently open project.
// Safe to call from the main/game thread; logs are queued and flushed by
// Update(), because FlyCloudModule logs from a detached worker thread.
void DispatchCommand(const std::string& full_command);

// Flushes queued FlyCloud log lines into the editor log. Call once per frame.
void Update();

} // namespace fcloud