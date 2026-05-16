#include "list_manager.h"
#include "../common/xwave_paths.h"
#include "../json.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <cctype>

namespace xwave {

using Json = nlohmann::ordered_json;

ListManager::ListManager() {
}

ListManager::~ListManager() {
}

static bool lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

static bool unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

static Json list_to_json(const SignalList& list) {
    Json j;
    j["name"] = list.name;
    j["signals"] = list.signals;
    return j;
}

static bool json_to_list(const Json& j, SignalList& list) {
    if (!j.is_object()) return false;
    list.name = j.value("name", "");
    list.signals.clear();
    if (j.contains("signals") && j["signals"].is_array()) {
        for (const auto& sig : j["signals"]) {
            if (sig.is_string()) list.signals.push_back(sig.get<std::string>());
        }
    }
    return !list.name.empty();
}

bool ListManager::parse_legacy_line(const char* line, SignalList& list, int& session_id) {
    char name_buf[256];
    if (sscanf(line, "%d|%255[^|\n]", &session_id, name_buf) != 2) {
        return false;
    }
    list.name = name_buf;
    list.signals.clear();

    char* mutable_line = strdup(line);
    char* p = strchr(mutable_line, '|');
    if (p) {
        p = strchr(p + 1, '|');
        while (p) {
            p++;
            char* end = strchr(p, '|');
            if (end) *end = '\0';
            if (strlen(p) > 0) list.signals.push_back(p);
            p = end;
        }
    }
    free(mutable_line);
    return true;
}

bool ListManager::migrate_legacy(int session_id, std::vector<SignalList>& lists) {
    lists.clear();
    if (!xwave_legacy_registry_has_session(session_id)) return false;

    std::string legacy_path = xwave_legacy_lists_path();
    int fd = open(legacy_path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return false;
    }

    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        SignalList list;
        int sid = 0;
        if (parse_legacy_line(line, list, sid) && sid == session_id) lists.push_back(list);
    }
    fclose(fp);
    if (!lists.empty()) save_session(session_id, lists);
    return true;
}

bool ListManager::load_session(int session_id, std::vector<SignalList>& lists) {
    lists.clear();
    std::string path = xwave_lists_path(session_id);
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return migrate_legacy(session_id, lists);
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
        if (!root.is_object() || !root.value("lists", Json::array()).is_array()) return true;
        for (const auto& item : root["lists"]) {
            SignalList list;
            if (json_to_list(item, list)) lists.push_back(list);
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool ListManager::save_session(int session_id, const std::vector<SignalList>& lists) {
    if (!xwave_ensure_session_dir(session_id)) return false;
    std::string path = xwave_lists_path(session_id);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    if (!lock_file(fd)) {
        close(fd);
        return false;
    }
    Json root;
    root["version"] = 1;
    root["lists"] = Json::array();
    for (const auto& list : lists) root["lists"].push_back(list_to_json(list));
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());
    unlock_file(fd);
    close(fd);
    return ok;
}

bool ListManager::create_list(int session_id, const std::string& name) {
    std::vector<SignalList> lists;
    load_session(session_id, lists);
    for (const auto& list : lists) {
        if (list.name == name) return false;
    }
    SignalList list;
    list.name = name;
    lists.push_back(list);
    return save_session(session_id, lists);
}

bool ListManager::delete_list(int session_id, const std::string& name) {
    std::vector<SignalList> lists;
    load_session(session_id, lists);
    std::vector<SignalList> out;
    bool found = false;
    for (const auto& list : lists) {
        if (list.name == name) {
            found = true;
            continue;
        }
        out.push_back(list);
    }
    return found && save_session(session_id, out);
}

bool ListManager::add_signal(int session_id, const std::string& list_name, const std::string& signal) {
    std::vector<SignalList> lists;
    load_session(session_id, lists);
    for (size_t i = 0; i < lists.size(); ++i) {
        if (lists[i].name == list_name) {
            lists[i].signals.push_back(signal);
            SignalList modified = lists[i];
            lists.erase(lists.begin() + i);
            lists.push_back(modified);
            return save_session(session_id, lists);
        }
    }
    return false;
}

bool ListManager::del_signal(int session_id, const std::string& list_name, const std::string& path_or_index) {
    std::vector<SignalList> lists;
    load_session(session_id, lists);
    for (size_t i = 0; i < lists.size(); ++i) {
        if (lists[i].name != list_name) continue;
        bool is_index = true;
        for (char c : path_or_index) {
            if (!isdigit(static_cast<unsigned char>(c))) {
                is_index = false;
                break;
            }
        }
        if (is_index) {
            int idx = atoi(path_or_index.c_str());
            if (idx <= 0 || idx > static_cast<int>(lists[i].signals.size())) return false;
            lists[i].signals.erase(lists[i].signals.begin() + (idx - 1));
        } else {
            bool found = false;
            for (auto it = lists[i].signals.begin(); it != lists[i].signals.end(); ++it) {
                if (*it == path_or_index) {
                    lists[i].signals.erase(it);
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        SignalList modified = lists[i];
        lists.erase(lists.begin() + i);
        lists.push_back(modified);
        return save_session(session_id, lists);
    }
    return false;
}

bool ListManager::get_list(int session_id, const std::string& name, SignalList& list) {
    std::vector<SignalList> lists;
    load_session(session_id, lists);
    for (const auto& candidate : lists) {
        if (candidate.name == name) {
            list = candidate;
            return true;
        }
    }
    return false;
}

bool ListManager::get_latest_list(int session_id, std::string& name) {
    std::vector<SignalList> lists;
    load_session(session_id, lists);
    if (lists.empty()) return false;
    name = lists.back().name;
    return true;
}

std::vector<SignalList> ListManager::list_all(int session_id) {
    std::vector<SignalList> lists;
    load_session(session_id, lists);
    return lists;
}

} // namespace xwave
