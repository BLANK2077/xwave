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
#include <cerrno>
#include <cstdarg>
#include <strings.h>

namespace xwave {

namespace {

int open_registry_lock() {
    char lock_path[SOCK_PATH_LEN];
    get_registry_path(lock_path);
    strncat(lock_path, ".lock", sizeof(lock_path) - strlen(lock_path) - 1);
    return open(lock_path, O_RDWR | O_CREAT, 0600);
}

bool env_debug_enabled() {
    const char* env = getenv("XWAVE_DEBUG");
    return env && env[0] != '\0' && strcmp(env, "0") != 0 &&
           strcasecmp(env, "false") != 0 && strcasecmp(env, "off") != 0;
}

int session_start_timeout_sec() {
    const char* env = getenv("XWAVE_SESSION_START_TIMEOUT_SEC");
    if (!env || env[0] == '\0') return 60;
    int value = atoi(env);
    return value > 0 ? value : 60;
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
    debug_.enabled = env_debug_enabled();
}

SessionManager::~SessionManager() {
}

void SessionManager::set_debug_enabled(bool enabled) {
    debug_.enabled = enabled;
}

bool SessionManager::debug_enabled() const {
    return debug_.enabled;
}

void SessionManager::debug_log(const char* fmt, ...) const {
    if (!debug_.enabled) return;
    fprintf(stderr, "[xwave-debug] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
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

WaitForServerResult SessionManager::wait_for_server(int session_id, pid_t pid) {
    WaitForServerResult result;
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);

    int timeout_sec = session_start_timeout_sec();
    int iterations = timeout_sec * 10;
    if (iterations <= 0) iterations = 600;
    result.timeout_sec = timeout_sec;

    debug_log("wait_for_server: session=%d pid=%d socket=%s timeout_sec=%d",
              session_id, pid, sock_path, timeout_sec);

    for (int i = 0; i < iterations; ++i) {
        usleep(100000);
        result.elapsed_ms = (i + 1) * 100L;

        result.socket_exists = access(sock_path, F_OK) == 0;
        if (result.socket_exists) {
            int fd = connect_socket_path(sock_path);
            if (fd >= 0) {
                result.connect_ok = true;
                close(fd);
                result.ping_ok = ping_socket_path(sock_path);
                debug_log("wait_for_server: socket_exists=1 connect_ok=1 ping_ok=%d elapsed_ms=%ld",
                          result.ping_ok ? 1 : 0, result.elapsed_ms);
                if (result.ping_ok) {
                    result.ok = true;
                    result.reason = "ready";
                    return result;
                }
            } else {
                debug_log("wait_for_server: socket_exists=1 connect_ok=0 elapsed_ms=%ld errno=%d(%s)",
                          result.elapsed_ms, errno, strerror(errno));
            }
        }

        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            result.child_exited = true;
            result.child_status = status;
            result.reason = "child_exited";
            debug_log("wait_for_server: child_exited status=%d elapsed_ms=%ld socket_exists=%d connect_ok=%d ping_ok=%d",
                      status, result.elapsed_ms, result.socket_exists ? 1 : 0,
                      result.connect_ok ? 1 : 0, result.ping_ok ? 1 : 0);
            return result;
        }
    }

    result.reason = result.socket_exists
        ? (result.connect_ok ? "ping_failed" : "socket_connect_failed")
        : "timeout_waiting_socket";
    debug_log("wait_for_server: timeout reason=%s elapsed_ms=%ld child_alive=%d socket_exists=%d connect_ok=%d ping_ok=%d",
              result.reason.c_str(), result.elapsed_ms,
              kill(pid, 0) == 0 ? 1 : 0,
              result.socket_exists ? 1 : 0,
              result.connect_ok ? 1 : 0,
              result.ping_ok ? 1 : 0);
    if (kill(pid, 0) == 0 && result.reason == "timeout_waiting_socket") {
        char log_path[SOCK_PATH_LEN];
        get_debug_log_path(log_path, session_id);
        debug_log("wait_for_server: server is still alive; it may still be opening FSDB. Increase XWAVE_SESSION_START_TIMEOUT_SEC or inspect %s", log_path);
    }
    return result;
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
    debug_log("create_session: input_fsdb=%s canonical_fsdb=%s", fsdb_file.c_str(), canonical.c_str());
    SessionInfo metadata;
    if (!populate_fsdb_metadata(canonical, metadata)) {
        debug_log("create_session: reason=fsdb_stat_failed path=%s errno=%d(%s)",
                  canonical.c_str(), errno, strerror(errno));
        return 0;
    }
    debug_log("create_session: fsdb_stat mtime=%ld size=%lld dev=%llu inode=%llu",
              metadata.fsdb_mtime, metadata.fsdb_size,
              metadata.fsdb_dev, metadata.fsdb_inode);

    int lock_fd = open_registry_lock();
    if (lock_fd < 0) {
        char lock_path[SOCK_PATH_LEN];
        get_registry_path(lock_path);
        strncat(lock_path, ".lock", sizeof(lock_path) - strlen(lock_path) - 1);
        debug_log("create_session: reason=registry_lock_open_failed lock=%s errno=%d(%s)",
                  lock_path, errno, strerror(errno));
        return 0;
    }

    if (flock(lock_fd, LOCK_EX) != 0) {
        debug_log("create_session: reason=registry_lock_failed errno=%d(%s)",
                  errno, strerror(errno));
        close(lock_fd);
        return 0;
    }
    debug_log("create_session: registry_lock_acquired fd=%d", lock_fd);

    // Clean up stale sessions first
    debug_log("create_session: cleanup_stale_begin");
    cleanup();
    debug_log("create_session: cleanup_stale_done");

    std::vector<SessionInfo> existing;
    registry_->load_all(existing);
    debug_log("create_session: existing_sessions=%zu", existing.size());
    std::vector<SessionInfo> unhealthy_same_fsdb;
    for (const auto& session : existing) {
        if (session.fsdb_file != canonical) continue;

        SessionHealth health = diagnose_session(session.session_id);
        if (health.healthy) {
            debug_log("create_session: reuse_existing_session session=%d pid=%d socket=%s",
                      session.session_id, session.server_pid, session.socket_path.c_str());
            registry_->touch(session.session_id, time(nullptr));
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            return session.session_id;
        }

        debug_log("create_session: found_unhealthy_same_fsdb session=%d status=%s message=%s",
                  session.session_id,
                  session_health_status_name(health.status),
                  health.message.c_str());
        unhealthy_same_fsdb.push_back(session);
    }

    if (!unhealthy_same_fsdb.empty()) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        for (const auto& session : unhealthy_same_fsdb) {
            debug_log("create_session: removing_unhealthy_same_fsdb session=%d pid=%d socket=%s",
                      session.session_id,
                      session.server_pid,
                      session.socket_path.c_str());
            stop_process(session, true, false);
        }
        return create_session(canonical);
    }

