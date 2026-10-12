#pragma once
// Game scripts and plugins compiled INTO the executable (an exported game), instead of being built and loaded from
// .so/.dll files at runtime. The export step links a small generated translation unit that registers them from a
// static initializer, before main(); the NativeScriptHost then uses them where it would have dlopen()ed a library.
//
// Deliberately free of other engine headers so the generated glue stays tiny.
#include <cstddef>

struct FlyApi;
struct FlyScriptModule;
struct CP_PluginEntry;

namespace NativeScript {

using StaticScriptEntry = const FlyScriptModule* (*)(const FlyApi*);   // the script module's FlyScript_GetModule

struct StaticPlugin {
    const char* name;
    CP_PluginEntry* (*getEntry)(void);                                 // the plugin's CP_GetPluginEntry
};

void SetStaticScriptEntry(StaticScriptEntry entry);
void SetStaticPlugins(const StaticPlugin* plugins, size_t count);     // the array must stay valid (static storage)
bool HasStaticModules();                                               // anything registered at all

} // namespace NativeScript
