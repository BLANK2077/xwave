#include "session_manager.h"
#include "../event/event_manager.h"
#include "../protocol/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>
#include <set>
#include <limits.h>

namespace xwave {

namespace {

int open_registry_lock() {
    char lock_path[SOCK_PATH_LEN];
    get_registry_path(lock_path);
    strncat(lock_path, ".lock", sizeof(lock_path) - strlen(lock_path) - 1);
    return open(lock_path, O_RDWR | O_CREAT, 0600);
}

int connect_socket_path(const char* sock_path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

bool ping_socket_path(const char* sock_path) {
    int fd = connect_socket_path(sock_path);
    if (fd < 0) return false;

    const char* ping_msg = CMD_PING "\n";
    if (write(fd, ping_msg, strlen(ping_msg)) < 0) {
        close(fd);
        return false;
    }

    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[64];
    bool got_pong = false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        got_pong = strstr(buf, "PONG") != nullptr;
    }

    close(fd);
    return got_pong;
}

}  // namespace

const char* session_health_status_name(SessionHealthStatus status) {
    switch (status) {
        case SessionHealthStatus::Healthy:
            return "healthy";
        case SessionHealthStatus::RegistryMissing:
            return "registry_missing";
        case SessionHealthStatus::ProcessExited:
            return "process_exited";
        case SessionHealthStatus::SocketMissing:
            return "socket_missing";
        case SessionHealthStatus::ConnectFailed:
            return "connect_failed";
        case SessionHealthStatus::PingFailed:
            return "ping_failed";
        case SessionHealthStatus::FsdbChanged:
            return "fsdb_changed";
        case SessionHealthStatus::FsdbMissing:
            return "fsdb_missing";
    }
    return "unknown";
}

SessionManager::SessionManager() : registry_(new SessionRegistry()) {
}

SessionManager::~SessionManager() {
}

std::string SessionManager::canonicalize_fsdb_path(const std::string& fsdb_file) {
    char resolved[PATH_MAX];
    if (realpath(fsdb_file.c_str(), resolved)) {
        return std::string(resolved);
    }
    return fsdb_file;
}

bool SessionManager::populate_fsdb_metadata(const std::string& fsdb_file, SessionInfo& session) {
    struct stat st;
    if (stat(fsdb_file.c_str(), &st) != 0) return false;
    session.fsdb_file = fsdb_file;
    session.fsdb_mtime = static_cast<long>(st.st_mtime);
    session.fsdb_size = static_cast<long long>(st.st_size);
    session.fsdb_dev = static_cast<unsigned long long>(st.st_dev);
    session.fsdb_inode = static_cast<unsigned long long>(st.st_ino);
    return true;
}

bool SessionManager::current_fsdb_metadata(const SessionInfo& session, SessionInfo& current) {
    current = session;
    return populate_fsdb_metadata(session.fsdb_file, current);
}

bool SessionManager::fsdb_metadata_matches(const SessionInfo& expected, const SessionInfo& current) const {
    return expected.fsdb_mtime == current.fsdb_mtime &&
           expected.fsdb_size == current.fsdb_size &&
           expected.fsdb_dev == current.fsdb_dev &&
           expected.fsdb_inode == current.fsdb_inode;
}

bool SessionManager::wait_for_server(int session_id, pid_t pid) {
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);

    for (int i = 0; i < 100; ++i) {
        usleep(100000);

        if (access(sock_path, F_OK) == 0 && ping_socket_path(sock_path)) {
            return true;
        }

        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            return false;
        }
    }
    return false;
}

pid_t SessionManager::spawn_server(int session_id, const std::string& fsdb_file) {
    // Get path to current executable
    char self_path[1024] = {};
    ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
    if (len < 0) {
        return -1;
    }

    // Build server argv: [exe, "--server", session_id, fsdb_file]
    std::vector<char*> argv;
    argv.push_back(self_path);
    argv.push_back((char*)"--server");

    char session_id_str[16];
    snprintf(session_id_str, sizeof(session_id_str), "%d", session_id);
    argv.push_back(session_id_str);

    char* fsdb_file_str = const_cast<char*>(fsdb_file.c_str());
    argv.push_back(fsdb_file_str);
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        // Detach the session server from the caller so it survives short-lived
        // CLI invocations such as `xwave open ...`.
        if (setsid() < 0) {
            _exit(1);
        }
        signal(SIGHUP, SIG_IGN);

        // Child process - exec server
        execv(self_path, argv.data());
        perror("execv");
        _exit(1);
    }

    return pid;
}

