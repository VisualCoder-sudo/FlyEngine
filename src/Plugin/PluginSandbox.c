/*
 * PluginSandbox.c - Sandboxing implementation for native plugins (Linux)
 * 
 * Implements seccomp-bpf syscall filtering and cgroups v2 resource limits.
 * 
 * Requires: Linux kernel 4.15+ (for seccomp), cgroups v2 mounted
 */

#include "PluginSandbox.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/prctl.h>
#include <linux/seccomp.h>
#include <linux/filter.h>
#include <linux/audit.h>
#include <linux/bpf.h>
#include <sys/resource.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>

// BPF instruction builder functions (avoid macro issues with compound literals)
static inline struct sock_filter bpf_stmt(unsigned short code, unsigned int k) {
    struct sock_filter f = { code, 0, 0, k };
    return f;
}

static inline struct sock_filter bpf_jump(unsigned short code, unsigned int k, unsigned char jt, unsigned char jf) {
    struct sock_filter f = { code, jt, jf, k };
    return f;
}

#define BPF_LD 0x00
#define BPF_ABS 0x20
#define BPF_JMP 0x05
#define BPF_JEQ 0x10
#define BPF_K 0x00
#define BPF_RET 0x06

// ============================================================================
// DEFAULT CONFIGURATION
// ============================================================================

const PluginSandboxConfig g_default_sandbox_config = {
    .enable_seccomp = true,
    .max_memory_bytes = 256 * 1024 * 1024,  // 256 MB
    .max_cpu_percent = 5000,                 // 50%
    .max_processes = 64,
    .max_open_files = 1024,
    .allowed_read_paths = NULL,
    .allowed_read_paths_count = 0,
    .allowed_write_paths = NULL,
    .allowed_write_paths_count = 0,
    .allow_network = false,
    .drop_capabilities = 0,
};

// ============================================================================
// SECCOMP FILTER DEFINITIONS
// ============================================================================

// Essential syscalls that plugins need
// x86_64 Linux syscall numbers (from asm/unistd_64.h)
static const int g_allowed_syscalls[] = {
    // Process/thread
    60,   // exit
    231,  // exit_group
    39,   // getpid
    186,  // gettid
    56,   // clone
    57,   // fork
    58,   // vfork
    61,   // wait4
    61,   // waitpid (same as wait4 on x86_64)
    
    // Memory
    12,   // brk
    9,    // mmap
    11,   // munmap
    10,   // mprotect
    25,   // mremap
    26,   // msync
    28,   // madvise
    
    // File I/O
    2,    // open
    257,  // openat
    3,    // close
    0,    // read
    1,    // write
    17,   // pread64
    18,   // pwrite64
    8,    // lseek
    4,    // stat
    5,    // fstat
    6,    // lstat
    262,  // newfstatat
    21,   // access
    269,  // faccessat
    83,   // mkdir
    258,  // mkdirat
    84,   // rmdir
    87,   // unlink
    263,  // unlinkat
    82,   // rename
    264,  // renameat
    86,   // link
    265,  // linkat
    88,   // symlink
    266,  // symlinkat
    89,   // readlink
    267,  // readlinkat
    90,   // chmod
    91,   // fchmod
    92,   // chown
    93,   // fchown
    94,   // fchownat
    261,  // utimensat
    
    // Directory
    217,  // getdents64
    79,   // getcwd
    80,   // chdir
    81,   // fchdir
    
    // Poll/I/O
    7,    // poll
    271,  // ppoll
    23,   // select
    
    // Epoll
    291,  // epoll_create1
    233,  // epoll_ctl
    232,  // epoll_wait
    281,  // epoll_pwait
    
    // Time
    228,  // clock_gettime
    229,  // clock_getres
    35,   // nanosleep
    230,  // clock_nanosleep
    96,   // gettimeofday
    201,  // time
    
    // Signals
    13,   // rt_sigaction
    14,   // rt_sigprocmask
    15,   // rt_sigpending
    219,  // rt_sigtimedwait
    129,  // rt_sigqueueinfo
    62,   // kill
    200,  // tkill
    234,  // tgkill
    131,  // sigaltstack
    
    // Thread synchronization
    202,  // futex
    99,   // set_robust_list
    100,  // get_robust_list
    
    // Misc
    102,  // getuid
    107,  // geteuid
    104,  // getgid
    108,  // getegid
    39,   // getpid
    110,  // getppid
    186,  // gettid
    177,  // prlimit64
    97,   // getrlimit
    160,  // setrlimit
    99,   // sysinfo
    63,   // uname
    158,  // arch_prctl
    24,   // sched_yield
    203,  // sched_getaffinity
    204,  // sched_setaffinity
};

