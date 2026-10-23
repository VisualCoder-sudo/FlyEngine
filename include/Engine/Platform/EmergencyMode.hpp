// EmergencyMode.hpp -- what to do when there is no display server to open a window on.
//
// sokol_app on Linux talks to X11 only (Wayland sessions work through XWayland) and panics when it cannot
// connect. Call RunEmergencyModeIfNoDisplay() first thing in main(): on a machine where a window can be
// opened it returns at once; otherwise it does not return -- it runs a text-mode console in the current
// terminal, or, with no terminal attached, re-launches this program inside a terminal emulator.
//
//   --emergency                force emergency mode (for testing)
//   FLYENGINE_NO_EMERGENCY=1   never enter it (the old behaviour: sokol aborts)
//
// No-op on Windows and macOS.
#pragma once

namespace platform {

void RunEmergencyModeIfNoDisplay(int argc, char** argv);

} // namespace platform
