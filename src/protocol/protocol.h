#pragma once

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define PROTOCOL_VERSION    "1.0"

// Socket path configuration
#define SOCK_PATH_PREFIX    ".xwave"
#define SOCK_PATH_LEN       256
#define REGISTRY_FILE       ".xwave.registry"

// Protocol commands (client -> server)
#define CMD_PING            "PING"
#define CMD_QUIT            "QUIT"
#define CMD_VALUE           "VALUE"
#define CMD_LIST_VALUE      "LIST_VALUE"
#define CMD_LIST_DIFF       "LIST_DIFF"
#define CMD_SIGNAL_CHECK    "SIGNAL_CHECK"
#define CMD_LIST_VALIDATE   "LIST_VALIDATE"
#define CMD_SCOPE           "SCOPE"
#define CMD_APB_WR          "APB_WR"
#define CMD_APB_RD          "APB_RD"
#define CMD_APB_BEGIN       "APB_BEGIN"
#define CMD_APB_NEXT        "APB_NEXT"
#define CMD_APB_PREV        "APB_PREV"
#define CMD_APB_LAST        "APB_LAST"

#define CMD_AXI_WR          "AXI_WR"
#define CMD_AXI_RD          "AXI_RD"
#define CMD_AXI_BEGIN       "AXI_BEGIN"
#define CMD_AXI_NEXT        "AXI_NEXT"
#define CMD_AXI_PREV        "AXI_PREV"
#define CMD_AXI_LAST        "AXI_LAST"
#define CMD_AXI_LATENCY     "AXI_LATENCY"
#define CMD_AXI_OSD         "AXI_OSD"

#define CMD_EVENT_FIND      "EVENT_FIND"
#define CMD_EVENT_EXPORT    "EVENT_EXPORT"
#define CMD_EVENT_FIND_CTX  "EVENT_FIND_CTX"
#define CMD_EVENT_EXPORT_CTX "EVENT_EXPORT_CTX"
#define CMD_AI_QUERY        "AI_QUERY"

// End-of-response marker (server -> client)
#define END_MARKER          "##END##\n"
#define ERROR_PREFIX        "ERROR: "

// Get socket path for a given session ID
inline void get_sock_path(char* buf, int session_id) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, SOCK_PATH_LEN, "%s/%s.%d.sock", home, SOCK_PATH_PREFIX, session_id);
}

// Get registry file path
inline void get_registry_path(char* buf) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, SOCK_PATH_LEN, "%s/%s", home, REGISTRY_FILE);
}

// Get server-side debug log path for a session
inline void get_debug_log_path(char* buf, int session_id) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, SOCK_PATH_LEN, "%s/%s.%d.debug.log", home, SOCK_PATH_PREFIX, session_id);
}
