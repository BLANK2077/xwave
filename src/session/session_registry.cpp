#include "session_registry.h"
#include "../protocol/protocol.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>

namespace xwave {

SessionRegistry::SessionRegistry() {
    get_registry_path(registry_path_);
}

SessionRegistry::~SessionRegistry() {
}

bool SessionRegistry::lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

bool SessionRegistry::unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

std::string SessionRegistry::serialize(const SessionInfo& session) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "%d|%s|%s|%d|%ld|%ld|%ld|%lld|%llu|%llu\n",
             session.session_id,
             session.socket_path.c_str(),
             session.fsdb_file.c_str(),
             session.server_pid,
             session.created_at,
             session.last_active,
             session.fsdb_mtime,
             session.fsdb_size,
             session.fsdb_dev,
             session.fsdb_inode);
    return std::string(buf);
}

bool SessionRegistry::parse_line(const char* line, SessionInfo& session) {
    char socket_path[512] = {};
    char fsdb_file[1024] = {};
    int pid;
    long created_at;
    long last_active = 0;
    long fsdb_mtime = 0;
    long long fsdb_size = 0;
    unsigned long long fsdb_dev = 0;
    unsigned long long fsdb_inode = 0;

    int matched = sscanf(line, "%d|%511[^|]|%1023[^|]|%d|%ld|%ld|%ld|%lld|%llu|%llu",
               &session.session_id,
               socket_path,
               fsdb_file,
               &pid,
               &created_at,
               &last_active,
               &fsdb_mtime,
               &fsdb_size,
               &fsdb_dev,
               &fsdb_inode);
    if (matched != 5 && matched != 10) {
        return false;
    }

    session.socket_path = socket_path;
    session.fsdb_file = fsdb_file;
    session.server_pid = pid;
    session.created_at = created_at;
    session.last_active = (matched == 10) ? last_active : created_at;
    session.fsdb_mtime = fsdb_mtime;
    session.fsdb_size = fsdb_size;
    session.fsdb_dev = fsdb_dev;
    session.fsdb_inode = fsdb_inode;
    if (matched == 5) {
        struct stat st;
        if (stat(session.fsdb_file.c_str(), &st) == 0) {
            session.fsdb_mtime = static_cast<long>(st.st_mtime);
            session.fsdb_size = static_cast<long long>(st.st_size);
            session.fsdb_dev = static_cast<unsigned long long>(st.st_dev);
            session.fsdb_inode = static_cast<unsigned long long>(st.st_ino);
        }
    }
    return true;
}

bool SessionRegistry::load_all(std::vector<SessionInfo>& sessions) {
    sessions.clear();

    int fd = open(registry_path_, O_RDONLY | O_CREAT, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        unlock_file(fd);
        close(fd);
        return false;
    }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        SessionInfo session;
        if (parse_line(line, session)) {
            sessions.push_back(session);
        }
    }

    fclose(fp);  // This also closes fd
    return true;
}

bool SessionRegistry::add(const SessionInfo& session) {
    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    std::string data = serialize(session);
    ssize_t written = write(fd, data.c_str(), data.length());

    unlock_file(fd);
    close(fd);

    return written == (ssize_t)data.length();
}

bool SessionRegistry::upsert(const SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    load_all(sessions);

    bool replaced = false;
    for (auto& s : sessions) {
        if (s.session_id == session.session_id) {
            s = session;
            replaced = true;
            break;
        }
    }
    if (!replaced) sessions.push_back(session);

    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    bool ok = true;
    for (const auto& s : sessions) {
        std::string data = serialize(s);
        if (write(fd, data.c_str(), data.length()) != (ssize_t)data.length()) ok = false;
    }

    unlock_file(fd);
    close(fd);
    return ok;
}

bool SessionRegistry::touch(int session_id, time_t last_active) {
    SessionInfo session;
    if (!get(session_id, session)) return false;
    session.last_active = last_active;
    return upsert(session);
}

bool SessionRegistry::remove(int session_id) {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    // Rewrite registry without the removed session
    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    bool found = false;
    for (const auto& session : sessions) {
        if (session.session_id == session_id) {
            found = true;
            continue;
        }
        std::string data = serialize(session);
        write(fd, data.c_str(), data.length());
    }

    unlock_file(fd);
    close(fd);

    return found;
}

bool SessionRegistry::get(int session_id, SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    for (const auto& s : sessions) {
        if (s.session_id == session_id) {
            session = s;
            return true;
        }
    }
    return false;
}

bool SessionRegistry::get_latest(SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions) || sessions.empty()) return false;

    // Find session with highest ID
    int max_id = -1;
    size_t max_idx = 0;
    for (size_t i = 0; i < sessions.size(); ++i) {
        if (sessions[i].session_id > max_id) {
            max_id = sessions[i].session_id;
            max_idx = i;
        }
    }

    session = sessions[max_idx];
    return true;
}

int SessionRegistry::get_next_id() {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return 1;

    int max_id = 0;
    for (const auto& s : sessions) {
        if (s.session_id > max_id) {
            max_id = s.session_id;
        }
    }
    return max_id + 1;
}

bool SessionRegistry::cleanup_stale() {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    std::vector<SessionInfo> valid_sessions;

    for (const auto& session : sessions) {
        // Check if process is still alive
        bool is_alive = (kill(session.server_pid, 0) == 0);

        // Also check if socket file exists
        bool socket_exists = (access(session.socket_path.c_str(), F_OK) == 0);

        if (is_alive && socket_exists) {
            valid_sessions.push_back(session);
        } else {
            // Clean up stale socket file
            if (socket_exists) {
                unlink(session.socket_path.c_str());
            }
        }
    }

    // Rewrite registry with only valid sessions
    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    for (const auto& session : valid_sessions) {
        std::string data = serialize(session);
        write(fd, data.c_str(), data.length());
    }

    unlock_file(fd);
    close(fd);

    return true;
}

bool SessionRegistry::cleanup_idle(time_t now, int timeout_sec) {
    if (timeout_sec <= 0) return true;

    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    std::vector<SessionInfo> valid_sessions;
    for (const auto& session : sessions) {
        time_t last = session.last_active ? session.last_active : session.created_at;
        if (last > 0 && now - last > timeout_sec) {
            if (kill(session.server_pid, 0) == 0) kill(session.server_pid, SIGTERM);
            unlink(session.socket_path.c_str());
        } else {
            valid_sessions.push_back(session);
        }
    }

    int fd = open(registry_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    bool ok = true;
    for (const auto& session : valid_sessions) {
        std::string data = serialize(session);
        if (write(fd, data.c_str(), data.length()) != (ssize_t)data.length()) ok = false;
    }

    unlock_file(fd);
    close(fd);
    return ok;
}

bool SessionRegistry::clear_all() {
    // Remove all socket files first
    std::vector<SessionInfo> sessions;
    if (load_all(sessions)) {
        for (const auto& session : sessions) {
            unlink(session.socket_path.c_str());
        }
    }

    // Delete registry file
    unlink(registry_path_);
    return true;
}

} // namespace xtrace
