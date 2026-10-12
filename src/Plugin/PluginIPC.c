/*
 * PluginIPC.c - Inter-process communication for sandboxed plugins (Linux)
 * 
 * Uses Unix domain sockets for message passing between engine host and plugin processes.
 */

#include "PluginIPC.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/poll.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

// ============================================================================
// INTERNAL STRUCTURES
// ============================================================================

struct CP_IPC_Channel {
    int socket_fd;              // Connected socket
    bool is_server;             // true = engine (listener), false = plugin (client)
    char socket_path[108];      // Unix domain socket path
    uint32_t plugin_id;         // Plugin identifier
    uint32_t next_seq_id;       // Next sequence ID for requests
};

// Message buffer for receiving
#define MAX_MSG_SIZE (64 * 1024)  // 64KB max message

// ============================================================================
// INTERNAL HELPERS
// ============================================================================

static bool send_all(int fd, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t sent = send(fd, p, remaining, MSG_NOSIGNAL);
        if (sent <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += sent;
        remaining -= sent;
    }
    return true;
}

static bool recv_all(int fd, void* buf, size_t len) {
    char* p = (char*)buf;
    size_t remaining = len;
    while (remaining > 0) {
        ssize_t recvd = recv(fd, p, remaining, 0);
        if (recvd <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += recvd;
        remaining -= recvd;
    }
    return true;
}

static bool send_message(int fd, CP_IPC_MsgType type, uint32_t seq_id, 
                         const void* payload, uint32_t payload_size) {
    CP_IPC_MsgHeader header = {
        .type = type,
        .sequence_id = seq_id,
        .payload_size = payload_size,
        .plugin_id = 0  // Will be set by caller
    };
    
    if (!send_all(fd, &header, sizeof(header))) return false;
    if (payload_size > 0 && payload) {
        if (!send_all(fd, payload, payload_size)) return false;
    }
    return true;
}

static bool recv_message(int fd, int timeout_ms, 
                         CP_IPC_MsgHeader* header_out, void** payload_out, uint32_t* payload_size_out) {
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int poll_result = poll(&pfd, 1, timeout_ms);
    if (poll_result <= 0) return false;
    
    CP_IPC_MsgHeader header;
    if (!recv_all(fd, &header, sizeof(header))) return false;
    
    if (header.payload_size > MAX_MSG_SIZE) return false;
    
    void* payload = NULL;
    if (header.payload_size > 0) {
        payload = malloc(header.payload_size);
        if (!payload) return false;
        if (!recv_all(fd, payload, header.payload_size)) {
            free(payload);
            return false;
        }
    }
    
    *header_out = header;
    *payload_out = payload;
    *payload_size_out = header.payload_size;
    return true;
}

// ============================================================================
// CHANNEL MANAGEMENT
// ============================================================================

CP_IPC_Channel* CP_IPC_CreateChannel(uint32_t plugin_id, char* socket_path_out, size_t path_capacity) {
    CP_IPC_Channel* channel = calloc(1, sizeof(CP_IPC_Channel));
    if (!channel) return NULL;
    
    channel->plugin_id = plugin_id;
    channel->is_server = true;
    channel->next_seq_id = 1;
    
    // Create socket
    channel->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (channel->socket_fd < 0) {
        free(channel);
        return NULL;
    }
    
    // Generate unique socket path
    snprintf(channel->socket_path, sizeof(channel->socket_path), 
             "/tmp/flyengine_plugin_%u_%d.sock", plugin_id, getpid());
    
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, channel->socket_path, sizeof(addr.sun_path) - 1);
    
    // Remove any existing socket file
    unlink(channel->socket_path);
    
    if (bind(channel->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(channel->socket_fd);
        free(channel);
        return NULL;
    }
    
    if (listen(channel->socket_fd, 1) < 0) {
        close(channel->socket_fd);
        unlink(channel->socket_path);
        free(channel);
        return NULL;
    }
    
    // Set non-blocking for accept with timeout
    int flags = fcntl(channel->socket_fd, F_GETFL, 0);
    fcntl(channel->socket_fd, F_SETFL, flags | O_NONBLOCK);
    
    if (socket_path_out && path_capacity > 0) {
        strncpy(socket_path_out, channel->socket_path, path_capacity - 1);
        socket_path_out[path_capacity - 1] = '\0';
    }
    
    return channel;
}

CP_IPC_Channel* CP_IPC_ConnectChannel(const char* socket_path) {
    CP_IPC_Channel* channel = calloc(1, sizeof(CP_IPC_Channel));
    if (!channel) return NULL;
    
    channel->is_server = false;
    strncpy(channel->socket_path, socket_path, sizeof(channel->socket_path) - 1);
    
    channel->socket_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (channel->socket_fd < 0) {
        free(channel);
        return NULL;
    }
    
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    
    // Connect with retry
    int retries = 50;
    while (retries-- > 0) {
        if (connect(channel->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            break;
        }
        if (errno != ENOENT && errno != ECONNREFUSED) {
            close(channel->socket_fd);
            free(channel);
            return NULL;
        }
        usleep(10000);  // 10ms
    }
    
    if (retries == 0) {
        close(channel->socket_fd);
        free(channel);
        return NULL;
    }
    
    return channel;
}

void CP_IPC_CloseChannel(CP_IPC_Channel* channel) {
    if (!channel) return;
    if (channel->socket_fd >= 0) {
        close(channel->socket_fd);
    }
    if (channel->is_server) {
        unlink(channel->socket_path);
    }
    free(channel);
}

// ============================================================================
// SEND/RECEIVE
// ============================================================================

bool CP_IPC_Send(CP_IPC_Channel* channel, CP_IPC_MsgType type, uint32_t seq_id, 
                 const void* payload, uint32_t payload_size) {
    if (!channel || channel->socket_fd < 0) return false;
    return send_message(channel->socket_fd, type, seq_id, payload, payload_size);
}

CP_IPC_MsgType CP_IPC_Receive(CP_IPC_Channel* channel, uint32_t timeout_ms,
                              CP_IPC_MsgHeader* header_out, void** payload_out, uint32_t* payload_size_out) {
    if (!channel || channel->socket_fd < 0) return CP_IPC_MSG_NONE;
    
    // For server, accept connection first
    if (channel->is_server) {
        struct pollfd pfd = { .fd = channel->socket_fd, .events = POLLIN };
        int poll_result = poll(&pfd, 1, timeout_ms);
        if (poll_result <= 0) return CP_IPC_MSG_NONE;
        
        int client_fd = accept(channel->socket_fd, NULL, NULL);
        if (client_fd < 0) return CP_IPC_MSG_NONE;
        
        // Replace listening socket with connected socket
        close(channel->socket_fd);
        channel->socket_fd = client_fd;
        channel->is_server = false;  // Now it's a connected socket
    }
    
    CP_IPC_MsgHeader header;
    void* payload = NULL;
    uint32_t payload_size = 0;
    
    if (!recv_message(channel->socket_fd, timeout_ms, &header, &payload, &payload_size)) {
        return CP_IPC_MSG_NONE;
    }
    
    if (header_out) *header_out = header;
    if (payload_out) *payload_out = payload;
    else if (payload) free(payload);
    if (payload_size_out) *payload_size_out = header.payload_size;
    
    return header.type;
}

void CP_IPC_FreePayload(void* payload) {
    free(payload);
}

// ============================================================================
// PLUGIN PROCESS MANAGEMENT
// ============================================================================

static void setup_child_env(const char* socket_path) {
    // Set environment variable for plugin to find socket
    setenv("FLYENGINE_IPC_SOCKET", socket_path, 1);
}

int CP_IPC_SpawnPluginProcess(const char* plugin_path, const char* plugin_name, 
                              char* socket_path_out, size_t path_capacity) {
    // Create channel first to get socket path
    CP_IPC_Channel* channel = CP_IPC_CreateChannel(0, socket_path_out, path_capacity);
    if (!channel) return -1;
    
    // Get the socket path before closing channel
    char socket_path[108];
    strncpy(socket_path, channel->socket_path, sizeof(socket_path) - 1);
    CP_IPC_CloseChannel(channel);
    
    // Fork
    pid_t pid = fork();
    if (pid < 0) {
        unlink(socket_path);
        return -1;
    }
    
    if (pid == 0) {
        // Child process - exec plugin
        // The plugin will be a small loader that connects to IPC and loads the actual .so
        execlp("flyplugin", "flyplugin", "run", "--socket", socket_path, 
               "--plugin", plugin_path, NULL);
        
        // If we get here, exec failed
        _exit(127);
    }
    
    // Parent - wait for connection
    // The channel was closed, we'll reconnect in the engine
    return pid;
}

int CP_IPC_WaitPluginProcess(int pid, int timeout_ms) {
    if (pid <= 0) return -1;
    
    int status;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (true) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return -1;
        }
        
        clock_gettime(CLOCK_MONOTONIC, &now);
        long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000 + 
                          (now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= timeout_ms) return -1;
        
        usleep(10000);  // 10ms
    }
}

void CP_IPC_KillPluginProcess(int pid) {
    if (pid > 0) {
        kill(pid, SIGTERM);
        usleep(100000);  // 100ms
        kill(pid, SIGKILL);  // Force if still alive
        waitpid(pid, NULL, 0);
    }
}

uint32_t CP_IPC_GetNextSeqId(CP_IPC_Channel* channel) {
    if (!channel) return 0;
    return channel->next_seq_id++;
}

int CP_IPC_GetSocketFd(CP_IPC_Channel* channel) {
    if (!channel) return -1;
    return channel->socket_fd;
}