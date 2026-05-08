#include "session_manager.h"
#include "../protocol/protocol.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/file.h>

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
    }
    return "unknown";
}

SessionManager::SessionManager() : registry_(new SessionRegistry()) {
}

SessionManager::~SessionManager() {
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

    // Get next session ID
    int session_id = registry_->get_next_id();

    // Spawn server process
    pid_t pid = spawn_server(session_id, fsdb_file);
    if (pid < 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        return 0;
    }

    // Get socket path
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);

    // Wait for server to create a responsive socket (poll for up to 10 seconds)
    bool server_ready = false;
    for (int i = 0; i < 100; ++i) {
        usleep(100000);  // 100ms

        if (access(sock_path, F_OK) == 0) {
            server_ready = ping_socket_path(sock_path);
            if (server_ready) break;
        }

        // Check if child process exited with error
        int status;
        if (waitpid(pid, &status, WNOHANG) > 0) {
            flock(lock_fd, LOCK_UN);
            close(lock_fd);
            return 0;
        }
    }

    if (!server_ready) {
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
    session.fsdb_file = fsdb_file;
    session.created_at = time(nullptr);

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

bool SessionManager::kill_session(int session_id) {
    SessionInfo session;
    if (!registry_->get(session_id, session)) {
        return false;
    }

    SessionHealth health = diagnose_session(session_id);
    if (!health.healthy) {
        if (kill(session.server_pid, 0) == 0) {
            kill(session.server_pid, SIGTERM);
        }
        registry_->remove(session_id);
        unlink(session.socket_path.c_str());
        return true;
    }

    // Connect to server and send QUIT command
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, session.socket_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        // Server might be dead, clean up anyway
        registry_->remove(session_id);
        unlink(session.socket_path.c_str());
        return false;
    }

    // Send QUIT command
    const char* quit_msg = CMD_QUIT "\n";
    write(fd, quit_msg, strlen(quit_msg));

    // Wait for response or timeout (short, non-blocking to user)
    char buf[64];
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 500000;  // 500ms
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (read(fd, buf, sizeof(buf)) > 0) {
        // Drain response
    }

    close(fd);

    // Give server a brief moment to exit gracefully
    int status;
    usleep(300000);  // 300ms
    waitpid(session.server_pid, &status, WNOHANG);

    // Force kill if still alive
    if (kill(session.server_pid, 0) == 0) {
        kill(session.server_pid, SIGTERM);
    }

    // Remove from registry
    registry_->remove(session_id);

    // Remove socket file
    unlink(session.socket_path.c_str());

    return true;
}

bool SessionManager::kill_all_sessions() {
    std::vector<SessionInfo> sessions = list_sessions();
    for (const auto& session : sessions) {
        kill_session(session.session_id);
    }
    registry_->clear_all();
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
    registry_->cleanup_stale();
}

} // namespace xtrace
