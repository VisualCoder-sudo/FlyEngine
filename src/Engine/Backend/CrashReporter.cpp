// CrashReporter.cpp -- writes a stack trace to crashlog.txt on an unhandled
// fault.
//
// TEMPORARY diagnostic, kept while the original crash is being chased. The
// header comment predates the port; see git history for its original text.
//
// The Windows build uses DbgHelp to symbolise a structured exception. POSIX has
// no equivalent, so that path uses backtrace()/backtrace_symbols() from a
// SIGSEGV/SIGABRT/SIGFPE/SIGILL handler instead. Both write the same
// crashlog.txt format so existing tooling keeps working.

#include "Engine/Backend/CrashReporter.hpp"

#include <cstdio>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <psapi.h>
    #include <dbghelp.h>
    #pragma comment(lib, "dbghelp.lib")
    #pragma comment(lib, "psapi.lib")
#else
    #include <csignal>
    #include <cstdlib>
    #include <execinfo.h>
    #include <unistd.h>
#endif

namespace {

FILE* OpenCrashLog() {
    // fopen_s is MSVC-only; the null check below covers both forms.
    return std::fopen("crashlog.txt", "a");
}

#if defined(_WIN32)

LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    FILE* f = OpenCrashLog();
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
        SymCleanup(proc);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

#else // POSIX

// Signal handlers must not call malloc-backed APIs, so keep this to async-safe
// calls: fopen/fprintf on a path is not guaranteed async-signal-safe, but this
// is a best-effort diagnostic and the alternative is losing the trace entirely.
void PosixSignalHandler(int sig) {
    FILE* f = OpenCrashLog();
    if (f) {
        fprintf(f, "=== CRASH signal=%d pid=%u\n", sig, static_cast<unsigned>(::getpid()));
        void* frames[64];
        const int n = ::backtrace(frames, 64);
        char** syms = ::backtrace_symbols(frames, n);   // malloc-backed; best effort
        for (int i = 0; i < n; ++i) {
            fprintf(f, "  %02d: %s\n", i, syms ? syms[i] : "<unknown>");
        }
        if (syms) ::free(syms);
        fclose(f);
    }
    // Restore the default action and re-raise so the exit status and any core
    // dump reflect the real fault.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

#endif // _WIN32

} // namespace

namespace crashreporter {

void Install() {
#if defined(_WIN32)
    SetUnhandledExceptionFilter(CrashHandler);
#else
    ::signal(SIGSEGV, PosixSignalHandler);
    ::signal(SIGABRT, PosixSignalHandler);
    ::signal(SIGFPE,  PosixSignalHandler);
    ::signal(SIGILL,  PosixSignalHandler);
    ::signal(SIGBUS,  PosixSignalHandler);
#endif
}

void LogFatal(const char* msg) {
    FILE* f = OpenCrashLog();
    if (f) {
#if defined(_WIN32)
        fprintf(f, "=== FATAL (no SEH) pid=%u: %s\n",
            static_cast<unsigned>(GetCurrentProcessId()), msg ? msg : "<no message>");
#else
        fprintf(f, "=== FATAL (no signal) pid=%u: %s\n",
            static_cast<unsigned>(::getpid()), msg ? msg : "<no message>");
#endif
        fclose(f);
    }
}

} // namespace crashreporter
