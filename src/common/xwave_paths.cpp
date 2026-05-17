#include "xwave_paths.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xwave {

namespace {

std::string home_dir() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    return std::string(home);
}

bool ensure_dir(const std::string& path) {
    if (mkdir(path.c_str(), 0700) == 0) return true;
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool remove_file_if_exists(const std::string& path) {
    if (unlink(path.c_str()) == 0) return true;
    return access(path.c_str(), F_OK) != 0;
}

} // namespace

std::string xwave_home_dir() {
    return home_dir() + "/.xwave";
}

std::string xwave_sessions_dir() {
    return xwave_home_dir() + "/sessions";
}

std::string xwave_session_dir(int session_id) {
    return xwave_sessions_dir() + "/" + std::to_string(session_id);
}

std::string xwave_registry_path() {
    return xwave_home_dir() + "/registry.json";
}

std::string xwave_registry_lock_path() {
    return xwave_home_dir() + "/registry.lock";
}

std::string xwave_session_json_path(int session_id) {
    return xwave_session_dir(session_id) + "/session.json";
}

std::string xwave_socket_path(int session_id) {
    return xwave_session_dir(session_id) + "/socket";
}

std::string xwave_debug_log_path(int session_id) {
    return xwave_session_dir(session_id) + "/debug.log";
}

std::string xwave_lists_path(int session_id) {
    return xwave_session_dir(session_id) + "/lists.json";
}

std::string xwave_apb_path(int session_id) {
    return xwave_session_dir(session_id) + "/apb.json";
}

std::string xwave_axi_path(int session_id) {
    return xwave_session_dir(session_id) + "/axi.json";
}

std::string xwave_events_path(int session_id) {
    return xwave_session_dir(session_id) + "/events.json";
}

std::string xwave_cursors_path(int session_id) {
    return xwave_session_dir(session_id) + "/cursors.json";
}

std::string xwave_legacy_registry_path() {
    return home_dir() + "/.xwave.registry";
}

std::string xwave_legacy_lists_path() {
    return home_dir() + "/.xwave.lists";
}

std::string xwave_legacy_apb_path() {
    return home_dir() + "/.xwave.apb";
}

std::string xwave_legacy_axi_path() {
    return home_dir() + "/.xwave.axi";
}

std::string xwave_legacy_events_path() {
    return home_dir() + "/.xwave.events";
}

bool xwave_ensure_home() {
    return ensure_dir(xwave_home_dir()) && ensure_dir(xwave_sessions_dir());
}

bool xwave_ensure_session_dir(int session_id) {
    return xwave_ensure_home() && ensure_dir(xwave_session_dir(session_id));
}

bool xwave_remove_session_dir(int session_id) {
    std::string dir = xwave_session_dir(session_id);
    remove_file_if_exists(dir + "/session.json");
    remove_file_if_exists(dir + "/socket");
    remove_file_if_exists(dir + "/debug.log");
    remove_file_if_exists(dir + "/lists.json");
    remove_file_if_exists(dir + "/apb.json");
    remove_file_if_exists(dir + "/axi.json");
    remove_file_if_exists(dir + "/events.json");
    remove_file_if_exists(dir + "/cursors.json");
    if (rmdir(dir.c_str()) == 0) return true;
    return access(dir.c_str(), F_OK) != 0;
}

bool xwave_legacy_registry_has_session(int session_id) {
    FILE* fp = fopen(xwave_legacy_registry_path().c_str(), "r");
    if (!fp) return false;

    char line[4096];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char* end = nullptr;
        long sid = strtol(line, &end, 10);
        if (end != line && sid == session_id && *end == '|') {
            found = true;
            break;
        }
    }

    fclose(fp);
    return found;
}

} // namespace xwave
