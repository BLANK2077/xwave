#pragma once

#include "event_config.h"
#include <string>
#include <vector>

namespace xwave {

class EventManager {
public:
    EventManager();

    bool create_event(int session_id, const std::string& fsdb_file, const EventConfig& config);
    bool delete_event(int session_id, const std::string& fsdb_file, const std::string& name);
    bool delete_session_events(int session_id);
    bool get_event(int session_id, const std::string& fsdb_file, const std::string& name, EventConfig& config);
    bool get_latest_event(int session_id, const std::string& fsdb_file, std::string& name);
    std::vector<std::string> list_events(int session_id, const std::string& fsdb_file);

private:
    bool load_session(int session_id, std::vector<EventConfig>& configs, std::vector<std::string>& fsdb_files);
    bool save_session(int session_id, const std::vector<EventConfig>& configs, const std::vector<std::string>& fsdb_files);
    bool migrate_legacy(int session_id, std::vector<EventConfig>& configs, std::vector<std::string>& fsdb_files);
};

} // namespace xwave
