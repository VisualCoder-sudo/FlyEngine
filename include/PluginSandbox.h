/*
 * PluginSandbox.h - Sandboxing for native plugins
 * 
 * Provides seccomp-bpf syscall filtering and cgroups v2 resource limits
 * for Linux plugin processes.
 */

#ifndef PLUGIN_SANDBOX_H
#define PLUGIN_SANDBOX_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

// ============================================================================
// SANDBOX CONFIGURATION
// ============================================================================

typedef struct {
    // Seccomp policy
    bool enable_seccomp;
    
    // Resource limits (cgroups v2)
    int64_t max_memory_bytes;      // Memory limit (0 = unlimited)
    int64_t max_cpu_percent;       // CPU quota as percentage * 10000 (e.g., 5000 = 50%)
    int max_processes;             // Max processes/threads (pids.max)
    int max_open_files;            // Max file descriptors
    
    // Filesystem restrictions
    const char** allowed_read_paths;    // Paths allowed for reading
    int allowed_read_paths_count;
    const char** allowed_write_paths;   // Paths allowed for writing
    int allowed_write_paths_count;
    
    // Network
    bool allow_network;
    
    // Capabilities to drop (Linux capabilities)
    uint64_t drop_capabilities;  // Bitmask of capabilities to drop
} PluginSandboxConfig;

// Default sandbox configuration
extern const PluginSandboxConfig g_default_sandbox_config;

// ============================================================================
// SANDBOX OPERATIONS
// ============================================================================

// Apply sandbox to current process (call in child after fork, before exec)
// Returns 0 on success, -1 on failure
int PluginSandbox_Apply(const PluginSandboxConfig* config);

// Setup cgroups v2 for a process (call in parent before fork)
// Returns cgroup path on success, NULL on failure
char* PluginSandbox_CreateCgroup(const char* cgroup_name, const PluginSandboxConfig* config);

// Add process to cgroup
bool PluginSandbox_AddToCgroup(const char* cgroup_path, pid_t pid);

// Remove cgroup
void PluginSandbox_DestroyCgroup(const char* cgroup_path);

// ============================================================================
// SECCOMP HELPERS
// ============================================================================

// Default seccomp policy - allows only essential syscalls
// Returns seccomp filter program (caller must free)
struct sock_fprog* PluginSandbox_CreateDefaultSeccompFilter();

// Custom seccomp policy builder
typedef struct SeccompRule {
    int syscall_nr;
    uint32_t action;  // SECCOMP_RET_ALLOW, SECCOMP_RET_KILL_PROCESS, etc.
    // For complex rules, additional fields would be needed
} SeccompRule;

struct sock_fprog* PluginSandbox_CreateCustomFilter(const SeccompRule* rules, int rule_count);

#endif // PLUGIN_SANDBOX_H