int SessionManager::create_session(const std::string& fsdb_file) {
    std::string canonical = canonicalize_fsdb_path(fsdb_file);
    SessionInfo metadata;
    if (!populate_fsdb_metadata(canonical, metadata)) {
        return 0;
    }

    int lock_fd = open_registry_lock();
    if (lock_fd < 0) {
        return 0;
    }

    if (flock(lock_fd, LOCK_EX) != 0) {
        close(lock_fd);
        return 0;
    }

    // Clean up stale sessions first
    cleanup();

    std::vector<SessionInfo> existing;
    registry_->load_all(existing);
    for (const auto& session : existing) {
        if (session.fsdb_file == canonical && diagnose_session(session.session_id).healthy) {
            registry_->touch(session.session_id, time(nullptr));
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            return session.session_id;
        }
    }

    // Get next session ID
    int session_id = registry_->get_next_id();

    // Spawn server process
    pid_t pid = spawn_server(session_id, canonical);
    if (pid < 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    // Get socket path
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);

    if (!wait_for_server(session_id, pid)) {
        // Kill the server process if it didn't start properly
        kill(pid, SIGTERM);
        unlink(sock_path);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    // Create session info
    SessionInfo session;
    session.session_id = session_id;
    session.socket_path = sock_path;
    session.server_pid = pid;
    session.fsdb_file = canonical;
    session.created_at = time(nullptr);
    session.last_active = session.created_at;
    populate_fsdb_metadata(canonical, session);

    // Add to registry
    if (!registry_->add(session)) {
        kill(pid, SIGTERM);
        unlink(sock_path);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    SessionHealth health = diagnose_session(session_id);
    if (!health.healthy) {
        kill(pid, SIGTERM);
        registry_->remove(session_id);
        unlink(sock_path);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return session_id;
}

bool SessionManager::stop_process(const SessionInfo& session, bool remove_record, bool remove_events) {
    int fd = connect_socket_path(session.socket_path.c_str());
    if (fd >= 0) {
        const char* quit_msg = CMD_QUIT "\n";
        write(fd, quit_msg, strlen(quit_msg));

        char buf[64];
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 500000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (read(fd, buf, sizeof(buf)) > 0) {
        }
        close(fd);
    }

    int status;
    for (int i = 0; i < 10; ++i) {
        if (waitpid(session.server_pid, &status, WNOHANG) > 0) break;
        if (kill(session.server_pid, 0) != 0) break;
        usleep(100000);
    }
    if (kill(session.server_pid, 0) == 0) {
        kill(session.server_pid, SIGTERM);
        usleep(300000);
    }
    if (kill(session.server_pid, 0) == 0) {
        kill(session.server_pid, SIGKILL);
        usleep(100000);
    }
    waitpid(session.server_pid, &status, WNOHANG);

    unlink(session.socket_path.c_str());
    if (remove_record) registry_->remove(session.session_id);
    if (remove_events) {
        EventManager event_manager;
        event_manager.delete_session_events(session.session_id);
    }
    return true;
}

bool SessionManager::restart_session(int session_id) {
    int lock_fd = open_registry_lock();
    if (lock_fd < 0) return false;
    if (flock(lock_fd, LOCK_EX) != 0) {
        close(lock_fd);
        return false;
    }

    SessionInfo old_session;
    if (!registry_->get(session_id, old_session)) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return false;
    }

    SessionInfo metadata;
    if (!current_fsdb_metadata(old_session, metadata)) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return false;
    }

    stop_process(old_session, false, false);

    pid_t pid = spawn_server(session_id, old_session.fsdb_file);
    if (pid < 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return false;
    }
    if (!wait_for_server(session_id, pid)) {
        kill(pid, SIGTERM);
        char sock_path[SOCK_PATH_LEN];
        get_sock_path(sock_path, session_id);
        unlink(sock_path);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return false;
    }

    SessionInfo session = old_session;
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);
    session.socket_path = sock_path;
    session.server_pid = pid;
    session.last_active = time(nullptr);
    populate_fsdb_metadata(session.fsdb_file, session);

    bool ok = registry_->upsert(session);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return ok;
}

