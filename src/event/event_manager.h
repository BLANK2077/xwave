#pragma once

#include "event_config.h"
#include <string>
#include <vector>

namespace xwave {

class EventManager {
public:
    EventManager();

    bool create_event(const std::string& session_id, const std::string& fsdb_file, const EventConfig& config);
    bool create_event(int session_id, const std::string& fsdb_file, const EventConfig& config) { return create_event(std::to_string(session_id), fsdb_file, config); }
    bool delete_event(const std::string& session_id, const std::string& fsdb_file, const std::string& name);
    bool delete_session_events(const std::string& session_id);
    bool get_event(const std::string& session_id, const std::string& fsdb_file, const std::string& name, EventConfig& config);
    bool get_event(int session_id, const std::string& fsdb_file, const std::string& name, EventConfig& config) { return get_event(std::to_string(session_id), fsdb_file, name, config); }
    bool get_latest_event(const std::string& session_id, const std::string& fsdb_file, std::string& name);
    bool get_latest_event(int session_id, const std::string& fsdb_file, std::string& name) { return get_latest_event(std::to_string(session_id), fsdb_file, name); }
    std::vector<std::string> list_events(const std::string& session_id, const std::string& fsdb_file);
    std::vector<std::string> list_events(int session_id, const std::string& fsdb_file) { return list_events(std::to_string(session_id), fsdb_file); }

private:
    bool load_session(const std::string& session_id, std::vector<EventConfig>& configs, std::vector<std::string>& fsdb_files);
    bool save_session(const std::string& session_id, const std::vector<EventConfig>& configs, const std::vector<std::string>& fsdb_files);
    bool migrate_legacy(const std::string& session_id, std::vector<EventConfig>& configs, std::vector<std::string>& fsdb_files);
};

} // namespace xwave
