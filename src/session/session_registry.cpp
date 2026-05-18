#include "session_registry.h"
#include "../common/xwave_paths.h"
#include "../json.hpp"
#include "../protocol/protocol.h"

#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <cctype>

namespace xwave {

using Json = nlohmann::ordered_json;

SessionRegistry::SessionRegistry() {
    xwave_ensure_home();
    registry_path_ = xwave_registry_path();
}

SessionRegistry::~SessionRegistry() {
}

bool SessionRegistry::lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

bool SessionRegistry::unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

static Json session_to_json(const SessionInfo& session) {
    Json j;
    j["id"] = session.session_id;
    j["session_id"] = session.session_id;
    j["socket_path"] = session.socket_path;
    j["fsdb_file"] = session.fsdb_file;
    j["server_pid"] = session.server_pid;
    j["created_at"] = static_cast<long long>(session.created_at);
    j["last_active"] = static_cast<long long>(session.last_active);
    j["fsdb_mtime"] = session.fsdb_mtime;
    j["fsdb_size"] = session.fsdb_size;
    j["fsdb_dev"] = session.fsdb_dev;
    j["fsdb_inode"] = session.fsdb_inode;
    return j;
}

static bool json_to_session(const Json& j, SessionInfo& session) {
    if (!j.is_object()) return false;
    session.session_id = j.value("id", j.value("session_id", std::string()));
    session.socket_path = j.value("socket_path", "");
    session.fsdb_file = j.value("fsdb_file", "");
    session.server_pid = static_cast<pid_t>(j.value("server_pid", 0));
    session.created_at = static_cast<time_t>(j.value("created_at", 0LL));
    session.last_active = static_cast<time_t>(j.value("last_active", 0LL));
    session.fsdb_mtime = j.value("fsdb_mtime", 0L);
    session.fsdb_size = j.value("fsdb_size", 0LL);
    session.fsdb_dev = j.value("fsdb_dev", 0ULL);
    session.fsdb_inode = j.value("fsdb_inode", 0ULL);
    return !session.session_id.empty() && !session.fsdb_file.empty();
}

bool SessionRegistry::parse_legacy_line(const char* line, SessionInfo& session) {
    (void)line;
    (void)session;
    return false;
}

bool SessionRegistry::load_legacy(std::vector<SessionInfo>& sessions) {
    sessions.clear();
    return false;
}

bool SessionRegistry::load_all(std::vector<SessionInfo>& sessions) {
    sessions.clear();
    xwave_ensure_home();

    int fd = open(registry_path_.c_str(), O_RDONLY);
    if (fd < 0) {
        return true;
    }

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

    std::string text;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) text += buf;
    fclose(fp);

    if (text.empty()) return true;
    try {
        Json root = Json::parse(text);
        if (!root.is_object() || !root.value("sessions", Json::array()).is_array()) return true;
        for (const auto& item : root["sessions"]) {
            SessionInfo session;
            if (json_to_session(item, session)) {
                if (session.socket_path.empty()) session.socket_path = xwave_socket_path(session.session_id);
                sessions.push_back(session);
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool SessionRegistry::write_session_file(const SessionInfo& session) {
    if (!xwave_ensure_session_dir(session.session_id)) return false;
    std::string path = xwave_session_json_path(session.session_id);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    Json root = {
        {"version", 1},
        {"session", session_to_json(session)}
    };
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());
    close(fd);
    return ok;
}

bool SessionRegistry::save_all(const std::vector<SessionInfo>& sessions) {
    xwave_ensure_home();
    int fd = open(registry_path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;

    if (!lock_file(fd)) {
        close(fd);
        return false;
    }

    Json root;
    root["version"] = 1;
    root["sessions"] = Json::array();
    for (const auto& session : sessions) {
        root["sessions"].push_back(session_to_json(session));
    }
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());

    unlock_file(fd);
    close(fd);

    if (!ok) return false;
    for (const auto& session : sessions) {
        write_session_file(session);
    }
    return true;
}

bool SessionRegistry::add(const SessionInfo& session) {
    std::vector<SessionInfo> sessions;
    load_all(sessions);
    sessions.push_back(session);
    return save_all(sessions);
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
    return save_all(sessions);
}

bool SessionRegistry::exists(const std::string& session_id) {
    SessionInfo session;
    return get(session_id, session);
}

bool SessionRegistry::is_valid_session_name(const std::string& name) {
    if (name.empty() || name.size() > 256) return false;
    for (unsigned char c : name) {
        if (!std::isalnum(c) && c != '_' && c != '.' && c != '-') return false;
    }
    return true;
}

bool SessionRegistry::touch(const std::string& session_id, time_t last_active) {
    SessionInfo session;
    if (!get(session_id, session)) return false;
    session.last_active = last_active;
    return upsert(session);
}

bool SessionRegistry::remove(const std::string& session_id) {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    std::vector<SessionInfo> kept;
    bool found = false;
    for (const auto& session : sessions) {
        if (session.session_id == session_id) {
            found = true;
            continue;
        }
        kept.push_back(session);
    }
    if (!found) return false;
    bool ok = save_all(kept);
    xwave_remove_session_dir(session_id);
    return ok;
}

bool SessionRegistry::get(const std::string& session_id, SessionInfo& session) {
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
    session = sessions.back();
    return true;
}

bool SessionRegistry::cleanup_stale() {
    std::vector<SessionInfo> sessions;
    if (!load_all(sessions)) return false;

    std::vector<SessionInfo> valid_sessions;
    for (const auto& session : sessions) {
        bool is_alive = (kill(session.server_pid, 0) == 0);
        bool socket_exists = (access(session.socket_path.c_str(), F_OK) == 0);
        if (is_alive && socket_exists) {
            valid_sessions.push_back(session);
        } else {
            xwave_remove_session_dir(session.session_id);
        }
    }
    return save_all(valid_sessions);
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
            xwave_remove_session_dir(session.session_id);
        } else {
            valid_sessions.push_back(session);
        }
    }
    return save_all(valid_sessions);
}

bool SessionRegistry::clear_all() {
    std::vector<SessionInfo> sessions;
    if (load_all(sessions)) {
        for (const auto& session : sessions) {
            xwave_remove_session_dir(session.session_id);
        }
    }
    std::vector<SessionInfo> empty;
    return save_all(empty);
}

} // namespace xwave
