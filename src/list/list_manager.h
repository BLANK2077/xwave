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
    char lists_path_[256];

    bool load_all(std::vector<SignalList>& lists, std::vector<int>& session_ids);
    bool save_all(const std::vector<SignalList>& lists, const std::vector<int>& session_ids);
    bool parse_line(const char* line, SignalList& list, int& session_id);
};

} // namespace xwave
