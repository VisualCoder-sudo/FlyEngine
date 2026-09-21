#pragma once
namespace crashreporter {
void Install();
// Writes a non-SEH fatal (uncaught C++ exception / terminate) to crashlog.txt,
// which the SEH handler would otherwise never capture.
void LogFatal(const char* msg);
}
