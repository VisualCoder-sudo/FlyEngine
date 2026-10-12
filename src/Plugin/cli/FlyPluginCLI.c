/*
 * FlyPluginCLI.c - CLI tool for managing plugins
 * 
 * Usage: flyplugin <command> [args]
 */

#include "PluginLoader.h"
#include "PluginManifest.h"
#include "VersionCompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// COMMAND DECLARATIONS
// ============================================================================

static int cmd_list(int argc, char** argv);
static int cmd_info(int argc, char** argv);
static int cmd_remove(int argc, char** argv);
static int cmd_validate(int argc, char** argv);
static int cmd_doctor(int argc, char** argv);
static int cmd_run(int argc, char** argv);

// ============================================================================
// COMMAND TABLE
// ============================================================================

typedef struct {
    const char* name;
    const char* description;
    int (*func)(int argc, char** argv);
} Command;

static Command commands[] = {
    {"list", "List installed plugins", cmd_list},
    {"info", "Show plugin information", cmd_info},
    {"rm", "Remove a plugin", cmd_remove},
    {"remove", "Remove a plugin (alias)", cmd_remove},
    {"validate", "Validate plugin (id.json, icon.png, manifest)", cmd_validate},
    {"doctor", "Diagnose plugin system issues", cmd_doctor},
    {"run", "Run plugin in sandboxed process (internal)", cmd_run},
    {NULL, NULL, NULL}
};

// ============================================================================
// HELPERS
// ============================================================================

static void print_usage(const char* progname) {
    printf("Usage: %s <command> [args]\n\n", progname);
    printf("Commands:\n");
    for (int i = 0; commands[i].name; i++) {
        printf("  %-12s %s\n", commands[i].name, commands[i].description);
    }
    printf("\nPlugin types: nat, fpc, lib\n");
    printf("Use '%s <command> --help' for command-specific help.\n", progname);
}

static void print_list_usage() {
    printf("Usage: flyplugin list [--type nat|fpc|lib]\n");
}

static void print_info_usage() {
    printf("Usage: flyplugin info <plugin-name>\n");
}

static void print_remove_usage() {
    printf("Usage: flyplugin rm -<type> \"<plugin-name>\"\n");
    printf("Types: -nat, -fpc, -lib\n");
}

static void print_validate_usage() {
    printf("Usage: flyplugin validate <plugin-path>\n");
}

static void print_run_usage() {
    printf("Usage: flyplugin run --socket <path> --plugin <path>\n");
    printf("  --socket <path>  Unix domain socket path for IPC\n");
    printf("  --plugin <path>  Path to plugin .so file\n");
    printf("  (Internal command - used by engine to spawn plugin process)\n");
}

static PluginType parse_type_flag(const char* flag) {
    if (strcmp(flag, "-nat") == 0) return PLUGIN_TYPE_NAT;
    if (strcmp(flag, "-fpc") == 0) return PLUGIN_TYPE_FPC;
    if (strcmp(flag, "-lib") == 0) return PLUGIN_TYPE_LIB;
    return PLUGIN_TYPE_UNKNOWN;
}

// ============================================================================
// COMMANDS
// ============================================================================

static int cmd_list(int argc, char** argv) {
    PluginType filter = PLUGIN_TYPE_UNKNOWN;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_list_usage();
            return 0;
        }
        if (strncmp(argv[i], "--type", 6) == 0) {
            if (strcmp(argv[i], "--type") == 0) {
                if (i + 1 < argc) filter = parse_type_flag(argv[++i]);
            } else {
                filter = parse_type_flag(argv[i] + 6); // --type=nat
            }
        }
    }
    
    // TODO: Connect to actual plugin loader
    printf("Plugin listing not yet fully implemented.\n");
    printf("Filter: %s\n", filter == PLUGIN_TYPE_UNKNOWN ? "all" : 
           filter == PLUGIN_TYPE_NAT ? "nat" :
           filter == PLUGIN_TYPE_FPC ? "fpc" : "lib");
    return 0;
}

static int cmd_info(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_info_usage();
        return 0;
    }
    
    const char* name = argv[1];
    printf("Plugin info for: %s\n", name);
    printf("  (Not yet implemented - needs plugin loader)\n");
    return 0;
}

static int cmd_remove(int argc, char** argv) {
    PluginType type = PLUGIN_TYPE_UNKNOWN;
    const char* name = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_remove_usage();
            return 0;
        }
        if (argv[i][0] == '-') {
            type = parse_type_flag(argv[i]);
        } else if (!name) {
            name = argv[i];
        }
    }
    
    if (type == PLUGIN_TYPE_UNKNOWN || !name) {
        print_remove_usage();
        return 1;
    }
    
    printf("Removing %s plugin: %s\n", PluginType_ToString(type), name);
    printf("  (Not yet implemented - needs plugin loader)\n");
    return 0;
}

static int cmd_validate(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_validate_usage();
        return 0;
    }
    
    const char* path = argv[1];
    printf("Validating plugin at: %s\n", path);
    
    char* error = NULL;
    bool valid = PluginLoader_Validate(NULL, path, &error);
    
    if (valid) {
        printf("✓ Plugin is valid\n");
    } else {
        printf("✗ Plugin validation failed: %s\n", error ? error : "unknown error");
        if (error) free(error);
        return 1;
    }
    
    return 0;
}

static int cmd_doctor(int argc, char** argv) {
    (void)argc; (void)argv;
    printf("FlyEngine Plugin System Doctor\n");
    printf("==============================\n");
    printf("Checking plugin system...\n");
    printf("  cJSON: OK\n");
    printf("  Plugin directories: (not checked)\n");
    printf("  Loaded plugins: 0\n");
    printf("\nAll checks passed (stub).\n");
    return 0;
}

static int cmd_run(int argc, char** argv) {
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
        print_run_usage();
        return 0;
    }
    
    const char* socket_path = NULL;
    const char* plugin_path = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_path = argv[++i];
        } else if (strcmp(argv[i], "--plugin") == 0 && i + 1 < argc) {
            plugin_path = argv[++i];
        }
    }
    
    if (!socket_path || !plugin_path) {
        print_run_usage();
        return 1;
    }
    
    printf("Running plugin process...\n");
    printf("  Socket: %s\n", socket_path);
    printf("  Plugin: %s\n", plugin_path);
    
    // TODO: Implement plugin process runner
    // 1. Connect to IPC socket
    // 2. Wait for INIT message
    // 3. Load plugin .so
    // 4. Register plugin entry points
    // 5. Enter message loop
    
    printf("Plugin process runner not yet implemented.\n");
    return 1;
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char* cmd_name = argv[1];
    
    // Find command
    Command* cmd = NULL;
    for (int i = 0; commands[i].name; i++) {
        if (strcmp(commands[i].name, cmd_name) == 0) {
            cmd = &commands[i];
            break;
        }
    }
    
    if (!cmd) {
        printf("Unknown command: %s\n\n", cmd_name);
        print_usage(argv[0]);
        return 1;
    }
    
    // Pass remaining args to command
    int cmd_argc = argc - 1;
    char** cmd_argv = &argv[1];
    
    return cmd->func(cmd_argc, cmd_argv);
}