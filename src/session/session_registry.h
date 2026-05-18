#pragma once

#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>

namespace xwave {

// Session information structure
struct SessionInfo {
    std::string session_id;         // Unique session name
    std::string socket_path;    // Unix domain socket path
    std::string fsdb_file;      // FSDB file opened
    pid_t server_pid;           // Server process ID
    time_t created_at;          // Creation timestamp
    time_t last_active;         // Last command activity timestamp
    long fsdb_mtime;            // FSDB modification timestamp
    long long fsdb_size;        // FSDB size in bytes
    unsigned long long fsdb_dev;    // FSDB device ID
    unsigned long long fsdb_inode;  // FSDB inode

    SessionInfo()
        : session_id(),
          server_pid(0),
          created_at(0),
          last_active(0),
          fsdb_mtime(0),
          fsdb_size(0),
          fsdb_dev(0),
          fsdb_inode(0) {}
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

    // Replace or add a session record
    bool upsert(const SessionInfo& session);

    bool exists(const std::string& session_id);

    static bool is_valid_session_name(const std::string& name);

    // Update last active timestamp
    bool touch(const std::string& session_id, time_t last_active);
    bool touch(int session_id, time_t last_active) { return touch(std::to_string(session_id), last_active); }

    // Remove a session from registry
    bool remove(const std::string& session_id);
    bool remove(int session_id) { return remove(std::to_string(session_id)); }

    // Get session by ID
    bool get(const std::string& session_id, SessionInfo& session);
    bool get(int session_id, SessionInfo& session) { return get(std::to_string(session_id), session); }

    // Get the latest session (highest ID)
    bool get_latest(SessionInfo& session);

    // Clean up stale sessions (dead processes)
    bool cleanup_stale();

    // Clean up sessions idle longer than timeout_sec
    bool cleanup_idle(time_t now, int timeout_sec);

    // Clear all sessions
    bool clear_all();

private:
    std::string registry_path_;

    // File locking for concurrent access
    bool lock_file(int fd);
    bool unlock_file(int fd);

    // Parse a single line from legacy registry file
    bool parse_legacy_line(const char* line, SessionInfo& session);

    bool load_legacy(std::vector<SessionInfo>& sessions);
    bool save_all(const std::vector<SessionInfo>& sessions);
    bool write_session_file(const SessionInfo& session);
};

} // namespace xtrace
