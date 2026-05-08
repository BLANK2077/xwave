#pragma once

#include "session_registry.h"
#include <memory>
#include <string>

namespace xwave {

enum class SessionHealthStatus {
    Healthy,
    RegistryMissing,
    ProcessExited,
    SocketMissing,
    ConnectFailed,
    PingFailed
};

struct SessionHealth {
    int session_id = 0;
    bool healthy = false;
    SessionHealthStatus status = SessionHealthStatus::RegistryMissing;
    std::string message;
    SessionInfo info;
};

const char* session_health_status_name(SessionHealthStatus status);

// Session manager - high-level session lifecycle management
class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    // Create a new session, returns session ID (0 on failure)
    // This spawns the server process
    int create_session(const std::string& fsdb_file);

    // Kill a specific session (calls npi_end() in server)
    bool kill_session(int session_id);

    // Kill all sessions
    bool kill_all_sessions();

    // Get session info by ID
    bool get_session(int session_id, SessionInfo& info);

    // Get the latest (most recent) session
    bool get_latest_session(SessionInfo& info);

    // List all active sessions
    std::vector<SessionInfo> list_sessions();

    // Diagnose a session without mutating the registry
    SessionHealth diagnose_session(int session_id);

    // Check if a session is alive
    bool is_session_alive(int session_id);

    // Get socket path for a session
    std::string get_socket_path(int session_id);

    // Clean up stale sessions
    void cleanup();

private:
    std::unique_ptr<SessionRegistry> registry_;

    // Fork and exec server process
    pid_t spawn_server(int session_id, const std::string& fsdb_file);
};

} // namespace xtrace
