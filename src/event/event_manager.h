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
    bool load_all(std::vector<std::string>& lines);
    bool save_all(const std::vector<std::string>& lines);

    char events_path_[256];
};

} // namespace xwave
