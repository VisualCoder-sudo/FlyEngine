// ProcessRunner.cpp -- spawn a child process and capture its output.
//
// Replaces the CreateProcess/CreatePipe/ReadFile/WaitForSingleObject blocks
// that were duplicated in ScriptCompiler.cpp (running the compiler) and
// ScriptLauncher.cpp (probing vswhere.exe).

#include "Engine/Platform/Platform.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <cerrno>
    #include <csignal>
    #include <fcntl.h>
    #include <poll.h>
    #include <spawn.h>
    #include <sys/wait.h>
    #include <unistd.h>
    extern char** environ;
#endif

namespace platform {

#if defined(_WIN32)

namespace {

// Build a single command line. Arguments are quoted only when they contain
// whitespace or a quote, which is enough for the tools we launch.
std::string QuoteArg(const std::string& a) {
    if (a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string out = "\"";
    for (char c : a) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

} // namespace

ProcessResult RunProcessCapture(const std::string& exe,
                                const std::vector<std::string>& args,
                                int timeoutMs) {
    ProcessResult result;

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    // The read end must not be inherited or the child would hold it open and
    // ReadFile would never see EOF.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = QuoteArg(exe);
    for (const std::string& a : args) cmd += " " + QuoteArg(a);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    BOOL ok = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        result.error = "CreateProcess failed for: " + exe;
        return result;
    }
    result.launched = true;

    // Drain on a timeout loop rather than a single blocking ReadFile, so we
    // never wedge if the child stalls.
    const DWORD start = GetTickCount();
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr)) break;
        if (avail > 0) {
            DWORD got = 0;
            if (ReadFile(readPipe, buf, sizeof(buf), &got, nullptr) && got > 0) {
                result.output.append(buf, got);
                continue;
            }
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        if (timeoutMs > 0 && (GetTickCount() - start) > static_cast<DWORD>(timeoutMs)) {
            TerminateProcess(pi.hProcess, 1);
            break;
        }
        Sleep(5);
    }
    CloseHandle(readPipe);

    DWORD code = 1;
    if (GetExitCodeProcess(pi.hProcess, &code)) result.exitCode = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}

#else // POSIX

namespace {

std::string ErrnoText() { return std::strerror(errno); }

} // namespace

ProcessResult RunProcessCapture(const std::string& exe,
                                const std::vector<std::string>& args,
                                int timeoutMs) {
    ProcessResult result;

    int fds[2];
    if (::pipe(fds) != 0) {
        result.error = std::string("pipe failed: ") + ErrnoText();
        return result;
    }

    // Read end non-blocking so the drain loop can also poll the child.
    const int flags = ::fcntl(fds[0], F_GETFL, 0);
    ::fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);

    // posix_spawnp takes a plain char**; build a NUL-separated argv.
    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(exe);
    for (const std::string& a : args) storage.push_back(a);
    std::vector<char*> argv;
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&fa, fds[0]);
    ::posix_spawn_file_actions_addclose(&fa, fds[1]);

    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);

    if (rc != 0) {
        ::close(fds[0]);
        result.error = "could not launch '" + exe + "': " + std::strerror(rc);
        return result;
    }
    result.launched = true;

    // Drain until EOF, honouring the timeout. poll() rather than a bare read()
    // so a silent child cannot spin the CPU.
    const int64_t deadlineMs =
        timeoutMs > 0 ? static_cast<int64_t>(timeoutMs) : -1;
    int64_t waitedMs = 0;
    char buf[4096];
    bool timedOut = false;

    for (;;) {
        struct pollfd pfd{fds[0], POLLIN, 0};
        const int pollTimeout = 50;
        const int pr = ::poll(&pfd, 1, pollTimeout);
        if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
            for (;;) {
                const ssize_t n = ::read(fds[0], buf, sizeof(buf));
                if (n > 0) { result.output.append(buf, static_cast<size_t>(n)); continue; }
                if (n == 0) { /* EOF */ }
                break; // EAGAIN or error
            }
            if (pfd.revents & (POLLHUP | POLLERR)) {
                // Child closed its end; confirm it really exited.
                int status = 0;
                if (::waitpid(pid, &status, 0) == pid) {
                    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
                    pid = 0;
                }
                break;
            }
        }
        waitedMs += pollTimeout;
        if (deadlineMs > 0 && waitedMs >= deadlineMs) {
            timedOut = true;
            ::kill(pid, SIGKILL);
            break;
        }
    }

    if (pid != 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    ::close(fds[0]);

    if (timedOut) {
        result.error = "timed out after " + std::to_string(timeoutMs) + "ms";
        if (result.exitCode < 0) result.exitCode = 1;
    }
    return result;
}

#endif // _WIN32

bool LaunchDetached(const std::string& exe, const std::vector<std::string>& args) {
    if (exe.empty()) return false;

    std::vector<std::string> storage;
    storage.reserve(args.size() + 1);
    storage.push_back(exe);
    for (const std::string& a : args) storage.push_back(a);
    std::vector<char*> argv;
    for (std::string& s : storage) argv.push_back(s.data());
    argv.push_back(nullptr);

#if defined(_WIN32)
    // ShellExecute is the right tool here: it goes through the shell
    // association table, so it handles .cpp files, non-executable targets, and
    // the "no path given means use the default app" case uniformly.
    std::string params;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) params += ' ';
        params += QuoteArg(args[i]);
    }
    std::vector<char> mutableExe(exe.begin(), exe.end());
    mutableExe.push_back('\0');
    std::vector<char> mutableParams(params.begin(), params.end());
    mutableParams.push_back('\0');
    const HINSTANCE r = ShellExecuteA(nullptr, L"open",
                                      mutableExe.data(),
                                      mutableParams.empty() ? nullptr : mutableParams.data(),
                                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
#else
    // stdio goes to /dev/null: an editor that writes to inherited stdout would
    // scribble over the engine's own console output.
    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        ::posix_spawn_file_actions_adddup2(&fa, devnull, STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&fa, devnull, STDOUT_FILENO);
        ::posix_spawn_file_actions_adddup2(&fa, devnull, STDERR_FILENO);
        ::posix_spawn_file_actions_addclose(&fa, devnull);
    }

    // A new session detaches the child from our terminal, so closing the
    // engine's console does not take the editor down with it.
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);

    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
    ::posix_spawnattr_destroy(&attr);
    ::posix_spawn_file_actions_destroy(&fa);
    if (devnull >= 0) ::close(devnull);

    if (rc != 0) return false;

    // Reap the intermediate child; posix_spawn already forked and exec'd, so
    // this returns immediately and cannot block on the editor itself.
    int status = 0;
    ::waitpid(pid, &status, 0);
    return true;
#endif
}

} // namespace platform
