#pragma once

// Debug stress harness for the water system. Launched via `Flyengine.exe
// --testwater [count] [frames]`. Opens a window, places a single large water
// body with its reflections enabled, drops N unanchored objects into it, and
// drives the camera on an orbit while sampling per-pass frame times (shadow /
// reflection / opaque / transparent) and water coverage state (chunk counts by
// LOD, far-shell resolution). A summary is written to water_stress_report.txt
// in the working directory.
namespace watertest {

void Run(int argc, char** argv);

} // namespace watertest