// TEMPORARY diagnostic: writes a stack trace to crashlog.txt on an unhandled
// exception. Removed once the crash is fixed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <dbghelp.h>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")

static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    FILE* f = nullptr;
    fopen_s(&f, "crashlog.txt", "a");
    if (f) {
        fprintf(f, "=== CRASH code=0x%08X at=0x%p pid=%u\n",
            ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress,
            static_cast<unsigned>(GetCurrentProcessId()));

        HANDLE proc = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();
        SymInitialize(proc, nullptr, TRUE);

        CONTEXT ctx = *ep->ContextRecord;
        STACKFRAME64 sf = {};
#ifdef _WIN64
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrStack.Offset = ctx.Rsp;
        const DWORD machine = IMAGE_FILE_MACHINE_AMD64;
#else
        sf.AddrPC.Offset = ctx.Eip;
        sf.AddrFrame.Offset = ctx.Ebp;
        sf.AddrStack.Offset = ctx.Esp;
        const DWORD machine = IMAGE_FILE_MACHINE_I386;
#endif
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Mode = AddrModeFlat;

        for (int i = 0; i < 48 && StackWalk64(machine, proc, thread, &sf, &ctx,
                 nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
            char name[512] = "<unknown>";
            DWORD64 disp = 0;
            BYTE buffer[sizeof(SYMBOL_INFO) + 1024] = {};
            SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(buffer);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 1024;
            if (SymFromAddr(proc, sf.AddrPC.Offset, &disp, sym)) {
                snprintf(name, sizeof(name), "%s+0x%llX", sym->Name, static_cast<unsigned long long>(disp));
            }
            char mod[MAX_PATH] = "";
            HMODULE hm = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(sf.AddrPC.Offset), &hm)) {
                GetModuleBaseNameA(proc, hm, mod, sizeof(mod));
            }
            fprintf(f, "  %02d: %s!%s\n", i, mod, name);
        }
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

namespace crashreporter {
void Install() {
    SetUnhandledExceptionFilter(CrashHandler);
}

void LogFatal(const char* msg) {
    FILE* f = nullptr;
    fopen_s(&f, "crashlog.txt", "a");
    if (f) {
        fprintf(f, "=== FATAL (no SEH) pid=%u: %s\n",
            static_cast<unsigned>(GetCurrentProcessId()), msg ? msg : "<no message>");
        fclose(f);
    }
}
} // namespace crashreporter
