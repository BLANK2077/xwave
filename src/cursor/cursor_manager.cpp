#include "cursor_manager.h"

#include "../common/xwave_paths.h"
#include "../json.hpp"

#include <ctime>
#include <cctype>
#include <fcntl.h>
#include <unistd.h>

namespace xwave {

using Json = nlohmann::ordered_json;

namespace {

Json cursor_to_json(const Cursor& c) {
    Json j;
    j["name"] = c.name;
    j["time"] = c.time;
    j["time_text"] = c.time_text;
    j["note"] = c.note;
    j["origin"] = c.origin.empty() ? "manual" : c.origin;
    j["clock"] = c.clock;
    j["created_at"] = c.created_at;
    j["updated_at"] = c.updated_at;
    return j;
}

bool json_to_cursor(const Json& j, Cursor& c) {
    if (!j.is_object()) return false;
    c.name = j.value("name", "");
    c.time = j.value("time", static_cast<uint64_t>(0));
    c.time_text = j.value("time_text", "");
    c.note = j.value("note", "");
    c.origin = j.value("origin", "manual");
    c.clock = j.value("clock", "");
    c.created_at = j.value("created_at", 0L);
    c.updated_at = j.value("updated_at", 0L);
    return !c.name.empty();
}

bool load_store(int session_id, Json& root) {
    root = Json::object();
    root["version"] = 1;
    root["active_cursor"] = "";
    root["cursors"] = Json::array();

    std::string path = xwave_cursors_path(session_id);
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return true;

    std::string text;
    char buf[4096];
    ssize_t n = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) text.append(buf, n);
    close(fd);
    if (text.empty()) return true;

    try {
        Json parsed = Json::parse(text);
        if (parsed.is_object()) root = parsed;
        if (!root.contains("cursors") || !root["cursors"].is_array()) root["cursors"] = Json::array();
        if (!root.contains("active_cursor")) root["active_cursor"] = "";
    } catch (...) {
        return false;
    }
    return true;
}

bool save_store(int session_id, const Json& root) {
    if (!xwave_ensure_session_dir(session_id)) return false;
    std::string path = xwave_cursors_path(session_id);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());
    close(fd);
    return ok;
}

bool valid_cursor_name(const std::string& name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

} // namespace

bool CursorManager::set_cursor(int session_id, const Cursor& cursor, bool make_active) {
    if (!valid_cursor_name(cursor.name)) return false;
    Json root;
    if (!load_store(session_id, root)) return false;
    long now = static_cast<long>(time(nullptr));

    Cursor c = cursor;
    if (c.created_at == 0) c.created_at = now;
    c.updated_at = now;
    if (c.origin.empty()) c.origin = "manual";

    bool replaced = false;
    for (auto& item : root["cursors"]) {
        Cursor old;
        if (json_to_cursor(item, old) && old.name == c.name) {
            if (c.created_at == now && old.created_at > 0) c.created_at = old.created_at;
            item = cursor_to_json(c);
            replaced = true;
            break;
        }
    }
    if (!replaced) root["cursors"].push_back(cursor_to_json(c));
    if (make_active) root["active_cursor"] = c.name;
    return save_store(session_id, root);
}

bool CursorManager::get_cursor(int session_id, const std::string& name, Cursor& cursor) const {
    Json root;
    if (!load_store(session_id, root)) return false;
    for (const auto& item : root["cursors"]) {
        Cursor c;
        if (json_to_cursor(item, c) && c.name == name) {
            cursor = c;
            return true;
        }
    }
    return false;
}

bool CursorManager::delete_cursor(int session_id, const std::string& name) {
    Json root;
    if (!load_store(session_id, root)) return false;
    Json kept = Json::array();
    bool found = false;
    for (const auto& item : root["cursors"]) {
        Cursor c;
        if (json_to_cursor(item, c) && c.name == name) {
            found = true;
            continue;
        }
        kept.push_back(item);
    }
    if (!found) return false;
    root["cursors"] = kept;
    if (root.value("active_cursor", std::string()) == name) root["active_cursor"] = "";
    return save_store(session_id, root);
}

bool CursorManager::use_cursor(int session_id, const std::string& name) {
    Cursor c;
    if (!get_cursor(session_id, name, c)) return false;
    Json root;
    if (!load_store(session_id, root)) return false;
    root["active_cursor"] = name;
    return save_store(session_id, root);
}

bool CursorManager::get_active_cursor(int session_id, std::string& name) const {
    Json root;
    if (!load_store(session_id, root)) return false;
    name = root.value("active_cursor", std::string());
    return !name.empty();
}

std::vector<Cursor> CursorManager::list_cursors(int session_id) const {
    Json root;
    std::vector<Cursor> out;
    if (!load_store(session_id, root)) return out;
    for (const auto& item : root["cursors"]) {
        Cursor c;
        if (json_to_cursor(item, c)) out.push_back(c);
    }
    return out;
}

} // namespace xwave