bool SessionManager::ensure_session_current(int session_id) {
    SessionInfo session;
    if (!registry_->get(session_id, session)) return false;

    SessionInfo current;
    if (!current_fsdb_metadata(session, current)) return false;
    if (!fsdb_metadata_matches(session, current)) {
        fprintf(stderr, "FSDB changed, restarting session %d...\n", session_id);
        return restart_session(session_id);
    }
    return diagnose_session(session_id).healthy;
}

bool SessionManager::touch_session(int session_id) {
    return registry_->touch(session_id, time(nullptr));
}

bool SessionManager::kill_session(int session_id) {
    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        return false;
    }

    SessionHealth health = diagnose_session(session_id);
    if (!health.healthy) {
        if (kill(session.server_pid, 0) == 0) {
            kill(session.server_pid, SIGTERM);
            usleep(300000);
            if (kill(session.server_pid, 0) == 0) kill(session.server_pid, SIGKILL);
        }
        registry_->remove(session_id);
        EventManager event_manager;
        event_manager.delete_session_events(session_id);
        unlink(session.socket_path.c_str());
        return true;
    }

    return stop_process(session, true, true);
}

bool SessionManager::kill_all_sessions() {
    std::vector<SessionInfo> sessions = list_sessions();
    EventManager event_manager;
    for (const auto& session : sessions) {
        kill_session(session.session_id);
    }
    registry_->clear_all();
    for (const auto& session : sessions) {
        event_manager.delete_session_events(session.session_id);
    }
    return true;
}

bool SessionManager::get_session(int session_id, SessionInfo& info) {
    return registry_->get(session_id, info);
}

bool SessionManager::get_latest_session(SessionInfo& info) {
    return registry_->get_latest(info);
}

std::vector<SessionInfo> SessionManager::list_sessions() {
    cleanup();
    std::vector<SessionInfo> sessions;
    registry_->load_all(sessions);
    return sessions;
}

bool SessionManager::gc_sessions() {
    cleanup();
    const char* env_timeout = getenv("XWAVE_IDLE_TIMEOUT_SEC");
    int timeout = env_timeout ? atoi(env_timeout) : 1800;
    registry_->cleanup_idle(time(nullptr), timeout);
    cleanup();
    return true;
}

SessionHealth SessionManager::diagnose_session(int session_id) {
    SessionHealth health;
    health.session_id = session_id;

    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        health.status = SessionHealthStatus::RegistryMissing;
        health.message = "Session is not present in the registry";
        return health;
    }

    health.info = session;

    SessionInfo current;
    if (!current_fsdb_metadata(session, current)) {
        health.status = SessionHealthStatus::FsdbMissing;
        health.message = "FSDB file is missing or cannot be stat'ed";
        return health;
    }
    if (!fsdb_metadata_matches(session, current)) {
        health.status = SessionHealthStatus::FsdbChanged;
        health.message = "FSDB file changed since session was opened";
        return health;
    }

    if (kill(session.server_pid, 0) != 0) {
        health.status = SessionHealthStatus::ProcessExited;
        health.message = "Server process is not running";
        return health;
    }

    if (access(session.socket_path.c_str(), F_OK) != 0) {
        health.status = SessionHealthStatus::SocketMissing;
        health.message = "Server socket file is missing";
        return health;
    }

    int fd = connect_socket_path(session.socket_path.c_str());
    if (fd < 0) {
        health.status = SessionHealthStatus::ConnectFailed;
        health.message = "Server socket exists but cannot be connected";
        return health;
    }
    close(fd);

    if (!ping_socket_path(session.socket_path.c_str())) {
        health.status = SessionHealthStatus::PingFailed;
        health.message = "Server did not respond to PING";
        return health;
    }

    health.healthy = true;
    health.status = SessionHealthStatus::Healthy;
    health.message = "Session is healthy";
    return health;
}

bool SessionManager::is_session_alive(int session_id) {
    return diagnose_session(session_id).healthy;
}

std::string SessionManager::get_socket_path(int session_id) {
    char path[SOCK_PATH_LEN];
    get_sock_path(path, session_id);
    return std::string(path);
}

void SessionManager::cleanup() {
    std::vector<SessionInfo> before;
    registry_->load_all(before);
    registry_->cleanup_stale();
    std::vector<SessionInfo> after;
    registry_->load_all(after);
    std::set<int> live_ids;
    for (const auto& session : after) live_ids.insert(session.session_id);
    EventManager event_manager;
    for (const auto& session : before) {
        if (live_ids.find(session.session_id) == live_ids.end()) {
            event_manager.delete_session_events(session.session_id);
        }
    }
}

} // namespace xtrace