// ============================================================================
// SECCOMP FILTER CREATION
// ============================================================================

struct sock_fprog* PluginSandbox_CreateDefaultSeccompFilter() {
    int num_syscalls = sizeof(g_allowed_syscalls) / sizeof(g_allowed_syscalls[0]);
    
    // BPF program: load syscall number, compare against allowed list
    // Each syscall needs: LD + JEQ + JUMP
    // Plus: LD + RET for default action
    int num_insns = num_syscalls * 3 + 4;
    
    struct sock_filter* filter = calloc(num_insns, sizeof(struct sock_filter));
    if (!filter) return NULL;
    
    struct sock_fprog* prog = calloc(1, sizeof(struct sock_fprog));
    if (!prog) {
        free(filter);
        return NULL;
    }
    
    int idx = 0;
    
    // Compute offsets (offsetof is a macro that expands to a constant)
    const unsigned int arch_offset = offsetof(struct seccomp_data, arch);
    const unsigned int nr_offset = offsetof(struct seccomp_data, nr);
    
    // Load architecture (to verify we're on the right arch)
    filter[idx++] = bpf_stmt(BPF_LD | BPF_W | BPF_ABS, arch_offset);
    filter[idx++] = bpf_jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
    filter[idx++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    
    // Load syscall number
    filter[idx++] = bpf_stmt(BPF_LD | BPF_W | BPF_ABS, nr_offset);
    
    // Check each allowed syscall
    for (int i = 0; i < num_syscalls; i++) {
        filter[idx++] = bpf_jump(BPF_JMP | BPF_JEQ | BPF_K, g_allowed_syscalls[i], 0, 1);
        filter[idx++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    }
    
    // Default: kill process
    filter[idx++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    
    prog->len = idx;
    prog->filter = filter;
    
    return prog;
}

struct sock_fprog* PluginSandbox_CreateCustomFilter(const SeccompRule* rules, int rule_count) {
    if (!rules || rule_count <= 0) return NULL;
    
    struct sock_filter* filter = calloc(rule_count * 3 + 4, sizeof(struct sock_filter));
    if (!filter) return NULL;
    
    struct sock_fprog* prog = calloc(1, sizeof(struct sock_fprog));
    if (!prog) {
        free(filter);
        return NULL;
    }
    
    int idx = 0;
    
    // Compute offsets
    const unsigned int arch_offset = offsetof(struct seccomp_data, arch);
    const unsigned int nr_offset = offsetof(struct seccomp_data, nr);
    
    // Architecture check
    filter[idx++] = bpf_stmt(BPF_LD | BPF_W | BPF_ABS, arch_offset);
    filter[idx++] = bpf_jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0);
    filter[idx++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    
    // Load syscall number
    filter[idx++] = bpf_stmt(BPF_LD | BPF_W | BPF_ABS, nr_offset);
    
    // Check each rule
    for (int i = 0; i < rule_count; i++) {
        filter[idx++] = bpf_jump(BPF_JMP | BPF_JEQ | BPF_K, rules[i].syscall_nr, 0, 1);
        filter[idx++] = bpf_stmt(BPF_RET | BPF_K, rules[i].action);
    }
    
    // Default: kill process
    filter[idx++] = bpf_stmt(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    
    prog->len = idx;
    prog->filter = filter;
    
    return prog;
}

void PluginSandbox_FreeFilter(struct sock_fprog* prog) {
    if (prog) {
        free(prog->filter);
        free(prog);
    }
}

// ============================================================================
// SECCOMP APPLICATION
// ============================================================================

static int apply_seccomp(const PluginSandboxConfig* config) {
    if (!config->enable_seccomp) return 0;
    
    struct sock_fprog* prog = PluginSandbox_CreateDefaultSeccompFilter();
    if (!prog) return -1;
    
    // PR_SET_NO_NEW_PRIVS must be set before seccomp
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0) {
        PluginSandbox_FreeFilter(prog);
        return -1;
    }
    
    int ret = syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, 0, prog);
    
    PluginSandbox_FreeFilter(prog);
    return ret;
}

// ============================================================================
// CGROUP V2 OPERATIONS
// ============================================================================

static const char* find_cgroup_mount() {
    static char path[256];
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) return NULL;
    
    char dev[256], mnt[256], fstype[64];
    while (fscanf(f, "%255s %255s %63s", dev, mnt, fstype) == 3) {
        if (strcmp(fstype, "cgroup2") == 0) {
            strncpy(path, mnt, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            fclose(f);
            return path;
        }
    }
    fclose(f);
    return NULL;
}

static int write_cgroup_file(const char* cgroup_path, const char* filename, const char* value) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", cgroup_path, filename);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    
    ssize_t written = write(fd, value, strlen(value));
    close(fd);
    
    return written >= 0 ? 0 : -1;
}

static int write_cgroup_file_int(const char* cgroup_path, const char* filename, int64_t value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%ld", value);
    return write_cgroup_file(cgroup_path, filename, buf);
}

char* PluginSandbox_CreateCgroup(const char* cgroup_name, const PluginSandboxConfig* config) {
    if (!config) config = &g_default_sandbox_config;
    
    const char* cgroup_root = find_cgroup_mount();
    if (!cgroup_root) return NULL;
    
    char cgroup_path[512];
    snprintf(cgroup_path, sizeof(cgroup_path), "%s/flyengine/%s", cgroup_root, cgroup_name);
    
    // Create cgroup directory
    if (mkdir(cgroup_path, 0755) < 0 && errno != EEXIST) {
        return NULL;
    }
    
    // Enable controllers
    char controllers_path[512];
    snprintf(controllers_path, sizeof(controllers_path), "%s/cgroup.subtree_control", cgroup_root);
    
    int fd = open(controllers_path, O_WRONLY);
    if (fd >= 0) {
        const char* controllers = "+memory +cpu +pids";
        write(fd, controllers, strlen(controllers));
        close(fd);
    }
    
    // Apply limits
    if (config->max_memory_bytes > 0) {
        if (write_cgroup_file_int(cgroup_path, "memory.max", config->max_memory_bytes) < 0) {
            // Not fatal, just warn
        }
    }
    
    if (config->max_cpu_percent > 0) {
        // cpu.max format: "quota period" (in microseconds)
        // 100% = 100000 100000, 50% = 50000 100000
        int64_t quota = (config->max_cpu_percent * 100000) / 10000;
        char cpu_max[64];
        snprintf(cpu_max, sizeof(cpu_max), "%ld 100000", quota);
        write_cgroup_file(cgroup_path, "cpu.max", cpu_max);
    }
    
    if (config->max_processes > 0) {
        write_cgroup_file_int(cgroup_path, "pids.max", config->max_processes);
    }
    
    // Also set fd limit via rlimit in the process itself (done in child)
    
    char* result = strdup(cgroup_path);
    return result;
}

bool PluginSandbox_AddToCgroup(const char* cgroup_path, pid_t pid) {
    if (!cgroup_path || pid <= 0) return false;
    
    char path[512];
    snprintf(path, sizeof(path), "%s/cgroup.procs", cgroup_path);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) return false;
    
