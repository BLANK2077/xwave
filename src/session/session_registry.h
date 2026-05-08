#pragma once

#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>

namespace xwave {

// Session information structure
struct SessionInfo {
    int session_id;             // Unique session ID
    std::string socket_path;    // Unix domain socket path
    std::string fsdb_file;      // FSDB file opened
    pid_t server_pid;           // Server process ID
    time_t created_at;          // Creation timestamp

    SessionInfo() : session_id(0), server_pid(0), created_at(0) {}
};

// Session registry - manages persistent storage of session info
class SessionRegistry {
public:
    SessionRegistry();
    ~SessionRegistry();

    // Load all sessions from registry file
    bool load_all(std::vector<SessionInfo>& sessions);

    // Add a new session to registry
    bool add(const SessionInfo& session);

    // Remove a session from registry
    bool remove(int session_id);

    // Get session by ID
    bool get(int session_id, SessionInfo& session);

    // Get the latest session (highest ID)
    bool get_latest(SessionInfo& session);

    // Get next available session ID
    int get_next_id();

    // Clean up stale sessions (dead processes)
    bool cleanup_stale();

    // Clear all sessions
    bool clear_all();

private:
    char registry_path_[256];

    // File locking for concurrent access
    bool lock_file(int fd);
    bool unlock_file(int fd);

    // Parse a single line from registry file
    bool parse_line(const char* line, SessionInfo& session);

    // Serialize session to string
    std::string serialize(const SessionInfo& session);
};

} // namespace xtrace
