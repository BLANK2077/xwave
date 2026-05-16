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
    PingFailed,
    FsdbChanged,
    FsdbMissing
};

struct SessionHealth {
    int session_id = 0;
    bool healthy = false;
    SessionHealthStatus status = SessionHealthStatus::RegistryMissing;
    std::string message;
    SessionInfo info;
};

struct SessionDebugOptions {
    bool enabled = false;
};

struct WaitForServerResult {
    bool ok = false;
    std::string reason;
    long elapsed_ms = 0;
    bool socket_exists = false;
    bool connect_ok = false;
    bool ping_ok = false;
    bool child_exited = false;
    int child_status = 0;
    int timeout_sec = 0;
};

const char* session_health_status_name(SessionHealthStatus status);

// Session manager - high-level session lifecycle management
class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    void set_debug_enabled(bool enabled);
    bool debug_enabled() const;

    // Create a new session, returns session ID (0 on failure)
    // This spawns the server process
    int create_session(const std::string& fsdb_file);

    // Restart a session daemon in place, preserving session ID and configs
    bool restart_session(int session_id);

    // Ensure the daemon matches the current FSDB fingerprint
    bool ensure_session_current(int session_id);

    // Update activity timestamp
    bool touch_session(int session_id);

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

    // Clean up stale and idle sessions
    bool gc_sessions();

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
    SessionDebugOptions debug_;

    // Fork and exec server process
    pid_t spawn_server(int session_id, const std::string& fsdb_file);

    bool stop_process(const SessionInfo& session, bool remove_record, bool remove_events);
    bool populate_fsdb_metadata(const std::string& fsdb_file, SessionInfo& session);
    bool current_fsdb_metadata(const SessionInfo& session, SessionInfo& current);
    bool fsdb_metadata_matches(const SessionInfo& expected, const SessionInfo& current) const;
    WaitForServerResult wait_for_server(int session_id, pid_t pid);
    std::string canonicalize_fsdb_path(const std::string& fsdb_file);
    void debug_log(const char* fmt, ...) const;
};

} // namespace xtrace
