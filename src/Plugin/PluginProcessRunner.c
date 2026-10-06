/*
 * PluginProcessRunner.c - Plugin process entry point for sandboxed execution
 * 
 * This runs inside the child process spawned by the engine.
 * It connects to the IPC socket, loads the plugin .so, and forwards calls.
 */

#include "PluginIPC.h"
#include "CPluginAPI.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

// ============================================================================
// GLOBAL STATE
// ============================================================================

typedef struct {
    CP_IPC_Channel* channel;
    void* plugin_handle;
    const CP_PluginEntry* plugin_entry;
    bool running;
    uint32_t plugin_id;
} PluginProcessState;

static PluginProcessState g_state = {0};

// ============================================================================
// SIGNAL HANDLING
// ============================================================================

static void handle_signal(int sig) {
    g_state.running = false;
}

// ============================================================================
// PLUGIN LOADING
// ============================================================================

static bool load_plugin(const char* plugin_path) {
    // Load the plugin shared library
    g_state.plugin_handle = dlopen(plugin_path, RTLD_LAZY | RTLD_LOCAL);
    if (!g_state.plugin_handle) {
        fprintf(stderr, "[PluginRunner] Failed to load plugin: %s\n", dlerror());
        return false;
    }
    
    // Get plugin entry point
    CP_PluginEntry* (*get_entry)(void) = dlsym(g_state.plugin_handle, "CP_GetPluginEntry");
    if (!get_entry) {
        fprintf(stderr, "[PluginRunner] Plugin missing CP_GetPluginEntry symbol\n");
        dlclose(g_state.plugin_handle);
        return false;
    }
    
    g_state.plugin_entry = get_entry();
    if (!g_state.plugin_entry) {
        fprintf(stderr, "[PluginRunner] CP_GetPluginEntry returned NULL\n");
        dlclose(g_state.plugin_handle);
        return false;
    }
    
    // Verify API version compatibility
    CP_API_Version engine_ver = CP_API_CURRENT_VERSION;
    CP_API_Version plugin_ver = g_state.plugin_entry->api_version;
    if (!CP_API_VersionAtLeast(engine_ver, plugin_ver)) {
        fprintf(stderr, "[PluginRunner] Plugin API version newer than engine supports\n");
        dlclose(g_state.plugin_handle);
        return false;
    }
    
    fprintf(stderr, "[PluginRunner] Loaded plugin: %s v%s\n", 
            g_state.plugin_entry->plugin_name, g_state.plugin_entry->plugin_version);
    return true;
}

// ============================================================================
// MESSAGE HANDLERS
// ============================================================================

static bool handle_init(const CP_IPC_PayloadInit* payload) {
    // The engine sends INIT with API version and capabilities
    // We need to call the plugin's on_load with the engine API
    
    if (g_state.plugin_entry && g_state.plugin_entry->on_load) {
        // For now, we can't pass the real engine API from the host
        // In a full implementation, the host would send function pointers
        // For now, we'll call on_load with a minimal API
        int result = g_state.plugin_entry->on_load(NULL);
        if (result != 0) {
            fprintf(stderr, "[PluginRunner] Plugin on_load failed with code %d\n", result);
            return false;
        }
    }
    return true;
}

static bool handle_shutdown() {
    if (g_state.plugin_entry && g_state.plugin_entry->on_unload) {
        g_state.plugin_entry->on_unload();
    }
    return true;
}

static bool handle_update(float delta_time) {
    if (g_state.plugin_entry && g_state.plugin_entry->on_update) {
        g_state.plugin_entry->on_update(delta_time);
    }
    return true;
}

static bool handle_event(const CP_Event* event) {
    if (g_state.plugin_entry && g_state.plugin_entry->on_event) {
        g_state.plugin_entry->on_event(event);
    }
    return true;
}

