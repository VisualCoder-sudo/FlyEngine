#pragma once

namespace benchmark {

// Runs a performance benchmark before the engine launches.
// Measures rendering frame rate under 3D/2D workload.
// If the benchmark achieves less than 60 FPS, a warning dialog is shown:
// "Your computer failed to meet the benchmark, This may cause Flyengine to be unstable, Are you sure you want to proceed?"
// Returns true if the benchmark passed or the user chose 'Yes' to proceed.
// Returns false if the user chose 'No' / closed the warning.
bool RunStartupBenchmark();

} // namespace benchmark