    char pid_str[32];
    int len = snprintf(pid_str, sizeof(pid_str), "%d", pid);
    bool ok = write(fd, pid_str, len) == len;
    close(fd);
    return ok;
}

void PluginSandbox_DestroyCgroup(const char* cgroup_path) {
    if (!cgroup_path) return;
    
    // First, move any remaining processes to parent
    char procs_path[512];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);
    
    FILE* f = fopen(procs_path, "r");
    if (f) {
        char line[64];
        while (fgets(line, sizeof(line), f)) {
            pid_t pid = atoi(line);
            if (pid > 0) {
                char parent_path[512];
                char* last_slash = strrchr(cgroup_path, '/');
                if (last_slash) {
                    *last_slash = '\0';
                    snprintf(parent_path, sizeof(parent_path), "%s/cgroup.procs", cgroup_path);
                    *last_slash = '/';
                    
                    int fd = open(parent_path, O_WRONLY);
                    if (fd >= 0) {
                        char pid_str[32];
                        int len = snprintf(pid_str, sizeof(pid_str), "%d", pid);
                        write(fd, pid_str, len);
                        close(fd);
                    }
                }
            }
        }
        fclose(f);
    }
    
    // Remove cgroup directory
    rmdir(cgroup_path);
}

// ============================================================================
// MAIN SANDBOX APPLICATION
// ============================================================================

int PluginSandbox_Apply(const PluginSandboxConfig* config) {
    if (!config) config = &g_default_sandbox_config;
    
    // Set resource limits
    if (config->max_open_files > 0) {
        struct rlimit rlim = { config->max_open_files, config->max_open_files };
        if (setrlimit(RLIMIT_NOFILE, &rlim) < 0) {
            // Non-fatal
        }
    }
    
    // Apply seccomp filter
    if (apply_seccomp(config) < 0) {
        return -1;
    }
    
    return 0;
}