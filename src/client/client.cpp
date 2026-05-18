#include "client.h"
#include "../protocol/protocol.h"
#include "../session/session_manager.h"

#include <cstdio>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>

namespace xwave {

int session_connect(const std::string& session_id) {
    char sock_path[SOCK_PATH_LEN];
    get_sock_path(sock_path, session_id);

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

bool send_command_and_print(const std::string& session_id, const char* cmd) {
    SessionManager manager;
    if (!manager.ensure_session_current(session_id)) {
        SessionHealth health = manager.diagnose_session(session_id);
        fprintf(stderr, "Error: Session %s unavailable: %s (status=%s)\n",
                session_id.c_str(),
                health.message.c_str(),
                session_health_status_name(health.status));
        return false;
    }

    int fd = session_connect(session_id);
    if (fd < 0) {
        SessionHealth health = manager.diagnose_session(session_id);
        fprintf(stderr, "Error: Session %s unavailable: %s (status=%s)\n",
                session_id.c_str(),
                health.message.c_str(),
                session_health_status_name(health.status));
        return false;
    }

    // Send command
    std::string msg = std::string(cmd) + "\n";
    if (write(fd, msg.c_str(), msg.length()) < 0) {
        close(fd);
        return false;
    }

    // Read response until END_MARKER
    std::string buf;
    const std::string end_marker(END_MARKER);
    char tmp[4096];

    bool server_error = false;
    while (true) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);

        size_t pos = buf.find(end_marker);
        if (pos != std::string::npos) {
            std::string payload = buf.substr(0, pos);
            server_error = payload.compare(0, strlen(ERROR_PREFIX), ERROR_PREFIX) == 0;
            FILE* stream = server_error ? stderr : stdout;
            fwrite(payload.c_str(), 1, payload.size(), stream);
            break;
        }

        // Flush safe prefix if buffer grows large
        if (buf.size() > sizeof(tmp) + end_marker.size()) {
            size_t safe = buf.size() - end_marker.size();
            fwrite(buf.c_str(), 1, safe, stdout);
            buf.erase(0, safe);
        }
    }
    fflush(stdout);
    fflush(stderr);
    close(fd);
    return !server_error;
}

bool send_command_capture(const std::string& session_id, const char* cmd, std::string& payload) {
    payload.clear();
    SessionManager manager;
    if (!manager.ensure_session_current(session_id)) return false;

    int fd = session_connect(session_id);
    if (fd < 0) return false;

    std::string msg = std::string(cmd) + "\n";
    if (write(fd, msg.c_str(), msg.length()) < 0) {
        close(fd);
        return false;
    }

    std::string buf;
    const std::string end_marker(END_MARKER);
    char tmp[4096];
    bool server_error = false;
    while (true) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t pos = buf.find(end_marker);
        if (pos != std::string::npos) {
            payload = buf.substr(0, pos);
            server_error = payload.compare(0, strlen(ERROR_PREFIX), ERROR_PREFIX) == 0;
            break;
        }
    }
    close(fd);
    return !server_error;
}

bool session_ping(const std::string& session_id) {
    int fd = session_connect(session_id);
    if (fd < 0) return false;

    const char* ping = CMD_PING "\n";
    if (write(fd, ping, strlen(ping)) < 0) {
        close(fd);
        return false;
    }

    // Wait for PONG with timeout
    char buf[64];
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    bool got_pong = false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        if (strstr(buf, "PONG")) {
            got_pong = true;
        }
    }

    close(fd);
    return got_pong;
}

} // namespace xtrace
