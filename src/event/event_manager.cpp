#include "event_manager.h"
#include "../json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace xwave {

using Json = nlohmann::ordered_json;

EventManager::EventManager() {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(events_path_, sizeof(events_path_), "%s/.xwave.events", home);
}

static Json field_to_json(const EventField& field) {
    Json j;
    j["signal"] = field.signal_alias;
    j["left"] = field.left;
    j["right"] = field.right;
    return j;
}

static Json config_to_json(int session_id, const EventConfig& config) {
    Json j;
    j["session_id"] = session_id;
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

static bool json_to_config(const Json& j, int& session_id, EventConfig& config) {
    if (!j.is_object()) return false;
    session_id = j.value("session_id", -1);
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
    return session_id > 0 && !config.name.empty() && !config.clk.empty();
}

bool EventManager::load_all(std::vector<std::string>& lines) {
    lines.clear();
    int fd = open(events_path_, O_RDONLY | O_CREAT, 0600);
    if (fd < 0) return false;
    FILE* fp = fdopen(fd, "r");
    if (!fp) {
        close(fd);
        return false;
    }
    char* line = nullptr;
    size_t len = 0;
    while (getline(&line, &len, fp) != -1) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) lines.push_back(s);
    }
    if (line) free(line);
    fclose(fp);
    return true;
}

bool EventManager::save_all(const std::vector<std::string>& lines) {
    int fd = open(events_path_, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    FILE* fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return false;
    }
    for (const auto& line : lines) fprintf(fp, "%s\n", line.c_str());
    fclose(fp);
    return true;
}

bool EventManager::create_event(int session_id, const EventConfig& config) {
    std::vector<std::string> lines;
    if (!load_all(lines)) return false;

    std::vector<std::string> out;
    for (const auto& line : lines) {
        try {
            Json j = Json::parse(line);
            int sid = j.value("session_id", -1);
            std::string name = j.value("name", "");
            if (sid == session_id && name == config.name) continue;
        } catch (...) {
        }
        out.push_back(line);
    }
    out.push_back(config_to_json(session_id, config).dump());
    return save_all(out);
}

bool EventManager::delete_event(int session_id, const std::string& name) {
    std::vector<std::string> lines;
    if (!load_all(lines)) return false;
    std::vector<std::string> out;
    bool removed = false;
    for (const auto& line : lines) {
        try {
            Json j = Json::parse(line);
            int sid = j.value("session_id", -1);
            std::string cfg_name = j.value("name", "");
            if (sid == session_id && cfg_name == name) {
                removed = true;
                continue;
            }
        } catch (...) {
        }
        out.push_back(line);
    }
    return removed && save_all(out);
}

bool EventManager::get_event(int session_id, const std::string& name, EventConfig& config) {
    std::vector<std::string> lines;
    if (!load_all(lines)) return false;
    for (const auto& line : lines) {
        try {
            Json j = Json::parse(line);
            int sid = -1;
            EventConfig cfg;
            if (json_to_config(j, sid, cfg) && sid == session_id && cfg.name == name) {
                config = cfg;
                return true;
            }
        } catch (...) {
        }
    }
    return false;
}

bool EventManager::get_latest_event(int session_id, std::string& name) {
    std::vector<std::string> lines;
    if (!load_all(lines)) return false;
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        try {
            Json j = Json::parse(*it);
            if (j.value("session_id", -1) == session_id) {
                name = j.value("name", "");
                return !name.empty();
            }
        } catch (...) {
        }
    }
    return false;
}

std::vector<std::string> EventManager::list_events(int session_id) {
    std::vector<std::string> lines;
    std::vector<std::string> names;
    if (!load_all(lines)) return names;
    for (const auto& line : lines) {
        try {
            Json j = Json::parse(line);
            if (j.value("session_id", -1) == session_id) {
                std::string name = j.value("name", "");
                if (!name.empty()) names.push_back(name);
            }
        } catch (...) {
        }
    }
    return names;
}

} // namespace xwave
