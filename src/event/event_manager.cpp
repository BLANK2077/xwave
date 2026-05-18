#include "event_manager.h"
#include "../common/xwave_paths.h"
#include "../json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

namespace xwave {

using Json = nlohmann::ordered_json;

EventManager::EventManager() {
}

static Json field_to_json(const EventField& field) {
    Json j;
    j["signal"] = field.signal_alias;
    j["left"] = field.left;
    j["right"] = field.right;
    return j;
}

static Json config_to_json(const std::string& fsdb_file, const EventConfig& config) {
    Json j;
    j["fsdb_file"] = fsdb_file;
    j["name"] = config.name;
    j["clk"] = config.clk;
    j["rst_n"] = config.rst_n;
    j["edge"] = config.posedge ? "posedge" : "negedge";
    j["signals"] = config.signals;
    Json fields = Json::object();
    for (const auto& kv : config.fields) fields[kv.first] = field_to_json(kv.second);
    j["fields"] = fields;
    return j;
}

static bool json_to_config(const Json& j, std::string& fsdb_file, EventConfig& config) {
    if (!j.is_object()) return false;
    fsdb_file = j.value("fsdb_file", "");
    config.name = j.value("name", "");
    config.clk = j.value("clk", "");
    config.rst_n = j.value("rst_n", "");
    const std::string edge = j.value("edge", "posedge");
    config.posedge = (edge != "negedge");
    config.signals.clear();
    config.fields.clear();
    if (j.contains("signals") && j["signals"].is_object()) {
        for (auto it = j["signals"].begin(); it != j["signals"].end(); ++it) {
            if (it.value().is_string()) config.signals[it.key()] = it.value().get<std::string>();
        }
    }
    if (j.contains("fields") && j["fields"].is_object()) {
        for (auto it = j["fields"].begin(); it != j["fields"].end(); ++it) {
            if (!it.value().is_object()) continue;
            EventField field;
            field.signal_alias = it.value().value("signal", "");
            field.left = it.value().value("left", 0);
            field.right = it.value().value("right", 0);
            config.fields[it.key()] = field;
        }
    }
    return !fsdb_file.empty() && !config.name.empty() && !config.clk.empty();
}

static bool lock_file(int fd) {
    return flock(fd, LOCK_EX) == 0;
}

static bool unlock_file(int fd) {
    return flock(fd, LOCK_UN) == 0;
}

bool EventManager::migrate_legacy(const std::string& session_id, std::vector<EventConfig>& configs, std::vector<std::string>& fsdb_files) {
    (void)session_id;
    (void)configs;
    (void)fsdb_files;
    return false;
}

bool EventManager::load_session(const std::string& session_id, std::vector<EventConfig>& configs, std::vector<std::string>& fsdb_files) {
    configs.clear();
    fsdb_files.clear();
    int fd = open(xwave_events_path(session_id).c_str(), O_RDONLY);
    if (fd < 0) {
        migrate_legacy(session_id, configs, fsdb_files);
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
        if (!root.is_object() || !root.value("events", Json::array()).is_array()) return true;
        for (const auto& item : root["events"]) {
            std::string fsdb_file;
            EventConfig cfg;
            if (json_to_config(item, fsdb_file, cfg)) {
                configs.push_back(cfg);
                fsdb_files.push_back(fsdb_file);
            }
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool EventManager::save_session(const std::string& session_id, const std::vector<EventConfig>& configs, const std::vector<std::string>& fsdb_files) {
    if (!xwave_ensure_session_dir(session_id)) return false;
    int fd = open(xwave_events_path(session_id).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    if (!lock_file(fd)) {
        close(fd);
        return false;
    }
    Json root;
    root["version"] = 1;
    root["events"] = Json::array();
    for (size_t i = 0; i < configs.size(); ++i) {
        root["events"].push_back(config_to_json(fsdb_files[i], configs[i]));
    }
    std::string data = root.dump(2) + "\n";
    bool ok = write(fd, data.c_str(), data.size()) == static_cast<ssize_t>(data.size());
    unlock_file(fd);
    close(fd);
    return ok;
}

bool EventManager::create_event(const std::string& session_id, const std::string& fsdb_file, const EventConfig& config) {
    std::vector<EventConfig> configs;
    std::vector<std::string> fsdb_files;
    if (!load_session(session_id, configs, fsdb_files)) return false;

    std::vector<EventConfig> out_configs;
    std::vector<std::string> out_fsdbs;
    for (size_t i = 0; i < configs.size(); ++i) {
        if (fsdb_files[i] == fsdb_file && configs[i].name == config.name) continue;
        out_configs.push_back(configs[i]);
        out_fsdbs.push_back(fsdb_files[i]);
    }
    out_configs.push_back(config);
    out_fsdbs.push_back(fsdb_file);
    return save_session(session_id, out_configs, out_fsdbs);
}

bool EventManager::delete_event(const std::string& session_id, const std::string& fsdb_file, const std::string& name) {
    std::vector<EventConfig> configs;
    std::vector<std::string> fsdb_files;
    if (!load_session(session_id, configs, fsdb_files)) return false;
    std::vector<EventConfig> out_configs;
    std::vector<std::string> out_fsdbs;
    bool removed = false;
    for (size_t i = 0; i < configs.size(); ++i) {
        if (fsdb_files[i] == fsdb_file && configs[i].name == name) {
            removed = true;
            continue;
        }
        out_configs.push_back(configs[i]);
        out_fsdbs.push_back(fsdb_files[i]);
    }
    return removed && save_session(session_id, out_configs, out_fsdbs);
}

bool EventManager::delete_session_events(const std::string& session_id) {
    std::string path = xwave_events_path(session_id);
    if (unlink(path.c_str()) == 0) return true;
    return access(path.c_str(), F_OK) != 0;
}

bool EventManager::get_event(const std::string& session_id, const std::string& fsdb_file, const std::string& name, EventConfig& config) {
    std::vector<EventConfig> configs;
    std::vector<std::string> fsdb_files;
    if (!load_session(session_id, configs, fsdb_files)) return false;
    for (size_t i = 0; i < configs.size(); ++i) {
        if (fsdb_files[i] == fsdb_file && configs[i].name == name) {
            config = configs[i];
            return true;
        }
    }
    return false;
}

bool EventManager::get_latest_event(const std::string& session_id, const std::string& fsdb_file, std::string& name) {
    std::vector<EventConfig> configs;
    std::vector<std::string> fsdb_files;
    if (!load_session(session_id, configs, fsdb_files)) return false;
    for (int i = static_cast<int>(configs.size()) - 1; i >= 0; --i) {
        if (fsdb_files[i] == fsdb_file) {
            name = configs[i].name;
            return !name.empty();
        }
    }
    return false;
}

std::vector<std::string> EventManager::list_events(const std::string& session_id, const std::string& fsdb_file) {
    std::vector<EventConfig> configs;
    std::vector<std::string> fsdb_files;
    std::vector<std::string> names;
    if (!load_session(session_id, configs, fsdb_files)) return names;
    for (size_t i = 0; i < configs.size(); ++i) {
        if (fsdb_files[i] == fsdb_file && !configs[i].name.empty()) names.push_back(configs[i].name);
    }
    return names;
}

} // namespace xwave