    // Get next session ID
    int session_id = registry_->get_next_id();
    debug_log("create_session: next_session_id=%d", session_id);

    // Spawn server process
    pid_t pid = spawn_server(session_id, canonical);
    if (pid < 0) {
        debug_log("create_session: reason=spawn_failed session=%d errno=%d(%s)",
                  session_id, errno, strerror(errno));
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }
    debug_log("create_session: spawned_server session=%d pid=%d", session_id, pid);

    // Get socket path
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);
    char debug_log_path[SOCK_PATH_LEN];
    get_debug_log_path(debug_log_path, session_id);
    debug_log("create_session: socket_path=%s debug_log=%s", sock_path, debug_log_path);

    WaitForServerResult wait = wait_for_server(session_id, pid);
    if (!wait.ok) {
        // Kill the server process if it didn't start properly
        debug_log("create_session: reason=%s elapsed_ms=%ld child_exited=%d child_status=%d socket_exists=%d connect_ok=%d ping_ok=%d",
                  wait.reason.c_str(), wait.elapsed_ms, wait.child_exited ? 1 : 0,
                  wait.child_status, wait.socket_exists ? 1 : 0,
                  wait.connect_ok ? 1 : 0, wait.ping_ok ? 1 : 0);
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
        debug_log("create_session: reason=registry_add_failed session=%d registry_write_failed",
                  session_id);
        kill(pid, SIGTERM);
        unlink(sock_path);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    SessionHealth health = diagnose_session(session_id);
    if (!health.healthy) {
        debug_log("create_session: reason=post_create_health_failed session=%d status=%s message=%s",
                  session_id, session_health_status_name(health.status), health.message.c_str());
        kill(pid, SIGTERM);
        registry_->remove(session_id);
        unlink(sock_path);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    debug_log("create_session: success session=%d pid=%d socket=%s", session_id, pid, sock_path);
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
    debug_log("restart_session: begin session=%d", session_id);
    int lock_fd = open_registry_lock();
    if (lock_fd < 0) {
        debug_log("restart_session: reason=registry_lock_open_failed errno=%d(%s)",
                  errno, strerror(errno));
        return false;
    }
    if (flock(lock_fd, LOCK_EX) != 0) {
        debug_log("restart_session: reason=registry_lock_failed errno=%d(%s)",
                  errno, strerror(errno));
        close(lock_fd);
        return false;
    }

    SessionInfo old_session;
    if (!registry_->get(session_id, old_session)) {
        debug_log("restart_session: reason=registry_missing session=%d", session_id);
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return false;
    }

    SessionInfo metadata;
    if (!current_fsdb_metadata(old_session, metadata)) {
        debug_log("restart_session: reason=fsdb_stat_failed path=%s errno=%d(%s)",
                  old_session.fsdb_file.c_str(), errno, strerror(errno));
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return false;
    }
    debug_log("restart_session: fsdb_stat mtime=%ld size=%lld dev=%llu inode=%llu",
              metadata.fsdb_mtime, metadata.fsdb_size,
              metadata.fsdb_dev, metadata.fsdb_inode);

    debug_log("restart_session: stopping_old_process pid=%d socket=%s",
              old_session.server_pid, old_session.socket_path.c_str());
    stop_process(old_session, false, false);

    pid_t pid = spawn_server(session_id, old_session.fsdb_file);
    if (pid < 0) {
        debug_log("restart_session: reason=spawn_failed errno=%d(%s)",
                  errno, strerror(errno));
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return false;
    }
    debug_log("restart_session: spawned_server pid=%d", pid);
    WaitForServerResult wait = wait_for_server(session_id, pid);
    if (!wait.ok) {
        debug_log("restart_session: reason=%s elapsed_ms=%ld child_exited=%d child_status=%d socket_exists=%d connect_ok=%d ping_ok=%d",
                  wait.reason.c_str(), wait.elapsed_ms, wait.child_exited ? 1 : 0,
                  wait.child_status, wait.socket_exists ? 1 : 0,
                  wait.connect_ok ? 1 : 0, wait.ping_ok ? 1 : 0);
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
    debug_log("restart_session: registry_upsert=%d session=%d pid=%d",
              ok ? 1 : 0, session_id, pid);
    flock(lock_fd, LOCK_UN);
    close(lock_fd);
    return ok;
}

bool SessionManager::ensure_session_current(int session_id) {
    debug_log("ensure_session_current: begin session=%d", session_id);
    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        debug_log("ensure_session_current: reason=registry_missing session=%d", session_id);
        return false;
    }

    SessionInfo current;
    if (!current_fsdb_metadata(session, current)) {
        debug_log("ensure_session_current: reason=fsdb_stat_failed path=%s errno=%d(%s)",
                  session.fsdb_file.c_str(), errno, strerror(errno));
        return false;
    }
    if (!fsdb_metadata_matches(session, current)) {
        fprintf(stderr, "FSDB changed, restarting session %d...\n", session_id);
        debug_log("ensure_session_current: fsdb_changed session=%d old_mtime=%ld new_mtime=%ld old_size=%lld new_size=%lld",
                  session_id, session.fsdb_mtime, current.fsdb_mtime,
                  session.fsdb_size, current.fsdb_size);
        return restart_session(session_id);
    }
    SessionHealth health = diagnose_session(session_id);
    debug_log("ensure_session_current: diagnose status=%s healthy=%d message=%s",
              session_health_status_name(health.status), health.healthy ? 1 : 0,
              health.message.c_str());
    return health.healthy;
}

bool SessionManager::touch_session(int session_id) {
    return registry_->touch(session_id, time(nullptr));
}

bool SessionManager::kill_session(int session_id) {
    debug_log("kill_session: begin session=%d", session_id);
    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        debug_log("kill_session: reason=registry_missing session=%d", session_id);
        return false;
    }

    SessionHealth health = diagnose_session(session_id);
    debug_log("kill_session: health status=%s healthy=%d message=%s",
              session_health_status_name(health.status), health.healthy ? 1 : 0,
              health.message.c_str());
    if (!health.healthy) {
        if (kill(session.server_pid, 0) == 0) {
            debug_log("kill_session: stale_process_alive pid=%d sending SIGTERM", session.server_pid);
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
    debug_log("kill_all_sessions: count=%zu", sessions.size());
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
    debug_log("gc_sessions: begin");
    cleanup();
    const char* env_timeout = getenv("XWAVE_IDLE_TIMEOUT_SEC");
    int timeout = env_timeout ? atoi(env_timeout) : 1800;
    if (timeout <= 0) timeout = 1800;
    debug_log("gc_sessions: idle_timeout_sec=%d", timeout);
    std::vector<SessionInfo> sessions;
    registry_->load_all(sessions);
    time_t now = time(nullptr);
    for (const auto& session : sessions) {
        time_t last = session.last_active ? session.last_active : session.created_at;
        if (last > 0 && now - last > timeout) {
            debug_log("gc_sessions: removing_idle session=%d pid=%d idle_sec=%ld timeout_sec=%d",
                      session.session_id,
                      session.server_pid,
                      static_cast<long>(now - last),
                      timeout);
            stop_process(session, true, true);
        }
    }
    cleanup();
    debug_log("gc_sessions: done");
    return true;
}

SessionHealth SessionManager::diagnose_session(int session_id) {
    SessionHealth health;
    health.session_id = session_id;
    debug_log("diagnose_session: begin session=%d", session_id);

    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        health.status = SessionHealthStatus::RegistryMissing;
        health.message = "Session is not present in the registry";
        debug_log("diagnose_session: status=%s message=%s",
                  session_health_status_name(health.status),
                  health.message.c_str());
        return health;
    }

    health.info = session;
    debug_log("diagnose_session: registry pid=%d socket=%s fsdb=%s",
              session.server_pid,
              session.socket_path.c_str(),
              session.fsdb_file.c_str());

    SessionInfo current;
    if (!current_fsdb_metadata(session, current)) {
        health.status = SessionHealthStatus::FsdbMissing;
        health.message = "FSDB file is missing or cannot be stat'ed";
        debug_log("diagnose_session: status=%s message=%s",
                  session_health_status_name(health.status),
                  health.message.c_str());
        return health;
    }
    debug_log("diagnose_session: fsdb_stat old_mtime=%ld new_mtime=%ld old_size=%lld new_size=%lld old_dev=%llu new_dev=%llu old_inode=%llu new_inode=%llu",
              (long)session.fsdb_mtime,
              (long)current.fsdb_mtime,
              (long long)session.fsdb_size,
              (long long)current.fsdb_size,
              (unsigned long long)session.fsdb_dev,
              (unsigned long long)current.fsdb_dev,
              (unsigned long long)session.fsdb_inode,
              (unsigned long long)current.fsdb_inode);
    if (!fsdb_metadata_matches(session, current)) {
        health.status = SessionHealthStatus::FsdbChanged;
        health.message = "FSDB file changed since session was opened";
        debug_log("diagnose_session: status=%s message=%s",
                  session_health_status_name(health.status),
                  health.message.c_str());
        return health;
    }

    if (kill(session.server_pid, 0) != 0) {
        health.status = SessionHealthStatus::ProcessExited;
        health.message = "Server process is not running";
        debug_log("diagnose_session: status=%s pid=%d errno=%d(%s)",
                  session_health_status_name(health.status),
                  session.server_pid,
                  errno,
                  strerror(errno));
        return health;
    }
    debug_log("diagnose_session: process_alive pid=%d", session.server_pid);

    if (access(session.socket_path.c_str(), F_OK) != 0) {
        health.status = SessionHealthStatus::SocketMissing;
        health.message = "Server socket file is missing";
        debug_log("diagnose_session: status=%s socket=%s errno=%d(%s)",
                  session_health_status_name(health.status),
                  session.socket_path.c_str(),
                  errno,
                  strerror(errno));
        return health;
    }
    debug_log("diagnose_session: socket_exists path=%s", session.socket_path.c_str());

    int fd = connect_socket_path(session.socket_path.c_str());
    if (fd < 0) {
        health.status = SessionHealthStatus::ConnectFailed;
        health.message = "Server socket exists but cannot be connected";
        debug_log("diagnose_session: status=%s socket=%s",
                  session_health_status_name(health.status),
                  session.socket_path.c_str());
        return health;
    }
    close(fd);
    debug_log("diagnose_session: connect_ok socket=%s", session.socket_path.c_str());

    if (!ping_socket_path(session.socket_path.c_str())) {
        health.status = SessionHealthStatus::PingFailed;
        health.message = "Server did not respond to PING";
        debug_log("diagnose_session: status=%s socket=%s",
                  session_health_status_name(health.status),
                  session.socket_path.c_str());
        return health;
    }
    debug_log("diagnose_session: ping_ok socket=%s", session.socket_path.c_str());

    health.healthy = true;
    health.status = SessionHealthStatus::Healthy;
    health.message = "Session is healthy";
    debug_log("diagnose_session: status=%s message=%s",
              session_health_status_name(health.status),
              health.message.c_str());
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
    debug_log("cleanup: before_count=%zu", before.size());
    registry_->cleanup_stale();
    std::vector<SessionInfo> after;
    registry_->load_all(after);
    debug_log("cleanup: after_count=%zu", after.size());
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