static bool handle_call_function(const CP_IPC_PayloadCallFunction* payload, uint32_t seq_id) {
    // The engine is calling a function exported by the plugin
    // We need to look up the function in the plugin's symbol table
    
    if (!g_state.plugin_handle) {
        // Send error response
        return false;
    }
    
    void* func = dlsym(g_state.plugin_handle, payload->function_name);
    if (!func) {
        fprintf(stderr, "[PluginRunner] Function not found: %s\n", payload->function_name);
        return false;
    }
    
    // For now, we can't actually call the function without knowing its signature
    // In a full implementation, we'd need a proper FFI mechanism
    fprintf(stderr, "[PluginRunner] Function call requested: %s (not implemented)\n", payload->function_name);
    return true;
}

// ============================================================================
// MAIN MESSAGE LOOP
// ============================================================================

static void run_message_loop() {
    g_state.running = true;
    
    // Set up signal handlers
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    
    while (g_state.running) {
        CP_IPC_MsgHeader header;
        void* payload = NULL;
        uint32_t payload_size = 0;
        
        CP_IPC_MsgType msg_type = CP_IPC_Receive(g_state.channel, 100, &header, &payload, NULL);
        
        if (msg_type == CP_IPC_MSG_NONE) {
            // Timeout - send heartbeat
            CP_IPC_PayloadHeartbeat hb = {
                .timestamp = (uint64_t)time(NULL),
                .memory_used_kb = 0,
                .cpu_percent = 0
            };
            CP_IPC_Send(g_state.channel, CP_IPC_MSG_HEARTBEAT, CP_IPC_GetNextSeqId(g_state.channel), &hb, sizeof(hb));
            continue;
        }
        
        bool success = true;
        
        switch (msg_type) {
            case CP_IPC_MSG_INIT:
                success = handle_init((const CP_IPC_PayloadInit*)payload);
                break;
            case CP_IPC_MSG_SHUTDOWN:
                success = handle_shutdown();
                g_state.running = false;
                break;
            case CP_IPC_MSG_UPDATE:
                if (header.payload_size >= sizeof(float)) {
                    success = handle_update(*(const float*)payload);
                }
                break;
            case CP_IPC_MSG_EVENT:
                if (header.payload_size >= sizeof(CP_Event)) {
                    success = handle_event((const CP_Event*)payload);
                }
                break;
            case CP_IPC_MSG_CALL_FUNCTION:
                success = handle_call_function((const CP_IPC_PayloadCallFunction*)payload, header.sequence_id);
                break;
            default:
                fprintf(stderr, "[PluginRunner] Unknown message type: %d\n", msg_type);
                success = false;
                break;
        }
        
        // Send response if this was a request
        if (header.sequence_id > 0) {
            CP_IPC_PayloadCallResult result = {
                .sequence_id = header.sequence_id,
                .result_code = success ? 0 : -1,
                .result_size = 0
            };
            CP_IPC_Send(g_state.channel, CP_IPC_MSG_CALL_RESULT, header.sequence_id, &result, sizeof(result));
        }
        
        if (payload) CP_IPC_FreePayload(payload);
    }
}

// ============================================================================
// MAIN ENTRY POINT
// ============================================================================

int main(int argc, char** argv) {
    // Parse arguments
    const char* socket_path = NULL;
    const char* plugin_path = NULL;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            g_state.channel = CP_IPC_ConnectChannel(argv[++i]);
        } else if (strcmp(argv[i], "--plugin") == 0 && i + 1 < argc) {
            plugin_path = argv[++i];
        }
    }
    
    if (!g_state.channel || !plugin_path) {
        fprintf(stderr, "Usage: %s --socket <path> --plugin <path>\n", argv[0]);
        return 1;
    }
    
    fprintf(stderr, "[PluginRunner] Connected to socket, loading plugin...\n");
    
    // Load the plugin
    if (!load_plugin(plugin_path)) {
        if (g_state.channel) CP_IPC_CloseChannel(g_state.channel);
        return 1;
    }
    
    fprintf(stderr, "[PluginRunner] Entering message loop...\n");
    
    // Run message loop
    run_message_loop();
    
    // Cleanup
    if (g_state.plugin_handle) dlclose(g_state.plugin_handle);
    if (g_state.channel) CP_IPC_CloseChannel(g_state.channel);
    
    fprintf(stderr, "[PluginRunner] Exited cleanly\n");
    return 0;
}