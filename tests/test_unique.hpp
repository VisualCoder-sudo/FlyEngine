// Per-process suffix for scratch directories under the system temp dir. Tests used fixed names there, so two runs
// at once (ctest -j, two checkouts, a leftover from a killed run) deleted each other's files mid-test.
#pragma once
#include <string>
#if defined(_WIN32)
#include <process.h>
#define FLY_TEST_PID() _getpid()
#else
#include <unistd.h>
#define FLY_TEST_PID() getpid()
#endif
inline std::string TestUnique(const char* name) { return std::string(name) + "_" + std::to_string((long)FLY_TEST_PID()); }
