/*
 * PluginIPC.h - Inter-process communication for sandboxed plugins
 * 
 * Uses Unix domain sockets (Linux) / Named pipes (Windows) for message passing
 * between engine host and plugin processes.
 */

#ifndef PLUGIN_IPC_H
#define PLUGIN_IPC_H

#include "CPluginAPI.h"
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// MESSAGE TYPES
// ============================================================================

typedef enum {
    CP_IPC_MSG_NONE = 0,
    CP_IPC_MSG_INIT,              // Engine -> Plugin: initialize with API vtable
    CP_IPC_MSG_SHUTDOWN,          // Engine -> Plugin: shutdown gracefully
    CP_IPC_MSG_UPDATE,            // Engine -> Plugin: per-frame update
    CP_IPC_MSG_EVENT,             // Engine -> Plugin: event notification
    CP_IPC_MSG_CALL_FUNCTION,     // Engine -> Plugin: call exported function
    CP_IPC_MSG_CALL_RESULT,       // Plugin -> Engine: function call result
    CP_IPC_MSG_LOG,               // Plugin -> Engine: log message
    CP_IPC_MSG_ERROR,             // Plugin -> Engine: error message
    CP_IPC_MSG_HEARTBEAT,         // Plugin -> Engine: alive signal
    CP_IPC_MSG_REGISTER_API,      // Plugin -> Engine: register custom API
} CP_IPC_MsgType;

// ============================================================================
// MESSAGE HEADER
// ============================================================================

typedef struct {
    CP_IPC_MsgType type;
    uint32_t sequence_id;      // For request/response matching
    uint32_t payload_size;     // Size of payload following header
    uint32_t plugin_id;        // Unique plugin identifier
} CP_IPC_MsgHeader;

// ============================================================================
// PAYLOAD STRUCTURES
// ============================================================================

// INIT: Engine sends API vtable pointer (serialized function pointers not possible,
// so we send a capability bitmap and plugin looks up functions via name)
typedef struct {
    CP_API_Version api_version;
    uint32_t capability_flags;  // Which engine subsystems are available
    // Function pointers are resolved via CP_Internal_GetFunction in plugin process
} CP_IPC_PayloadInit;

// UPDATE: Per-frame delta time
typedef struct {
    float delta_time;
} CP_IPC_PayloadUpdate;

// EVENT: Engine event forwarded to plugin
typedef struct {
    CP_Event event;
} CP_IPC_PayloadEvent;

// CALL_FUNCTION: Engine calls plugin's exported function
typedef struct {
    char function_name[64];
    uint32_t args_size;
    // args follow immediately after this struct
} CP_IPC_PayloadCallFunction;

// CALL_RESULT: Plugin returns function call result
typedef struct {
    uint32_t sequence_id;      // Matches original call
    int32_t result_code;       // Return value
    uint32_t result_size;      // Size of result data
    // result data follows
} CP_IPC_PayloadCallResult;

// LOG: Plugin log message
typedef struct {
    CP_LogLevel level;
    char tag[32];
    uint32_t message_size;
    // message follows
} CP_IPC_PayloadLog;

// ERROR: Plugin error
typedef struct {
    int32_t error_code;
    uint32_t message_size;
    // message follows
} CP_IPC_PayloadError;

// HEARTBEAT: Plugin alive signal
typedef struct {
    uint64_t timestamp;
    uint32_t memory_used_kb;
    uint32_t cpu_percent;
} CP_IPC_PayloadHeartbeat;

// REGISTER_API: Plugin registers custom API for other plugins
typedef struct {
    char api_name[64];
    uint32_t function_count;
    // function names follow
} CP_IPC_PayloadRegisterAPI;

// ============================================================================
// IPC CHANNEL
// ============================================================================

typedef struct CP_IPC_Channel CP_IPC_Channel;

// Create IPC channel for plugin process (called by engine before spawn)
// Returns channel handle, sets socket_path for Unix domain socket
CP_IPC_Channel* CP_IPC_CreateChannel(uint32_t plugin_id, char* socket_path_out, size_t path_capacity);

// Connect to existing channel (called by plugin process on startup)
CP_IPC_Channel* CP_IPC_ConnectChannel(const char* socket_path);

// Close and cleanup channel
void CP_IPC_CloseChannel(CP_IPC_Channel* channel);

// Send message (blocks until sent)
bool CP_IPC_Send(CP_IPC_Channel* channel, CP_IPC_MsgType type, uint32_t seq_id, 
                 const void* payload, uint32_t payload_size);

// Receive message (blocks until received or timeout_ms)
// Returns message type, or CP_IPC_MSG_NONE on timeout/error
CP_IPC_MsgType CP_IPC_Receive(CP_IPC_Channel* channel, uint32_t timeout_ms,
                              CP_IPC_MsgHeader* header_out, void** payload_out, uint32_t* payload_size_out);

// Free payload received via CP_IPC_Receive
void CP_IPC_FreePayload(void* payload);

// Channel accessors (for plugin process runner)
uint32_t CP_IPC_GetNextSeqId(CP_IPC_Channel* channel);
int CP_IPC_GetSocketFd(CP_IPC_Channel* channel);

// ============================================================================
// PLUGIN PROCESS ENTRY POINT
// ============================================================================

// Called by engine to spawn plugin process
// Returns process ID, or -1 on failure
int CP_IPC_SpawnPluginProcess(const char* plugin_path, const char* plugin_name, 
                              char* socket_path_out, size_t path_capacity);

// Wait for plugin process to exit (with timeout)
// Returns exit code, or -1 on timeout/error
int CP_IPC_WaitPluginProcess(int pid, int timeout_ms);

// Kill plugin process
void CP_IPC_KillPluginProcess(int pid);

#endif // PLUGIN_IPC_H