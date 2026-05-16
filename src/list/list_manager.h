#pragma once
#include "signal_list.h"
#include <vector>
#include <string>

namespace xwave {

class ListManager {
public:
    ListManager();
    ~ListManager();

    bool create_list(int session_id, const std::string& name);
    bool delete_list(int session_id, const std::string& name);
    bool add_signal(int session_id, const std::string& list_name, const std::string& signal);
    bool del_signal(int session_id, const std::string& list_name, const std::string& path_or_index);
    bool get_list(int session_id, const std::string& name, SignalList& list);
    bool get_latest_list(int session_id, std::string& name);
    std::vector<SignalList> list_all(int session_id);

private:
    bool load_session(int session_id, std::vector<SignalList>& lists);
    bool save_session(int session_id, const std::vector<SignalList>& lists);
    bool migrate_legacy(int session_id, std::vector<SignalList>& lists);
    bool parse_legacy_line(const char* line, SignalList& list, int& session_id);
};

} // namespace xwave
