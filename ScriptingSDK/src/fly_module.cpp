// fly_module.cpp - the script module's entry point. The editor compiles this
// file into every project's script library alongside the user's Scripts/*.cpp.
// It owns the module-wide state declared in fly.hpp and adapts the registered
// fly::Script classes to the C ABI in FlyScriptABI.h.

#include "fly.hpp"

#include <exception>
#include <string>
#include <vector>

namespace fly::detail {

const FlyApi*& Api() {
    static const FlyApi* api = nullptr;
    return api;
}

float& Dt() {
    static float dt = 0.0f;
    return dt;
}

std::vector<ClassEntry>& Registry() {
    static std::vector<ClassEntry> entries;
    return entries;
}

} // namespace fly::detail

namespace {

struct Instance {
    const char* name;
    fly::Script* script;
    fly::Task task;
    bool started = false;
};

void ReportFailure(const char* name, const char* what) {
    std::string msg = std::string("[FlyScript] ") + name + " FAILED: " + what;
    fly::detail::Api()->Print(msg.c_str());
}

void* CreateInstance(const FlyScriptClass* cls) {
    auto* make = reinterpret_cast<fly::Script* (*)()>(cls->context);
    try {
        return new Instance{ cls->name, make(), {} };
    } catch (const std::exception& e) {
        ReportFailure(cls->name, e.what());
    } catch (...) {
        ReportFailure(cls->name, "unknown exception in constructor");
    }
    return nullptr;
}

int TickInstance(void* p, float dt) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst) return 0;
    fly::detail::Dt() = dt;
    try {
        if (!inst->started) {
            inst->started = true;
            inst->task = inst->script->Run();
        }
        return inst->task.Step(dt) ? 1 : 0;
    } catch (const std::exception& e) {
        ReportFailure(inst->name, e.what());
    } catch (...) {
        ReportFailure(inst->name, "unknown exception");
    }
    return 0;
}

void DestroyInstance(void* p) {
    auto* inst = static_cast<Instance*>(p);
    if (!inst) return;
    inst->task = fly::Task(); // destroy the coroutine frame before its script
    delete inst->script;
    delete inst;
}

} // namespace

extern "C" FLY_SCRIPT_EXPORT const FlyScriptModule* FlyScript_GetModule(const FlyApi* api) {
    if (!api || api->abi_version != FLY_SCRIPT_ABI_VERSION || api->size < sizeof(FlyApi))
        return nullptr;
    fly::detail::Api() = api;

    static std::vector<FlyScriptClass> classes;
    static FlyScriptModule module;
    classes.clear();
    for (const auto& e : fly::detail::Registry()) {
        classes.push_back({ e.name, reinterpret_cast<void*>(e.make),
                            &CreateInstance, &TickInstance, &DestroyInstance });
    }
    module.abi_version = FLY_SCRIPT_ABI_VERSION;
    module.class_count = static_cast<int>(classes.size());
    module.classes = classes.data();
    return &module;
}
